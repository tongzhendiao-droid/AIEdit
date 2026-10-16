#include "ui/FolderPanel.h"

#include "ui/Collation.h"
#include "ui/FolderSummary.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <iterator>
#include <limits>
#include <mutex>

namespace aiedit::ui {

namespace {

constexpr qsizetype kMaxRecentFolders = 20;
constexpr int kPreviewDelayMs = 150; // arrowing through files shows only the one stopped at
constexpr int kFilterDelayMs = 200;

QString key(const char* name)
{
    return QStringLiteral("folder/") + QLatin1String(name);
}

QString recentKey()
{
    return QStringLiteral("folders/recent");
}

bool isValidName(const QString& name)
{
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

QString displayName(const QString& folder)
{
    const QString name = QDir(folder).dirName();
    return name.isEmpty() ? QDir::toNativeSeparators(folder) : name;
}

} // namespace

// Files the listing found, waiting for the panel to take them.
struct FolderPanel::ListInbox
{
    std::mutex mutex;
    std::vector<ListedFile> files;
};

FolderPanel::FolderPanel(QWidget* parent) : QWidget(parent)
{
    m_showHidden = QSettings().value(key("showHidden"), false).toBool();
    createActions();

    m_title = new QLabel(this);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_title->setMinimumWidth(1); // a long name is cut off rather than widening the panel
    m_menu = new QMenu(this);
    connect(m_menu, &QMenu::aboutToShow, this, &FolderPanel::populateMenu);
    m_menuButton = new QToolButton(this);
    m_menuButton->setAutoRaise(true);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setToolTip(tr("Folder actions"));
    if (const QIcon icon = QIcon::fromTheme(QStringLiteral("open-menu-symbolic"),
                                            QIcon::fromTheme(QStringLiteral("application-menu")));
        !icon.isNull())
        m_menuButton->setIcon(icon);
    else
        m_menuButton->setText(QStringLiteral("⋯"));
    m_menuButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; }"));
    m_menuButton->setMenu(m_menu);

    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(tr("Filter by name: *.log, error…"));
    m_filter->setToolTip(tr("Lists the files of the whole folder whose names match: wildcards such as *.log, or a "
                            "part of the name. With a slash, the path within the folder. Several, separated by "
                            "spaces, match if any does."));
    m_filter->installEventFilter(this);
    m_filterTimer.setSingleShot(true);
    m_filterTimer.setInterval(kFilterDelayMs);
    connect(m_filter, &QLineEdit::textChanged, this, [this] { m_filterTimer.start(); });
    connect(&m_filterTimer, &QTimer::timeout, this, &FolderPanel::applyFilter);

    m_listModel = new FileListModel(&m_sniffer, this);
    m_tree = new QTreeView(this);
    m_list = new QTreeView(this);
    m_list->setModel(m_listModel);
    m_list->setRootIsDecorated(false);
    m_list->setItemsExpandable(false);
    for (QTreeView* view : {m_tree, m_list}) {
        // Names only, without a header: icons tell folders from files, and a name's tooltip
        // says the rest (size, time, type).
        view->setHeaderHidden(true);
        view->setUniformRowHeights(true);
        view->setTextElideMode(Qt::ElideMiddle);
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        view->installEventFilter(this);
        // Their shortcuts work where the files are.
        view->addActions({m_copyPathAction, m_renameAction, m_trashAction});
        connect(view, &QWidget::customContextMenuRequested, this,
                [this, view](const QPoint& pos) { showContextMenu(view, pos); });
        connect(view, &QAbstractItemView::clicked, this, [this](const QModelIndex& index) {
            const QString path = pathAt(index);
            if (QFileInfo(path).isFile()) {
                m_previewTimer.stop();
                emit previewRequested(path);
            }
        });
        // Folders open and close by themselves on a double-click.
        connect(view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
            const QString path = pathAt(index);
            if (QFileInfo(path).isFile())
                emit openRequested(path);
        });
    }
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { currentChanged(m_list); });
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderPanel::updateActions);
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(kPreviewDelayMs);
    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        const QString path = pathAt(currentView()->currentIndex());
        if (QFileInfo(path).isFile())
            emit previewRequested(path);
    });

    m_views = new QStackedWidget(this);
    m_views->addWidget(m_tree);
    m_views->addWidget(m_list);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setContentsMargins(4, 0, 4, 2);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->hide();

    m_folderPage = new QWidget(this);
    auto* header = new QHBoxLayout;
    header->setContentsMargins(4, 0, 0, 0);
    header->addWidget(m_title, 1);
    header->addWidget(m_menuButton);
    auto* folderLayout = new QVBoxLayout(m_folderPage);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->setSpacing(2);
    folderLayout->addLayout(header);
    folderLayout->addWidget(m_filter);
    folderLayout->addWidget(m_views, 1);
    folderLayout->addWidget(m_status);

    m_emptyPage = createEmptyPage();
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_emptyPage);
    m_pages->addWidget(m_folderPage);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_pages);

    connect(&m_listWatcher, &QFutureWatcher<FileListSummary>::progressValueChanged, this, [this] {
        if (!m_listInbox)
            return;
        takeListed();
        m_listStatus =
            tr("Looking for files… %1 found so far").arg(QLocale().toString(m_listModel->rowCount()));
        updateStatus();
    });
    connect(&m_listWatcher, &QFutureWatcher<FileListSummary>::finished, this, &FolderPanel::listingFinished);
    connect(&m_summaryWatcher, &QFutureWatcher<QString>::finished, this, [this] {
        updateStatus();
        updateActions();
        if (m_summaryWatcher.isCanceled() || m_summaryWatcher.future().resultCount() == 0)
            return;
        const QString summary = m_summaryWatcher.result();
        if (!summary.isEmpty())
            emit folderSummarized(m_summaryFolder, summary);
    });

    showNamesOnly(m_list);
    updateEmptyPage();
    updateActions();
}

