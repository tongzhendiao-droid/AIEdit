#include "ui/MainWindow.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/Offset.h"
#include "core/TextCodec.h"
#include "ui/AiPanel.h"
#include "ui/AiSettingsDialog.h"
#include "ui/DataInspector.h"
#include "ui/HexView.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

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
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

#include <algorithm>

namespace aiedit::ui {

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

QString codecName(const core::TextCodec& codec)
{
    return QString::fromLatin1(codec.name.data(), static_cast<qsizetype>(codec.name.size()));
}

QString lineEndingName(const core::LineEndings& endings)
{
    const int kinds = (endings.lf > 0) + (endings.crlf > 0) + (endings.cr > 0);
    if (kinds > 1)
        return MainWindow::tr("Mixed line endings");
    if (endings.crlf > 0)
        return QStringLiteral("CRLF");
    if (endings.cr > 0)
        return QStringLiteral("CR");
    return QStringLiteral("LF");
}

} // namespace

std::string_view MainWindow::preferredLegacyCodec()
{
    const QLocale locale;
    switch (locale.language()) {
    case QLocale::Chinese: {
        const bool traditional = locale.script() == QLocale::TraditionalChineseScript ||
                                 locale.territory() == QLocale::Taiwan || locale.territory() == QLocale::HongKong;
        return traditional ? "Big5" : "GB18030";
    }
    case QLocale::Japanese:
        return "Shift_JIS";
    case QLocale::Korean:
        return "EUC-KR";
    case QLocale::Russian:
    case QLocale::Ukrainian:
    case QLocale::Bulgarian:
    case QLocale::Belarusian:
        return "windows-1251";
    case QLocale::Polish:
    case QLocale::Czech:
    case QLocale::Slovak:
    case QLocale::Hungarian:
    case QLocale::Slovenian:
    case QLocale::Croatian:
    case QLocale::Romanian:
        return "windows-1250";
    case QLocale::Greek:
        return "windows-1253";
    case QLocale::Turkish:
        return "windows-1254";
    case QLocale::Arabic:
    case QLocale::Persian:
        return "windows-1256";
    default:
        return "windows-1252";
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_defaultPointSize = fixed.pointSize() > 0 ? fixed.pointSize() : 10;
    m_pointSize = m_defaultPointSize;

    setAcceptDrops(true);
    createWidgets();
    createActions();
    createMenus();
    createCodecMenu();
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

    m_aiPanel = new AiPanel;
    m_aiPanel->setContextProvider([this]() -> std::optional<AiContext> {
        if (DocumentView* doc = currentDocument())
            return doc->aiContext();
        return std::nullopt;
    });
    connect(m_aiPanel, &AiPanel::settingsRequested, this, &MainWindow::showAiSettings);
    m_aiDock = new QDockWidget(tr("AI Assistant"), this);
    m_aiDock->setObjectName(QStringLiteral("aiDock"));
    m_aiDock->setWidget(m_aiPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_aiDock);
    tabifyDockWidget(m_inspectorDock, m_aiDock);
    m_aiDock->raise();

    m_typeLabel = new QLabel;
    m_sizeLabel = new QLabel;
    m_codecButton = new QToolButton;
    m_codecButton->setAutoRaise(true);
    m_codecButton->setPopupMode(QToolButton::InstantPopup);
    m_codecButton->setToolTip(tr("Text encoding. Click to show the file in another encoding."));
    m_syntaxButton = new QToolButton;
    m_syntaxButton->setAutoRaise(true);
    m_syntaxButton->setPopupMode(QToolButton::InstantPopup);
    m_syntaxButton->setToolTip(tr("Syntax highlighting. Click to choose a language."));
    auto* syntaxMenu = new QMenu(this);
    // Hundreds of definitions: build the menu the first time it opens.
    connect(syntaxMenu, &QMenu::aboutToShow, this, &MainWindow::populateSyntaxMenu);
    m_syntaxButton->setMenu(syntaxMenu);
    m_lineEndingLabel = new QLabel;
    m_linesLabel = new QLabel;
    m_positionLabel = new QLabel;
    m_selectionLabel = new QLabel;
    statusBar()->addPermanentWidget(m_selectionLabel);
    statusBar()->addPermanentWidget(m_positionLabel);
    statusBar()->addPermanentWidget(m_linesLabel);
    statusBar()->addPermanentWidget(m_lineEndingLabel);
    statusBar()->addPermanentWidget(m_syntaxButton);
    statusBar()->addPermanentWidget(m_codecButton);
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

    m_reloadAction = new QAction(tr("&Reload"), this);
    m_reloadAction->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_reloadAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            doc->reload();
    });

    m_followAction = new QAction(tr("&Follow End of File"), this);
    m_followAction->setCheckable(true);
    m_followAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    m_followAction->setToolTip(tr("Keep showing the end of the file as it grows, like tail -f"));
    connect(m_followAction, &QAction::toggled, this, [this](bool on) {
        if (DocumentView* doc = currentDocument())
            doc->setFollowing(on);
    });

    m_wordWrapAction = new QAction(tr("&Word Wrap"), this);
    m_wordWrapAction->setCheckable(true);
    m_wordWrapAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Z));
    m_wordWrapAction->setToolTip(tr("Wrap long lines at the edge of the window instead of scrolling sideways"));
    connect(m_wordWrapAction, &QAction::toggled, this, &MainWindow::setWordWrap);

    m_quitAction = new QAction(tr("&Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    m_quitAction->setMenuRole(QAction::QuitRole);
    connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

    // Edit actions only fire while the documents have focus, so that other widgets (the data
    // inspector, dialogs) keep their own Ctrl+C and Ctrl+A.
    auto documentAction = [this](const QString& text, auto handler, const QKeySequence& shortcut = {}) {
        auto* action = new QAction(text, this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_tabs->addAction(action);
        connect(action, &QAction::triggered, this, [this, handler] {
            if (DocumentView* doc = currentDocument())
                handler(doc);
        });
        return action;
    };
    // The hex view always mirrors the text selection, so byte-oriented copies can use it.
    m_copyAction = documentAction(tr("&Copy"), [](DocumentView* doc) { doc->copy(); }, QKeySequence::Copy);
    m_copyHexAction = documentAction(tr("Copy as &Hex"), [](DocumentView* doc) { doc->hexView()->copyAsHex(); });
    m_copyTextAction = documentAction(tr("Copy as &Text"), [](DocumentView* doc) {
        if (doc->isTextActive())
            doc->textView()->copy();
        else
            doc->hexView()->copyAsText();
    });
    m_copyCArrayAction =
        documentAction(tr("Copy as C A&rray"), [](DocumentView* doc) { doc->hexView()->copyAsCArray(); });
    m_copyOffsetAction = documentAction(tr("Copy &Offset"), [](DocumentView* doc) { doc->hexView()->copyOffset(); });
    m_selectAllAction =
        documentAction(tr("Select &All"), [](DocumentView* doc) { doc->selectAll(); }, QKeySequence::SelectAll);

    m_goToAction = new QAction(tr("&Go To…"), this);
    m_goToAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(m_goToAction, &QAction::triggered, this, &MainWindow::goTo);

    // F3 rather than QKeySequence::FindNext, which includes Ctrl+G on Linux.
    m_findAction = new QAction(tr("&Find…"), this);
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            doc->showFindBar();
    });
    m_findNextAction = new QAction(tr("Find &Next"), this);
    m_findNextAction->setShortcut(QKeySequence(Qt::Key_F3));
    connect(m_findNextAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            doc->findNext(true);
    });
    m_findPreviousAction = new QAction(tr("Find Pre&vious"), this);
    m_findPreviousAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    connect(m_findPreviousAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            doc->findNext(false);
    });

    m_viewModeGroup = new QActionGroup(this);
    auto modeAction = [this](const QString& text, Qt::Key key, DocumentView::ViewMode mode) {
        QAction* action = m_viewModeGroup->addAction(text);
        action->setCheckable(true);
        action->setShortcut(QKeySequence(Qt::CTRL | key));
        connect(action, &QAction::triggered, this, [this, mode] { setViewMode(mode); });
        return action;
    };
    m_textModeAction = modeAction(tr("&Text View"), Qt::Key_1, DocumentView::ViewMode::Text);
    m_hexModeAction = modeAction(tr("&Hex View"), Qt::Key_2, DocumentView::ViewMode::Hex);
    m_splitModeAction = modeAction(tr("&Side by Side"), Qt::Key_3, DocumentView::ViewMode::Split);

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

    auto aiAction = [this](const QString& text, void (AiPanel::*action)(), const QKeySequence& shortcut = {}) {
        auto* a = new QAction(text, this);
        a->setShortcut(shortcut);
        connect(a, &QAction::triggered, this, [this, action] { runAiAction(action); });
        return a;
    };
    // Shared by the AI menu and the right-click menus of both views.
    m_aiExplainAction = aiAction(tr("AI &Explain"), &AiPanel::explainSelection, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    m_aiTranslateAction = aiAction(tr("AI &Translate"), &AiPanel::translateSelection);
    m_aiSummarizeAction = aiAction(tr("AI &Summarize"), &AiPanel::summarizeSelection);
    m_aiAskAction = aiAction(tr("&Ask AI…"), &AiPanel::focusInput);
    m_aiIdentifyAction = aiAction(tr("What Is This &File?"), &AiPanel::identifyFile);
    m_aiSettingsAction = new QAction(tr("AI Setti&ngs…"), this);
    connect(m_aiSettingsAction, &QAction::triggered, this, &MainWindow::showAiSettings);

    m_aboutAction = new QAction(tr("&About AIEdit"), this);
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
    file->addAction(m_reloadAction);
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
    edit->addAction(m_findAction);
    edit->addAction(m_findNextAction);
    edit->addAction(m_findPreviousAction);
    edit->addAction(m_goToAction);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addActions(m_viewModeGroup->actions());
    view->addSeparator();
    view->addAction(m_wordWrapAction);
    view->addAction(m_followAction);
    view->addSeparator();
    QMenu* hex = view->addMenu(tr("Hex &Layout"));
    hex->addActions(m_bytesPerRowGroup->actions());
    view->addSeparator();
    view->addAction(m_zoomInAction);
    view->addAction(m_zoomOutAction);
    view->addAction(m_resetZoomAction);
    view->addSeparator();
    view->addAction(m_inspectorDock->toggleViewAction());
    view->addSeparator();
    view->addAction(m_nextTabAction);
    view->addAction(m_previousTabAction);

    QMenu* ai = menuBar()->addMenu(tr("&AI"));
    QAction* showAi = m_aiDock->toggleViewAction();
    showAi->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    ai->addAction(showAi);
    ai->addSeparator();
    ai->addAction(m_aiExplainAction);
    ai->addAction(m_aiTranslateAction);
    ai->addAction(m_aiSummarizeAction);
    ai->addAction(m_aiAskAction);
    ai->addAction(m_aiIdentifyAction);
    ai->addSeparator();
    ai->addAction(m_aiSettingsAction);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_aboutAction);
    help->addAction(m_aboutQtAction);
}

