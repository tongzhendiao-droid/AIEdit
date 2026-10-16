#pragma once

#include "ui/FolderModels.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <string_view>

class QAbstractItemView;
class QAction;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QStackedWidget;
class QToolButton;
class QTreeView;

namespace aiedit::ui {

// A folder beside the documents, as a tree. Clicking a file shows it in the preview tab, which
// the next file clicked takes over; double-clicking (or Enter) opens it for good. Typing in the
// filter lists the files of the whole folder whose names match. The type of each file is told
// from its content, and a name that says otherwise is marked. Files and folders can be made,
// renamed and moved to the trash, and the AI can be asked what the folder holds.
//
// Not a project: nothing is stored in the folder, and nothing is built or run.
class FolderPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FolderPanel(QWidget* parent = nullptr);
    ~FolderPanel() override;

    QSize sizeHint() const override;

    QString folder() const { return m_folder; }
    // Shows a folder, and puts it first among the recent ones. False if it is not one.
    bool setFolder(const QString& folder);
    void closeFolder();
    // Most recent first.
    QStringList recentFolders() const;
    void clearRecentFolders();
    // Takes a folder off the recent ones, such as one that is gone.
    void forgetFolder(const QString& folder);

    // The legacy encoding to try first for text that is not UTF-8, as when opening files.
    void setLegacyCodec(std::string_view codec);
    // Names a document with unsaved changes at or under a path, which keeps it from being
    // renamed or moved to the trash; empty if there is none.
    void setUnsavedCheck(std::function<QString(const QString& path)> check) { m_unsavedCheck = std::move(check); }

    void setFilter(const QString& text);
    QString filter() const;
    // Whether the filter's list is shown rather than the tree.
    bool showsList() const;
    // From starting to list the files that pass the filter until they all are.
    bool isListing() const { return m_listInbox != nullptr; }
    bool showsHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    QTreeView* tree() const { return m_tree; }
    QTreeView* list() const { return m_list; }
    FolderModel* model() const { return m_model; }
    FileListModel* listModel() const { return m_listModel; }
    QString statusText() const;
    // Selects a file or folder in the tree, opening the folders above it.
    void select(const QString& path);
    // What the actions work on: the selection in the view shown.
    QStringList selectedPaths() const;

    // File operations. Each returns what it made, or false/empty with the reason in `error`.
    QString createFile(const QString& inFolder, const QString& name, QString* error);
    QString createFolder(const QString& inFolder, const QString& name, QString* error);
    bool rename(const QString& path, const QString& newName, QString* error);
    bool moveToTrash(const QString& path, QString* error, QString* pathInTrash = nullptr);

    // Looks through a folder on a worker thread, then hands what it found to folderSummarized().
    void summarize(const QString& folder);
    bool isSummarizing() const { return m_summaryWatcher.isRunning(); }

public slots:
    void showOpenDialog();
    void goUp();
    // In a folder, after asking for a name.
    void newFile(const QString& folder);
    void newFolder(const QString& folder);
    // On what is selected, after asking for a name or a confirmation.
    void renameSelected();
    void trashSelected();
    // Any file or folder, after asking for its new name.
    void renamePath(const QString& path);

signals:
    // Show the file in the preview tab; open it for good.
    void previewRequested(const QString& path);
    void openRequested(const QString& path);
    void folderChanged(const QString& folder);
    void findInFolderRequested(const QString& folder);
    void folderSummarized(const QString& folder, const QString& summary);
    void pathRenamed(const QString& from, const QString& to);
    void pathTrashed(const QString& path);
    // Two files selected, to compare (in name order: left, right).
    void compareRequested(const QString& left, const QString& right);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct ListInbox;

    void createActions();
    QWidget* createEmptyPage();
    void ensureModel();
    void applyFilter();
    void stopListing();
    void takeListed();
    void listingFinished();
    void currentChanged(QAbstractItemView* view);
    void activate(const QModelIndex& index);
    void showContextMenu(QAbstractItemView* view, const QPoint& pos);
    void populateMenu();
    void populateRecent(QMenu* menu);
    void updateEmptyPage();
    void updateActions();
    // The views show names; the other columns are in the tooltips.
    void showNamesOnly(QTreeView* view);
    void updateStatus();
    void rememberFolder(const QString& folder);
    void openRecentFolder(const QString& folder);
    void copyPaths(bool relative);
    QString pathAt(const QModelIndex& index) const;
    QAbstractItemView* currentView() const;
    QString shown(const QString& path) const; // within the folder, for messages

    QString m_folder;
    FileTypeSniffer m_sniffer;
    FolderModel* m_model = nullptr; // created with the first folder
    FileListModel* m_listModel = nullptr;
    std::function<QString(const QString& path)> m_unsavedCheck;
    bool m_showHidden = false;
    bool m_selecting = false; // a selection made here, not by the user
    QTimer m_previewTimer; // previews a moment after the current file changes
    QTimer m_filterTimer;  // lists the files a moment after the filter changes
    QString m_pendingSelection;
    QFutureWatcher<FileListSummary> m_listWatcher;
    std::shared_ptr<ListInbox> m_listInbox;
    QString m_listStatus;
    QFutureWatcher<QString> m_summaryWatcher;
    QString m_summaryFolder;
    std::string_view m_legacyCodec = "windows-1252";

    QStackedWidget* m_pages = nullptr;
    QWidget* m_emptyPage = nullptr;
    QLabel* m_recentLabel = nullptr;
    QListWidget* m_recentList = nullptr;
    QWidget* m_folderPage = nullptr;
    QLabel* m_title = nullptr;
    QToolButton* m_menuButton = nullptr;
    QMenu* m_menu = nullptr;
    QLineEdit* m_filter = nullptr;
    QStackedWidget* m_views = nullptr;
    QTreeView* m_tree = nullptr;
    QTreeView* m_list = nullptr;
    QLabel* m_status = nullptr;

    QAction* m_openAction = nullptr;
    QAction* m_openFolderAction = nullptr;
    QAction* m_compareAction = nullptr;
    QAction* m_findHereAction = nullptr;
    QAction* m_askAiAction = nullptr;
    QAction* m_newFileAction = nullptr;
    QAction* m_newFolderAction = nullptr;
    QAction* m_renameAction = nullptr;
    QAction* m_trashAction = nullptr;
    QAction* m_copyPathAction = nullptr;
    QAction* m_copyRelativePathAction = nullptr;
    QAction* m_revealAction = nullptr;
    QAction* m_hiddenAction = nullptr;
    // Those that work on a folder (finding in it, asking about it, making files in it), and the
    // folder they work on: the one selected or right-clicked, the one shown, or none for a file.
    QList<QAction*> m_folderActions;
    QString m_actionFolder;
};

} // namespace aiedit::ui
