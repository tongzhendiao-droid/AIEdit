#include "ui/DocumentView.h"

#include "core/EditBuffer.h"
#include "core/FileSource.h"
#include "core/FileSummary.h"
#include "ui/AiEdit.h"
#include "ui/AiEditBar.h"
#include "ui/AiPanel.h"
#include "ui/Codec.h"
#include "ui/FilterPanel.h"
#include "ui/FilterRuleWriter.h"
#include "ui/FindBar.h"
#include "ui/HexView.h"
#include "ui/Paths.h"
#include "ui/Replace.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace aiedit::ui {

namespace {

// Writers append in bursts; reload once things have been quiet this long.
constexpr int kReloadDelayMs = 250;
constexpr int kPollIntervalMs = 1000;
// How much of a selection goes to the AI: text as it is, bytes as a hex dump (about 4x larger).
constexpr std::uint64_t kMaxContextTextBytes = 32 * 1024;
constexpr std::uint64_t kMaxContextHexBytes = 4 * 1024;
constexpr std::size_t kMaxSummaryStrings = 40;
constexpr qsizetype kMaxSummaryStringLength = 120;

// Descriptions for the model, so they stay in English.
QString describeForAi(const core::FileTypeInfo& type, const core::TextCodec& codec, std::uint64_t size)
{
    QString kind;
    switch (type.kind) {
    case core::ContentKind::Empty:
        kind = QStringLiteral("empty");
        break;
    case core::ContentKind::Binary:
        kind = type.format.empty() ? QStringLiteral("binary")
                                   : QStringLiteral("binary, %1").arg(QString::fromUtf8(type.format.data(),
                                                                                       static_cast<qsizetype>(type.format.size())));
        break;
    case core::ContentKind::Text:
        kind = QStringLiteral("text, decoded as %1").arg(QString::fromLatin1(codec.name.data(),
                                                                             static_cast<qsizetype>(codec.name.size())));
        break;
    }
    return QStringLiteral("%1, %2 bytes").arg(kind).arg(size);
}

QString summaryForAi(const core::FileSummary& summary, const QString& name, const QString& description)
{
    QString text = QStringLiteral("File: %1 (%2)\n").arg(name, description);
    text += QStringLiteral("Entropy of the first 64 KiB: %1 bits per byte\n").arg(summary.headEntropy, 0, 'f', 2);
    if (!summary.blockEntropy.empty()) {
        QStringList values;
        for (const double value : summary.blockEntropy)
            values << QString::number(value, 'f', 1);
        text += QStringLiteral("Entropy of evenly spaced 4 KiB blocks across the file: %1\n")
                    .arg(values.join(QLatin1Char(' ')));
    }
    if (!summary.strings.empty()) {
        text += QStringLiteral("Printable strings in the first 64 KiB:\n");
        for (std::size_t i = 0; i < std::min(summary.strings.size(), kMaxSummaryStrings); ++i)
            text += QStringLiteral("  %1\n").arg(QString::fromLatin1(summary.strings[i]).left(kMaxSummaryStringLength));
    }
    text += QStringLiteral("First bytes:\n```hexdump\n%1```").arg(QString::fromLatin1(summary.headDump));
    return text;
}

} // namespace

DocumentView::DocumentView(std::shared_ptr<const core::FileSource> source, const core::FileTypeInfo& fileType,
                           const core::TextCodec& codec, QWidget* parent)
    : QWidget(parent), m_source(std::move(source)), m_fileType(fileType)
{
    m_textDocument = new TextDocument(m_source, codec, this);

    m_textView = new TextView;
    m_textView->setFrameShape(QFrame::NoFrame);
    m_textView->setDocument(m_textDocument);
    m_hexView = new HexView;
    m_hexView->setFrameShape(QFrame::NoFrame);
    m_hexView->setSource(m_source);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_textView);
    m_splitter->addWidget(m_hexView);
    // Side by side, the hex view keeps the width of its columns and the text view takes the rest.
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);

    m_findBar = new FindBar(this);
    m_findBar->hide();
    m_findBar->setMode(fileType.kind == core::ContentKind::Binary ? SearchMode::Hex : SearchMode::Text);
    connect(m_findBar, &FindBar::searchRequested, this, &DocumentView::findNext);
    connect(m_findBar, &FindBar::matchFound, this, &DocumentView::selectMatch);
    connect(m_findBar, &FindBar::closeRequested, this, &DocumentView::closeFindBar);
    connect(m_findBar, &FindBar::replaceRequested, this, &DocumentView::replaceCurrent);
    connect(m_findBar, &FindBar::replaceAllRequested, this, &DocumentView::replaceAll);
    // A search runs against the index of one codec; a new codec makes it meaningless.
    connect(m_textDocument, &TextDocument::codecChanged, m_findBar, &FindBar::cancel);

    m_documentSplitter = new QSplitter(Qt::Vertical, this);
    m_documentSplitter->setChildrenCollapsible(false);
    m_documentSplitter->addWidget(m_splitter);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_documentSplitter);
    layout->addWidget(m_findBar);

    connect(m_textView, &TextView::cursorMoved, this, &DocumentView::syncFromText);
    connect(m_textView, &TextView::selectionChanged, this, &DocumentView::syncFromText);
    connect(m_hexView, &HexView::cursorMoved, this, &DocumentView::syncFromHex);
    connect(m_hexView, &HexView::selectionChanged, this, &DocumentView::syncFromHex);
    for (auto* view : {static_cast<QWidget*>(m_textView), static_cast<QWidget*>(m_hexView)})
        view->installEventFilter(this);
    connect(m_textView, &TextView::zoomRequested, this, &DocumentView::zoomRequested);
    connect(m_hexView, &HexView::zoomRequested, this, &DocumentView::zoomRequested);
    connect(m_textView, &TextView::statusMessage, this, &DocumentView::statusMessage);
    connect(m_hexView, &HexView::statusMessage, this, &DocumentView::statusMessage);
    connect(m_textDocument, &TextDocument::indexGrew, this, &DocumentView::indexProgressed);
    connect(m_textDocument, &TextDocument::indexFinished, this, &DocumentView::indexProgressed);
    connect(m_textDocument, &TextDocument::edited, this, &DocumentView::documentEdited);
    connect(m_textDocument, &TextDocument::modificationChanged, this, &DocumentView::modificationChanged);
    updateEditable();

    if (fileType.kind != core::ContentKind::Binary)
        m_textView->setSyntax(syntaxForFileName(QFileInfo(filePath()).fileName()));

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(kReloadDelayMs);
    // Edits made while a reload waited are kept: fileChanged() says so instead of reloading.
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        if (isModified())
            fileChanged();
        else
            reload();
    });
    if (!isUntitled())
        m_watcher.addPath(filePath());
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &DocumentView::fileChanged);
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, [this] {
        if (m_polling || isUntitled())
            return;
        m_polling = true;
        m_pollWatcher.setFuture(QtConcurrent::run([path = filePath()] { return diskState(path); }));
    });
    connect(&m_pollWatcher, &QFutureWatcher<DiskState>::finished, this, [this] {
        m_polling = false;
        const DiskState now = m_pollWatcher.result();
        if (now.path == filePath() && now != m_diskState) { // not of a file it showed before
            m_diskState = now;
            fileChanged();
        }
    });
    if (!isUntitled()) {
        m_diskState = diskState();
        m_pollTimer.start();
    }

    // Text files open as text, everything else as hex.
    m_viewMode = ViewMode::Split; // anything different from the target, so setViewMode() applies it
    setViewMode(fileType.kind == core::ContentKind::Binary ? ViewMode::Hex : ViewMode::Text);
}