void MainWindow::createCodecMenu()
{
    // Grouped by script; every codec here exists in core::knownCodecs().
    const std::pair<QString, std::initializer_list<std::string_view>> groups[] = {
        {tr("Unicode"), {"UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE"}},
        {tr("Chinese"), {"GB18030", "Big5"}},
        {tr("Japanese"), {"Shift_JIS", "EUC-JP"}},
        {tr("Korean"), {"EUC-KR"}},
        {tr("Western European"), {"windows-1252", "ISO-8859-1", "ISO-8859-15"}},
        {tr("Central European"), {"windows-1250", "ISO-8859-2"}},
        {tr("Cyrillic"), {"windows-1251", "KOI8-R"}},
        {tr("Greek"), {"windows-1253"}},
        {tr("Turkish"), {"windows-1254"}},
        {tr("Arabic"), {"windows-1256"}},
    };

    auto* menu = new QMenu(this);
    m_codecGroup = new QActionGroup(this);
    for (const auto& [title, names] : groups) {
        menu->addSection(title);
        for (const std::string_view name : names) {
            const core::TextCodec* codec = core::findCodec(name);
            QAction* action = menu->addAction(codecName(*codec));
            action->setCheckable(true);
            m_codecGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, codec] { setCodec(*codec); });
        }
    }
    m_codecButton->setMenu(menu);
}

