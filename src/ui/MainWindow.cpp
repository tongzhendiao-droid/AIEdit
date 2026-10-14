#include "ui/MainWindow.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/Offset.h"
#include "ui/DataInspector.h"
#include "ui/DocumentView.h"
#include "ui/HexView.h"
#include "ui/Paths.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>

#include <algorithm>

namespace cppedit::ui {

namespace {

constexpr int kMinPointSize = 6;
constexpr int kMaxPointSize = 48;
constexpr int kStatusTimeoutMs = 4000;
constexpr int kBytesPerRowChoices[] = {8, 16, 32};

QString hexOffset(std::uint64_t offset)
{
    return QStringLiteral("0x") + QString::number(offset, 16).toUpper();
}

QString encodingName(core::Encoding encoding)
{
    switch (encoding) {
    case core::Encoding::Ascii:
        return QStringLiteral("ASCII");
    case core::Encoding::Utf8:
        return QStringLiteral("UTF-8");
    case core::Encoding::Utf16LE:
        return QStringLiteral("UTF-16 LE");
    case core::Encoding::Utf16BE:
        return QStringLiteral("UTF-16 BE");
    case core::Encoding::Utf32LE:
        return QStringLiteral("UTF-32 LE");
    case core::Encoding::Utf32BE:
        return QStringLiteral("UTF-32 BE");
    case core::Encoding::Legacy:
        return MainWindow::tr("non-UTF-8 (GBK, Latin-1, …)");
    case core::Encoding::Unknown:
        break;
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_defaultPointSize = fixed.pointSize() > 0 ? fixed.pointSize() : 10;
    m_pointSize = m_defaultPointSize;

    setAcceptDrops(true);
    createWidgets();
    createActions();
    createMenus();
    readSettings();
    currentTabChanged();
}

MainWindow::~MainWindow() = default;

void MainWindow::createWidgets()
{
    auto* welcome = new QLabel(tr("Drop a file here, or press %1 to open one.")
                                   .arg(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText)));
    welcome->setAlignment(Qt::AlignCenter);
    welcome->setForegroundRole(QPalette::PlaceholderText);
    QFont welcomeFont = welcome->font();
    welcomeFont.setPointSizeF(welcomeFont.pointSizeF() * 1.3);
    welcome->setFont(welcomeFont);
    m_welcome = welcome;

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->tabBar()->setElideMode(Qt::ElideMiddle);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::currentTabChanged);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_welcome);
    m_stack->addWidget(m_tabs);
    setCentralWidget(m_stack);

    m_inspector = new DataInspector;
    m_inspectorDock = new QDockWidget(tr("Data Inspector"), this);
    m_inspectorDock->setObjectName(QStringLiteral("dataInspectorDock"));
    m_inspectorDock->setWidget(m_inspector);
    addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);

    m_typeLabel = new QLabel;
    m_sizeLabel = new QLabel;
    m_cursorLabel = new QLabel;
    m_selectionLabel = new QLabel;
    statusBar()->addPermanentWidget(m_selectionLabel);
    statusBar()->addPermanentWidget(m_cursorLabel);
    statusBar()->addPermanentWidget(m_sizeLabel);
    statusBar()->addPermanentWidget(m_typeLabel);
}

