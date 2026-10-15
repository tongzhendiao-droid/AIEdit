#include "ui/FilterPanel.h"

#include "core/FileSource.h"
#include "core/LineIndex.h"
#include "ui/AiPanel.h"
#include "ui/AiSettings.h"
#include "ui/FilterResultsView.h"
#include "ui/FilterRuleWriter.h"
#include "ui/SearchJob.h"
#include "ui/TextDocument.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <mutex>
#include <utility>

namespace aiedit::ui {

namespace {

constexpr int kRunDelayMs = 300;               // after the last keystroke in the rule
constexpr std::size_t kMaxResults = 5'000'000; // 80 MB of results; more is no longer a filter
constexpr std::size_t kTailBytes = 4096;       // compared to tell a log that grew from one that changed

QString lastRuleKey()
{
    return QStringLiteral("filter/lastRule");
}

struct Comparison
{
    QString field;
    QString op;
    double value = 0;
};

// A number test as typed: "ms > 1000", "status >= 500", "2 == 0".
std::optional<Comparison> parseComparison(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*([A-Za-z_]\w*|\d+)\s*(>=|<=|==|!=|=|>|<)\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)\s*$)"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    const QString op = match.captured(2);
    return Comparison{match.captured(1), op == u"=" ? QStringLiteral("==") : op,
                      match.captured(3).toDouble()};
}

QString comparisonText(const FilterCondition& condition)
{
    if (condition.field.isEmpty())
        return {};
    return QStringLiteral("%1 %2 %3")
        .arg(condition.field, condition.op, QString::number(condition.value, 'g', 15));
}

bool sameConditions(const FilterRule& a, const FilterRule& b)
{
    return a.conditions == b.conditions && a.anyInclusion == b.anyInclusion;
}

QByteArray tailOf(std::span<const std::uint8_t> bytes, std::size_t end)
{
    const std::size_t length = std::min(end, kTailBytes);
    return {reinterpret_cast<const char*>(bytes.data() + end - length), static_cast<qsizetype>(length)};
}

SearchContext contextOf(const TextDocument& document)
{
    return {document.source(), document.sharedIndex(), document.codec()};
}

} // namespace

// One condition of the rule: include or exclude, a pattern, its case, and a number test.
class ConditionEditor : public QWidget
{
public:
    explicit ConditionEditor(QWidget* parent) : QWidget(parent)
    {
        type = new QComboBox(this);
        type->addItem(FilterPanel::tr("Include"));
        type->addItem(FilterPanel::tr("Exclude"));
        type->setToolTip(FilterPanel::tr("Include shows the lines that match; Exclude hides them"));
        pattern = new QLineEdit(this);
        pattern->setPlaceholderText(FilterPanel::tr("Regular expression, e.g. ERROR|FATAL"));
        pattern->setClearButtonEnabled(true);
        caseSensitive = new QToolButton(this);
        caseSensitive->setText(QStringLiteral("Aa"));
        caseSensitive->setCheckable(true);
        caseSensitive->setAutoRaise(true);
        caseSensitive->setToolTip(FilterPanel::tr("Match case"));
        comparison = new QLineEdit(this);
        comparison->setPlaceholderText(FilterPanel::tr("Number test, e.g. ms > 1000"));
        comparison->setToolTip(FilterPanel::tr("Compares a number the expression captures with a value. For took "
                                               "(?<ms>\\d+)ms, write ms > 1000; also >=, <, <=, == and !="));
        comparison->setClearButtonEnabled(true);
        const QFontMetrics metrics = comparison->fontMetrics();
        comparison->setMinimumWidth(metrics.horizontalAdvance(comparison->placeholderText()) +
                                    3 * metrics.height());
        remove = new QToolButton(this);
        remove->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        remove->setAutoRaise(true);
        remove->setToolTip(FilterPanel::tr("Remove this condition"));

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        layout->addWidget(type);
        layout->addWidget(pattern, 2);
        layout->addWidget(caseSensitive);
        layout->addWidget(comparison, 1);
        layout->addWidget(remove);
    }

    void setCondition(const FilterCondition& condition)
    {
        type->setCurrentIndex(condition.exclude ? 1 : 0);
        pattern->setText(condition.pattern);
        caseSensitive->setChecked(condition.caseSensitive);
        comparison->setText(comparisonText(condition));
    }

    // Nothing typed: not part of the rule.
    bool isEmpty() const { return pattern->text().isEmpty() && comparison->text().trimmed().isEmpty(); }