FolderPanel::~FolderPanel()
{
    m_listWatcher.cancel();
    m_summaryWatcher.cancel();
    m_listWatcher.waitForFinished();
    m_summaryWatcher.waitForFinished();
    // The views and models ask the type sniffer for types; it is a member, gone before them.
    delete m_tree;
    delete m_list;
    delete m_model;
    delete m_listModel;
}

QSize FolderPanel::sizeHint() const
{
    return {fontMetrics().horizontalAdvance(QLatin1Char('x')) * 42, 400};
}

void FolderPanel::createActions()
{
    const auto action = [this](const QString& text, auto slot, const QKeySequence& shortcut = {}) {
        auto* a = new QAction(text, this);
        if (!shortcut.isEmpty()) {
            a->setShortcut(shortcut);
            a->setShortcutContext(Qt::WidgetShortcut);
        }
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    m_openAction = action(tr("&Open"), [this] {
        for (const QString& path : selectedPaths())
            if (QFileInfo(path).isFile())
                emit openRequested(path);
    });
    m_openFolderAction = action(tr("Open as &Folder"), [this] {
        const QStringList paths = selectedPaths();
        if (paths.size() == 1)
            setFolder(paths.first());
    });
    m_compareAction = action(tr("Co&mpare These Two Files"), [this] {
        QStringList paths = selectedPaths();
        if (paths.size() != 2)
            return;
        const QCollator collator = naturalCollator();
        std::sort(paths.begin(), paths.end(),
                  [&collator](const QString& a, const QString& b) { return collator.compare(a, b) < 0; });
        emit compareRequested(paths[0], paths[1]);
    });
    m_findHereAction = action(tr("&Find in This Folder…"), [this] {
        if (!m_actionFolder.isEmpty())
            emit findInFolderRequested(m_actionFolder);
    });
    m_askAiAction = action(tr("&What Is in This Folder?"), [this] { summarize(m_actionFolder); });
    m_askAiAction->setToolTip(tr("The AI is told what the folder holds: names, sizes and kinds of files, and the "
                                 "first lines of a few. The files themselves are not sent."));
    m_newFileAction = action(tr("New &File…"), [this] { newFile(m_actionFolder); });
    m_newFolderAction = action(tr("New Fol&der…"), [this] { newFolder(m_actionFolder); });
    m_renameAction = action(tr("&Rename…"), &FolderPanel::renameSelected, QKeySequence(Qt::Key_F2));
    m_trashAction = action(tr("Move to &Trash"), &FolderPanel::trashSelected, QKeySequence(QKeySequence::Delete));
    m_copyPathAction = action(tr("&Copy Path"), [this] { copyPaths(false); }, QKeySequence(QKeySequence::Copy));
    m_copyRelativePathAction = action(tr("Copy Re&lative Path"), [this] { copyPaths(true); });
    m_revealAction = action(tr("Show in File &Manager"), [this] {
        const QStringList paths = selectedPaths();
        reveal(paths.size() == 1 ? paths.first() : m_folder);
    });
    m_folderActions = {m_findHereAction, m_askAiAction, m_newFileAction, m_newFolderAction};
    m_hiddenAction = new QAction(tr("Show &Hidden Files"), this);
    m_hiddenAction->setCheckable(true);
    m_hiddenAction->setChecked(m_showHidden);
    connect(m_hiddenAction, &QAction::toggled, this, &FolderPanel::setShowHidden);
}

QWidget* FolderPanel::createEmptyPage()
{
    auto* page = new QWidget(this);
    auto* none = new QLabel(tr("No folder open"), page);
    none->setAlignment(Qt::AlignCenter);
    none->setForegroundRole(QPalette::PlaceholderText);
    auto* open = new QPushButton(tr("Open Folder…"), page);
    connect(open, &QPushButton::clicked, this, &FolderPanel::showOpenDialog);
    auto* hint = new QLabel(tr("Its files show here: click one to look at it, double-click to open it."), page);
    hint->setWordWrap(true);
    hint->setAlignment(Qt::AlignCenter);
    hint->setForegroundRole(QPalette::PlaceholderText);
    m_recentLabel = new QLabel(tr("Recent folders"), page);
    m_recentList = new QListWidget(page);
    m_recentList->setFrameShape(QFrame::NoFrame);
    m_recentList->setTextElideMode(Qt::ElideMiddle);
    m_recentList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recentList->viewport()->setBackgroundRole(QPalette::Window);
    const auto openRecent = [this](QListWidgetItem* item) { openRecentFolder(item->data(Qt::UserRole).toString()); };
    connect(m_recentList, &QListWidget::itemActivated, this, openRecent);
    connect(m_recentList, &QListWidget::itemClicked, this, openRecent);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(open);
    buttons->addStretch();
    auto* layout = new QVBoxLayout(page);
    layout->addSpacing(16);
    layout->addWidget(none);
    layout->addLayout(buttons);
    layout->addWidget(hint);
    layout->addSpacing(8);
    layout->addWidget(m_recentLabel);
    layout->addWidget(m_recentList, 1);
    return page;
}

void FolderPanel::ensureModel()
{
    if (m_model)
        return;
    m_model = new FolderModel(&m_sniffer, this);
    m_model->setReadOnly(true);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs |
                       (m_showHidden ? QDir::Hidden : QDir::Filters()));
    m_tree->setModel(m_model);
    showNamesOnly(m_tree);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { currentChanged(m_tree); });
    connect(m_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &FolderPanel::updateActions);
    // A file just made shows up a moment later: select it then.
    const auto retry = [this] {
        if (!m_pendingSelection.isEmpty())
            select(m_pendingSelection);
    };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, retry);
    connect(m_model, &QFileSystemModel::directoryLoaded, this, retry);
}

