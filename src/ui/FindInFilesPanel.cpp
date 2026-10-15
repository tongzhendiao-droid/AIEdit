#include "ui/FindInFilesPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <limits>
#include <mutex>
#include <optional>

namespace aiedit::ui {

namespace {

enum Role
{
    PathRole = Qt::UserRole + 1,
    LineRole,
    OffsetRole,
};

QString key(const char* name)
{
    return QStringLiteral("findInFiles/") + QLatin1String(name);
}

// What the filter panel ran last, wherever.
std::optional<FilterRule> lastFilterRule()
{
    const QByteArray json = QSettings().value(QStringLiteral("filter/lastRule")).toByteArray();
    return FilterRule::fromJson(QJsonDocument::fromJson(json).object());
}

// A rule in a few words, to show in place of a query.
QString describe(const FilterRule& rule)
{
    if (!rule.description.isEmpty())
        return rule.description;
    QStringList parts;
    for (const FilterCondition& condition : rule.conditions) {
        QString part =
            condition.exclude ? FindInFilesPanel::tr("not %1").arg(condition.pattern) : condition.pattern;
        if (!condition.field.isEmpty())
            part += QStringLiteral(" (%1 %2 %3)")
                        .arg(condition.field, condition.op, QString::number(condition.value, 'g', 15));
        parts << part;
    }
    return parts.join(rule.anyInclusion ? FindInFilesPanel::tr(" or ") : QStringLiteral(", "));
}

// A line as the results show it: tabs and control characters become spaces.
QString shown(QString text)
{
    for (QChar& c : text) {
        if (c.unicode() < 0x20)
            c = u' ';
    }
    return text;
}

} // namespace

// Files the search found, waiting for the panel to take them.
struct FindInFilesPanel::Inbox
{
    std::mutex mutex;
    std::vector<FileMatches> files;
};

FindInFilesPanel::FindInFilesPanel(QWidget* parent) : QWidget(parent)
{
    m_query = new QLineEdit(this);
    m_query->setClearButtonEnabled(true);
    m_query->setPlaceholderText(tr("What to find in the files"));
    m_query->installEventFilter(this);
    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Text"), static_cast<int>(Mode::Text));
    m_mode->addItem(tr("Regex"), static_cast<int>(Mode::Regex));
    m_mode->addItem(tr("Filter Rule"), static_cast<int>(Mode::FilterRule));
    m_mode->setItemData(2, tr("The rule the filter panel ran last (Edit > Filter Lines)"), Qt::ToolTipRole);
    m_caseSensitive = new QCheckBox(tr("Match case"), this);
    m_search = new QPushButton(this);
    connect(m_search, &QPushButton::clicked, this, [this] { isSearching() ? stop() : search(); });

    m_folder = new QLineEdit(this);
    m_folder->setPlaceholderText(tr("The folder to search in"));
    m_folder->installEventFilter(this);
    m_browse = new QToolButton(this);
    m_browse->setText(QStringLiteral("…"));
    m_browse->setToolTip(tr("Choose the folder"));
    connect(m_browse, &QToolButton::clicked, this, &FindInFilesPanel::browse);
    m_filters = new QLineEdit(this);
    m_filters->setPlaceholderText(tr("Which files, e.g. *.log; *.txt (all if empty)"));
    m_filters->installEventFilter(this);
    m_subfolders = new QCheckBox(tr("Subfolders"), this);

    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_results = new QTreeWidget(this);
    m_results->setHeaderHidden(true);
    m_results->setUniformRowHeights(true);
    m_results->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    connect(m_results, &QTreeWidget::itemActivated, this, &FindInFilesPanel::activate);

    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("Find:"), this), 0, 0);
    auto* findRow = new QHBoxLayout;
    findRow->addWidget(m_query, 1);
    findRow->addWidget(m_mode);
    findRow->addWidget(m_caseSensitive);
    findRow->addWidget(m_search);
    grid->addLayout(findRow, 0, 1);
    grid->addWidget(new QLabel(tr("In:"), this), 1, 0);
    auto* whereRow = new QHBoxLayout;
    whereRow->addWidget(m_folder, 2);
    whereRow->addWidget(m_browse);
    whereRow->addWidget(m_filters, 1);
    whereRow->addWidget(m_subfolders);
    grid->addLayout(whereRow, 1, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 3, 3, 3);
    layout->setSpacing(3);
    layout->addLayout(grid);
    layout->addWidget(m_status);
    layout->addWidget(m_results, 1);

    const QSettings settings;
    m_folder->setText(settings.value(key("folder")).toString());
    m_filters->setText(settings.value(key("files")).toString());
    m_subfolders->setChecked(settings.value(key("subfolders"), true).toBool());
    m_caseSensitive->setChecked(settings.value(key("caseSensitive"), false).toBool());
    m_mode->setCurrentIndex(std::clamp(settings.value(key("mode"), 0).toInt(), 0, 2));
    connect(m_mode, &QComboBox::currentIndexChanged, this, &FindInFilesPanel::modeChanged);

    connect(&m_watcher, &QFutureWatcher<FileSearchSummary>::progressValueChanged, this, [this](int files) {
        m_files = files;
        takeResults();
        showStatus(tr("Searching… %n file(s) so far", "", files));
    });
    connect(&m_watcher, &QFutureWatcher<FileSearchSummary>::finished, this, &FindInFilesPanel::searchFinished);
    modeChanged();
    updateButton();
}