    // The condition as typed; empty, with the problem, if it cannot run.
    std::optional<FilterCondition> condition(QString* problem) const
    {
        FilterCondition c;
        c.exclude = type->currentIndex() == 1;
        c.pattern = pattern->text();
        c.caseSensitive = caseSensitive->isChecked();
        if (const QString test = comparison->text().trimmed(); !test.isEmpty()) {
            const std::optional<Comparison> parsed = parseComparison(test);
            if (!parsed) {
                *problem = FilterPanel::tr(
                    "write the number test as a group, a comparison and a value, e.g. ms > 1000");
                return std::nullopt;
            }
            c.field = parsed->field;
            c.op = parsed->op;
            c.value = parsed->value;
        }
        *problem = c.check();
        return problem->isEmpty() ? std::optional(c) : std::nullopt;
    }

    QComboBox* type;
    QLineEdit* pattern;
    QToolButton* caseSensitive;
    QLineEdit* comparison;
    QToolButton* remove;
};

// Matches found on the filter's thread, waiting for the panel to take them.
struct FilterPanel::Inbox
{
    std::mutex mutex;
    std::vector<FilterMatch> matches;
};

FilterPanel::FilterPanel(TextDocument* document, QWidget* parent) : QWidget(parent), m_document(document)
{
    m_writer = new FilterRuleWriter(this);
    connect(m_writer, &FilterRuleWriter::ruleWritten, this, &FilterPanel::ruleWritten);
    connect(m_writer, &FilterRuleWriter::failed, this, &FilterPanel::writerFailed);
    connect(m_writer, &FilterRuleWriter::settingsRequested, this, &FilterPanel::aiSettingsRequested);

    auto* aiLabel = new QLabel(tr("AI:"), this);
    m_description = new QLineEdit(this);
    m_description->setPlaceholderText(
        tr("Describe the lines to show, e.g. errors that took over a second, without the heartbeats"));
    m_description->setClearButtonEnabled(true);
    m_description->installEventFilter(this);
    m_write = new QPushButton(this);
    m_write->setToolTip(tr("Let the AI write the conditions below from your description (Enter)"));
    connect(m_write, &QPushButton::clicked, this, &FilterPanel::writeRule);
    m_preview = new QToolButton(this);
    m_preview->setText(tr("Preview"));
    m_preview->setToolTip(tr("Show exactly what will be sent"));
    m_preview->setAutoRaise(true);
    connect(m_preview, &QToolButton::clicked, this, &FilterPanel::showPreview);
    m_close = new QToolButton(this);
    m_close->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    m_close->setToolTip(tr("Close (Esc)"));
    m_close->setAutoRaise(true);
    connect(m_close, &QToolButton::clicked, this, &FilterPanel::closeRequested);

    m_notice = new QLabel(this);
    m_notice->setWordWrap(true);
    m_notice->setForegroundRole(QPalette::PlaceholderText);
    connect(m_notice, &QLabel::linkActivated, this, &FilterPanel::aiSettingsRequested);

    m_conditionsLayout = new QVBoxLayout;
    m_conditionsLayout->setSpacing(2);

    m_add = new QToolButton(this);
    m_add->setText(tr("Add Condition"));
    m_add->setAutoRaise(true);
    connect(m_add, &QToolButton::clicked, this, [this] {
        addCondition()->pattern->setFocus(Qt::OtherFocusReason);
    });
    m_any = new QCheckBox(tr("Any include is enough"), this);
    m_any->setToolTip(tr("Show the lines that match at least one Include condition, instead of all of them"));
    connect(m_any, &QCheckBox::clicked, this, &FilterPanel::ruleEdited);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_results = new FilterResultsView(this);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setDocument(document);
    connect(m_results, &FilterResultsView::currentLineChanged, this, &FilterPanel::lineSelected);
    connect(m_results, &FilterResultsView::activated, this, &FilterPanel::lineActivated);
    connect(m_results, &FilterResultsView::statusMessage, this, &FilterPanel::statusMessage);

    auto* aiRow = new QHBoxLayout;
    aiRow->addWidget(aiLabel);
    aiRow->addWidget(m_description, 1);
    aiRow->addWidget(m_write);
    aiRow->addWidget(m_preview);
    aiRow->addWidget(m_close);
    auto* controls = new QHBoxLayout;
    controls->addWidget(m_add);
    controls->addWidget(m_any);
    controls->addSpacing(12);
    controls->addWidget(m_status, 1);
    auto* editor = new QVBoxLayout;
    editor->setContentsMargins(6, 3, 3, 3);
    editor->setSpacing(3);
    editor->addLayout(aiRow);
    editor->addWidget(m_notice);
    editor->addLayout(m_conditionsLayout);
    editor->addLayout(controls);
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(editor);
    layout->addWidget(line);
    layout->addWidget(m_results, 1);

    m_runTimer.setSingleShot(true);
    m_runTimer.setInterval(kRunDelayMs);
    connect(&m_runTimer, &QTimer::timeout, this, &FilterPanel::apply);
    connect(&m_watcher, &QFutureWatcher<void>::progressValueChanged, this, [this](int permille) {
        m_progress = permille;
        takeResults();
        updateStatus();
    });
    connect(&m_watcher, &QFutureWatcher<void>::finished, this, &FilterPanel::filterFinished);

    connect(document, &TextDocument::sourceChanged, this, &FilterPanel::sourceChanged);
    connect(document, &TextDocument::edited, this, &FilterPanel::documentChanged);
    connect(document, &TextDocument::codecChanged, this, &FilterPanel::documentChanged);

    // The rule used last, wherever: the logs one reads tend to be filtered alike.
    const QJsonObject last = QJsonDocument::fromJson(QSettings().value(lastRuleKey()).toByteArray()).object();
    if (const std::optional<FilterRule> rule = FilterRule::fromJson(last))
        setRule(*rule);
    else
        addCondition();
    updateWriteButton();
    updateNotice();
    updateStatus();
}