bool FolderPanel::setFolder(const QString& folder)
{
    if (folder.isEmpty()) {
        closeFolder();
        return true;
    }
    const QFileInfo info(folder);
    if (!info.isDir())
        return false;
    const QString path = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    rememberFolder(path);
    m_pages->setCurrentWidget(m_folderPage);
    if (path == m_folder)
        return true;

    ensureModel();
    stopListing();
    m_sniffer.cancelPending();
    m_previewTimer.stop();
    m_pendingSelection.clear();
    m_folder = path;
    m_tree->setRootIndex(m_model->setRootPath(path));
    m_tree->scrollToTop();
    {
        const QSignalBlocker blocker(m_filter);
        m_filter->clear();
    }
    m_filterTimer.stop();
    applyFilter(); // back to the tree
    m_title->setText(displayName(path));
    m_title->setToolTip(QDir::toNativeSeparators(path));
    updateActions();
    emit folderChanged(path);
    return true;
}

void FolderPanel::closeFolder()
{
    if (m_folder.isEmpty())
        return;
    stopListing();
    m_sniffer.cancelPending();
    m_previewTimer.stop();
    m_pendingSelection.clear();
    m_folder.clear();
    m_listModel->clear();
    m_title->clear();
    updateEmptyPage();
    m_pages->setCurrentWidget(m_emptyPage);
    updateActions();
    emit folderChanged({});
}