DocumentView::~DocumentView()
{
    m_ruleWatcher.cancel();
    m_ruleWatcher.waitForFinished();
    // The children outlive this destructor body (QWidget deletes them later), and the text
    // document goes first. Keep their signals and focus events from reaching a half-destroyed
    // DocumentView.
    for (QWidget* view : {static_cast<QWidget*>(m_textView), static_cast<QWidget*>(m_hexView)}) {
        view->removeEventFilter(this);
        view->disconnect(this);
    }
    m_textView->setDocument(nullptr);
}

QString DocumentView::filePath() const
{
    return toQString(m_source->path());
}

bool DocumentView::isUntitled() const
{
    return m_source->path().empty();
}

QString DocumentView::title() const
{
    return isUntitled() ? m_untitledTitle : QFileInfo(filePath()).fileName();
}

void DocumentView::setViewMode(ViewMode mode)
{
    if (mode == m_viewMode)
        return;
    const bool hadFocus = isAncestorOf(QApplication::focusWidget());
    m_viewMode = mode;
    m_textView->setVisible(mode != ViewMode::Hex);
    m_hexView->setVisible(mode != ViewMode::Text);
    if (mode != ViewMode::Split)
        m_textFocusedLast = mode == ViewMode::Text;
    else {
        m_splitSizesPending = true;
        applySplitSizes();
    }

    QWidget* active = isTextActive() ? static_cast<QWidget*>(m_textView) : m_hexView;
    setFocusProxy(active);
    if (hadFocus)
        active->setFocus();
    emit stateChanged();
}

bool DocumentView::isReadingMode() const
{
    return m_textView->isReadingMode();
}

void DocumentView::setReadingMode(bool on)
{
    if (on == isReadingMode())
        return;
    if (on && m_viewMode != ViewMode::Text)
        setViewMode(ViewMode::Text); // a book is read as text
    m_textView->setReadingMode(on);
    m_showsContents = on; // a book's chapters beside it; nothing beside other text
    emit readingModeChanged(on);
    emit stateChanged();
}

void DocumentView::showFromOffset(std::uint64_t offset)
{
    if (m_viewMode == ViewMode::Hex)
        setViewMode(ViewMode::Text);
    m_textView->setCursorOffset(offset);
    m_textView->scrollToOffset(offset);
}

bool DocumentView::isTextActive() const
{
    switch (m_viewMode) {
    case ViewMode::Text:
        return true;
    case ViewMode::Hex:
        return false;
    case ViewMode::Split:
        return m_textFocusedLast;
    }
    return true;
}

std::uint64_t DocumentView::cursorOffset() const
{
    return isTextActive() ? m_textView->cursorOffset() : m_hexView->cursorOffset();
}

bool DocumentView::hasSelection() const
{
    return isTextActive() ? m_textView->hasSelection() : m_hexView->selectionLength() > 1;
}

void DocumentView::setContextActions(const QList<QAction*>& textActions, const QList<QAction*>& hexActions)
{
    m_textView->setContextActions(textActions);
    m_hexView->setContextActions(hexActions);
}

void DocumentView::setCodec(const core::TextCodec& codec)
{
    m_codecChosen = true;
    m_textDocument->setCodec(codec);
    emit stateChanged();
}

DocumentView::State DocumentView::state() const
{
    if (m_pendingState)
        return *m_pendingState; // not even there yet
    State state;
    state.path = filePath();
    state.cursor = cursorOffset();
    state.top = m_textView->firstVisibleOffset();
    state.mode = m_viewMode;
    state.reading = isReadingMode();
    if (m_codecChosen)
        state.codec = QString::fromLatin1(m_textDocument->codec().name.data());
    state.bookmarks = m_textView->bookmarks();
    return state;
}

void DocumentView::restoreState(const State& state)
{
    if (const core::TextCodec* codec = core::findCodec(state.codec.toStdString());
        codec && codec->name != m_textDocument->codec().name)
        setCodec(*codec);
    setViewMode(state.mode);
    setReadingMode(state.reading);
    m_textView->setBookmarks(state.bookmarks); // those the index has not reached settle later
    const auto apply = [this, state] {
        m_pendingState.reset();
        if (isTextActive()) {
            m_textView->setCursorOffset(state.cursor);
            m_textView->scrollToOffset(state.top);
        } else {
            m_hexView->setCursorOffset(std::min(state.cursor, m_source->size() > 0 ? m_source->size() - 1 : 0));
        }
    };
    // In a large file still being indexed, wait until the index gets there.
    const core::LineIndex& index = m_textDocument->index();
    if (index.isComplete() || std::max(state.cursor, state.top) < index.indexedBytes()) {
        apply();
    } else {
        m_pendingState = state;
        connect(m_textDocument, &TextDocument::indexFinished, this, apply, Qt::SingleShotConnection);
    }
}

void DocumentView::showFindBar(bool replace)
{
    // Seed the query with a short selection, as text or as hex bytes.
    if (isTextActive() && m_textView->hasSelection()) {
        const QString selected = m_textView->selectedText();
        if (!selected.isEmpty() && selected.size() <= 256 && !selected.contains(QLatin1Char('\n'))) {
            m_findBar->setQuery(selected);
            if (m_findBar->mode() == SearchMode::Hex)
                m_findBar->setMode(SearchMode::Text);
        }
    } else if (!isTextActive() && m_hexView->selectionLength() > 1 && m_hexView->selectionLength() <= 64) {
        m_findBar->setQuery(m_hexView->selectionAsHex());
        m_findBar->setMode(SearchMode::Hex);
    }
    m_findBar->activate(replace);
}