void MainWindow::populateSyntaxMenu()
{
    QMenu* menu = m_syntaxButton->menu();
    if (!menu->isEmpty())
        return;
    const auto choose = [this](const QString& name) {
        if (DocumentView* doc = currentDocument())
            doc->setSyntax(name);
    };
    menu->addAction(tr("Plain Text"), this, [choose] { choose(QString()); });
    menu->addSeparator();
    QMenu* section = nullptr;
    for (const SyntaxInfo& syntax : availableSyntaxes()) {
        if (!section || section->title() != syntax.translatedSection)
            section = menu->addMenu(syntax.translatedSection);
        const QString name = syntax.name;
        section->addAction(syntax.translatedName, this, [choose, name] { choose(name); });
    }
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
    setWordWrap(settings.value(QStringLiteral("view/wordWrap"), true).toBool());
}

void MainWindow::writeSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("files/lastDirectory"), m_lastDirectory);
    settings.setValue(QStringLiteral("view/pointSize"), m_pointSize);
    settings.setValue(QStringLiteral("view/bytesPerRow"), m_bytesPerRow);
    settings.setValue(QStringLiteral("view/wordWrap"), m_wordWrap);
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

    const core::FileTypeInfo type = core::detectFileType(source->bytes());
    const core::TextCodec& codec = core::defaultCodec(type, source->bytes(), preferredLegacyCodec());
    auto* doc = new DocumentView(source, type, codec);
    applyViewSettings(doc);
    doc->setContextActions({m_aiExplainAction, m_aiTranslateAction, m_aiSummarizeAction, m_aiAskAction},
                           {m_aiExplainAction, m_aiSummarizeAction, m_aiAskAction, m_aiIdentifyAction});
    connect(doc, &DocumentView::stateChanged, this, [this, doc] { documentStateChanged(doc); });
    connect(doc, &DocumentView::sourceChanged, this, [this, doc] {
        if (doc != currentDocument())
            return;
        m_inspector->setSource(doc->source());
        updateFileStatus();
    });
    connect(doc, &DocumentView::indexProgressed, this, [this, doc] {
        if (doc == currentDocument())
            updateTextStatus();
    });
    connect(doc, &DocumentView::zoomRequested, this, &MainWindow::zoomBy);
    connect(doc, &DocumentView::statusMessage, this,
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
    DocumentView* doc = currentDocument();
    m_inspector->setSource(doc ? doc->source() : nullptr);
    setWindowTitle(doc ? tr("%1 — AIEdit").arg(QFileInfo(doc->filePath()).fileName()) : QStringLiteral("AIEdit"));
    updateFileStatus();
    documentStateChanged(doc);
    updateActions();
}