FilterPanel::~FilterPanel()
{
    m_writer->cancel();
    m_watcher.cancel(); // the thread lets go of its copy of the document soon after
}

// The rule --------------------------------------------------------------------------------------------

FilterRule FilterPanel::rule(QString* error) const
{
    FilterRule rule;
    rule.anyInclusion = m_any->isChecked();
    rule.description = m_ruleDescription;
    for (qsizetype i = 0; i < m_conditions.size(); ++i) {
        const ConditionEditor* editor = m_conditions[i];
        if (editor->isEmpty())
            continue;
        QString problem;
        if (const std::optional<FilterCondition> condition = editor->condition(&problem))
            rule.conditions.append(*condition);
        else if (error && error->isEmpty())
            *error = tr("Condition %1: %2").arg(i + 1).arg(problem);
    }
    return rule;
}

void FilterPanel::setRule(const FilterRule& rule)
{
    for (ConditionEditor* editor : std::exchange(m_conditions, {})) {
        editor->hide();
        editor->deleteLater();
    }
    for (const FilterCondition& condition : rule.conditions)
        addCondition(condition);
    if (m_conditions.isEmpty())
        addCondition();
    m_any->setChecked(rule.anyInclusion);
    m_ruleDescription = rule.description;
    apply();
}

bool FilterPanel::apply()
{
    m_runTimer.stop();
    QString error;
    const FilterRule rule = this->rule(&error);
    if (!error.isEmpty()) {
        showMessage(error, true); // the last results stay until the rule can run again
        return false;
    }
    showMessage({});
    if (rule.conditions.isEmpty()) {
        stop();
        m_shown.reset();
        m_results->clear();
        m_results->setHighlights({});
        updateStatus();
        return false;
    }
    QSettings().setValue(lastRuleKey(), QJsonDocument(rule.toJson()).toJson(QJsonDocument::Compact));
    if (m_shown && sameConditions(*m_shown, rule) && (m_complete || isFiltering() || m_stale)) {
        m_shown = rule;
        return true; // already shown, or on its way
    }
    run(rule);
    return true;
}

ConditionEditor* FilterPanel::addCondition(const FilterCondition& condition)
{
    auto* editor = new ConditionEditor(this);
    editor->setCondition(condition);
    // Only what the user does counts as an edit; setRule() fills the editors without one.
    connect(editor->pattern, &QLineEdit::textEdited, this, &FilterPanel::ruleEdited);
    connect(editor->comparison, &QLineEdit::textEdited, this, &FilterPanel::ruleEdited);
    connect(editor->type, &QComboBox::activated, this, &FilterPanel::ruleEdited);
    connect(editor->caseSensitive, &QToolButton::clicked, this, &FilterPanel::ruleEdited);
    connect(editor->remove, &QToolButton::clicked, this, [this, editor] { removeCondition(editor); });
    editor->pattern->installEventFilter(this);
    editor->comparison->installEventFilter(this);
    m_conditionsLayout->addWidget(editor);
    m_conditions.append(editor);
    return editor;
}

void FilterPanel::removeCondition(ConditionEditor* editor)
{
    m_conditions.removeOne(editor);
    editor->hide();
    editor->deleteLater(); // its own button is still on the call stack
    if (m_conditions.isEmpty())
        addCondition()->pattern->setFocus(Qt::OtherFocusReason);
    apply();
}

QLineEdit* FilterPanel::patternEdit(int condition) const
{
    return m_conditions.value(condition) ? m_conditions.value(condition)->pattern : nullptr;
}

