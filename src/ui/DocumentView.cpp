#include "ui/DocumentView.h"

#include "core/FileSource.h"
#include "ui/FindBar.h"
#include "ui/HexView.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QApplication>
#include <QEvent>
#include <QFileInfo>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace cppedit::ui {

namespace {

// Writers append in bursts; reload once things have been quiet this long.
constexpr int kReloadDelayMs = 250;

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
    // A search runs against the index of one codec; a new codec makes it meaningless.
    connect(m_textDocument, &TextDocument::codecChanged, m_findBar, &FindBar::cancel);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_splitter);
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

    if (fileType.kind != core::ContentKind::Binary)
        m_textView->setSyntax(syntaxForFileName(QFileInfo(filePath()).fileName()));

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(kReloadDelayMs);
    connect(&m_reloadTimer, &QTimer::timeout, this, &DocumentView::reload);
    m_watcher.addPath(filePath());
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &DocumentView::fileChanged);

    // Text files open as text, everything else as hex.
    m_viewMode = ViewMode::Split; // anything different from the target, so setViewMode() applies it
    setViewMode(fileType.kind == core::ContentKind::Binary ? ViewMode::Hex : ViewMode::Text);
}

DocumentView::~DocumentView()
{
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

void DocumentView::setCodec(const core::TextCodec& codec)
{
    m_textDocument->setCodec(codec);
    emit stateChanged();
}

void DocumentView::showFindBar()
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
    m_findBar->activate();
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
    m_findBar->start(request, {m_source, m_textDocument->sharedIndex(), m_textDocument->codec()});
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

void DocumentView::fileChanged()
{
    // A file that shrank (truncated, rotated) must be remapped at once: the old mapping now
    // reaches past its end. Growth can wait for the writer to finish its burst.
    if (QFileInfo(filePath()).size() < static_cast<qint64>(m_source->size()))
        reload();
    else
        m_reloadTimer.start();
}

bool DocumentView::reload()
{
    m_reloadTimer.stop();
    const QString path = filePath();
    // Watching stops when a file is replaced (rotation, atomic save); watch the new one.
    if (!m_watcher.files().contains(path) && QFileInfo::exists(path))
        m_watcher.addPath(path);

    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec);
    if (!source) {
        emit statusMessage(QFileInfo::exists(path) ? tr("Cannot reload: %1").arg(QString::fromLocal8Bit(ec.message()))
                                                   : tr("The file was deleted or moved; showing the last version"));
        return false;
    }

    m_findBar->cancel();
    m_lastMatch.reset();
    m_source = std::move(source);
    m_fileType = core::detectFileType(m_source->bytes());
    {
        const QScopedValueRollback guard(m_syncing, true); // both views are updated directly
        m_textDocument->setSource(m_source);
        m_hexView->replaceSource(m_source);
    }
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

QString DocumentView::syntax() const
{
    return m_textView->syntax();
}

void DocumentView::setSyntax(const QString& name)
{
    m_textView->setSyntax(name);
    emit stateChanged();
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

} // namespace cppedit::ui