SearchContext DocumentView::searchContext() const
{
    return {m_source, m_textDocument->sharedIndex(), m_textDocument->codec()};
}

void DocumentView::replaceCurrent()
{
    if (!isEditable() || m_findBar->mode() == SearchMode::Hex)
        return;
    if (m_findBar->query().isEmpty()) {
        showFindBar(true);
        return;
    }
    ReplaceRequest request{m_findBar->mode(), m_findBar->query(), m_findBar->caseSensitive(),
                           m_findBar->replacement(), m_textDocument->lineTerminator()};
    // Only what the search found and is still selected; otherwise this finds the first match.
    const std::uint64_t start = m_textView->selectionStart();
    const std::uint64_t end = m_textView->selectionEnd();
    if (m_lastMatch && *m_lastMatch == std::pair(start, end) && end > start) {
        QString error;
        const std::optional<QString> text = replacementFor(request, searchContext(), start, end, &error);
        if (!text) {
            m_findBar->showMessage(error, true);
            return;
        }
        if (!m_textView->replaceSelectedText(*text))
            return; // the status bar says why
    }
    findNext(true);
}

void DocumentView::replaceAll()
{
    if (!isEditable() || m_findBar->mode() == SearchMode::Hex)
        return;
    if (m_findBar->query().isEmpty()) {
        showFindBar(true);
        return;
    }
    m_findBar->cancel();
    const ReplaceRequest request{m_findBar->mode(), m_findBar->query(), m_findBar->caseSensitive(),
                                 m_findBar->replacement(), m_textDocument->lineTerminator()};
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const ReplaceAllResult result = ui::replaceAll(request, searchContext());
    QGuiApplication::restoreOverrideCursor();
    if (!result.error.isEmpty()) {
        m_findBar->showMessage(result.error, true);
        return;
    }
    if (result.count == 0) {
        m_findBar->showMessage(tr("Nothing to replace"), true);
        return;
    }
    m_textDocument->breakUndoMerge();
    m_textDocument->replaceBytes(result.start, result.end, result.bytes);
    m_textView->setCursorOffset(result.start); // at the first replacement
    m_findBar->showMessage(tr("Replaced %n time(s)", nullptr, static_cast<int>(result.count)));
}

void DocumentView::findNext(bool forward)
{
    if (m_findBar->query().isEmpty()) {
        showFindBar();
        return;
    }

    // Continue after the match found last; otherwise start at the caret or selection.
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    if (isTextActive()) {
        start = m_textView->selectionStart();
        end = m_textView->selectionEnd();
    } else {
        start = m_hexView->selectionStart();
        end = start + m_hexView->selectionLength();
        if (!(m_lastMatch && *m_lastMatch == std::pair(start, end)) && m_hexView->selectionLength() <= 1)
            end = start; // a plain cursor: a match may start right there
    }

    SearchRequest request;
    request.mode = m_findBar->mode();
    request.query = m_findBar->query();
    request.caseSensitive = m_findBar->caseSensitive();
    request.forward = forward;
    request.from = forward ? end : start;
    m_findBar->start(request, searchContext());
}

void DocumentView::findSelection(bool forward)
{
    if (!isTextActive()) {
        // Bytes: the selection, as the find bar takes it.
        if (m_hexView->selectionLength() > 1 && m_hexView->selectionLength() <= 64) {
            m_findBar->setQuery(m_hexView->selectionAsHex());
            m_findBar->setMode(SearchMode::Hex);
        }
        findNext(forward);
        return;
    }
    QString text = m_textView->selectedText();
    if (text.isEmpty() || text.contains(QLatin1Char('\n')) || text.size() > 256) {
        const std::uint64_t caret = m_textView->cursorOffset();
        if (!m_textView->selectWordAtCursor())
            return;
        text = m_textView->selectedText();
        if (text.trimmed().isEmpty()) { // between words: nothing to look for
            m_textView->setCursorOffset(caret);
            return;
        }
    }
    m_findBar->setQuery(text);
    m_findBar->setMode(SearchMode::Text);
    findNext(forward);
}

// AI edits ------------------------------------------------------------------------------------------

namespace {

// Where a line starts; the end of the text for a line past the last.
std::uint64_t startOfLine(const core::LineIndex& index, std::uint64_t line, std::uint64_t size)
{
    if (line >= index.lineCount())
        return size;
    const std::optional<std::uint64_t> row = index.rowIndexOfLine(line);
    return row ? index.row(*row).start : size;
}

} // namespace

AiEditor* DocumentView::aiEditor()
{
    if (m_aiEditor)
        return m_aiEditor;
    m_aiEditor = new AiEditor(this);
    connect(m_aiEditor, &AiEditor::progressed, this, [this](int characters) {
        if (m_aiEditBar)
            m_aiEditBar->showMessage(
                tr("The AI is writing… %1 characters").arg(QLocale().toString(characters)));
    });
    connect(m_aiEditor, &AiEditor::textWritten, this, &DocumentView::aiTextWritten);
    connect(m_aiEditor, &AiEditor::ruleWritten, this, [this](const EditRule& rule, const QString& note) {
        if (m_aiEditBar)
            m_aiEditBar->showMessage(tr("Running the AI's rule here…"));
        const QString json = QString::fromUtf8(QJsonDocument(rule.toJson()).toJson(QJsonDocument::Indented));
        m_ruleWatcher.setFuture(QtConcurrent::run(
            [rule, context = m_aiContext, start = m_aiEdit.start, end = m_aiEdit.end, json,
             note](QPromise<RuleRun>& promise) {
                RuleRun run;
                run.rule = json;
                run.note = note;
                run.bytes = applyEditRule(rule, *context, start, end, &run.error,
                                          [&promise] { return promise.isCanceled(); });
                promise.addResult(std::move(run));
            }));
    });
    connect(m_aiEditor, &AiEditor::failed, this, &DocumentView::aiEditFailed);
    connect(m_aiEditor, &AiEditor::settingsRequested, this, &DocumentView::aiSettingsRequested);
    connect(&m_ruleWatcher, &QFutureWatcher<RuleRun>::finished, this, &DocumentView::aiRuleRan);
    return m_aiEditor;
}