FindInFilesPanel::~FindInFilesPanel()
{
    m_watcher.cancel(); // the search stops at the next line block
}

QString FindInFilesPanel::folder() const
{
    return m_folder->text();
}

void FindInFilesPanel::setFolder(const QString& folder)
{
    m_folder->setText(QDir::toNativeSeparators(folder));
}

void FindInFilesPanel::setQuery(const QString& query)
{
    m_query->setText(query);
}

void FindInFilesPanel::setMode(Mode mode)
{
    m_mode->setCurrentIndex(m_mode->findData(static_cast<int>(mode)));
}

void FindInFilesPanel::setNameFilters(const QString& filters)
{
    m_filters->setText(filters);
}

void FindInFilesPanel::focusQuery()
{
    QLineEdit* target = m_query->isEnabled() ? m_query : m_folder;
    target->setFocus(Qt::ShortcutFocusReason);
    target->selectAll();
}

QString FindInFilesPanel::statusText() const
{
    return m_status->text();
}

void FindInFilesPanel::modeChanged()
{
    // A filter rule takes the place of the query; it is edited in the filter panel.
    const bool rule = static_cast<Mode>(m_mode->currentData().toInt()) == Mode::FilterRule;
    m_query->setEnabled(!rule);
    m_caseSensitive->setEnabled(!rule);
    if (rule) {
        const std::optional<FilterRule> last = lastFilterRule();
        m_query->setPlaceholderText(last ? describe(*last)
                                         : tr("No filter rule yet: run one in Edit > Filter Lines"));
        m_query->clear();
    } else {
        m_query->setPlaceholderText(tr("What to find in the files"));
    }
}

void FindInFilesPanel::browse()
{
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Search in Folder"), m_folder->text());
    if (!folder.isEmpty())
        setFolder(folder);
}

void FindInFilesPanel::search()
{
    stop();
    FileSearchRequest request;
    QString folder = QDir::fromNativeSeparators(m_folder->text().trimmed());
    if (folder == u"~" || folder.startsWith(QStringLiteral("~/")))
        folder.replace(0, 1, QDir::homePath());
    request.folder = QDir::cleanPath(folder);
    if (folder.isEmpty() || !QFileInfo(request.folder).isDir()) {
        showStatus(tr("Choose a folder to search in"), true);
        return;
    }
    const Mode mode = static_cast<Mode>(m_mode->currentData().toInt());
    if (mode == Mode::FilterRule) {
        const std::optional<FilterRule> rule = lastFilterRule();
        if (!rule) {
            showStatus(tr("There is no filter rule yet: run one in Edit > Filter Lines first"), true);
            return;
        }
        request.rule = *rule;
    } else {
        if (m_query->text().isEmpty()) {
            showStatus(tr("Type what to find"), true);
            return;
        }
        request.rule = queryRule(m_query->text(), mode == Mode::Regex, m_caseSensitive->isChecked());
        if (const QString problem = request.rule.conditions.first().check(); !problem.isEmpty()) {
            showStatus(problem, true);
            return;
        }
    }
    static const QRegularExpression separators(QStringLiteral("[;,\\s]+"));
    request.nameFilters = m_filters->text().split(separators, Qt::SkipEmptyParts);
    request.subfolders = m_subfolders->isChecked();
    request.legacyCodec = m_legacyCodec;
    saveSettings();

    m_results->clear();
    m_matches = 0;
    m_files = 0;
    m_searchedFolder = request.folder;
    auto inbox = std::make_shared<Inbox>();
    m_inbox = inbox;
    m_watcher.setFuture(QtConcurrent::run([request, inbox](QPromise<FileSearchSummary>& promise) {
        promise.setProgressRange(0, std::numeric_limits<int>::max());
        FileSearchHooks hooks;
        hooks.cancelled = [&promise] { return promise.isCanceled(); };
        hooks.progress = [&promise](int files) { promise.setProgressValue(files); };
        hooks.found = [&inbox](FileMatches&& file) {
            const std::lock_guard lock(inbox->mutex);
            inbox->files.push_back(std::move(file));
        };
        promise.addResult(searchFiles(request, hooks));
    }));
    showStatus(tr("Searching…"));
    updateButton();
}

