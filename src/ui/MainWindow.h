#pragma once

#include <QMainWindow>

#include <system_error>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QStackedWidget;
class QTabWidget;

namespace cppedit::core {
struct FileTypeInfo;
}

namespace cppedit::ui {

class DataInspector;
class DocumentView;
class HexView;

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

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createWidgets();
    void createActions();
    void createMenus();
    void readSettings();
    void writeSettings() const;

    void showOpenDialog();
    void closeTab(int index);
    void currentTabChanged();
    void goToOffset();
    void setBytesPerRow(int bytesPerRow);
    void zoomBy(int steps);
    void resetZoom();
    void applyViewSettings(HexView* view) const;
    void showAbout();

    void updateFileStatus();
    void updateCursorStatus();
    void updateActions();
    HexView* currentHexView() const;
    QString describeFileType(const core::FileTypeInfo& type) const;
    QString describeError(const std::error_code& ec) const;

    QStackedWidget* m_stack = nullptr;
    QWidget* m_welcome = nullptr;
    QTabWidget* m_tabs = nullptr;
    QDockWidget* m_inspectorDock = nullptr;
    DataInspector* m_inspector = nullptr;

    QLabel* m_typeLabel = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    QLabel* m_selectionLabel = nullptr;

    QAction* m_openAction = nullptr;
    QAction* m_closeAction = nullptr;
    QAction* m_quitAction = nullptr;
    QAction* m_copyAction = nullptr;
    QAction* m_copyHexAction = nullptr;
    QAction* m_copyTextAction = nullptr;
    QAction* m_copyCArrayAction = nullptr;
    QAction* m_copyOffsetAction = nullptr;
    QAction* m_selectAllAction = nullptr;
    QAction* m_goToAction = nullptr;
    QAction* m_nextTabAction = nullptr;
    QAction* m_previousTabAction = nullptr;
    QAction* m_zoomInAction = nullptr;
    QAction* m_zoomOutAction = nullptr;
    QAction* m_resetZoomAction = nullptr;
    QActionGroup* m_bytesPerRowGroup = nullptr;
    QAction* m_aboutAction = nullptr;
    QAction* m_aboutQtAction = nullptr;

    int m_bytesPerRow = 16;
    int m_defaultPointSize = 10;
    int m_pointSize = 10;
    QString m_lastDirectory;
    QString m_lastGoTo;
};

} // namespace cppedit::ui
