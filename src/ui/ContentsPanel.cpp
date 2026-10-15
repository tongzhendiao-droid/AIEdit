#include "ui/ContentsPanel.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/LineIndex.h"
#include "ui/DocumentView.h"
#include "ui/FilterRuleWriter.h"
#include "ui/TextDecoder.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace aiedit::ui {

namespace {

constexpr int kFollowDelayMs = 120;           // after scrolling, before the chapter is marked
constexpr int kRescanDelayMs = 600;           // after an edit
constexpr qsizetype kMaxChapterChars = 20000; // of a chapter sent to the AI
constexpr int kFirstLines = 40;               // samples for the AI: the start of the book …
constexpr int kShortLines = 120;              // … and short lines from all through it
constexpr qsizetype kMaxShortLine = 40;
constexpr std::uint64_t kMaxSampledBytes = 64ull * 1024 * 1024;

// What the model is told; in English, which models follow best.
const char* const kHeadingInstructions = R"(You find the headings of books for AIEdit, a text viewer. You are shown lines of one book, each after its line number ("1234: " is not part of the line): the first lines of the book, then short lines from all through it. Write a rule whose include conditions match the lines that are headings (titles of volumes, parts, chapters, sections) and no other lines, and answer with a single JSON object and nothing else:

{"description": "<what the headings look like, in a few words, in the language of the book>",
 "match": "all" or "any",
 "conditions": [{"type": "include" or "exclude", "regex": "<PCRE pattern>"}]}

Each regex is tested against one whole line, without its line break; anchor it with ^ and $. A line is a heading if it matches the include conditions (all of them with "match": "all", at least one with "any") and none of the exclude conditions. Base the patterns on the lines you see: how the headings are numbered (Chinese numerals, Arabic numbers), worded and how long they are. Prefer missing a doubtful line to taking in ordinary text. Double the backslashes inside JSON strings.)";

QString rulesKey()
{
    return QStringLiteral("contents/rules");
}

} // namespace

ContentsPanel::ContentsPanel(QWidget* parent) : QWidget(parent)
{
    m_writer = new FilterRuleWriter(this);
    m_writer->setInstructions(QString::fromUtf8(kHeadingInstructions),
                              QStringLiteral("Lines of the book, each after its line number:"));
    connect(m_writer, &FilterRuleWriter::ruleWritten, this, &ContentsPanel::ruleWritten);
    connect(m_writer, &FilterRuleWriter::failed, this, [this](const QString& message) {
        m_aiNote = message;
        updateButtons();
    });
    connect(m_writer, &FilterRuleWriter::settingsRequested, this, &ContentsPanel::aiSettingsRequested);

    m_ai = new QPushButton(this);
    m_ai->setToolTip(tr("Let the AI work out what this book's headings look like. It sees the first lines and "
                        "short lines from all through the book; the rule it writes runs here."));
    connect(m_ai, &QPushButton::clicked, this, &ContentsPanel::findWithAi);
    m_builtIn = new QToolButton(this);
    m_builtIn->setText(tr("Built-in Rule"));
    m_builtIn->setToolTip(tr("Find the headings with AIEdit's own rule again"));
    m_builtIn->setAutoRaise(true);
    connect(m_builtIn, &QToolButton::clicked, this, &ContentsPanel::useBuiltInRule);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // One click goes there, as in the contents of a book.
    connect(m_tree, &QTreeWidget::itemClicked, this, &ContentsPanel::activate);
    connect(m_tree, &QTreeWidget::itemActivated, this, &ContentsPanel::activate);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ContentsPanel::showMenu);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_ai);
    buttons->addWidget(m_builtIn);
    buttons->addStretch();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    layout->addLayout(buttons);
    layout->addWidget(m_status);
    layout->addWidget(m_tree, 1);

    m_followTimer.setSingleShot(true);
    m_followTimer.setInterval(kFollowDelayMs);
    connect(&m_followTimer, &QTimer::timeout, this, &ContentsPanel::followReading);
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(kRescanDelayMs);
    connect(&m_rescanTimer, &QTimer::timeout, this, [this] {
        if (isVisible())
            scan();
    });
    connect(&m_watcher, &QFutureWatcher<std::vector<Heading>>::finished, this, &ContentsPanel::scanned);
    updateButtons();
}

