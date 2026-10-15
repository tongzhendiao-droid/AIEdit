#pragma once

#include "ui/BackupStore.h"
#include "ui/DocumentView.h"

#include <QHash>
#include <QMainWindow>
#include <QTimer>

#include <string_view>
#include <system_error>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QStackedWidget;
class QTabWidget;
class QToolButton;

namespace aiedit::core {
struct FileTypeInfo;
struct TextCodec;
}

namespace aiedit::ui {

class AiPanel;
class DataInspector;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Opens the file in a new tab, or switches to its tab if it is already open.
    // On failure the user is told why and false is returned.
    bool openFile(const QString& path);
    // Brings back the unsaved changes that windows which have ended left behind, and with
    // `reopenTabs` also the other files that were open when AIEdit last closed, as they were.
    void restoreSession(bool reopenTabs = true);
    // Backs up unsaved changes now rather than shortly, and waits until they are written.
    void flushBackups();

    int documentCount() const;
    DocumentView* document(int index) const;
    DocumentView* currentDocument() const;
    DataInspector* dataInspector() const { return m_inspector; }

    // The legacy encoding to try first for non-UTF-8 text, from the user's locale.
    static std::string_view preferredLegacyCodec();

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createWidgets();
    void createActions();
    void createMenus();
    void createCodecMenu();
    void populateSyntaxMenu();
    void readSettings();
    void writeSettings() const;

    void showOpenDialog();
    // A new, empty document in a new tab; it gets a file when it is first saved.
    void newDocument();
    void addDocument(DocumentView* doc);
    void closeTab(int index);
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
    // Recent files: what is remembered of each, most recent first.
    void rememberDocument(const DocumentView* doc);
    void moveToFrontOfRecent(const QString& path);
    void saveRecentFiles() const;
    void populateRecentMenu();
    void currentTabChanged();
    void goTo();
    void setViewMode(DocumentView::ViewMode mode);
    void setCodec(const core::TextCodec& codec);
    // Writes the current document in another codec, asking first if anything would be lost.
    void saveWithCodec(const core::TextCodec& codec);
    void convertLineEndings(const QString& terminator);
    void setBytesPerRow(int bytesPerRow);
    void setWordWrap(bool on);
    void zoomBy(int steps);
    void resetZoom();
    void applyViewSettings(DocumentView* doc) const;
    void showAbout();
    void showAiSettings();
    void runAiAction(void (AiPanel::*action)());

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
    QTabWidget* m_tabs = nullptr;
    QDockWidget* m_inspectorDock = nullptr;
    DataInspector* m_inspector = nullptr;
    QDockWidget* m_aiDock = nullptr;
    AiPanel* m_aiPanel = nullptr;

    QLabel* m_typeLabel = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QToolButton* m_codecButton = nullptr;
    QToolButton* m_syntaxButton = nullptr;
    QToolButton* m_lineEndingButton = nullptr;
    QLabel* m_linesLabel = nullptr;
    QLabel* m_positionLabel = nullptr;
    QLabel* m_selectionLabel = nullptr;

    QAction* m_newAction = nullptr;
    QAction* m_openAction = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_cutAction = nullptr;
    QAction* m_pasteAction = nullptr;
    QAction* m_closeAction = nullptr;
    QAction* m_reloadAction = nullptr;
    QAction* m_followAction = nullptr;
    QAction* m_wordWrapAction = nullptr;
    QAction* m_quitAction = nullptr;
    QAction* m_copyAction = nullptr;
    QAction* m_copyHexAction = nullptr;
    QAction* m_copyTextAction = nullptr;
    QAction* m_copyCArrayAction = nullptr;
    QAction* m_copyOffsetAction = nullptr;
    QAction* m_selectAllAction = nullptr;
    QAction* m_goToAction = nullptr;
    QAction* m_findAction = nullptr;
    QAction* m_findNextAction = nullptr;
    QAction* m_findPreviousAction = nullptr;
    QAction* m_replaceAction = nullptr;
    QAction* m_filterAction = nullptr;
    QList<QAction*> m_lineActions; // sorting and cleaning, then moving and copying
    QAction* m_nextTabAction = nullptr;
    QAction* m_previousTabAction = nullptr;
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
    QAction* m_aiAskAction = nullptr;
    QAction* m_aiSettingsAction = nullptr;
    QAction* m_aboutAction = nullptr;
    QAction* m_aboutQtAction = nullptr;

    int m_bytesPerRow = 16;
    int m_untitledCount = 0;
    QList<DocumentView::State> m_recentFiles;
    QMenu* m_recentMenu = nullptr;
    bool m_wordWrap = true;
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