QStringList FolderPanel::recentFolders() const
{
    return QSettings().value(recentKey()).toStringList();
}

void FolderPanel::clearRecentFolders()
{
    QSettings().remove(recentKey());
    updateEmptyPage();
}

void FolderPanel::forgetFolder(const QString& folder)
{
    QStringList recent = recentFolders();
    if (recent.removeAll(folder) == 0)
        return;
    QSettings().setValue(recentKey(), recent);
    updateEmptyPage();
}

void FolderPanel::openRecentFolder(const QString& folder)
{
    if (setFolder(folder))
        return;
    forgetFolder(folder);
    QMessageBox::warning(this, tr("Open Folder"), tr("%1 no longer exists.").arg(QDir::toNativeSeparators(folder)));
}

void FolderPanel::rememberFolder(const QString& folder)
{
    QStringList recent = recentFolders();
    recent.removeAll(folder);
    recent.prepend(folder);
    if (recent.size() > kMaxRecentFolders)
        recent.resize(kMaxRecentFolders);
    QSettings().setValue(recentKey(), recent);
    updateEmptyPage();
}

void FolderPanel::setLegacyCodec(std::string_view codec)
{
    m_legacyCodec = codec;
    m_sniffer.setLegacyCodec(codec);
}

void FolderPanel::setFilter(const QString& text)
{
    {
        const QSignalBlocker blocker(m_filter);
        m_filter->setText(text);
    }
    applyFilter();
}

QString FolderPanel::filter() const
{
    return m_filter->text();
}

bool FolderPanel::showsList() const
{
    return m_views->currentWidget() == m_list;
}

void FolderPanel::setShowHidden(bool show)
{
    if (show == m_showHidden)
        return;
    m_showHidden = show;
    QSettings().setValue(key("showHidden"), show);
    {
        const QSignalBlocker blocker(m_hiddenAction);
        m_hiddenAction->setChecked(show);
    }
    if (m_model)
        m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs |
                           (show ? QDir::Hidden : QDir::Filters()));
    if (showsList())
        applyFilter();
}

QString FolderPanel::statusText() const
{
    return m_status->text();
}

void FolderPanel::applyFilter()
{
    m_filterTimer.stop();
    stopListing();
    m_listModel->clear();
    const NameFilter filter(m_filter->text());
    if (filter.isEmpty() || m_folder.isEmpty()) {
        m_listStatus.clear();
        m_views->setCurrentWidget(m_tree);
        updateStatus();
        updateActions();
        return;
    }
    m_views->setCurrentWidget(m_list);
    updateActions();
    auto inbox = std::make_shared<ListInbox>();
    m_listInbox = inbox;
    m_listWatcher.setFuture(QtConcurrent::run(
        [folder = m_folder, filter, hidden = m_showHidden, inbox](QPromise<FileListSummary>& promise) {
            promise.setProgressRange(0, std::numeric_limits<int>::max());
            FileListHooks hooks;
            hooks.cancelled = [&promise] { return promise.isCanceled(); };
            hooks.found = [&inbox, &promise](std::vector<ListedFile>&& files, int visited) {
                {
                    const std::lock_guard lock(inbox->mutex);
                    inbox->files.insert(inbox->files.end(), std::make_move_iterator(files.begin()),
                                        std::make_move_iterator(files.end()));
                }
                promise.setProgressValue(visited);
            };
            promise.addResult(listFiles(folder, filter, hidden, hooks));
        }));
    m_listStatus = tr("Looking for files…");
    updateStatus();
}

void FolderPanel::stopListing()
{
    if (!m_listInbox)
        return;
    m_listWatcher.cancel();
    m_listInbox.reset();
}

void FolderPanel::takeListed()
{
    if (!m_listInbox)
        return;
    std::vector<ListedFile> files;
    {
        const std::lock_guard lock(m_listInbox->mutex);
        files.swap(m_listInbox->files);
    }
    m_listModel->append(std::move(files));
}