void MainWindow::createActions()
{
    m_openAction = new QAction(tr("&Open…"), this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::showOpenDialog);

    m_closeAction = new QAction(tr("&Close Tab"), this);
    m_closeAction->setShortcut(QKeySequence::Close);
    connect(m_closeAction, &QAction::triggered, this, [this] { closeTab(m_tabs->currentIndex()); });

    m_quitAction = new QAction(tr("&Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setMenuRole(QAction::QuitRole);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    // Edit actions only fire while the documents have focus, so that other widgets (the data
    // inspector, dialogs) keep their own Ctrl+C and Ctrl+A.
    auto documentAction = [this](const QString& text, auto slot, const QKeySequence& shortcut = {}) {
        auto* action = new QAction(text, this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_tabs->addAction(action);
        connect(action, &QAction::triggered, this, [this, slot] {
            if (HexView* view = currentHexView())
                (view->*slot)();
        });
        return action;
    };
    m_copyAction = documentAction(tr("&Copy"), &HexView::copy, QKeySequence::Copy);
    m_copyHexAction = documentAction(tr("Copy as &Hex"), &HexView::copyAsHex);
    m_copyTextAction = documentAction(tr("Copy as &Text"), &HexView::copyAsText);
    m_copyCArrayAction = documentAction(tr("Copy as C A&rray"), &HexView::copyAsCArray);
    m_copyOffsetAction = documentAction(tr("Copy &Offset"), &HexView::copyOffset);
    m_selectAllAction = documentAction(tr("Select &All"), &HexView::selectAll, QKeySequence::SelectAll);

    m_goToAction = new QAction(tr("&Go to Offset…"), this);
    m_goToAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(m_goToAction, &QAction::triggered, this, &MainWindow::goToOffset);

    m_nextTabAction = new QAction(tr("&Next Tab"), this);
    m_nextTabAction->setShortcut(QKeySequence::NextChild);
    connect(m_nextTabAction, &QAction::triggered, this, [this] {
        if (m_tabs->count() > 1)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    });
    m_previousTabAction = new QAction(tr("&Previous Tab"), this);
    m_previousTabAction->setShortcut(QKeySequence::PreviousChild);
    connect(m_previousTabAction, &QAction::triggered, this, [this] {
        if (m_tabs->count() > 1)
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    });

    m_zoomInAction = new QAction(tr("Zoom &In"), this);
    m_zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    connect(m_zoomInAction, &QAction::triggered, this, [this] { zoomBy(1); });
    m_zoomOutAction = new QAction(tr("Zoom &Out"), this);
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(m_zoomOutAction, &QAction::triggered, this, [this] { zoomBy(-1); });
    m_resetZoomAction = new QAction(tr("&Reset Zoom"), this);
    m_resetZoomAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(m_resetZoomAction, &QAction::triggered, this, &MainWindow::resetZoom);

    m_bytesPerRowGroup = new QActionGroup(this);
    for (const int bytes : kBytesPerRowChoices) {
        auto* action = m_bytesPerRowGroup->addAction(tr("%1 Bytes per Row").arg(bytes));
        action->setCheckable(true);
        action->setData(bytes);
    }
    connect(m_bytesPerRowGroup, &QActionGroup::triggered, this,
            [this](QAction* action) { setBytesPerRow(action->data().toInt()); });

    m_aboutAction = new QAction(tr("&About cppEdit"), this);
    m_aboutAction->setMenuRole(QAction::AboutRole);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
    m_aboutQtAction = new QAction(tr("About &Qt"), this);
    m_aboutQtAction->setMenuRole(QAction::AboutQtRole);
    connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::createMenus()
{
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_openAction);
    file->addAction(m_closeAction);
    file->addSeparator();
    file->addAction(m_quitAction);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_copyAction);
    edit->addAction(m_copyHexAction);
    edit->addAction(m_copyTextAction);
    edit->addAction(m_copyCArrayAction);
    edit->addAction(m_copyOffsetAction);
    edit->addSeparator();
    edit->addAction(m_selectAllAction);
    edit->addSeparator();
    edit->addAction(m_goToAction);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addActions(m_bytesPerRowGroup->actions());
    view->addSeparator();
    view->addAction(m_zoomInAction);
    view->addAction(m_zoomOutAction);
    view->addAction(m_resetZoomAction);
    view->addSeparator();
    view->addAction(m_inspectorDock->toggleViewAction());
    view->addSeparator();
    view->addAction(m_nextTabAction);
    view->addAction(m_previousTabAction);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_aboutAction);
    help->addAction(m_aboutQtAction);
}

void MainWindow::readSettings()
{
    QSettings settings;
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1100, 700);
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
    m_lastDirectory = settings.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString();
    m_pointSize = std::clamp(settings.value(QStringLiteral("view/pointSize"), m_defaultPointSize).toInt(),
                             kMinPointSize, kMaxPointSize);
    m_inspector->setBigEndian(settings.value(QStringLiteral("inspector/bigEndian"), false).toBool());
    setBytesPerRow(settings.value(QStringLiteral("view/bytesPerRow"), 16).toInt());
}

void MainWindow::writeSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("files/lastDirectory"), m_lastDirectory);
    settings.setValue(QStringLiteral("view/pointSize"), m_pointSize);
    settings.setValue(QStringLiteral("view/bytesPerRow"), m_bytesPerRow);
    settings.setValue(QStringLiteral("inspector/bigEndian"), m_inspector->isBigEndian());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    writeSettings();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            openFile(url.toLocalFile());
    }
    event->acceptProposedAction();
}

