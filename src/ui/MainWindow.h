#pragma once

#include "ui/BackupStore.h"
#include "ui/Compare.h"
#include "ui/DocumentView.h"
#include "ui/QuickOpen.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <string_view>
#include <system_error>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QStackedWidget;
class QToolButton;

namespace aiedit::core {
struct FileTypeInfo;
struct TextCodec;
}

namespace aiedit::ui {

class AiPanel;
class CompareTab;
class ContentsPanel;
class DataInspector;
class FindInFilesPanel;
class FolderPanel;
class TabWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Opens the file in a new tab, or switches to its tab if it is already open (and keeps it,
    // if it was the preview tab). On failure the user is told why and false is returned.
    bool openFile(const QString& path);
    // Shows the file in the preview tab: the next file previewed takes its place, unless it is
    // kept first (edited, its tab double-clicked, or opened). Failures go to the status bar.
    bool previewFile(const QString& path);
    // Shows the folder in the folder panel.
    bool openFolder(const QString& path);
    // A file or a folder, from the command line, another launch or a drop.
    bool openPath(const QString& path);

    // Compares two versions in a tab of their own; nullptr (after saying why) if a side cannot
    // be had.
    CompareTab* compare(CompareSide left, CompareSide right);
    CompareTab* compareFiles(const QString& left, const QString& right);
    // A document as it is being edited (the current one by default) against its file on disk;
    // the current document against the clipboard.
    CompareTab* compareWithSaved(DocumentView* doc = nullptr);
    CompareTab* compareWithClipboard();
    CompareTab* currentCompareTab() const;
    // Brings back the unsaved changes that windows which have ended left behind, and with
    // `reopenTabs` also the other files that were open when AIEdit last closed, as they were.
    void restoreSession(bool reopenTabs = true);
    // Backs up unsaved changes now rather than shortly, and waits until they are written.
    void flushBackups();
    // A window of its own (aiedit --new-window) leaves the session to the first one: what it has
    // open does not come back next time, though its unsaved changes do.
    void setSecondary(bool secondary) { m_secondary = secondary; }

    int documentCount() const;
    DocumentView* document(int index) const;
    DocumentView* currentDocument() const;
    // The tab the folder panel previews files in; nullptr when there is none.
    DocumentView* previewDocument() const { return m_previewDoc; }
    DataInspector* dataInspector() const { return m_inspector; }
    // What right-clicking a tab offers, for that tab (or the empty part of the bar, for -1). The
    // caller owns the menu.
    QMenu* tabMenu(int index);
    QuickOpen* quickOpen() const { return m_quickOpen; }

    // The legacy encoding to try first for non-UTF-8 text, from the user's locale.
    static std::string_view preferredLegacyCodec();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createWidgets();
    void createActions();
    void createMenus();
    void createCodecMenu();
    void populateSyntaxMenu();
    // Colour schemes and the editor font (View menu).
    void populateSchemeMenu();
    void chooseColorScheme(const QString& name);
    void chooseEditorFont();
    void readSettings();
    void writeSettings() const;

    void showOpenDialog();
    // A new, empty document in a new tab; it gets a file when it is first saved.
    void newDocument();
    // Opens a file, or finds the tab it is open in; with `preview`, in the preview tab.
    DocumentView* showFile(const QString& path, bool preview, QString* error);
    // Puts the document in a tab at `index` (-1: last) and makes it the current one.
    void addDocument(DocumentView* doc, int index = -1, bool focus = true);
    // False if it was kept: its unsaved changes were to be saved, and that was cancelled.
    bool closeTab(int index);
    // Closes the tabs `pick` chooses, but not pinned ones; false if one of them was kept.
    bool closeTabs(const std::function<bool(int index)>& pick);
    void reopenClosedTab();
    void saveAll();
    void reloadDocument(DocumentView* doc);
    void showTabMenu(const QPoint& pos);
    // Selects the file in the folder panel, opening its folder there if need be.
    void showInFolderPanel(const QString& path);
    // The panels on the left (the folder, the contents): hidden together, and shown again.
    void setSideBarVisible(bool visible);
    void updateSideBarAction();
    // Removes a tab without asking anything, as when its file went to the trash.
    void discardDocument(DocumentView* doc);
    // The preview tab becomes an ordinary one.
    void keepDocument(DocumentView* doc);
    // Saving; false if it failed (after saying why) or was cancelled.
    bool saveDocument(DocumentView* doc);
    bool saveDocumentAs(DocumentView* doc);
    // Asks what to do with unsaved edits before a document goes away; false to keep it.
    bool confirmClose(DocumentView* doc);
    // Unsaved changes are backed up shortly after they are made (see BackupStore).
    void noteBackupChange(DocumentView* doc);
    void writeBackups();
    void dropBackup(DocumentView* doc);
    QJsonObject backupMeta(const DocumentView* doc, int order) const;
    // A document with the changes of a backup, opened from its file or as a new document.
    DocumentView* restoreBackup(const BackupStore::Entry& entry, QStringList* changedOnDisk, QStringList* gone);
    void updateDocumentTitle(DocumentView* doc);
    // Recent files: what is remembered of each, most recent first. A preview only updates what
    // is remembered of a file already listed, without moving it to the front.
    void rememberDocument(const DocumentView* doc, bool toFront = true);
    void moveToFrontOfRecent(const QString& path);
    void saveRecentFiles() const;
    void populateRecentMenu();
    void currentTabChanged();
    void goTo();
    // A line (+/- for relative), or an offset such as 0x1F, in the current document.
    void goToPosition(const QString& input);
    // Ctrl+P, and with ">" Ctrl+Shift+P: what it offers.
    QList<QuickOpen::Item> quickOpenFiles() const;
    QList<QuickOpen::Item> quickOpenCommands();
    // Back and forward, through the places jumped from: where the caret was before it moved
    // more than a few lines at once, or before another document was shown.
    struct Place
    {
        QPointer<DocumentView> doc;
        QString path; // to open it again, once closed
        std::uint64_t offset = 0;
        std::uint64_t line = 0;
    };
    Place placeOf(DocumentView* doc) const;
    void notePlace();
    void goBack();
    void goForward();
    bool goToPlace(const Place& place);
    void setViewMode(DocumentView::ViewMode mode);
    void setCodec(const core::TextCodec& codec);
    // Writes the current document in another codec, asking first if anything would be lost.
    void saveWithCodec(const core::TextCodec& codec);
    void convertLineEndings(const QString& terminator);
    void setBytesPerRow(int bytesPerRow);
    void setWordWrap(bool on);
    void setLineNumbersVisible(bool on);
    // Shows the contents or not, as the document in front wants (see DocumentView::showsContents).
    void placeContents();
    // The page shown with no tabs: buttons to start with, or with a folder open just a hint.
    void updateWelcome();
    void zoomBy(int steps);
    void resetZoom();
    void applyViewSettings(DocumentView* doc) const;
    void showAbout();
    void showAiSettings();
    void runAiAction(void (AiPanel::*action)());
    // What the AI proposes for a document, in a tab of its own to review it before it is written.
    void reviewAiEdit(DocumentView* doc, const DocumentView::AiEditProposal& proposal);
    // Find in Files, in the current document's folder at first.
    void showFindInFiles();
    // Reading mode for the current document, with its contents beside it.
    void setReadingMode(bool on);
    QFont readingFont() const;
    // A chapter for the AI: summarized, translated or explained.
    void askAboutChapter(int action, const QString& title, const QString& text, bool cut);
    // Opens a result of Find in Files at its line.
    void openAtLine(const QString& path, std::uint64_t line, std::uint64_t offset);
    // Opens a file in the hex view at a byte.
    void openAtOffset(const QString& path, std::uint64_t offset);
    void showFindInFilesIn(const QString& folder);
    // What the folder panel did to files on disk, for the documents showing them.
    void documentsMoved(const QString& from, const QString& to);
    void documentsTrashed(const QString& path);
    // The AI is asked what a folder holds, from a summary made here.
    void askAboutFolder(const QString& folder, const QString& summary);
    void showOpenFolderDialog();
    void populateRecentFoldersMenu();
    // A document against a file picked; two files picked.
    void compareWithFile(DocumentView* doc);
    void compareTwoFiles();
    // A document as a side of a comparison: its text as it is now, edits included.
    CompareSide documentSide(DocumentView* doc) const;
    void applyViewSettingsToTabs();
    void applyCompareSettings(CompareTab* tab) const;

    void documentStateChanged(DocumentView* doc);
    // What the Edit menu can do now: copying needs a selection, pasting text on the clipboard.
    void updateEditActions(DocumentView* doc);
    void updateFileStatus();
    void updateCursorStatus();
    void updateTextStatus();
    void updateActions();
    QString describeFileType(const core::FileTypeInfo& type) const;
    QString describeError(const std::error_code& ec) const;

    QStackedWidget* m_stack = nullptr;
    QWidget* m_welcome = nullptr;
    QLabel* m_welcomeHint = nullptr;
    QWidget* m_welcomeButtons = nullptr;
    TabWidget* m_tabs = nullptr;
    QDockWidget* m_inspectorDock = nullptr;
    DataInspector* m_inspector = nullptr;
    QDockWidget* m_aiDock = nullptr;
    AiPanel* m_aiPanel = nullptr;
    QDockWidget* m_findInFilesDock = nullptr;
    FindInFilesPanel* m_findInFiles = nullptr;
    QDockWidget* m_contentsDock = nullptr;
    ContentsPanel* m_contents = nullptr;
    QDockWidget* m_folderDock = nullptr;
    FolderPanel* m_folders = nullptr;
    QuickOpen* m_quickOpen = nullptr;
    QPointer<DocumentView> m_previewDoc;
    QList<QPointer<DocumentView>> m_recentDocs; // most recently shown first
    QList<Place> m_back;
    QList<Place> m_forward;
    Place m_here;              // where the caret is, as noted last
    bool m_navigating = false; // going back or forward: no place is noted

    QLabel* m_typeLabel = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QToolButton* m_codecButton = nullptr;
    QToolButton* m_syntaxButton = nullptr;
    QToolButton* m_lineEndingButton = nullptr;
    QLabel* m_linesLabel = nullptr;
    QToolButton* m_positionLabel = nullptr; // clicking it goes to a line
    QLabel* m_selectionLabel = nullptr;
    QLabel* m_compareStatus = nullptr; // what the comparison in the current tab found

    QAction* m_newAction = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_openFolderAction = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_cutAction = nullptr;
    QAction* m_pasteAction = nullptr;
    QAction* m_closeAction = nullptr;
    QAction* m_closeAllAction = nullptr;
    QAction* m_reopenTabAction = nullptr;
    QAction* m_saveAllAction = nullptr;
    QAction* m_reloadAction = nullptr;
    QAction* m_followAction = nullptr;
    QAction* m_wordWrapAction = nullptr;
    QAction* m_lineNumbersAction = nullptr;
    QAction* m_readingAction = nullptr;
    QAction* m_quitAction = nullptr;
    QAction* m_newWindowAction = nullptr;
    QAction* m_fullScreenAction = nullptr;
    QAction* m_sideBarAction = nullptr;
    QList<QPointer<QDockWidget>> m_hiddenSidePanels; // by the side bar's toggle, to show again
    QAction* m_copyAction = nullptr;
    QAction* m_copyHexAction = nullptr;
    QAction* m_copyTextAction = nullptr;
    QAction* m_copyCArrayAction = nullptr;
    QAction* m_copyOffsetAction = nullptr;
    QAction* m_selectAllAction = nullptr;
    QAction* m_goToAction = nullptr;
    QAction* m_quickOpenAction = nullptr;
    QAction* m_commandPaletteAction = nullptr;
    QAction* m_backAction = nullptr;
    QAction* m_forwardAction = nullptr;
    QList<QAction*> m_bookmarkActions; // toggle, next, previous, clear
    QAction* m_findSelectionAction = nullptr;
    QAction* m_findSelectionBackAction = nullptr;
    QAction* m_findAction = nullptr;
    QAction* m_findNextAction = nullptr;
    QAction* m_findPreviousAction = nullptr;
    QAction* m_replaceAction = nullptr;
    QAction* m_filterAction = nullptr;
    QAction* m_findInFilesAction = nullptr;
    QList<QAction*> m_lineActions; // sorting and cleaning, then moving, copying and the rest
    QAction* m_selectLineAction = nullptr;
    QAction* m_commentAction = nullptr;
    QList<QAction*> m_caseActions; // upper, lower
    QAction* m_nextTabAction = nullptr;
    QAction* m_previousTabAction = nullptr;
    QAction* m_moveTabLeftAction = nullptr;
    QAction* m_moveTabRightAction = nullptr;
    QAction* m_pinTabAction = nullptr;
    QActionGroup* m_viewModeGroup = nullptr;
    QAction* m_textModeAction = nullptr;
    QAction* m_hexModeAction = nullptr;
    QAction* m_splitModeAction = nullptr;
    QActionGroup* m_codecGroup = nullptr;
    QAction* m_zoomInAction = nullptr;
    QAction* m_zoomOutAction = nullptr;
    QAction* m_resetZoomAction = nullptr;
    QActionGroup* m_bytesPerRowGroup = nullptr;
    QAction* m_aiExplainAction = nullptr;
    QAction* m_aiTranslateAction = nullptr;
    QAction* m_aiSummarizeAction = nullptr;
    QAction* m_aiIdentifyAction = nullptr;
    QAction* m_aiFolderAction = nullptr;
    QAction* m_compareSavedAction = nullptr;
    QAction* m_compareClipboardAction = nullptr;
    QAction* m_compareFileAction = nullptr;
    QAction* m_compareTwoAction = nullptr;
    QAction* m_aiChapterSummaryAction = nullptr;
    QAction* m_aiChapterTranslationAction = nullptr;
    QAction* m_aiAskAction = nullptr;
    QAction* m_aiEditAction = nullptr;
    QAction* m_aiSettingsAction = nullptr;
    QAction* m_aboutAction = nullptr;
    QAction* m_aboutQtAction = nullptr;

    bool m_secondary = false;
    int m_bytesPerRow = 16;
    int m_untitledCount = 0;
    QList<DocumentView::State> m_recentFiles;
    // Closed tabs to reopen, the last closed last.
    struct ClosedTab
    {
        DocumentView::State state;
        int index = 0;
        bool pinned = false;
    };
    QList<ClosedTab> m_closedTabs;
    QMenu* m_recentMenu = nullptr;
    QMenu* m_recentFoldersMenu = nullptr;
    QMenu* m_schemeMenu = nullptr;
    QActionGroup* m_schemeGroup = nullptr;
    QAction* m_fontAction = nullptr;
    QString m_fontFamily; // empty: the system's fixed-width font
    bool m_wordWrap = true;
    bool m_lineNumbers = true;
    bool m_placingContents = false; // the contents shown or hidden for the document in front
    int m_defaultPointSize = 10;
    int m_pointSize = 10;
    QString m_lastDirectory;
    QString m_lastGoTo;

    struct BackupInfo
    {
        QString id;
        bool dirty = false;   // changed since the last backup
        bool written = false; // a backup is on disk
        // The file when the first unsaved change was made, to tell later changes on disk.
        qint64 baseSize = -1;
        qint64 baseModified = 0;
    };
    BackupStore m_backups;
    QTimer m_backupTimer;
    QHash<const DocumentView*, BackupInfo> m_backupInfo;
};

} // namespace aiedit::ui