void DocumentView::showAiEditBar()
{
    if (!m_aiEditBar) {
        m_aiEditBar = new AiEditBar(this);
        static_cast<QVBoxLayout*>(layout())->insertWidget(0, m_aiEditBar);
        connect(m_aiEditBar, &AiEditBar::editRequested, this, &DocumentView::requestAiEdit);
        const auto stop = [this] {
            aiEditor()->cancel();
            m_ruleWatcher.cancel();
            m_aiEditBar->setBusy(false);
        };
        connect(m_aiEditBar, &AiEditBar::stopRequested, this, [this, stop] {
            stop();
            m_aiEditBar->showMessage(tr("Stopped"));
        });
        connect(m_aiEditBar, &AiEditBar::closeRequested, this, [this, stop] {
            if (m_aiEditBar->isBusy())
                stop();
            m_aiEditBar->hide();
            (isTextActive() ? static_cast<QWidget*>(m_textView) : m_hexView)->setFocus();
        });
        connect(m_aiEditBar, &AiEditBar::previewRequested, this, [this](const QString& instruction) {
            std::uint64_t start = 0;
            std::uint64_t end = 0;
            const AiEditRequest request =
                aiEditRequest(instruction.isEmpty() ? tr("(what to change)") : instruction, &start, &end);
            showRequestPreview(this, aiEditor()->settings(), aiEditor()->previewRequest(request));
        });
        connect(m_textView, &TextView::selectionChanged, this, &DocumentView::updateAiEditScope);
    }
    m_aiEditBar->activate();
    updateAiEditScope();
}