QLineEdit* FilterPanel::comparisonEdit(int condition) const
{
    return m_conditions.value(condition) ? m_conditions.value(condition)->comparison : nullptr;
}

void FilterPanel::ruleEdited()
{
    m_runTimer.start();
}

// Running ---------------------------------------------------------------------------------------------

const std::vector<FilterMatch>& FilterPanel::matches() const
{
    return m_results->matches();
}

void FilterPanel::run(const FilterRule& rule, std::uint64_t firstRow)
{
    stop();
    if (firstRow == 0)
        m_results->clear();
    m_results->setHighlights(rule);
    m_shown = rule;
    m_complete = false;
    m_capped = false;
    m_progress = 0;
    m_stale = !isVisible();
    if (m_stale) { // runs when the panel shows again
        updateStatus();
        return;
    }

    auto inbox = std::make_shared<Inbox>();
    m_inbox = inbox;
    m_watcher.setFuture(QtConcurrent::run(
        [rule, context = contextOf(*m_document), firstRow, inbox](QPromise<void>& promise) {
            promise.setProgressRange(0, 1000);
            FilterHooks hooks;
            hooks.cancelled = [&promise] { return promise.isCanceled(); };
            hooks.progress = [&promise](int permille) { promise.setProgressValue(permille); };
            hooks.found = [&inbox](std::vector<FilterMatch>&& batch) {
                const std::lock_guard lock(inbox->mutex);
                inbox->matches.insert(inbox->matches.end(), batch.begin(), batch.end());
            };
            filterLines(rule, context, firstRow, hooks);
        }));
    updateStatus();
}

void FilterPanel::stop()
{
    if (m_watcher.isRunning())
        m_watcher.cancel();
    m_inbox.reset(); // what it still reports is dropped
}

void FilterPanel::takeResults()
{
    if (!m_inbox)
        return;
    std::vector<FilterMatch> batch;
    {
        const std::lock_guard lock(m_inbox->mutex);
        batch.swap(m_inbox->matches);
    }
    if (batch.empty())
        return;
    const std::size_t room = kMaxResults - std::min(kMaxResults, m_results->matches().size());
    if (batch.size() >= room) {
        batch.resize(room);
        m_capped = true;
        m_watcher.cancel();
    }
    m_results->append(batch);
}

void FilterPanel::filterFinished()
{
    // A run that was stopped after its end but before the end was reported has no inbox.
    if (!m_inbox)
        return;
    takeResults();
    if (m_watcher.isCanceled() && !m_capped)
        return;
    m_inbox.reset();
    m_complete = !m_capped;
    if (m_complete) {
        // Enough to read only what a growing log appends next time.
        const core::LineIndex& index = m_document->index();
        const auto bytes = m_document->bytes();
        m_scannedSize = bytes.size();
        m_scannedTail = tailOf(bytes, bytes.size());
        m_scannedLineBreak = static_cast<int>(index.lineBreak());
        const std::uint64_t rows = index.rowCount();
        m_lastLine = rows > 0 ? index.row(rows - 1).line : 0;
        m_lastLineRow = rows > 0 ? index.rowIndexOfLine(m_lastLine).value_or(0) : 0;
    }
    updateStatus();
}

void FilterPanel::sourceChanged()
{
    if (!m_shown)
        return;
    // A log that only grew: the lines before its last one are as they were, and the last one
    // may have gone on. Only those are read again.
    const auto bytes = m_document->bytes();
    const bool grew = m_complete && bytes.size() >= m_scannedSize &&
                      static_cast<int>(m_document->index().lineBreak()) == m_scannedLineBreak &&
                      tailOf(bytes, m_scannedSize) == m_scannedTail;
    const FilterRule rule = *m_shown;
    if (grew) {
        m_results->truncate(m_lastLine);
        run(rule, m_lastLineRow);
    } else {
        run(rule);
    }
}

void FilterPanel::documentChanged()
{
    if (!m_shown)
        return;
    // Edited or decoded anew: lines have moved. Run again once the typing pauses.
    stop();
    m_results->clear();
    m_complete = false;
    m_runTimer.start();
    updateStatus();
}

void FilterPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateNotice(); // the AI settings may have changed meanwhile
    if (m_stale && m_shown)
        run(*m_shown);
}

void FilterPanel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (isFiltering()) {
        stop();
        m_complete = false;
        m_stale = true;
    }
}

// Status ----------------------------------------------------------------------------------------------

QString FilterPanel::statusText() const
{
    return m_status->text();
}

void FilterPanel::showMessage(const QString& message, bool error)
{
    m_message = message;
    m_messageIsError = error;
    updateStatus();
}