ContentsPanel::~ContentsPanel()
{
    m_writer->cancel();
    m_watcher.cancel();
}

void ContentsPanel::setDocument(DocumentView* document)
{
    if (document == m_document)
        return;
    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
        disconnect(m_document->textDocument(), nullptr, this, nullptr);
        disconnect(m_document->textView()->verticalScrollBar(), nullptr, &m_followTimer, nullptr);
        disconnect(m_document, nullptr, &m_followTimer, nullptr);
    }
    m_watcher.cancel();
    m_scanning = false;
    m_writer->cancel();
    m_rescanTimer.stop();
    m_aiNote.clear();
    m_aiRule.reset();
    m_pending.reset();
    m_document = document;
    m_headings.clear();
    m_stale = true;
    rebuildTree();
    if (document) {
        // Edited, reloaded or read in another codec: the headings are found again, a moment later.
        const auto changed = [this] {
            m_stale = true;
            m_rescanTimer.start();
        };
        TextDocument* text = document->textDocument();
        connect(text, &TextDocument::sourceChanged, this, changed);
        connect(text, &TextDocument::edited, this, changed);
        connect(text, &TextDocument::codecChanged, this, changed);
        connect(document->textView()->verticalScrollBar(), &QScrollBar::valueChanged, &m_followTimer,
                qOverload<>(&QTimer::start));
        connect(document, &DocumentView::stateChanged, &m_followTimer, qOverload<>(&QTimer::start));
    }
    if (isVisible())
        scan();
    updateButtons();
}

void ContentsPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_stale)
        scan();
}

QString ContentsPanel::fileKey() const
{
    return m_document && !m_document->isUntitled() ? m_document->filePath() : QString();
}

FilterRule ContentsPanel::rule() const
{
    if (m_aiRule)
        return *m_aiRule;
    const QString key = fileKey();
    if (!key.isEmpty()) {
        const QVariantMap rules = QSettings().value(rulesKey()).toMap();
        const QJsonObject json = QJsonDocument::fromJson(rules.value(key).toByteArray()).object();
        if (std::optional<FilterRule> custom = FilterRule::fromJson(json))
            return *custom;
    }
    return defaultHeadingRule();
}

void ContentsPanel::scan()
{
    m_stale = false;
    m_watcher.cancel();
    m_scanning = false;
    if (!m_document || m_document->fileType().kind == core::ContentKind::Binary) {
        m_headings.clear();
        rebuildTree();
        updateButtons();
        return;
    }
    const TextDocument* text = m_document->textDocument();
    const SearchContext context{text->source(), text->sharedIndex(), text->codec()};
    m_scanning = true;
    m_watcher.setFuture(QtConcurrent::run([rule = rule(), context](QPromise<std::vector<Heading>>& promise) {
        promise.addResult(findHeadings(rule, context, [&promise] { return promise.isCanceled(); }));
    }));
    updateButtons();
}

void ContentsPanel::scanned()
{
    if (!m_scanning || m_watcher.isCanceled() || m_watcher.future().resultCount() == 0)
        return; // stopped for another document or another rule
    m_scanning = false;
    m_headings = m_watcher.result();
    rebuildTree();
    followReading();
    updateButtons();
    if (const std::optional<ChapterAction> pending = std::exchange(m_pending, std::nullopt))
        askAboutCurrentChapter(*pending);
}