void FolderPanel::listingFinished()
{
    if (!m_listInbox)
        return; // stopped, or another listing started
    takeListed();
    m_listInbox.reset();
    if (m_listWatcher.isCanceled() || m_listWatcher.future().resultCount() == 0)
        return;
    const FileListSummary summary = m_listWatcher.result();
    const QLocale locale;
    if (summary.matched == 0)
        m_listStatus = tr("No file names match");
    else if (summary.limited && summary.matched >= kMaxListedFiles)
        m_listStatus = tr("Showing the first %1 files that match").arg(locale.toString(summary.matched));
    else if (summary.limited)
        m_listStatus = tr("%1 files match among the first %2 looked at")
                           .arg(locale.toString(summary.matched), locale.toString(summary.visited));
    else
        m_listStatus = tr("%n file(s) match", "", summary.matched);
    updateStatus();
}

void FolderPanel::updateStatus()
{
    QString text;
    if (m_summaryWatcher.isRunning())
        text = tr("Looking through %1 for the AI…").arg(shown(m_summaryFolder));
    else if (showsList())
        text = m_listStatus;
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void FolderPanel::currentChanged(QAbstractItemView* view)
{
    // Only the user's moves preview: not a selection made here, nor one the model moved.
    if (m_selecting || view != currentView() || !view->hasFocus())
        return;
    if (QFileInfo(pathAt(view->currentIndex())).isFile())
        m_previewTimer.start();
    else
        m_previewTimer.stop();
}

void FolderPanel::activate(const QModelIndex& index)
{
    const QString path = pathAt(index);
    const QFileInfo info(path);
    if (info.isFile()) {
        m_previewTimer.stop();
        emit openRequested(path);
    } else if (info.isDir() && currentView() == m_tree) {
        const QModelIndex name = index.siblingAtColumn(NameColumn);
        m_tree->setExpanded(name, !m_tree->isExpanded(name));
    }
}

bool FolderPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);
    const auto* key = static_cast<QKeyEvent*>(event);
    const bool enter = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                       !(key->modifiers() & ~Qt::KeypadModifier);
    if (watched == m_filter) {
        if (key->key() == Qt::Key_Down || enter) {
            if (enter)
                applyFilter(); // at once, rather than in a moment
            QAbstractItemView* view = currentView();
            view->setFocus();
            if (!view->currentIndex().isValid() && view->model())
                view->setCurrentIndex(view->model()->index(0, 0, view->rootIndex()));
            return true;
        }
        if (key->key() == Qt::Key_Escape && !m_filter->text().isEmpty()) {
            m_filter->clear();
            return true;
        }
    } else if ((watched == m_tree || watched == m_list) && enter) {
        const QModelIndex current = static_cast<QAbstractItemView*>(watched)->currentIndex();
        if (current.isValid()) {
            activate(current);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FolderPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateEmptyPage(); // another window may have opened folders since
}

QString FolderPanel::pathAt(const QModelIndex& index) const
{
    if (!index.isValid())
        return {};
    if (index.model() == m_listModel)
        return m_listModel->filePath(index);
    if (m_model && index.model() == m_model)
        return m_model->filePath(index);
    return {};
}

QAbstractItemView* FolderPanel::currentView() const
{
    return showsList() ? static_cast<QAbstractItemView*>(m_list) : static_cast<QAbstractItemView*>(m_tree);
}

QStringList FolderPanel::selectedPaths() const
{
    QStringList paths;
    QAbstractItemView* view = currentView();
    if (!view->selectionModel())
        return paths;
    for (const QModelIndex& index : view->selectionModel()->selectedIndexes())
        if (index.column() == NameColumn)
            if (const QString path = pathAt(index); !path.isEmpty())
                paths << path;
    return paths;
}

QString FolderPanel::shown(const QString& path) const
{
    if (path == m_folder)
        return displayName(path);
    if (!m_folder.isEmpty() && path.startsWith(m_folder + QLatin1Char('/')))
        return QDir::toNativeSeparators(path.mid(m_folder.size() + 1));
    return QDir::toNativeSeparators(path);
}

void FolderPanel::select(const QString& path)
{
    if (m_folder.isEmpty() || !m_model)
        return;
    if (showsList()) {
        if (const QModelIndex row = m_listModel->indexOf(path); row.isValid()) {
            const QScopedValueRollback guard(m_selecting, true);
            m_list->selectionModel()->setCurrentIndex(row, QItemSelectionModel::ClearAndSelect |
                                                               QItemSelectionModel::Rows);
            m_list->scrollTo(row);
            return;
        }
        setFilter(QString()); // not among what the filter lists: show it in the tree
    }
    if (path != m_folder && !path.startsWith(m_folder + QLatin1Char('/')))
        return;
    const QModelIndex index = m_model->index(path);
    if (!index.isValid()) {
        m_pendingSelection = path; // once the model has it
        return;
    }
    m_pendingSelection.clear();
    for (QModelIndex parent = index.parent(); parent.isValid() && parent != m_tree->rootIndex();
         parent = parent.parent())
        m_tree->expand(parent);
    const QScopedValueRollback guard(m_selecting, true);
    m_tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_tree->scrollTo(index);
}

void FolderPanel::showOpenDialog()
{
    QString start = m_folder;
    if (start.isEmpty())
        start = recentFolders().value(0, QDir::homePath());
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Open Folder"), start);
    if (!folder.isEmpty())
        setFolder(folder);
}

void FolderPanel::goUp()
{
    QDir dir(m_folder);
    if (m_folder.isEmpty() || !dir.cdUp())
        return;
    const QString child = m_folder;
    setFolder(dir.absolutePath());
    select(child);
}

QString FolderPanel::createFile(const QString& inFolder, const QString& name, QString* error)
{
    if (!isValidName(name)) {
        *error = tr("“%1” is not a valid name.").arg(name);
        return {};
    }
    const QString path = QDir(inFolder).filePath(name);
    if (QFileInfo::exists(path)) {
        *error = tr("%1 already exists.").arg(shown(path));
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        *error = tr("Cannot make %1: %2").arg(shown(path), file.errorString());
        return {};
    }
    file.close();
    return path;
}

QString FolderPanel::createFolder(const QString& inFolder, const QString& name, QString* error)
{
    if (!isValidName(name)) {
        *error = tr("“%1” is not a valid name.").arg(name);
        return {};
    }
    const QString path = QDir(inFolder).filePath(name);
    if (QFileInfo::exists(path)) {
        *error = tr("%1 already exists.").arg(shown(path));
        return {};
    }
    if (!QDir(inFolder).mkdir(name)) {
        *error = tr("Cannot make the folder %1.").arg(shown(path));
        return {};
    }
    return path;
}

bool FolderPanel::rename(const QString& path, const QString& newName, QString* error)
{
    const QFileInfo info(path);
    if (!isValidName(newName)) {
        *error = tr("“%1” is not a valid name.").arg(newName);
        return false;
    }
    const QString to = info.dir().filePath(newName);
    if (to == path)
        return true;
    // Only the case changes: the same file, on a file system that ignores case.
    if (QFileInfo::exists(to) && to.compare(path, Qt::CaseInsensitive) != 0) {
        *error = tr("%1 already exists.").arg(shown(to));
        return false;
    }
    if (m_unsavedCheck) {
        if (const QString document = m_unsavedCheck(path); !document.isEmpty()) {
            *error = tr("%1 has unsaved changes. Save or close it first.").arg(document);
            return false;
        }
    }
    const bool renamed = info.isDir() ? QDir().rename(path, to) : QFile::rename(path, to);
    if (!renamed) {
        *error = tr("Cannot rename %1.").arg(shown(path));
        return false;
    }
    emit pathRenamed(path, to);
    return true;
}

bool FolderPanel::moveToTrash(const QString& path, QString* error, QString* pathInTrash)
{
    if (m_unsavedCheck) {
        if (const QString document = m_unsavedCheck(path); !document.isEmpty()) {
            *error = tr("%1 has unsaved changes. Save or close it first.").arg(document);
            return false;
        }
    }
    QString inTrash;
    if (!QFile::moveToTrash(path, &inTrash)) {
        *error = tr("Cannot move %1 to the trash; it is still there.").arg(shown(path));
        return false;
    }
    if (pathInTrash)
        *pathInTrash = inTrash;
    emit pathTrashed(path);
    return true;
}

void FolderPanel::newFile(const QString& folder)
{
    if (folder.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New File"),
                                               tr("Name of the new file in %1:").arg(shown(folder)),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    QString error;
    const QString path = createFile(folder, name, &error);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("New File"), error);
        return;
    }
    select(path);
    emit openRequested(path);
}