void FilterPanel::updateStatus()
{
    const QLocale locale;
    QString text = m_message;
    if (text.isEmpty() && m_shown) {
        const QString count = locale.toString(static_cast<qulonglong>(m_results->count()));
        if (isFiltering())
            text = tr("Filtering… %1% · %2 matching lines so far").arg(m_progress / 10).arg(count);
        else if (m_capped)
            text = tr("Showing the first %1 matching lines; narrow the rule to see the rest").arg(count);
        else if (m_complete)
            text = tr("%1 of %2 lines match")
                       .arg(count, locale.toString(static_cast<qulonglong>(m_document->index().lineCount())));
    }
    m_status->setText(text);
    QPalette pal = palette();
    if (m_messageIsError && !m_message.isEmpty()) {
        const bool dark = pal.color(QPalette::Window).lightness() < 128;
        pal.setColor(QPalette::WindowText, dark ? QColor(0xEF, 0x9A, 0x9A) : QColor(0xC6, 0x28, 0x28));
    }
    m_status->setPalette(pal);
}

void FilterPanel::setResultsFont(const QFont& font)
{
    m_results->setFont(font);
}

void FilterPanel::focusEditor()
{
    const bool noRule = std::all_of(m_conditions.begin(), m_conditions.end(),
                                    [](const ConditionEditor* editor) { return editor->isEmpty(); });
    QLineEdit* target = noRule ? m_description : m_conditions.first()->pattern;
    target->setFocus(Qt::ShortcutFocusReason);
    target->selectAll();
}

bool FilterPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && qobject_cast<QLineEdit*>(watched)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (watched == m_description)
                writeRule();
            else
                apply();
            return true;
        case Qt::Key_Escape:
            if (m_writer->isBusy())
                writeRule(); // stops it
            else
                emit closeRequested();
            return true;
        case Qt::Key_Down:
            if (watched != m_description && m_results->count() > 0) {
                m_results->setFocus(Qt::TabFocusReason);
                return true;
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FilterPanel::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        emit closeRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

// The AI ----------------------------------------------------------------------------------------------

void FilterPanel::writeRule()
{
    if (m_writer->isBusy()) {
        m_writer->cancel();
        m_aiNote = tr("Stopped.");
        updateWriteButton();
        updateNotice();
        return;
    }
    const QString description = m_description->text().trimmed();
    if (description.isEmpty()) {
        m_aiNote = tr("Describe the lines you want to see first.");
        updateNotice();
        m_description->setFocus(Qt::OtherFocusReason);
        return;
    }
    m_aiNote = tr("Asking %1 for a rule…").arg(m_writer->settings().chatUrl().host());
    m_writer->write(description, FilterRuleWriter::sampleLines(contextOf(*m_document)));
    updateWriteButton();
    updateNotice();
}

void FilterPanel::ruleWritten(const FilterRule& rule)
{
    m_aiNote = tr("The AI wrote the rule below (%1). It runs here, on the whole file.")
                   .arg(rule.description.isEmpty() ? tr("no description") : rule.description);
    setRule(rule);
    updateWriteButton();
    updateNotice();
}

void FilterPanel::writerFailed(const QString& message)
{
    m_aiNote = message;
    updateWriteButton();
    updateNotice();
}

void FilterPanel::updateWriteButton()
{
    m_write->setText(m_writer->isBusy() ? tr("Stop") : tr("Write Rule"));
}

void FilterPanel::updateNotice()
{
    const AiSettings settings = m_writer->settings();
    if (!m_aiNote.isEmpty()) {
        m_notice->setTextFormat(Qt::PlainText);
        m_notice->setText(m_aiNote);
    } else if (!settings.isConfigured()) {
        m_notice->setTextFormat(Qt::RichText);
        m_notice->setText(tr("The AI can write the rule from a description once it is set up: "
                             "<a href=\"settings\">open the AI settings</a>."));
    } else {
        m_notice->setTextFormat(Qt::PlainText);
        m_notice->setText(tr("Sends your description and up to %1 sample lines to %2. The rule then runs on this "
                             "computer; the rest of the file is not sent.")
                              .arg(FilterRuleWriter::kSampleLines)
                              .arg(settings.chatUrl().host()));
    }
}

void FilterPanel::showPreview()
{
    const QString typed = m_description->text().trimmed();
    showRequestPreview(this, m_writer->settings(),
                       m_writer->previewRequest(typed.isEmpty() ? tr("(your description)") : typed,
                                                FilterRuleWriter::sampleLines(contextOf(*m_document))));
}

} // namespace aiedit::ui