void ContentsPanel::rebuildTree()
{
    m_tree->clear();
    QTreeWidgetItem* volume = nullptr;
    for (std::size_t i = 0; i < m_headings.size(); ++i) {
        const Heading& heading = m_headings[i];
        // Chapters go under the volume before them, if there is one.
        QTreeWidgetItem* item = heading.level == 2 && volume ? new QTreeWidgetItem(volume)
                                                             : new QTreeWidgetItem(m_tree);
        if (heading.level == 1)
            volume = item;
        item->setText(0, heading.title);
        item->setToolTip(0, tr("Line %L1").arg(heading.line + 1));
        item->setData(0, Qt::UserRole, static_cast<int>(i));
    }
    m_tree->expandAll();
}

void ContentsPanel::followReading()
{
    if (!m_document || m_headings.empty())
        return;
    const int index = headingAt(m_headings, m_document->textView()->firstVisibleOffset());
    if (index < 0) {
        m_tree->setCurrentItem(nullptr);
        return;
    }
    // The item for the heading; the tree is built in heading order.
    QTreeWidgetItemIterator it(m_tree);
    for (; *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toInt() == index) {
            m_tree->setCurrentItem(*it); // marks it; going there is for clicks
            m_tree->scrollToItem(*it);
            break;
        }
    }
}

void ContentsPanel::activate(QTreeWidgetItem* item)
{
    if (!item || !m_document)
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index >= 0 && index < static_cast<int>(m_headings.size()))
        m_document->showFromOffset(m_headings[static_cast<std::size_t>(index)].start);
}

int ContentsPanel::chapterAt(std::uint64_t offset) const
{
    return headingAt(m_headings, offset);
}

QString ContentsPanel::chapterText(int index, bool* cut) const
{
    if (!m_document || index < 0 || index >= static_cast<int>(m_headings.size()))
        return {};
    const TextDocument* text = m_document->textDocument();
    const auto content = text->bytes();
    const std::uint64_t start = m_headings[static_cast<std::size_t>(index)].start;
    const std::uint64_t end = std::min<std::uint64_t>(
        headingEnd(m_headings, static_cast<std::size_t>(index), content.size()), content.size());
    // Four bytes are the most a character takes: enough bytes for the characters that go.
    const std::uint64_t length =
        std::min<std::uint64_t>(end - start, 4 * static_cast<std::uint64_t>(kMaxChapterChars));
    QString chapter = text->decoder().decodePlain(content.subspan(start, static_cast<std::size_t>(length)));
    const bool shortened = chapter.size() > kMaxChapterChars || length < end - start;
    if (chapter.size() > kMaxChapterChars)
        chapter.truncate(kMaxChapterChars);
    if (cut)
        *cut = shortened;
    return chapter;
}

void ContentsPanel::askAboutCurrentChapter(ChapterAction action)
{
    if (!m_document)
        return;
    if (m_stale || m_scanning) {
        m_pending = action;
        if (!m_scanning)
            scan();
        return;
    }
    const int index = chapterAt(m_document->textView()->firstVisibleOffset());
    if (index < 0) {
        m_status->setText(tr("There is no chapter here to send"));
        return;
    }
    bool cut = false;
    const QString text = chapterText(index, &cut);
    emit chapterAction(action, m_headings[static_cast<std::size_t>(index)].title, text, cut);
}