void FolderPanel::newFolder(const QString& folder)
{
    if (folder.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New Folder"),
                                               tr("Name of the new folder in %1:").arg(shown(folder)),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    QString error;
    const QString path = createFolder(folder, name, &error);
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("New Folder"), error);
        return;
    }
    select(path);
}

void FolderPanel::renameSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.size() != 1)
        return;
    const QString path = paths.first();
    const QFileInfo info(path);
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Rename"));
    dialog.setLabelText(tr("New name for %1:").arg(info.fileName()));
    dialog.setTextValue(info.fileName());
    // The name without its extension is selected, as in file managers.
    if (auto* edit = dialog.findChild<QLineEdit*>()) {
        const qsizetype dot = info.isDir() ? -1 : info.fileName().lastIndexOf(QLatin1Char('.'));
        const qsizetype length = dot > 0 ? dot : info.fileName().size();
        QTimer::singleShot(0, edit, [edit, length] { edit->setSelection(0, static_cast<int>(length)); });
    }
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString name = dialog.textValue().trimmed();
    if (name.isEmpty() || name == info.fileName())
        return;
    QString error;
    if (!rename(path, name, &error)) {
        QMessageBox::warning(this, tr("Rename"), error);
        return;
    }
    select(info.dir().filePath(name));
}