// Documents ------------------------------------------------------------------------------------------

int MainWindow::documentCount() const
{
    return m_tabs->count();
}

DocumentView* MainWindow::document(int index) const
{
    return qobject_cast<DocumentView*>(m_tabs->widget(index));
}

DocumentView* MainWindow::currentDocument() const
{
    return qobject_cast<DocumentView*>(m_tabs->currentWidget());
}

HexView* MainWindow::currentHexView() const
{
    const DocumentView* doc = currentDocument();
    return doc ? doc->hexView() : nullptr;
}

bool MainWindow::openFile(const QString& path)
{
    const QFileInfo info(path);
    const QString filePath = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (document(i)->filePath() == filePath) {
            m_tabs->setCurrentIndex(i);
            return true;
        }
    }

    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(filePath), ec);
    if (!source) {
        QMessageBox::warning(this, tr("Cannot Open File"),
                             tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(filePath), describeError(ec)));
        return false;
    }

    auto* doc = new DocumentView(source, core::detectFileType(source->bytes()));
    HexView* view = doc->hexView();
    applyViewSettings(view);
    connect(view, &HexView::cursorMoved, this, [this, doc](std::uint64_t offset) {
        if (doc == currentDocument()) {
            m_inspector->setOffset(offset);
            updateCursorStatus();
        }
    });
    connect(view, &HexView::selectionChanged, this, [this, doc] {
        if (doc == currentDocument())
            updateCursorStatus();
    });
    connect(view, &HexView::zoomRequested, this, &MainWindow::zoomBy);
    connect(view, &HexView::statusMessage, this,
            [this](const QString& message) { statusBar()->showMessage(message, kStatusTimeoutMs); });

    const int index = m_tabs->addTab(doc, QFileInfo(filePath).fileName());
    m_tabs->setTabToolTip(index, QDir::toNativeSeparators(filePath));
    m_tabs->setCurrentIndex(index);
    m_stack->setCurrentWidget(m_tabs);
    doc->setFocus();
    m_lastDirectory = QFileInfo(filePath).absolutePath();
    return true;
}

void MainWindow::showOpenDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open Files"), m_lastDirectory);
    for (const QString& path : paths)
        openFile(path);
}

void MainWindow::closeTab(int index)
{
    QWidget* widget = m_tabs->widget(index);
    if (!widget)
        return;
    m_tabs->removeTab(index);
    widget->deleteLater();
    if (m_tabs->count() == 0)
        m_stack->setCurrentWidget(m_welcome);
}

void MainWindow::currentTabChanged()
{
    const DocumentView* doc = currentDocument();
    m_inspector->setSource(doc ? doc->source() : nullptr);
    if (const HexView* view = currentHexView())
        m_inspector->setOffset(view->cursorOffset());
    setWindowTitle(doc ? tr("%1 — cppEdit").arg(QFileInfo(doc->filePath()).fileName()) : QStringLiteral("cppEdit"));
    updateFileStatus();
    updateCursorStatus();
    updateActions();
}

void MainWindow::updateActions()
{
    const bool hasDocument = currentDocument() != nullptr;
    for (QAction* action : {m_closeAction, m_copyAction, m_copyHexAction, m_copyTextAction, m_copyCArrayAction,
                            m_copyOffsetAction, m_selectAllAction, m_goToAction}) {
        action->setEnabled(hasDocument);
    }
}

// View settings -------------------------------------------------------------------------------------

void MainWindow::applyViewSettings(HexView* view) const
{
    QFont font = view->font();
    font.setPointSize(m_pointSize);
    view->setFont(font);
    view->setBytesPerRow(m_bytesPerRow);
}

void MainWindow::setBytesPerRow(int bytesPerRow)
{
    const auto* choice = std::find(std::begin(kBytesPerRowChoices), std::end(kBytesPerRowChoices), bytesPerRow);
    m_bytesPerRow = choice != std::end(kBytesPerRowChoices) ? bytesPerRow : 16;
    for (QAction* action : m_bytesPerRowGroup->actions())
        action->setChecked(action->data().toInt() == m_bytesPerRow);
    for (int i = 0; i < m_tabs->count(); ++i)
        document(i)->hexView()->setBytesPerRow(m_bytesPerRow);
}