void FindInFilesPanel::stop()
{
    if (!isSearching())
        return;
    m_watcher.cancel();
    takeResults(); // what was found so far stays
    m_inbox.reset();
    showStatus(tr("Stopped: %1 matches in %n file(s)", "", m_results->topLevelItemCount())
                   .arg(QLocale().toString(static_cast<qulonglong>(m_matches))));
    updateButton();
}

void FindInFilesPanel::takeResults()
{
    if (!m_inbox)
        return;
    std::vector<FileMatches> files;
    {
        const std::lock_guard lock(m_inbox->mutex);
        files.swap(m_inbox->files);
    }
    const QDir base(m_searchedFolder);
    for (const FileMatches& file : files) {
        auto* item = new QTreeWidgetItem(m_results);
        item->setText(0, QStringLiteral("%1 (%2)")
                             .arg(QDir::toNativeSeparators(base.relativeFilePath(file.path)))
                             .arg(file.matches.size()));
        item->setToolTip(0, QStringLiteral("%1 · %2").arg(QDir::toNativeSeparators(file.path), file.codec));
        item->setData(0, PathRole, file.path);
        QFont bold = m_results->font();
        bold.setBold(true);
        item->setFont(0, bold);
        for (std::size_t i = 0; i < file.matches.size(); ++i) {
            auto* line = new QTreeWidgetItem(item);
            const QString text = shown(file.texts.value(static_cast<qsizetype>(i)));
            line->setText(0, QStringLiteral("%1: %2").arg(file.matches[i].line + 1).arg(text));
            line->setData(0, PathRole, file.path);
            line->setData(0, LineRole, static_cast<qulonglong>(file.matches[i].line));
            line->setData(0, OffsetRole, static_cast<qulonglong>(file.matches[i].start));
        }
        item->setExpanded(true);
        m_matches += file.matches.size();
    }
}

void FindInFilesPanel::searchFinished()
{
    if (!m_inbox)
        return; // stopped
    takeResults();
    m_inbox.reset();
    updateButton();
    if (m_watcher.isCanceled() || m_watcher.future().resultCount() == 0)
        return;
    const FileSearchSummary summary = m_watcher.result();
    const QString matches = QLocale().toString(static_cast<qulonglong>(summary.matches));
    if (summary.matches == 0)
        showStatus(tr("No matches in %n file(s)", "", summary.filesSearched));
    else if (summary.limited)
        showStatus(tr("Showing the first %1 matches; narrow the search to see the rest").arg(matches));
    else
        showStatus(tr("%1 matches in %2 of %n file(s)", "", summary.filesSearched)
                       .arg(matches)
                       .arg(m_results->topLevelItemCount()));
}

void FindInFilesPanel::showStatus(const QString& text, bool error)
{
    m_status->setText(text);
    QPalette pal = palette();
    if (error) {
        const bool dark = pal.color(QPalette::Window).lightness() < 128;
        pal.setColor(QPalette::WindowText, dark ? QColor(0xEF, 0x9A, 0x9A) : QColor(0xC6, 0x28, 0x28));
    }
    m_status->setPalette(pal);
}

void FindInFilesPanel::updateButton()
{
    m_search->setText(isSearching() ? tr("Stop") : tr("Search"));
}

void FindInFilesPanel::activate(QTreeWidgetItem* item)
{
    if (!item)
        return;
    const QVariant line = item->data(0, LineRole);
    if (!line.isValid()) {
        item->setExpanded(!item->isExpanded()); // a file: its lines
        return;
    }
    emit openRequested(item->data(0, PathRole).toString(), line.toULongLong(),
                       item->data(0, OffsetRole).toULongLong());
}

void FindInFilesPanel::saveSettings() const
{
    QSettings settings;
    settings.setValue(key("folder"), m_folder->text());
    settings.setValue(key("files"), m_filters->text());
    settings.setValue(key("subfolders"), m_subfolders->isChecked());
    settings.setValue(key("caseSensitive"), m_caseSensitive->isChecked());
    settings.setValue(key("mode"), m_mode->currentIndex());
}

bool FindInFilesPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            search();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace aiedit::ui