void FolderPanel::trashSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    const QString question = paths.size() == 1
                                 ? tr("Move %1 to the trash?").arg(shown(paths.first()))
                                 : tr("Move %n item(s) to the trash?", "", static_cast<int>(paths.size()));
    if (QMessageBox::question(this, tr("Move to Trash"), question, QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Yes) != QMessageBox::Yes)
        return;
    QStringList errors;
    for (const QString& path : paths) {
        QString error;
        if (!moveToTrash(path, &error))
            errors << error;
    }
    if (!errors.isEmpty())
        QMessageBox::warning(this, tr("Move to Trash"), errors.join(QLatin1Char('\n')));
}

void FolderPanel::summarize(const QString& folder)
{
    if (folder.isEmpty())
        return;
    if (m_summaryWatcher.isRunning()) {
        m_summaryWatcher.cancel(); // it stops at the next folder
        m_summaryWatcher.waitForFinished();
    }
    m_summaryFolder = folder;
    m_summaryWatcher.setFuture(
        QtConcurrent::run([folder, codec = m_legacyCodec, hidden = m_showHidden](QPromise<QString>& promise) {
            promise.addResult(summarizeFolder(folder, codec, hidden, [&promise] { return promise.isCanceled(); }));
        }));
    updateStatus();
    updateActions();
}

void FolderPanel::copyPaths(bool relative)
{
    QStringList paths = selectedPaths();
    if (paths.isEmpty() && !m_folder.isEmpty())
        paths << m_folder;
    if (paths.isEmpty())
        return;
    const QDir base(m_folder);
    for (QString& path : paths)
        path = QDir::toNativeSeparators(relative ? base.relativeFilePath(path) : path);
    QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
}

void FolderPanel::reveal(const QString& path)
{
    if (path.isEmpty())
        return;
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#else
    // No portable way to select a file; its folder opens.
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? path : info.absolutePath()));
#endif
}

void FolderPanel::showContextMenu(QAbstractItemView* view, const QPoint& pos)
{
    const QModelIndex index = view->indexAt(pos);
    {
        // Right-clicking a file works on it, without showing it.
        const QScopedValueRollback guard(m_selecting, true);
        if (!index.isValid())
            view->clearSelection();
        else if (!view->selectionModel()->isSelected(index))
            view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                               QItemSelectionModel::Rows);
    }
    updateActions(); // also when the selection stayed as it was

    // The same menu for files and folders: what does not apply to the one clicked is greyed out.
    QMenu menu(this);
    menu.addActions({m_openAction, m_openFolderAction, m_compareAction});
    menu.addSeparator();
    menu.addActions({m_findHereAction, m_askAiAction});
    menu.addSeparator();
    menu.addActions({m_newFileAction, m_newFolderAction, m_renameAction, m_trashAction});
    menu.addSeparator();
    menu.addActions({m_copyPathAction, m_copyRelativePathAction, m_revealAction});
    menu.exec(view->viewport()->mapToGlobal(pos));
}

void FolderPanel::showNamesOnly(QTreeView* view)
{
    for (int column = SizeColumn; column < FolderColumnCount; ++column)
        view->setColumnHidden(column, true);
}