void MainWindow::zoomBy(int steps)
{
    const int pointSize = std::clamp(m_pointSize + steps, kMinPointSize, kMaxPointSize);
    if (pointSize == m_pointSize)
        return;
    m_pointSize = pointSize;
    for (int i = 0; i < m_tabs->count(); ++i)
        applyViewSettings(document(i)->hexView());
    statusBar()->showMessage(tr("Font size: %1 pt").arg(m_pointSize), kStatusTimeoutMs);
}

void MainWindow::resetZoom()
{
    zoomBy(m_defaultPointSize - m_pointSize);
}

void MainWindow::goToOffset()
{
    HexView* view = currentHexView();
    if (!view || view->size() == 0)
        return;

    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Go to Offset"),
                                               tr("Offset (0x1F or 1Fh: hex, 31: decimal, +/- prefix: relative):"),
                                               QLineEdit::Normal, m_lastGoTo, &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    m_lastGoTo = text.trimmed();

    const auto offset = core::parseOffset(text.toStdString(), view->cursorOffset());
    if (!offset) {
        statusBar()->showMessage(tr("Not a valid offset: %1").arg(text.trimmed()), kStatusTimeoutMs);
        return;
    }
    if (*offset >= view->size())
        statusBar()->showMessage(tr("%1 is past the end of the file; moved to the last byte").arg(hexOffset(*offset)),
                                 kStatusTimeoutMs);
    view->goTo(*offset);
    view->setFocus();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About cppEdit"),
                       tr("<h3>cppEdit %1</h3>"
                          "<p>A fast, AI-assisted viewer for text and binary files.</p>"
                          "<p>Licensed under the GNU General Public License, version 3 or later.</p>")
                           .arg(QCoreApplication::applicationVersion()));
}

// Status bar ------------------------------------------------------------------------------------------

QString MainWindow::describeFileType(const core::FileTypeInfo& type) const
{
    switch (type.kind) {
    case core::ContentKind::Empty:
        return tr("Empty file");
    case core::ContentKind::Binary:
        return type.format.empty()
                   ? tr("Binary")
                   : tr("Binary · %1")
                         .arg(QString::fromUtf8(type.format.data(), static_cast<qsizetype>(type.format.size())));
    case core::ContentKind::Text:
        return type.hasBom ? tr("Text · %1 with BOM").arg(encodingName(type.encoding))
                           : tr("Text · %1").arg(encodingName(type.encoding));
    }
    return {};
}

QString MainWindow::describeError(const std::error_code& ec) const
{
    if (ec.category() == core::fileErrorCategory()) {
        switch (static_cast<core::FileError>(ec.value())) {
        case core::FileError::NotRegularFile:
            return tr("Not a regular file. Pipes, sockets and devices are not supported yet.");
        case core::FileError::TooLargeToRead:
            return tr("The file cannot be memory-mapped and is too large to read into memory.");
        }
    }
    return QString::fromLocal8Bit(ec.message());
}

void MainWindow::updateFileStatus()
{
    const DocumentView* doc = currentDocument();
    m_typeLabel->setVisible(doc != nullptr);
    m_sizeLabel->setVisible(doc != nullptr);
    if (!doc)
        return;

    const QLocale locale;
    const auto size = doc->source()->size();
    m_typeLabel->setText(describeFileType(doc->fileType()));
    const QString bytes = locale.toString(static_cast<qulonglong>(size));
    m_sizeLabel->setText(size < 1024 ? tr("%1 bytes").arg(bytes)
                                     : tr("%1 (%2 bytes)").arg(locale.formattedDataSize(static_cast<qint64>(size)), bytes));
}

void MainWindow::updateCursorStatus()
{
    const HexView* view = currentHexView();
    const bool show = view && view->size() > 0;
    m_cursorLabel->setVisible(show);
    m_selectionLabel->setVisible(show && view->selectionLength() > 1);
    if (!show)
        return;

    const QLocale locale;
    const auto offset = view->cursorOffset();
    m_cursorLabel->setText(
        tr("Offset: %1 (%2)").arg(hexOffset(offset), locale.toString(static_cast<qulonglong>(offset))));
    const auto length = view->selectionLength();
    if (length > 1) {
        m_selectionLabel->setText(tr("Selected: %1 bytes (%2) from %3")
                                      .arg(locale.toString(static_cast<qulonglong>(length)), hexOffset(length),
                                           hexOffset(view->selectionStart())));
    }
}

} // namespace cppedit::ui