void DocumentView::updateAiEditScope()
{
    if (!m_aiEditBar || m_aiEditBar->isHidden() || m_aiEditBar->isBusy())
        return;
    const std::uint64_t start = m_textView->selectionStart();
    const std::uint64_t end = m_textView->selectionEnd();
    const std::uint64_t size = start == end ? m_source->size() : end - start;
    // A guess from the bytes; requestAiEdit() counts the characters.
    const bool tooLong = size > 4 * static_cast<std::uint64_t>(AiEditor::kMaxText);
    if (start == end) {
        m_aiEditBar->setScope(tooLong ? tr("the whole file, by a rule the AI writes") : tr("the whole file"));
        return;
    }
    const core::LineIndex& index = m_textDocument->index();
    const std::uint64_t first = index.rowCount() ? index.row(index.rowIndexAt(start)).line : 0;
    const std::uint64_t last = index.rowCount() ? index.row(index.rowIndexAt(end - 1)).line : 0;
    const auto lines = static_cast<int>(std::min<std::uint64_t>(last - first + 1, 1'000'000'000));
    m_aiEditBar->setScope(tooLong ? tr("the %n selected line(s), by a rule the AI writes", "", lines)
                                  : tr("the %n selected line(s)", "", lines));
}

AiEditRequest DocumentView::aiEditRequest(const QString& instruction, std::uint64_t* start, std::uint64_t* end) const
{
    const core::LineIndex& index = m_textDocument->index();
    const auto bytes = m_textDocument->bytes();
    *start = m_textView->selectionStart();
    *end = m_textView->selectionEnd();
    const bool whole = *start == *end;
    if (whole) {
        *start = m_textDocument->textStart();
        *end = bytes.size();
    }
    AiEditRequest request;
    request.instruction = instruction;
    request.fileName = isUntitled() ? QString() : QFileInfo(filePath()).fileName();
    request.language = syntax();
    const TextDecoder& decoder = m_textDocument->decoder();
    const auto decode = [&](std::uint64_t from, std::uint64_t to) {
        return decoder.decodePlain(bytes.subspan(from, to - from));
    };
    // Not decoded just to be counted when it is too long by far.
    const bool farTooLong = *end - *start > 4 * static_cast<std::uint64_t>(AiEditor::kMaxText);
    const QString text = farTooLong ? QString() : decode(*start, *end);
    if (!farTooLong && text.size() <= AiEditor::kMaxText) {
        request.text = text;
        if (!whole && index.rowCount() > 0) {
            // Some of what is around it, to read.
            const std::uint64_t firstLine = index.row(index.rowIndexAt(*start)).line;
            const std::uint64_t lastLine = index.row(index.rowIndexAt(*end > *start ? *end - 1 : *end)).line;
            const std::uint64_t context = AiEditor::kContextLines;
            const std::uint64_t from = startOfLine(index, firstLine > context ? firstLine - context : 0, bytes.size());
            const std::uint64_t to = startOfLine(index, lastLine + 1 + context, bytes.size());
            request.before = decode(std::min(from, *start), *start).right(AiEditor::kContextChars);
            request.after = decode(*end, std::max(to, *end)).left(AiEditor::kContextChars);
        }
        return request;
    }
    // Too long to send: whole lines, which a rule the AI writes changes here.
    if (index.rowCount() > 0) {
        *start = startOfLine(index, index.row(index.rowIndexAt(*start)).line, bytes.size());
        if (!whole && *end > 0 && startOfLine(index, index.row(index.rowIndexAt(*end)).line, bytes.size()) != *end)
            *end = startOfLine(index, index.row(index.rowIndexAt(*end)).line + 1, bytes.size());
    }
    request.asRule = true;
    request.samples = FilterRuleWriter::sampleLines(searchContext());
    return request;
}

void DocumentView::requestAiEdit(const QString& instruction)
{
    if (!m_aiEditBar)
        showAiEditBar();
    if (instruction.trimmed().isEmpty())
        return;
    if (!isEditable()) {
        m_aiEditBar->showMessage(tr("This document cannot be edited, so the AI cannot change it either."), true);
        return;
    }
    m_textDocument->indexNow(); // every line, for the range and the samples
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    const AiEditRequest request = aiEditRequest(instruction, &start, &end);
    const auto bytes = m_textDocument->bytes();
    const core::LineIndex& index = m_textDocument->index();
    m_aiEdit = {};
    m_aiEdit.start = start;
    m_aiEdit.end = end;
    m_aiEdit.firstLine = index.rowCount() ? index.row(index.rowIndexAt(start)).line : 0;
    m_aiEdit.before =
        QByteArray(reinterpret_cast<const char*>(bytes.data()) + start, static_cast<qsizetype>(end - start));
    m_aiEdit.instruction = instruction;
    m_aiEditText = request.text;
    m_aiContext = std::make_shared<const SearchContext>(searchContext());
    m_aiEditBar->setBusy(true);
    m_aiEditBar->showMessage(request.asRule
                                 ? tr("Too long to send: the AI writes a rule from samples, which then runs here…")
                                 : tr("Asking the AI…"));
    aiEditor()->edit(request);
}

void DocumentView::aiTextWritten(const QString& text, const QString& note)
{
    if (m_aiEditBar)
        m_aiEditBar->setBusy(false);
    // In the document's line breaks and codec, ending as the text did.
    const QString terminator = m_textDocument->lineTerminator();
    QString changed = text;
    changed.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (terminator != u"\n")
        changed.replace(QLatin1Char('\n'), terminator);
    const auto endsWithBreak = [](const QString& s) { return s.endsWith(u'\n') || s.endsWith(u'\r'); };
    if (endsWithBreak(m_aiEditText) && !endsWithBreak(changed))
        changed += terminator;
    else if (!endsWithBreak(m_aiEditText))
        while (endsWithBreak(changed))
            changed.chop(1);
    const std::optional<QByteArray> encoded = m_textDocument->encode(changed);
    if (!encoded) {
        aiEditFailed(tr("The AI's text has characters that %1 cannot hold; nothing was changed.")
                         .arg(QString::fromLatin1(m_textDocument->codec().name.data())));
        return;
    }
    if (*encoded == m_aiEdit.before) {
        if (m_aiEditBar)
            m_aiEditBar->showMessage(note.isEmpty() ? tr("The AI changed nothing.")
                                                    : tr("The AI changed nothing: %1").arg(note));
        return;
    }
    AiEditProposal proposal = m_aiEdit;
    proposal.after = *encoded;
    proposal.note = note;
    if (m_aiEditBar)
        m_aiEditBar->showMessage(tr("Review the changes in the tab next to this one; nothing is written until then."));
    emit aiEditProposed(proposal);
}

void DocumentView::aiRuleRan()
{
    if (m_ruleWatcher.isCanceled() || m_ruleWatcher.future().resultCount() == 0)
        return;
    const RuleRun run = m_ruleWatcher.result();
    if (m_aiEditBar)
        m_aiEditBar->setBusy(false);
    if (!run.bytes) {
        aiEditFailed(run.error.isEmpty() ? tr("The AI's rule could not run.")
                                         : tr("The AI's rule could not run: %1").arg(run.error));
        return;
    }
    if (*run.bytes == m_aiEdit.before) {
        if (m_aiEditBar)
            m_aiEditBar->showMessage(tr("The AI's rule changes nothing here."));
        return;
    }
    AiEditProposal proposal = m_aiEdit;
    proposal.after = *run.bytes;
    proposal.note = run.note;
    proposal.rule = run.rule;
    if (m_aiEditBar)
        m_aiEditBar->showMessage(tr("Review the changes in the tab next to this one; nothing is written until then."));
    emit aiEditProposed(proposal);
}

void DocumentView::aiEditFailed(const QString& message)
{
    if (!m_aiEditBar)
        return;
    m_aiEditBar->setBusy(false);
    m_aiEditBar->showMessage(message, true);
}

bool DocumentView::replaceIfUnchanged(std::uint64_t start, std::uint64_t end, const QByteArray& before,
                                      const QByteArray& bytes)
{
    const auto current = m_textDocument->bytes();
    if (!isEditable() || start > end || end > current.size() ||
        QByteArrayView(reinterpret_cast<const char*>(current.data()) + start, static_cast<qsizetype>(end - start)) !=
            QByteArrayView(before))
        return false;
    m_textDocument->breakUndoMerge(); // one step of its own
    m_textDocument->replaceBytes(start, end,
                                 {reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                  static_cast<std::size_t>(bytes.size())});
    m_textDocument->breakUndoMerge();
    m_textView->setCursorOffset(start);
    return true;
}

void DocumentView::selectMatch(std::uint64_t start, std::uint64_t end)
{
    m_lastMatch = std::pair(start, end);
    if (isTextActive())
        m_textView->select(start, end);
    else
        m_hexView->select(start, end - start);
}

void DocumentView::closeFindBar()
{
    m_findBar->cancel();
    m_findBar->hide();
    (isTextActive() ? static_cast<QWidget*>(m_textView) : m_hexView)->setFocus();
}

bool DocumentView::isFilterPanelVisible() const
{
    return m_filterPanel && m_filterPanel->isVisible();
}

void DocumentView::showFilterPanel()
{
    if (!m_filterPanel) {
        m_filterPanel = new FilterPanel(m_textDocument);
        m_filterPanel->setResultsFont(m_textView->font());
        connect(m_filterPanel, &FilterPanel::lineSelected, this,
                [this](std::uint64_t line) { goToFilteredLine(line, false); });
        connect(m_filterPanel, &FilterPanel::lineActivated, this,
                [this](std::uint64_t line) { goToFilteredLine(line, true); });
        connect(m_filterPanel, &FilterPanel::closeRequested, this, &DocumentView::closeFilterPanel);
        connect(m_filterPanel, &FilterPanel::aiSettingsRequested, this, &DocumentView::aiSettingsRequested);
        connect(m_filterPanel, &FilterPanel::statusMessage, this, &DocumentView::statusMessage);
        m_documentSplitter->addWidget(m_filterPanel);
        // Half each: the conditions take some of the panel, and the results need rows.
        const int height = std::max(m_documentSplitter->height(), 400);
        m_documentSplitter->setSizes({height / 2, height - height / 2});
    }
    m_filterPanel->show();
    m_filterPanel->focusEditor();
}

void DocumentView::closeFilterPanel()
{
    if (!m_filterPanel)
        return;
    const bool hadFocus = m_filterPanel->isAncestorOf(QApplication::focusWidget());
    m_filterPanel->hide();
    if (hadFocus)
        (isTextActive() ? static_cast<QWidget*>(m_textView) : m_hexView)->setFocus();
}

void DocumentView::goToFilteredLine(std::uint64_t line, bool focus)
{
    m_textView->goToLine(line); // the hex view follows the caret
    if (focus)
        (isTextActive() ? static_cast<QWidget*>(m_textView) : m_hexView)->setFocus();
}

void DocumentView::fileChanged()
{
    if (isModified()) {
        // Reloading would throw the edits away; saving writes over the change instead.
        emit statusMessage(tr("%1 changed on disk; your unsaved edits are kept")
                               .arg(QFileInfo(filePath()).fileName()));
        return;
    }
    // A file that shrank (truncated, rotated) must be remapped at once: the old mapping now
    // reaches past its end. Growth can wait for the writer to finish its burst.
    if (QFileInfo(filePath()).size() < static_cast<qint64>(m_source->size()))
        reload();
    else
        m_reloadTimer.start();
}

DocumentView::DiskState DocumentView::diskState(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? DiskState{path, info.size(), info.lastModified()} : DiskState{path, -1, {}};
}

DocumentView::DiskState DocumentView::diskState() const
{
    return isUntitled() ? DiskState{} : diskState(filePath());
}

bool DocumentView::reload()
{
    if (isUntitled())
        return false; // nothing on disk yet
    m_reloadTimer.stop();
    const QString path = filePath();
    // Watching stops when a file is replaced (rotation, atomic save); watch the new one.
    if (!m_watcher.files().contains(path) && QFileInfo::exists(path))
        m_watcher.addPath(path);

    // Taken before opening, so that a change while it opens is noticed on the next look.
    const DiskState seen = diskState();
    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec);
    if (!source) {
        emit statusMessage(QFileInfo::exists(path) ? tr("Cannot reload: %1").arg(QString::fromLocal8Bit(ec.message()))
                                                   : tr("The file was deleted or moved; showing the last version"));
        return false;
    }

    m_diskState = seen;
    m_findBar->cancel();
    m_lastMatch.reset();
    m_source = std::move(source);
    m_fileType = core::detectFileType(m_source->bytes());
    {
        const QScopedValueRollback guard(m_syncing, true); // both views are updated directly
        m_textDocument->setSource(m_source);
        m_hexView->replaceSource(m_source);
    }
    updateEditable();
    if (m_following)
        moveToEnd();
    emit sourceChanged();
    emit stateChanged();
    return true;
}

void DocumentView::setFollowing(bool following)
{
    if (following == m_following)
        return;
    m_following = following;
    if (m_following)
        moveToEnd();
    emit stateChanged();
}

void DocumentView::moveToEnd()
{
    const std::uint64_t size = m_source->size();
    if (isTextActive())
        m_textView->setCursorOffset(size);
    else
        m_hexView->goTo(size == 0 ? 0 : size - 1);
}

AiContext DocumentView::aiContext()
{
    AiContext context;
    context.fileName = title();
    context.fileDescription = describeForAi(m_fileType, m_textDocument->codec(), m_source->size());
    const auto bytes = m_source->bytes();
    const QLocale locale;

    if (isTextActive() && m_textView->hasSelection()) {
        const std::uint64_t start = m_textView->selectionStart();
        const std::uint64_t end = m_textView->selectionEnd();
        std::uint64_t limit = end;
        if (end - start > kMaxContextTextBytes) {
            limit = m_textDocument->snapToCharStart(start + kMaxContextTextBytes);
            context.truncated = true;
        }
        context.selectionContent = m_textDocument->decoder().decodePlain(bytes.subspan(start, limit - start));
        const core::LineIndex& index = m_textDocument->index();
        const std::uint64_t firstLine = index.row(index.rowIndexAt(start)).line + 1;
        const std::uint64_t lastLine = index.row(index.rowIndexAt(end - 1)).line + 1;
        const QString size = locale.toString(static_cast<qulonglong>(end - start));
        context.selectionLabel =
            firstLine == lastLine
                ? tr("line %1 (%2 bytes)").arg(locale.toString(static_cast<qulonglong>(firstLine)), size)
                : tr("lines %1–%2 (%3 bytes)")
                      .arg(locale.toString(static_cast<qulonglong>(firstLine)),
                           locale.toString(static_cast<qulonglong>(lastLine)), size);
        context.selectionFence = m_textView->syntax().toLower();
    } else if (!isTextActive() && m_hexView->selectionLength() > 1) {
        const std::uint64_t start = m_hexView->selectionStart();
        const std::uint64_t length = m_hexView->selectionLength();
        const std::uint64_t shown = std::min(length, kMaxContextHexBytes);
        context.truncated = shown < length;
        context.selectionContent =
            QString::fromLatin1(core::hexDump(bytes.subspan(start, static_cast<std::size_t>(shown)), start));
        context.selectionLabel = tr("bytes 0x%1–0x%2 (%3 bytes)")
                                     .arg(QString::number(start, 16).toUpper(),
                                          QString::number(start + length - 1, 16).toUpper(),
                                          locale.toString(static_cast<qulonglong>(length)));
        context.selectionFence = QStringLiteral("hexdump");
        context.selectionIsBinary = true;
    }

    ToolTarget target;
    target.fileName = context.fileName;
    target.source = m_source;
    target.index = m_textDocument->sharedIndex();
    target.codec = m_textDocument->codec();
    target.type = m_fileType;
    // The tools may finish after this view is gone (tab closed): only mark a view that is still there.
    target.markFields = [view = QPointer<DocumentView>(this)](const QList<FieldMark>& marks) {
        if (view)
            view->showFieldMarks(marks);
    };
    context.toolTarget = target;

    // Built only when asked for: it reads the start of the file and samples the rest.
    context.fileSummary = [source = m_source, name = context.fileName, description = context.fileDescription] {
        return summaryForAi(core::summarizeFile(source->bytes()), name, description);
    };
    return context;
}

void DocumentView::showFieldMarks(const QList<FieldMark>& marks)
{
    m_hexView->setFieldMarks(marks);
    if (marks.isEmpty())
        return;
    if (m_viewMode == ViewMode::Text)
        setViewMode(ViewMode::Split);
    m_hexView->goTo(m_hexView->fieldMarks().first().offset);
}

QString DocumentView::syntax() const
{
    return m_textView->syntax();
}

void DocumentView::setSyntax(const QString& name)
{
    m_textView->setSyntax(name);
    emit stateChanged();
}

bool DocumentView::isEditable() const
{
    return !m_textView->isReadOnly();
}

bool DocumentView::isModified() const
{
    return m_textDocument->isModified();
}

void DocumentView::restoreContent(std::span<const std::uint8_t> bytes)
{
    m_textDocument->replaceBytes(0, m_textDocument->bytes().size(), bytes);
}

bool DocumentView::applyLineOperation(LineOperation operation)
{
    if (!isEditable() || !isTextActive())
        return false;
    m_textDocument->indexNow();
    const core::LineIndex& index = m_textDocument->index();
    if (index.rowCount() == 0)
        return false;
    const std::uint64_t start = m_textView->selectionStart();
    const std::uint64_t end = m_textView->selectionEnd();
    // Without a selection, sorting and cleaning take every line; the others the caret's.
    const bool everyLine = start == end && (operation == LineOperation::SortAscending ||
                                            operation == LineOperation::SortDescending ||
                                            operation == LineOperation::RemoveDuplicates ||
                                            operation == LineOperation::RemoveEmpty ||
                                            operation == LineOperation::TrimTrailingWhitespace);
    const auto [first, last] = everyLine ? std::pair<std::uint64_t, std::uint64_t>(0, index.lineCount() - 1)
                                         : selectedLines();

    const std::optional<LineEdit> edit = lineOperation(operation, *m_textDocument, first, last, start, end);
    const auto lines = edit ? static_cast<int>(edit->lines) : 0;
    switch (operation) {
    case LineOperation::SortAscending:
    case LineOperation::SortDescending:
        emit statusMessage(edit ? tr("Sorted %n line(s)", "", lines) : tr("The lines are in order already"));
        break;
    case LineOperation::RemoveDuplicates:
        emit statusMessage(edit ? tr("Removed %n duplicate line(s)", "", lines) : tr("No line appears twice"));
        break;
    case LineOperation::RemoveEmpty:
        emit statusMessage(edit ? tr("Removed %n empty line(s)", "", lines) : tr("No empty lines"));
        break;
    case LineOperation::TrimTrailingWhitespace:
        emit statusMessage(edit ? tr("Trimmed %n line(s)", "", lines) : tr("No line ends in spaces or tabs"));
        break;
    default:
        break;
    }
    if (!edit)
        return false;
    applyLineEdit(*edit);
    return true;
}

std::pair<std::uint64_t, std::uint64_t> DocumentView::selectedLines() const
{
    const core::LineIndex& index = m_textDocument->index();
    const std::uint64_t start = m_textView->selectionStart();
    const std::uint64_t end = m_textView->selectionEnd();
    const std::uint64_t first = index.row(index.rowIndexAt(start)).line;
    const core::Row endRow = index.row(index.rowIndexAt(end));
    std::uint64_t last = endRow.line;
    // A selection that ends where a line starts does not take that line in.
    if (end > start && last > first && endRow.start == end && !endRow.continuation)
        --last;
    return {first, last};
}

void DocumentView::applyLineEdit(const LineEdit& edit)
{
    m_textDocument->breakUndoMerge(); // one step of its own
    m_textDocument->replaceBytes(edit.start, edit.end,
                                 {reinterpret_cast<const std::uint8_t*>(edit.bytes.constData()),
                                  static_cast<std::size_t>(edit.bytes.size())});
    m_textDocument->breakUndoMerge();
    if (edit.selectionStart == edit.selectionEnd)
        m_textView->setCursorOffset(edit.selectionStart);
    else
        m_textView->select(edit.selectionStart, edit.selectionEnd);
}

bool DocumentView::toggleComment()
{
    if (!isEditable() || !isTextActive())
        return false;
    const CommentMarkers markers = commentMarkers(syntax(), isUntitled() ? QString() : filePath());
    if (markers.isEmpty()) {
        emit statusMessage(syntax().isEmpty() ? tr("Plain text has no comments; choose a language first")
                                              : tr("%1 has no comments").arg(translatedSyntaxName(syntax())));
        return false;
    }
    m_textDocument->indexNow();
    if (m_textDocument->index().rowCount() == 0)
        return false;
    const auto [first, last] = selectedLines();
    const std::optional<LineEdit> edit = ui::toggleComment(*m_textDocument, markers, first, last,
                                                           m_textView->selectionStart(), m_textView->selectionEnd());
    if (!edit)
        return false;
    applyLineEdit(*edit);
    return true;
}

bool DocumentView::convertCase(bool upper)
{
    if (!isEditable() || !isTextActive())
        return false;
    // The selection, or the word at the caret.
    if (!m_textView->hasSelection() && !m_textView->selectWordAtCursor())
        return false;
    const std::uint64_t start = m_textView->selectionStart();
    const QString text = m_textView->selectedText();
    const QString converted = upper ? text.toUpper() : text.toLower();
    if (text.isEmpty() || converted == text)
        return false;
    if (!m_textView->replaceSelectedText(converted))
        return false;
    m_textView->select(start, m_textView->cursorOffset());
    return true;
}

void DocumentView::updateEditable()
{
    // Decided when the file is opened or reloaded; edits may then make it larger.
    QString reason;
    if (m_fileType.kind == core::ContentKind::Binary)
        reason = tr("Binary files can only be viewed here");
    else if (m_source->size() > core::EditBuffer::kMaxEditableSize)
        reason = tr("Files larger than %1 can only be viewed here")
                     .arg(QLocale().formattedDataSize(static_cast<qint64>(core::EditBuffer::kMaxEditableSize)));
    m_textView->setReadOnly(!reason.isEmpty(), reason);
    m_findBar->setReplaceAvailable(reason.isEmpty(), reason);
}

void DocumentView::documentEdited()
{
    // Matches found before no longer point at the same text.
    m_findBar->cancel();
    m_lastMatch.reset();
    m_source = m_textDocument->source();
    {
        const QScopedValueRollback guard(m_syncing, true); // the text view places the caret
        m_hexView->replaceSource(m_source);
    }
    emit sourceChanged();
    emit stateChanged();
}

bool DocumentView::writeTo(const QString& path, QString* error)
{
    return writeBytes(path, m_source->bytes(), error);
}

bool DocumentView::writeBytes(const QString& path, std::span<const std::uint8_t> bytes, QString* error)
{
    // QSaveFile writes a temporary file and renames it over the target, so a failure halfway
    // leaves the old file as it was. It keeps the file's permissions and follows symlinks; where
    // the folder is not writable but the file is, it writes the file in place.
    QSaveFile file(path);
    file.setDirectWriteFallback(true);
    const bool watched = m_watcher.files().contains(path);
    if (watched)
        m_watcher.removePath(path); // our own write is no change on disk to react to
    const bool ok = file.open(QIODevice::WriteOnly) &&
                    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) ==
                        static_cast<qint64>(bytes.size()) &&
                    file.commit();
    // The file written is a new one; watch it, also if the old one had gone (deleted, moved).
    if ((watched || path == filePath()) && QFileInfo::exists(path))
        m_watcher.addPath(path);
    if (path == filePath())
        m_diskState = diskState();
    if (!ok && error)
        *error = file.errorString();
    return ok;
}