void FolderPanel::populateMenu()
{
    // The panel's own menu is about the folder shown, whatever is selected in it.
    m_actionFolder = m_folder;
    for (QAction* action : std::as_const(m_folderActions))
        action->setEnabled(!m_folder.isEmpty());
    m_askAiAction->setEnabled(!m_folder.isEmpty() && !m_summaryWatcher.isRunning());
    m_menu->clear();
    m_menu->addAction(tr("&Open Folder…"), this, &FolderPanel::showOpenDialog);
    populateRecent(m_menu->addMenu(tr("&Recent Folders")));
    QDir parent(m_folder);
    m_menu->addAction(tr("Go &Up"), this, &FolderPanel::goUp)->setEnabled(!m_folder.isEmpty() && parent.cdUp());
    m_menu->addAction(tr("&Close Folder"), this, &FolderPanel::closeFolder)->setEnabled(!m_folder.isEmpty());
    m_menu->addSeparator();
    m_menu->addAction(m_hiddenAction);
    m_menu->addSeparator();
    m_menu->addActions({m_newFileAction, m_newFolderAction});
    m_menu->addSeparator();
    m_menu->addActions({m_findHereAction, m_askAiAction});
    m_menu->addAction(m_revealAction->text(), this, [this] { reveal(m_folder); })->setEnabled(!m_folder.isEmpty());
}

void FolderPanel::populateRecent(QMenu* menu)
{
    const QStringList recent = recentFolders();
    for (const QString& folder : recent) {
        QString name = displayName(folder);
        name.replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction* action = menu->addAction(name, this, [this, folder] { openRecentFolder(folder); });
        action->setToolTip(QDir::toNativeSeparators(folder));
    }
    if (recent.isEmpty()) {
        menu->addAction(tr("No Recent Folders"))->setEnabled(false);
        return;
    }
    menu->addSeparator();
    menu->addAction(tr("&Clear List"), this, &FolderPanel::clearRecentFolders);
}

void FolderPanel::updateEmptyPage()
{
    m_recentList->clear();
    const QString home = QDir::homePath();
    for (const QString& folder : recentFolders()) {
        QString where = QDir(folder).absolutePath();
        if (where.startsWith(home + QLatin1Char('/')))
            where = QLatin1Char('~') + where.mid(home.size());
        const QString text = QStringLiteral("%1\n%2").arg(displayName(folder), QDir::toNativeSeparators(where));
        auto* item = new QListWidgetItem(text, m_recentList);
        item->setData(Qt::UserRole, folder);
        item->setToolTip(QDir::toNativeSeparators(folder));
    }
    m_recentList->setVisible(m_recentList->count() > 0);
    m_recentLabel->setVisible(m_recentList->count() > 0);
}

void FolderPanel::updateActions()
{
    // From the models, which know what is a folder without asking the disk again.
    QModelIndexList selected;
    if (QAbstractItemView* view = currentView(); view->selectionModel())
        for (const QModelIndex& index : view->selectionModel()->selectedIndexes())
            if (index.column() == NameColumn)
                selected << index;
    const auto folders =
        static_cast<qsizetype>(std::count_if(selected.cbegin(), selected.cend(), [this](const QModelIndex& index) {
            return index.model() == m_model && m_model->isDir(index);
        }));
    const qsizetype files = selected.size() - folders;
    const bool open = !m_folder.isEmpty();

    m_openAction->setEnabled(files > 0);
    m_openFolderAction->setEnabled(selected.size() == 1 && folders == 1);
    m_compareAction->setEnabled(selected.size() == 2 && files == 2);
    m_renameAction->setEnabled(selected.size() == 1);
    m_trashAction->setEnabled(!selected.isEmpty());
    m_copyPathAction->setEnabled(open);
    m_copyRelativePathAction->setEnabled(!selected.isEmpty());
    m_revealAction->setEnabled(open && selected.size() <= 1);
    // What concerns a folder works on the folder selected, or on the one shown when nothing is;
    // not on files.
    if (selected.isEmpty())
        m_actionFolder = m_folder;
    else if (selected.size() == 1 && folders == 1)
        m_actionFolder = pathAt(selected.first());
    else
        m_actionFolder.clear();
    for (QAction* action : std::as_const(m_folderActions))
        action->setEnabled(!m_actionFolder.isEmpty());
    m_askAiAction->setEnabled(!m_actionFolder.isEmpty() && !m_summaryWatcher.isRunning());
}

} // namespace aiedit::ui
