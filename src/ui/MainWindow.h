#pragma once

#include "ui/DocumentView.h"

#include <QMainWindow>

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
    void closeTab(int index);
    void currentTabChanged();
    void goTo();
    void setViewMode(DocumentView::ViewMode mode);
    void setCodec(const core::TextCodec& codec);
    void setBytesPerRow(int bytesPerRow);
    void setWordWrap(bool on);
    void zoomBy(int steps);
    void resetZoom();
    void applyViewSettings(DocumentView* doc) const;
    void showAbout();
    void showAiSettings();
    void runAiAction(void (AiPanel::*action)());

    void documentStateChanged(DocumentView* doc);
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
    QLabel* m_lineEndingLabel = nullptr;
    QLabel* m_linesLabel = nullptr;
    QLabel* m_positionLabel = nullptr;
    QLabel* m_selectionLabel = nullptr;

    QAction* m_openAction = nullptr;
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
    bool m_wordWrap = true;
    int m_defaultPointSize = 10;
    int m_pointSize = 10;
    QString m_lastDirectory;
    QString m_lastGoTo;
};

} // namespace aiedit::ui