bool DocumentView::save(QString* error)
{
    if (isUntitled()) {
        if (error)
            *error = tr("It has no file yet; use Save As");
        return false;
    }
    if (!writeTo(filePath(), error))
        return false;
    m_textDocument->setSaved();
    return true;
}

bool DocumentView::saveAs(const QString& path, QString* error)
{
    if (!isUntitled() && QFileInfo(path).absoluteFilePath() == QFileInfo(filePath()).absoluteFilePath())
        return save(error);
    if (!writeTo(path, error))
        return false;
    // Show the new file from disk, as if it had been opened (the undo history goes with the
    // old one).
    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec);
    if (!source) {
        if (error)
            *error = QString::fromLocal8Bit(ec.message());
        return false;
    }
    m_watcher.removePaths(m_watcher.files());
    m_watcher.addPath(path);
    m_findBar->cancel();
    m_lastMatch.reset();
    m_source = std::move(source);
    m_diskState = diskState(); // of the file it shows now
    m_pollTimer.start();
    {
        const QScopedValueRollback guard(m_syncing, true);
        m_textDocument->setSource(m_source);
        m_hexView->replaceSource(m_source);
    }
    if (m_fileType.kind != core::ContentKind::Binary)
        m_textView->setSyntax(syntaxForFileName(QFileInfo(path).fileName()));
    updateEditable();
    emit pathChanged();
    emit sourceChanged();
    emit stateChanged();
    return true;
}