void MainWindow::documentStateChanged(DocumentView* doc)
{
    if (doc != currentDocument())
        return;
    if (doc) {
        m_inspector->setOffset(doc->cursorOffset());
        const DocumentView::ViewMode mode = doc->viewMode();
        m_textModeAction->setChecked(mode == DocumentView::ViewMode::Text);
        m_hexModeAction->setChecked(mode == DocumentView::ViewMode::Hex);
        m_splitModeAction->setChecked(mode == DocumentView::ViewMode::Split);
        const QSignalBlocker blocker(m_followAction);
        m_followAction->setChecked(doc->isFollowing());
    }
    // Explaining, translating and summarizing need a selection; asking and identifying do not.
    const bool selected = doc && doc->hasSelection();
    for (QAction* action : {m_aiExplainAction, m_aiTranslateAction, m_aiSummarizeAction})
        action->setEnabled(selected);
    updateCursorStatus();
    updateTextStatus();
    m_aiPanel->refreshContext();
}

void MainWindow::updateActions()
{
    const bool hasDocument = currentDocument() != nullptr;
    for (QAction* action : {m_closeAction, m_reloadAction, m_followAction, m_copyAction, m_copyHexAction, m_copyTextAction, m_copyCArrayAction,
                            m_copyOffsetAction, m_selectAllAction, m_goToAction, m_findAction, m_findNextAction,
                            m_findPreviousAction, m_aiAskAction, m_aiIdentifyAction}) {
        action->setEnabled(hasDocument);
    }
    m_viewModeGroup->setEnabled(hasDocument);
}