void ContentsPanel::showMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item)
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    QMenu menu(this);
    menu.addAction(tr("&Go There"), this, [this, item] { activate(item); });
    menu.addSeparator();
    const auto send = [this, index](ChapterAction action) {
        bool cut = false;
        const QString text = chapterText(index, &cut);
        emit chapterAction(action, m_headings[static_cast<std::size_t>(index)].title, text, cut);
    };
    menu.addAction(tr("&Summarize This Chapter"), this, [send] { send(ChapterAction::Summarize); });
    menu.addAction(tr("&Translate This Chapter"), this, [send] { send(ChapterAction::Translate); });
    menu.addAction(tr("&Explain This Chapter"), this, [send] { send(ChapterAction::Explain); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void ContentsPanel::findWithAi()
{
    if (m_writer->isBusy()) {
        m_writer->cancel();
        m_aiNote = tr("Stopped.");
        updateButtons();
        return;
    }
    if (!m_document || m_document->fileType().kind == core::ContentKind::Binary)
        return;

    // The first lines show how the book begins; the short lines are where headings hide.
    const TextDocument* text = m_document->textDocument();
    const core::LineIndex& index = text->index();
    const auto content = text->bytes();
    const TextDecoder& decoder = text->decoder();
    QStringList first;
    QStringList shortLines;
    QList<std::uint64_t> shortLineNumbers;
    for (std::uint64_t row = 0; row < index.rowCount(); row += 4096) {
        for (const core::Row& r : index.rows(row, 4096)) {
            if (r.continuation || r.start > kMaxSampledBytes || r.contentEnd - r.start > 4 * kMaxShortLine)
                continue;
            const std::size_t size = static_cast<std::size_t>(r.contentEnd - r.start);
            const QString line = decoder.decodePlain(content.subspan(r.start, size)).trimmed();
            if (line.isEmpty())
                continue;
            if (first.size() < kFirstLines)
                first << QStringLiteral("%1: %2").arg(r.line + 1).arg(line);
            else if (line.size() <= kMaxShortLine)
                shortLines << QStringLiteral("%1: %2").arg(r.line + 1).arg(line);
        }
    }
    QStringList samples = first;
    const qsizetype step = std::max<qsizetype>(1, shortLines.size() / kShortLines);
    for (qsizetype i = 0; i < shortLines.size() && samples.size() < kFirstLines + kShortLines; i += step)
        samples << shortLines[i];

    m_aiNote = tr("Asking %1 to find the headings…").arg(m_writer->settings().chatUrl().host());
    m_writer->write(QStringLiteral("Find the lines that are headings in this book."), samples);
    updateButtons();
}

void ContentsPanel::ruleWritten(const FilterRule& rule)
{
    const QString key = fileKey();
    if (!key.isEmpty()) {
        QSettings settings;
        QVariantMap rules = settings.value(rulesKey()).toMap();
        rules.insert(key, QJsonDocument(rule.toJson()).toJson(QJsonDocument::Compact));
        settings.setValue(rulesKey(), rules);
    }
    m_aiRule = rule;
    m_aiNote = tr("The AI's rule: %1").arg(rule.description.isEmpty() ? tr("no description") : rule.description);
    scan();
}

void ContentsPanel::useBuiltInRule()
{
    QSettings settings;
    QVariantMap rules = settings.value(rulesKey()).toMap();
    rules.remove(fileKey());
    settings.setValue(rulesKey(), rules);
    m_aiRule.reset();
    m_aiNote.clear();
    scan();
}

QString ContentsPanel::statusText() const
{
    return m_status->text();
}

void ContentsPanel::showStatus(const QString& text)
{
    m_status->setText(text);
}

void ContentsPanel::updateButtons()
{
    const bool text = m_document && m_document->fileType().kind != core::ContentKind::Binary;
    m_ai->setText(m_writer->isBusy() ? tr("Stop") : tr("Find with AI"));
    m_ai->setEnabled(text || m_writer->isBusy());
    const bool custom =
        m_aiRule || (!fileKey().isEmpty() && QSettings().value(rulesKey()).toMap().contains(fileKey()));
    m_builtIn->setVisible(custom);

    QString status;
    if (!m_document)
        status = tr("Open a book to see its contents.");
    else if (!text)
        status = tr("A binary file has no contents to show.");
    else if (isScanning())
        status = tr("Looking for headings…");
    else if (m_headings.empty())
        status = tr("No headings found. The AI can work out what this book's headings look like.");
    else
        status = tr("%n heading(s)", "", static_cast<int>(m_headings.size()));
    if (!m_aiNote.isEmpty())
        status = m_aiNote + QLatin1Char('\n') + status;
    showStatus(status);
}

} // namespace aiedit::ui