bool DocumentView::followMove(const QString& path)
{
    if (isUntitled() || isModified())
        return false;
    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec);
    if (!source)
        return false;
    State kept = state();
    kept.path = path;
    m_reloadTimer.stop();
    m_watcher.removePaths(m_watcher.files());
    m_watcher.addPath(path);
    m_findBar->cancel();
    m_lastMatch.reset();
    m_source = std::move(source);
    m_diskState = diskState(); // of the file it shows now
    m_pollTimer.start();
    m_fileType = core::detectFileType(m_source->bytes());
    {
        const QScopedValueRollback guard(m_syncing, true);
        m_textDocument->setSource(m_source);
        m_hexView->replaceSource(m_source);
    }
    if (m_fileType.kind != core::ContentKind::Binary)
        m_textView->setSyntax(syntaxForFileName(QFileInfo(path).fileName()));
    updateEditable();
    restoreState(kept);
    emit pathChanged();
    emit sourceChanged();
    emit stateChanged();
    return true;
}

bool DocumentView::convertLineEndings(const QString& terminator)
{
    if (!isEditable())
        return false;
    // The caret keeps its line; offsets after the first change move.
    const std::uint64_t line = m_textView->cursorLine();
    if (!m_textDocument->convertLineEndings(terminator))
        return false;
    if (const auto row = m_textDocument->index().rowIndexOfLine(line))
        m_textView->setCursorOffset(m_textDocument->index().row(*row).start);
    return true;
}