void MainWindow::setViewMode(DocumentView::ViewMode mode)
{
    if (DocumentView* doc = currentDocument())
        doc->setViewMode(mode);
}

void MainWindow::setCodec(const core::TextCodec& codec)
{
    DocumentView* doc = currentDocument();
    if (!doc)
        return;
    doc->setCodec(codec);
    // Choosing an encoding means the user wants to read text.
    if (doc->viewMode() == DocumentView::ViewMode::Hex)
        doc->setViewMode(DocumentView::ViewMode::Text);
}

// View settings -------------------------------------------------------------------------------------

void MainWindow::applyViewSettings(DocumentView* doc) const
{
    for (QWidget* view : {static_cast<QWidget*>(doc->textView()), static_cast<QWidget*>(doc->hexView())}) {
        QFont font = view->font();
        font.setPointSize(m_pointSize);
        view->setFont(font);
    }
    doc->hexView()->setBytesPerRow(m_bytesPerRow);
    doc->textView()->setWordWrap(m_wordWrap);
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

void MainWindow::setWordWrap(bool on)
{
    m_wordWrap = on;
    const QSignalBlocker blocker(m_wordWrapAction);
    m_wordWrapAction->setChecked(on);
    for (int i = 0; i < m_tabs->count(); ++i)
        document(i)->textView()->setWordWrap(on);
}

void MainWindow::zoomBy(int steps)
{
    const int pointSize = std::clamp(m_pointSize + steps, kMinPointSize, kMaxPointSize);
    if (pointSize == m_pointSize)
        return;
    m_pointSize = pointSize;
    for (int i = 0; i < m_tabs->count(); ++i)
        applyViewSettings(document(i));
    statusBar()->showMessage(tr("Font size: %1 pt").arg(m_pointSize), kStatusTimeoutMs);
}

void MainWindow::resetZoom()
{
    zoomBy(m_defaultPointSize - m_pointSize);
}

void MainWindow::goTo()
{
    DocumentView* doc = currentDocument();
    if (!doc || doc->source()->size() == 0)
        return;

    const bool text = doc->isTextActive();
    bool ok = false;
    const QString prompt = text ? tr("Line number (+/- prefix: relative), or a byte offset such as 0x1F or 1Fh:")
                                : tr("Offset (0x1F or 1Fh: hex, 31: decimal, +/- prefix: relative):");
    const QString input =
        QInputDialog::getText(this, tr("Go To"), prompt, QLineEdit::Normal, m_lastGoTo, &ok).trimmed();
    if (!ok || input.isEmpty())
        return;
    m_lastGoTo = input;

    // In the text view a plain number is a line; anything that looks hexadecimal is an offset.
    const bool looksLikeOffset = input.contains(QLatin1Char('x'), Qt::CaseInsensitive) ||
                                 input.endsWith(QLatin1Char('h'), Qt::CaseInsensitive);
    if (text && !looksLikeOffset) {
        TextView* view = doc->textView();
        const auto line = core::parseLine(input.toStdString(), view->cursorLine() + 1);
        if (!line) {
            statusBar()->showMessage(tr("Not a valid line number: %1").arg(input), kStatusTimeoutMs);
        } else if (!view->goToLine(*line - 1)) {
            statusBar()->showMessage(
                tr("Line %1 does not exist (yet)").arg(QLocale().toString(static_cast<qulonglong>(*line))),
                kStatusTimeoutMs);
        }
        view->setFocus();
        return;
    }

    const auto offset = core::parseOffset(input.toStdString(), doc->cursorOffset());
    if (!offset) {
        statusBar()->showMessage(tr("Not a valid offset: %1").arg(input), kStatusTimeoutMs);
        return;
    }
    if (*offset >= doc->source()->size())
        statusBar()->showMessage(tr("%1 is past the end of the file; moved to the last byte").arg(hexOffset(*offset)),
                                 kStatusTimeoutMs);
    if (text) {
        doc->textView()->setCursorOffset(std::min(*offset, doc->source()->size() - 1));
        doc->textView()->setFocus();
    } else {
        doc->hexView()->goTo(*offset);
        doc->hexView()->setFocus();
    }
}

void MainWindow::runAiAction(void (AiPanel::*action)())
{
    m_aiDock->show();
    m_aiDock->raise();
    (m_aiPanel->*action)();
}

void MainWindow::showAiSettings()
{
    AiSettingsDialog dialog(AiSettings::load(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    dialog.settings().save();
    m_aiPanel->settingsChanged();
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About AIEdit"),
                       tr("<h3>AIEdit %1</h3>"
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

void MainWindow::updateTextStatus()
{
    const DocumentView* doc = currentDocument();
    // Lines, line endings and syntax mean nothing for a binary file in the hex view; the codec
    // stays, since choosing one is how such a file is read as text.
    const bool textual = doc && (doc->fileType().kind != core::ContentKind::Binary || doc->isTextActive());
    m_codecButton->setVisible(doc != nullptr);
    m_syntaxButton->setVisible(textual && isSyntaxHighlightingAvailable());
    m_lineEndingLabel->setVisible(textual);
    m_linesLabel->setVisible(textual);
    if (!doc)
        return;

    const TextDocument* text = doc->textDocument();
    const QString codec = codecName(text->codec());
    m_codecButton->setText(codec);
    for (QAction* action : m_codecGroup->actions())
        action->setChecked(action->text() == codec);
    m_lineEndingLabel->setText(lineEndingName(text->lineEndings()));
    m_syntaxButton->setText(doc->syntax().isEmpty() ? tr("Plain Text") : translatedSyntaxName(doc->syntax()));

    const core::LineIndex& index = text->index();
    const QLocale locale;
    if (index.isComplete()) {
        m_linesLabel->setText(tr("Lines: %1").arg(locale.toString(static_cast<qulonglong>(index.lineCount()))));
    } else {
        const auto size = std::max<std::uint64_t>(1, doc->source()->size());
        const int percent = static_cast<int>(index.indexedBytes() * 100 / size);
        m_linesLabel->setText(tr("Indexing… %1%").arg(percent));
    }
}

void MainWindow::updateCursorStatus()
{
    const DocumentView* doc = currentDocument();
    const bool show = doc && doc->source()->size() > 0;
    m_positionLabel->setVisible(show);
    m_selectionLabel->setVisible(false);
    if (!show)
        return;

    const QLocale locale;
    const auto offset = doc->cursorOffset();
    std::uint64_t selectionStart = 0;
    std::uint64_t selectionLength = 0;
    if (doc->isTextActive()) {
        const TextView* view = doc->textView();
        const auto column = view->cursorColumn();
        const QString line = locale.toString(static_cast<qulonglong>(view->cursorLine() + 1));
        m_positionLabel->setText(column ? tr("Ln %1, Col %2 · %3")
                                              .arg(line, locale.toString(static_cast<qulonglong>(*column + 1)),
                                                   hexOffset(offset))
                                        : tr("Ln %1 · %2").arg(line, hexOffset(offset)));
        selectionStart = view->selectionStart();
        selectionLength = view->selectionEnd() - view->selectionStart();
    } else {
        const HexView* view = doc->hexView();
        m_positionLabel->setText(
            tr("Offset: %1 (%2)").arg(hexOffset(offset), locale.toString(static_cast<qulonglong>(offset))));
        selectionStart = view->selectionStart();
        selectionLength = view->selectionLength() > 1 ? view->selectionLength() : 0;
    }
    if (selectionLength > 0) {
        m_selectionLabel->setText(tr("Selected: %1 bytes (%2) from %3")
                                      .arg(locale.toString(static_cast<qulonglong>(selectionLength)),
                                           hexOffset(selectionLength), hexOffset(selectionStart)));
        m_selectionLabel->setVisible(true);
    }
}

} // namespace aiedit::ui