DocumentView::Conversion DocumentView::saveWithCodec(const core::TextCodec& codec, bool replaceUnreadable,
                                                     QString* error)
{
    // The text as it reads now (unsaved edits included), without its old byte order mark.
    const auto bytes = m_source->bytes().subspan(static_cast<std::size_t>(m_textDocument->textStart()));
    bool unreadable = false;
    const QString text = Codec(m_textDocument->codec())
                             .decode(QByteArrayView(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<qsizetype>(bytes.size())),
                                     &unreadable);
    if (unreadable && !replaceUnreadable)
        return Conversion::Unreadable;

    // UTF-16 and UTF-32 need their byte order mark to be recognised; UTF-8 and the rest do not.
    const bool wide = codec.scheme == core::CharScheme::Utf16LE || codec.scheme == core::CharScheme::Utf16BE ||
                      codec.scheme == core::CharScheme::Utf32LE || codec.scheme == core::CharScheme::Utf32BE;
    const std::optional<QByteArray> converted =
        Codec(codec, wide ? QStringConverter::Flag::WriteBom : QStringConverter::Flag::Default).encode(text);
    if (!converted)
        return Conversion::Unwritable;
    if (!writeBytes(filePath(), {reinterpret_cast<const std::uint8_t*>(converted->constData()),
                                 static_cast<std::size_t>(converted->size())},
                    error))
        return Conversion::Failed;

    m_codecChosen = true;
    // Show the file as written, in its new codec; the undo history goes with the old bytes.
    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(filePath()), ec);
    if (!source) {
        if (error)
            *error = QString::fromLocal8Bit(ec.message());
        return Conversion::Failed;
    }
    m_findBar->cancel();
    m_lastMatch.reset();
    m_source = std::move(source);
    {
        const QScopedValueRollback guard(m_syncing, true);
        m_textDocument->setCodec(codec);
        m_textDocument->setSource(m_source);
        m_hexView->replaceSource(m_source);
    }
    emit sourceChanged();
    emit stateChanged();
    return Conversion::Saved;
}

void DocumentView::cut()
{
    if (isTextActive())
        m_textView->cut();
}

void DocumentView::paste()
{
    if (isTextActive())
        m_textView->paste();
}

void DocumentView::undo()
{
    if (isTextActive())
        m_textView->undo();
}

void DocumentView::redo()
{
    if (isTextActive())
        m_textView->redo();
}

void DocumentView::copy()
{
    if (isTextActive())
        m_textView->copy();
    else
        m_hexView->copy();
}

void DocumentView::selectAll()
{
    if (isTextActive())
        m_textView->selectAll();
    else
        m_hexView->selectAll();
}

void DocumentView::applySplitSizes()
{
    if (!m_splitSizesPending || !isVisible())
        return;
    // The window is still being laid out during show and resize events, so wait until it settles.
    QTimer::singleShot(0, this, [this] {
        const int total = m_splitter->width();
        if (!m_splitSizesPending || total <= 0)
            return;
        m_splitSizesPending = false;
        // The hex view gets the width of its columns (at most half), the text view the rest.
        const int hexWidth = std::min(m_hexView->sizeHint().width(), total / 2);
        m_splitter->setSizes({total - hexWidth, hexWidth});
    });
}

void DocumentView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    applySplitSizes();
}

void DocumentView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    applySplitSizes();
}

bool DocumentView::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FontChange && watched == m_textView && m_filterPanel)
        m_filterPanel->setResultsFont(m_textView->font()); // zoomed: the results follow
    if (event->type() == QEvent::FocusIn && m_viewMode == ViewMode::Split) {
        const bool text = watched == m_textView;
        if (text != m_textFocusedLast) {
            m_textFocusedLast = text;
            setFocusProxy(text ? static_cast<QWidget*>(m_textView) : m_hexView);
            emit stateChanged();
        }
    }
    return QWidget::eventFilter(watched, event);
}

// The text view selects [start, end) between characters; the hex view selects whole bytes
// [start, start + length) and always has its cursor byte selected.
void DocumentView::syncFromText()
{
    if (!m_syncing) {
        const QScopedValueRollback guard(m_syncing, true);
        if (m_textView->hasSelection())
            m_hexView->select(m_textView->selectionStart(), m_textView->selectionEnd() - m_textView->selectionStart());
        else
            m_hexView->setCursorOffset(m_textView->cursorOffset());
    }
    emit stateChanged();
}

void DocumentView::syncFromHex()
{
    if (!m_syncing) {
        const QScopedValueRollback guard(m_syncing, true);
        const std::uint64_t length = m_hexView->selectionLength();
        if (length > 1)
            m_textView->select(m_hexView->selectionStart(), m_hexView->selectionStart() + length);
        else
            m_textView->setCursorOffset(m_hexView->cursorOffset());
    }
    emit stateChanged();
}

} // namespace aiedit::ui
