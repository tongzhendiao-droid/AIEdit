#include "ui/MainWindow.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/Offset.h"
#include "core/TextCodec.h"
#include "ui/AiPanel.h"
#include "ui/AiSettingsDialog.h"
#include "ui/CompareTab.h"
#include "ui/CompareView.h"
#include "ui/HexCompareView.h"
#include "ui/ContentsPanel.h"
#include "ui/DataInspector.h"
#include "ui/FindInFilesPanel.h"
#include "ui/FolderPanel.h"
#include "ui/HexView.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontDialog>
#include <QIcon>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>

#include <algorithm>

namespace aiedit::ui {

namespace {

constexpr int kMinPointSize = 6;
constexpr int kMaxPointSize = 48;
constexpr int kStatusTimeoutMs = 4000;
constexpr int kNoticeTimeoutMs = 15000; // for what should not go unnoticed
// Unsaved changes are on disk at most this long after they are made.
constexpr int kBackupDelayMs = 5000;
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

// How many files are remembered, and how many of them the File menu lists.
constexpr qsizetype kMaxRememberedFiles = 50;
constexpr qsizetype kRecentFilesInMenu = 10;

QVariantMap toVariant(const DocumentView::State& state)
{
    return {{QStringLiteral("path"), state.path},
            {QStringLiteral("cursor"), static_cast<qulonglong>(state.cursor)},
            {QStringLiteral("top"), static_cast<qulonglong>(state.top)},
            {QStringLiteral("mode"), static_cast<int>(state.mode)},
            {QStringLiteral("codec"), state.codec},
            {QStringLiteral("reading"), state.reading}};
}

DocumentView::State stateFromVariant(const QVariant& value)
{
    const QVariantMap map = value.toMap();
    DocumentView::State state;
    state.path = map.value(QStringLiteral("path")).toString();
    state.cursor = map.value(QStringLiteral("cursor")).toULongLong();
    state.top = map.value(QStringLiteral("top")).toULongLong();
    state.mode = static_cast<DocumentView::ViewMode>(std::clamp(map.value(QStringLiteral("mode")).toInt(), 0, 2));
    state.codec = map.value(QStringLiteral("codec")).toString();
    state.reading = map.value(QStringLiteral("reading")).toBool();
    return state;
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
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            [this] { updateEditActions(currentDocument()); });
    m_backupTimer.setSingleShot(true);
    m_backupTimer.setInterval(kBackupDelayMs);
    connect(&m_backupTimer, &QTimer::timeout, this, &MainWindow::writeBackups);
}

MainWindow::~MainWindow()
{
    // The documents go with the window, and what they report while going (a scroll bar back
    // at the top) must not reach a window that is half gone.
    for (int i = 0; i < m_tabs->count(); ++i) {
        DocumentView* doc = document(i);
        if (!doc)
            continue;
        doc->disconnect(this);
        doc->textView()->verticalScrollBar()->disconnect(this);
    }
}

void MainWindow::createWidgets()
{
    // Shown while nothing is open: where to start.
    auto* hint = new QLabel(tr("Drop files here to open them"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setForegroundRole(QPalette::PlaceholderText);
    QFont hintFont = hint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * 1.3);
    hint->setFont(hintFont);
    auto* newButton = new QPushButton(tr("New File"));
    newButton->setToolTip(QKeySequence(QKeySequence::New).toString(QKeySequence::NativeText));
    connect(newButton, &QPushButton::clicked, this, &MainWindow::newDocument);
    auto* openButton = new QPushButton(tr("Open File…"));
    openButton->setToolTip(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText));
    connect(openButton, &QPushButton::clicked, this, &MainWindow::showOpenDialog);
    auto* folderButton = new QPushButton(tr("Open Folder…"));
    connect(folderButton, &QPushButton::clicked, this, &MainWindow::showOpenFolderDialog);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(newButton);
    buttons->addWidget(openButton);
    buttons->addWidget(folderButton);
    buttons->addStretch();
    m_welcome = new QWidget;
    auto* welcomeLayout = new QVBoxLayout(m_welcome);
    welcomeLayout->addStretch();
    welcomeLayout->addWidget(hint);
    welcomeLayout->addSpacing(12);
    welcomeLayout->addLayout(buttons);
    welcomeLayout->addStretch();

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->tabBar()->setElideMode(Qt::ElideMiddle);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::currentTabChanged);
    // A new tab from the + button, or by double-clicking the empty part of the tab bar.
    auto* newTab = new QToolButton;
    newTab->setAutoRaise(true);
    newTab->setToolTip(tr("New file (%1)").arg(QKeySequence(QKeySequence::New).toString(QKeySequence::NativeText)));
    if (const QIcon icon = QIcon::fromTheme(QStringLiteral("tab-new"), QIcon::fromTheme(QStringLiteral("list-add")));
        !icon.isNull())
        newTab->setIcon(icon);
    else
        newTab->setText(QStringLiteral("+"));
    connect(newTab, &QToolButton::clicked, this, &MainWindow::newDocument);
    m_tabs->setCornerWidget(newTab, Qt::TopRightCorner);
    // Double-clicking a tab keeps it, if it is the preview tab.
    connect(m_tabs, &QTabWidget::tabBarDoubleClicked, this, [this](int index) {
        if (index < 0)
            newDocument();
        else
            keepDocument(document(index));
    });

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

    m_contents = new ContentsPanel;
    connect(m_contents, &ContentsPanel::chapterAction, this,
            [this](ContentsPanel::ChapterAction action, const QString& title, const QString& text, bool cut) {
                askAboutChapter(static_cast<int>(action), title, text, cut);
            });
    connect(m_contents, &ContentsPanel::aiSettingsRequested, this, &MainWindow::showAiSettings);
    m_contentsDock = new QDockWidget(tr("Contents"), this);
    m_contentsDock->setObjectName(QStringLiteral("contentsDock"));
    m_contentsDock->setWidget(m_contents);
    addDockWidget(Qt::LeftDockWidgetArea, m_contentsDock);
    m_contentsDock->hide();

    m_folders = new FolderPanel;
    m_folders->setLegacyCodec(preferredLegacyCodec());
    // Renaming or trashing would leave unsaved changes without a file.
    m_folders->setUnsavedCheck([this](const QString& path) {
        const QString prefix = path + QLatin1Char('/');
        for (int i = 0; i < m_tabs->count(); ++i) {
            const DocumentView* doc = document(i);
            if (doc && doc->isModified() && !doc->isUntitled() &&
                (doc->filePath() == path || doc->filePath().startsWith(prefix)))
                return doc->title();
        }
        return QString();
    });
    connect(m_folders, &FolderPanel::previewRequested, this, &MainWindow::previewFile);
    connect(m_folders, &FolderPanel::openRequested, this, [this](const QString& path) {
        if (openFile(path))
            currentDocument()->setFocus();
    });
    connect(m_folders, &FolderPanel::findInFolderRequested, this, &MainWindow::showFindInFilesIn);
    connect(m_folders, &FolderPanel::folderSummarized, this, &MainWindow::askAboutFolder);
    connect(m_folders, &FolderPanel::pathRenamed, this, &MainWindow::documentsMoved);
    connect(m_folders, &FolderPanel::pathTrashed, this, &MainWindow::documentsTrashed);
    connect(m_folders, &FolderPanel::folderChanged, this, [this] { updateActions(); });
    connect(m_folders, &FolderPanel::compareRequested, this, &MainWindow::compareFiles);
    m_folderDock = new QDockWidget(tr("Folder"), this);
    m_folderDock->setObjectName(QStringLiteral("folderDock"));
    m_folderDock->setWidget(m_folders);
    addDockWidget(Qt::LeftDockWidgetArea, m_folderDock);
    tabifyDockWidget(m_contentsDock, m_folderDock);
    m_folderDock->hide();

    m_findInFiles = new FindInFilesPanel;
    m_findInFiles->setLegacyCodec(preferredLegacyCodec());
    connect(m_findInFiles, &FindInFilesPanel::openRequested, this, &MainWindow::openAtLine);
    m_findInFilesDock = new QDockWidget(tr("Find in Files"), this);
    m_findInFilesDock->setObjectName(QStringLiteral("findInFilesDock"));
    m_findInFilesDock->setWidget(m_findInFiles);
    addDockWidget(Qt::BottomDockWidgetArea, m_findInFilesDock);
    m_findInFilesDock->hide();

    m_typeLabel = new QLabel;
    m_sizeLabel = new QLabel;
    m_codecButton = new QToolButton;
    m_codecButton->setAutoRaise(true);
    m_codecButton->setPopupMode(QToolButton::InstantPopup);
    m_codecButton->setToolTip(tr("Text encoding. Click to read the file in another encoding, or to save it in one."));
    m_syntaxButton = new QToolButton;
    m_syntaxButton->setAutoRaise(true);
    m_syntaxButton->setPopupMode(QToolButton::InstantPopup);
    m_syntaxButton->setToolTip(tr("Syntax highlighting. Click to choose a language."));
    auto* syntaxMenu = new QMenu(this);
    // Hundreds of definitions: build the menu the first time it opens.
    connect(syntaxMenu, &QMenu::aboutToShow, this, &MainWindow::populateSyntaxMenu);
    m_syntaxButton->setMenu(syntaxMenu);
    m_lineEndingButton = new QToolButton;
    m_lineEndingButton->setAutoRaise(true);
    m_lineEndingButton->setPopupMode(QToolButton::InstantPopup);
    m_lineEndingButton->setToolTip(tr("Line endings. Click to give every line the same one."));
    auto* lineEndingMenu = new QMenu(this);
    const std::pair<QString, QString> lineEndings[] = {
        {tr("Convert to LF (Linux, macOS)"), QStringLiteral("\n")},
        {tr("Convert to CRLF (Windows)"), QStringLiteral("\r\n")},
        {tr("Convert to CR (classic Mac OS)"), QStringLiteral("\r")},
    };
    for (const auto& [text, terminator] : lineEndings)
        lineEndingMenu->addAction(text, this, [this, terminator] { convertLineEndings(terminator); });
    // Only documents that can be edited can be converted.
    connect(lineEndingMenu, &QMenu::aboutToShow, this, [this, lineEndingMenu] {
        const DocumentView* doc = currentDocument();
        for (QAction* action : lineEndingMenu->actions())
            action->setEnabled(doc && doc->isEditable());
    });
    m_lineEndingButton->setMenu(lineEndingMenu);
    m_linesLabel = new QLabel;
    m_positionLabel = new QLabel;
    m_selectionLabel = new QLabel;
    m_compareStatus = new QLabel;
    m_compareStatus->hide();
    statusBar()->addWidget(m_compareStatus, 1);
    statusBar()->addPermanentWidget(m_selectionLabel);
    statusBar()->addPermanentWidget(m_positionLabel);
    statusBar()->addPermanentWidget(m_linesLabel);
    statusBar()->addPermanentWidget(m_lineEndingButton);
    statusBar()->addPermanentWidget(m_syntaxButton);
    statusBar()->addPermanentWidget(m_codecButton);
    statusBar()->addPermanentWidget(m_sizeLabel);
    statusBar()->addPermanentWidget(m_typeLabel);
}

void MainWindow::createActions()
{
    m_newAction = new QAction(tr("&New"), this);
    m_newAction->setShortcut(QKeySequence::New);
    connect(m_newAction, &QAction::triggered, this, &MainWindow::newDocument);

    m_openAction = new QAction(tr("&Open…"), this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::showOpenDialog);
    m_openFolderAction = new QAction(tr("Open &Folder…"), this);
    m_openFolderAction->setToolTip(tr("Show a folder's files beside the documents"));
    connect(m_openFolderAction, &QAction::triggered, this, &MainWindow::showOpenFolderDialog);

    m_closeAction = new QAction(tr("&Close Tab"), this);
    m_closeAction->setShortcut(QKeySequence::Close);
    connect(m_closeAction, &QAction::triggered, this, [this] { closeTab(m_tabs->currentIndex()); });

    m_saveAction = new QAction(tr("&Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            saveDocument(doc);
    });
    m_saveAsAction = new QAction(tr("Save &As…"), this);
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            saveDocumentAs(doc);
    });

    m_reloadAction = new QAction(tr("&Reload"), this);
    m_reloadAction->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_reloadAction, &QAction::triggered, this, [this] {
        DocumentView* doc = currentDocument();
        if (!doc)
            return;
        if (doc->isModified() &&
            QMessageBox::question(this, tr("Reload"),
                                  tr("Discard your changes to %1 and load it from disk again?").arg(doc->title()),
                                  QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
            return;
        doc->reload();
    });

    m_followAction = new QAction(tr("&Follow End of File"), this);
    m_followAction->setCheckable(true);
    // T for tail -f; Ctrl+Shift+F is Find in Files, as in most editors.
    m_followAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    m_followAction->setToolTip(tr("Keep showing the end of the file as it grows, like tail -f"));
    connect(m_followAction, &QAction::toggled, this, [this](bool on) {
        if (DocumentView* doc = currentDocument())
            doc->setFollowing(on);
    });

    m_readingAction = new QAction(tr("Rea&ding Mode"), this);
    m_readingAction->setCheckable(true);
    m_readingAction->setShortcut(QKeySequence(Qt::Key_F9)); // as in Firefox's reader view
    m_readingAction->setToolTip(tr("Read a book: no line numbers, the text in a column in the middle with room "
                                   "between the lines, and its contents beside it"));
    connect(m_readingAction, &QAction::triggered, this, &MainWindow::setReadingMode);

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
    m_undoAction = documentAction(tr("&Undo"), [](DocumentView* doc) { doc->undo(); }, QKeySequence::Undo);
    m_redoAction = documentAction(tr("&Redo"), [](DocumentView* doc) { doc->redo(); });
    QList<QKeySequence> redoKeys = QKeySequence::keyBindings(QKeySequence::Redo);
    if (!redoKeys.contains(QKeySequence(Qt::CTRL | Qt::Key_Y)))
        redoKeys << QKeySequence(Qt::CTRL | Qt::Key_Y);
    m_redoAction->setShortcuts(redoKeys);
    m_cutAction = documentAction(tr("Cu&t"), [](DocumentView* doc) { doc->cut(); }, QKeySequence::Cut);
    m_pasteAction = documentAction(tr("&Paste"), [](DocumentView* doc) { doc->paste(); }, QKeySequence::Paste);
    // The hex view always mirrors the text selection, so byte-oriented copies can use it.
    m_copyAction = documentAction(tr("&Copy"), [](DocumentView* doc) { doc->copy(); }, QKeySequence::Copy);
    m_copyHexAction = documentAction(tr("Copy as &Hex"), [](DocumentView* doc) { doc->hexView()->copyAsHex(); });
    m_copyTextAction = documentAction(tr("Copy as Te&xt"), [](DocumentView* doc) {
        if (doc->isTextActive())
            doc->textView()->copy();
        else
            doc->hexView()->copyAsText();
    });
    m_copyCArrayAction =
        documentAction(tr("Copy as C Arra&y"), [](DocumentView* doc) { doc->hexView()->copyAsCArray(); });
    m_copyOffsetAction = documentAction(tr("Copy &Offset"), [](DocumentView* doc) { doc->hexView()->copyOffset(); });
    m_selectAllAction =
        documentAction(tr("Select &All"), [](DocumentView* doc) { doc->selectAll(); }, QKeySequence::SelectAll);
    const auto lineAction = [&](const QString& text, LineOperation operation, const QKeySequence& shortcut = {}) {
        m_lineActions << documentAction(
            text, [operation](DocumentView* doc) { doc->applyLineOperation(operation); }, shortcut);
    };
    lineAction(tr("Sort &Ascending"), LineOperation::SortAscending);
    lineAction(tr("Sort &Descending"), LineOperation::SortDescending);
    lineAction(tr("Remove D&uplicate Lines"), LineOperation::RemoveDuplicates);
    lineAction(tr("Remove &Empty Lines"), LineOperation::RemoveEmpty);
    lineAction(tr("&Trim Trailing Whitespace"), LineOperation::TrimTrailingWhitespace);
    lineAction(tr("Move Line U&p"), LineOperation::MoveUp, QKeySequence(Qt::ALT | Qt::Key_Up));
    lineAction(tr("Move Line Do&wn"), LineOperation::MoveDown, QKeySequence(Qt::ALT | Qt::Key_Down));
    lineAction(tr("Dup&licate Line"), LineOperation::Duplicate, QKeySequence(Qt::CTRL | Qt::Key_D));

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
    m_replaceAction = new QAction(tr("R&eplace…"), this);
    m_replaceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(m_replaceAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            doc->showFindBar(true);
    });
    m_filterAction = new QAction(tr("Fi&lter Lines…"), this);
    m_filterAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    m_filterAction->setToolTip(tr("Show only the lines that match a rule, written by hand or by the AI"));
    connect(m_filterAction, &QAction::triggered, this, [this] {
        if (DocumentView* doc = currentDocument())
            doc->showFilterPanel();
    });
    m_findInFilesAction = new QAction(tr("Find in F&iles…"), this);
    m_findInFilesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    m_findInFilesAction->setToolTip(tr("Search every file in a folder, such as all the logs of a program"));
    connect(m_findInFilesAction, &QAction::triggered, this, &MainWindow::showFindInFiles);
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
    m_fontAction = new QAction(tr("&Editor Font…"), this);
    m_fontAction->setToolTip(tr("The fixed-width font of the text and hex views"));
    connect(m_fontAction, &QAction::triggered, this, &MainWindow::chooseEditorFont);

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
    m_compareSavedAction = new QAction(tr("With the &Saved Version"), this);
    m_compareSavedAction->setToolTip(tr("What has been changed in the document since it was saved"));
    connect(m_compareSavedAction, &QAction::triggered, this, &MainWindow::compareWithSaved);
    m_compareClipboardAction = new QAction(tr("With the Clip&board"), this);
    connect(m_compareClipboardAction, &QAction::triggered, this, &MainWindow::compareWithClipboard);
    m_compareFileAction = new QAction(tr("With Another &File…"), this);
    connect(m_compareFileAction, &QAction::triggered, this, &MainWindow::compareWithFile);
    m_compareTwoAction = new QAction(tr("&Two Files…"), this);
    connect(m_compareTwoAction, &QAction::triggered, this, &MainWindow::compareTwoFiles);

    m_aiFolderAction = new QAction(tr("What Is in This Fol&der?"), this);
    m_aiFolderAction->setToolTip(tr("The AI is told what the open folder holds: names, sizes and kinds of files, "
                                    "and the first lines of a few. The files themselves are not sent."));
    connect(m_aiFolderAction, &QAction::triggered, this, [this] { m_folders->summarize(m_folders->folder()); });
    // The chapter being read, by the contents of the book.
    m_aiChapterSummaryAction = new QAction(tr("Summarize This &Chapter"), this);
    connect(m_aiChapterSummaryAction, &QAction::triggered, this, [this] {
        m_contentsDock->show(); // where the chapters are found, and where it says if there are none
        m_contents->askAboutCurrentChapter(ContentsPanel::ChapterAction::Summarize);
    });
    m_aiChapterTranslationAction = new QAction(tr("Translate This C&hapter"), this);
    connect(m_aiChapterTranslationAction, &QAction::triggered, this, [this] {
        m_contentsDock->show();
        m_contents->askAboutCurrentChapter(ContentsPanel::ChapterAction::Translate);
    });
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
    file->addAction(m_newAction);
    file->addAction(m_openAction);
    file->addAction(m_openFolderAction);
    m_recentMenu = file->addMenu(tr("Recen&t Files"));
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::populateRecentMenu);
    m_recentFoldersMenu = file->addMenu(tr("Recent Fold&ers"));
    connect(m_recentFoldersMenu, &QMenu::aboutToShow, this, &MainWindow::populateRecentFoldersMenu);
    QMenu* compareMenu = file->addMenu(tr("Co&mpare"));
    compareMenu->addActions({m_compareSavedAction, m_compareClipboardAction, m_compareFileAction});
    compareMenu->addSeparator();
    compareMenu->addAction(m_compareTwoAction);
    file->addAction(m_saveAction);
    file->addAction(m_saveAsAction);
    file->addAction(m_reloadAction);
    file->addAction(m_closeAction);
    file->addSeparator();
    file->addAction(m_quitAction);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(m_undoAction);
    edit->addAction(m_redoAction);
    edit->addSeparator();
    edit->addAction(m_cutAction);
    edit->addAction(m_copyAction);
    edit->addAction(m_pasteAction);
    edit->addAction(m_copyHexAction);
    edit->addAction(m_copyTextAction);
    edit->addAction(m_copyCArrayAction);
    edit->addAction(m_copyOffsetAction);
    edit->addSeparator();
    edit->addAction(m_selectAllAction);
    // On the selected lines, or on all of them.
    QMenu* lines = edit->addMenu(tr("Line Operation&s"));
    lines->addActions(m_lineActions.first(5));
    lines->addSeparator();
    lines->addActions(m_lineActions.sliced(5));
    edit->addSeparator();
    edit->addAction(m_findAction);
    edit->addAction(m_findNextAction);
    edit->addAction(m_findPreviousAction);
    edit->addAction(m_replaceAction);
    edit->addAction(m_findInFilesAction);
    edit->addAction(m_filterAction);
    edit->addAction(m_goToAction);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addActions(m_viewModeGroup->actions());
    view->addSeparator();
    view->addAction(m_readingAction);
    QAction* showContents = m_contentsDock->toggleViewAction();
    showContents->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    view->addAction(showContents);
    QAction* showFolder = m_folderDock->toggleViewAction();
    showFolder->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)); // D for directory
    view->addAction(showFolder);
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
    // Filled when first opened: listing the schemes loads the highlighting definitions.
    m_schemeMenu = view->addMenu(tr("&Color Scheme"));
    connect(m_schemeMenu, &QMenu::aboutToShow, this, &MainWindow::populateSchemeMenu);
    view->addAction(m_fontAction);
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
    ai->addAction(m_aiFolderAction);
    ai->addSeparator();
    ai->addAction(m_aiChapterSummaryAction);
    ai->addAction(m_aiChapterTranslationAction);
    ai->addSeparator();
    ai->addAction(m_aiSettingsAction);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_aboutAction);
    help->addAction(m_aboutQtAction);
}

void MainWindow::createCodecMenu()
{
    // Grouped by script; every codec here exists in core::knownCodecs(). (A vector, not an
    // initializer_list: inside a pair, that would outlive the array it points to.)
    const std::pair<QString, std::vector<std::string_view>> groups[] = {
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
    auto* saveMenu = new QMenu(tr("Save with Encoding"), menu);
    m_codecGroup = new QActionGroup(this);
    for (const auto& [title, names] : groups) {
        menu->addSection(title);
        saveMenu->addSection(title);
        for (const std::string_view name : names) {
            const core::TextCodec* codec = core::findCodec(name);
            QAction* action = menu->addAction(codecName(*codec));
            action->setCheckable(true);
            m_codecGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, codec] { setCodec(*codec); });
            QAction* save = saveMenu->addAction(codecName(*codec));
            save->setData(QString::fromLatin1(codec->name.data()));
            connect(save, &QAction::triggered, this, [this, codec] { saveWithCodec(*codec); });
        }
    }
    menu->addSeparator();
    menu->addMenu(saveMenu);
    // Saving needs an editable document with a file, in a codec other than its own.
    connect(saveMenu, &QMenu::aboutToShow, this, [this, saveMenu] {
        const DocumentView* doc = currentDocument();
        const bool possible = doc && doc->isEditable() && !doc->isUntitled();
        const QString current = doc ? QString::fromLatin1(doc->textDocument()->codec().name.data()) : QString();
        for (QAction* action : saveMenu->actions())
            action->setEnabled(possible && action->data().toString() != current);
    });
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

void MainWindow::populateSchemeMenu()
{
    if (m_schemeGroup) {
        for (QAction* action : m_schemeGroup->actions())
            action->setChecked(action->data().toString() == colorScheme());
        return;
    }
    m_schemeGroup = new QActionGroup(this);
    const auto add = [this](const QString& text, const QString& name) {
        QAction* action = m_schemeMenu->addAction(text);
        action->setCheckable(true);
        action->setData(name);
        action->setChecked(name == colorScheme());
        m_schemeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, name] { chooseColorScheme(name); });
    };
    add(tr("Follow the &System"), QString());
    bool dark = false;
    m_schemeMenu->addSection(tr("Light"));
    for (const ColorScheme& scheme : availableColorSchemes()) {
        if (scheme.dark && !dark) {
            m_schemeMenu->addSection(tr("Dark"));
            dark = true;
        }
        add(scheme.displayName, scheme.name);
    }
}

void MainWindow::chooseColorScheme(const QString& name)
{
    setColorScheme(name);
    QSettings().setValue(QStringLiteral("view/colorScheme"), colorScheme());
}

void MainWindow::chooseEditorFont()
{
    QFont current = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (const DocumentView* doc = currentDocument())
        current = doc->textView()->font();
    bool ok = false;
    const QFont font =
        QFontDialog::getFont(&ok, current, this, tr("Editor Font"), QFontDialog::MonospacedFonts);
    if (!ok)
        return;
    m_fontFamily = font.family();
    const int pointSize = font.pointSize() > 0 ? font.pointSize() : m_pointSize;
    m_pointSize = std::clamp(pointSize, kMinPointSize, kMaxPointSize);
    applyViewSettingsToTabs();
    QSettings settings;
    settings.setValue(QStringLiteral("view/fontFamily"), m_fontFamily);
    settings.setValue(QStringLiteral("view/pointSize"), m_pointSize);
}

void MainWindow::readSettings()
{
    QSettings settings;
    setColorScheme(settings.value(QStringLiteral("view/colorScheme")).toString());
    m_fontFamily = settings.value(QStringLiteral("view/fontFamily")).toString();
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray()))
        resize(1100, 700);
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());
    m_lastDirectory = settings.value(QStringLiteral("files/lastDirectory"), QDir::homePath()).toString();
    m_pointSize = std::clamp(settings.value(QStringLiteral("view/pointSize"), m_defaultPointSize).toInt(),
                             kMinPointSize, kMaxPointSize);
    m_inspector->setBigEndian(settings.value(QStringLiteral("inspector/bigEndian"), false).toBool());
    setBytesPerRow(settings.value(QStringLiteral("view/bytesPerRow"), 16).toInt());
    setWordWrap(settings.value(QStringLiteral("view/wordWrap"), true).toBool());
    for (const QVariant& value : settings.value(QStringLiteral("files/recent")).toList()) {
        DocumentView::State state = stateFromVariant(value);
        if (!state.path.isEmpty())
            m_recentFiles.append(std::move(state));
    }
}

void MainWindow::writeSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    settings.setValue(QStringLiteral("files/lastDirectory"), m_lastDirectory);
    settings.setValue(QStringLiteral("view/pointSize"), m_pointSize);
    settings.setValue(QStringLiteral("view/fontFamily"), m_fontFamily);
    settings.setValue(QStringLiteral("view/bytesPerRow"), m_bytesPerRow);
    settings.setValue(QStringLiteral("view/wordWrap"), m_wordWrap);
    settings.setValue(QStringLiteral("inspector/bigEndian"), m_inspector->isBigEndian());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Unsaved changes are not asked about: they stay in their backups and come back next time,
    // as in Notepad++. Only a document whose backup cannot be written gets the question.
    m_backupTimer.stop();
    QSet<const DocumentView*> kept;
    for (int i = 0; i < m_tabs->count(); ++i) {
        DocumentView* doc = document(i);
        if (!doc || !doc->isModified())
            continue;
        BackupInfo& backup = m_backupInfo[doc];
        if (m_backups.writeNow(backup.id, backupMeta(doc, i), doc->textDocument()->bytes())) {
            backup.written = true;
            backup.dirty = false;
            kept.insert(doc);
            continue;
        }
        if (!confirmClose(doc)) {
            event->ignore();
            return;
        }
        if (backup.written) // an older backup must not bring back what was just given up
            m_backups.remove(backup.id);
        backup.written = false;
    }

    // The tabs to reopen, in order; a new document left empty is not among them.
    QVariantList tabs;
    int current = -1;
    for (int i = 0; i < m_tabs->count(); ++i) {
        const DocumentView* doc = document(i);
        if (!doc)
            continue; // a comparison is not kept
        rememberDocument(doc, doc != m_previewDoc);
        QVariantMap tab;
        if (!doc->isUntitled())
            tab.insert(QStringLiteral("path"), doc->filePath());
        if (kept.contains(doc))
            tab.insert(QStringLiteral("backup"), m_backupInfo.value(doc).id);
        if (tab.isEmpty())
            continue;
        if (doc == m_previewDoc)
            tab.insert(QStringLiteral("preview"), true);
        if (doc == currentDocument())
            current = static_cast<int>(tabs.size());
        tabs << tab;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("session/tabs"), tabs);
    settings.setValue(QStringLiteral("session/currentTab"), current);
    settings.setValue(QStringLiteral("session/folder"), m_folders->folder());
    settings.remove(QStringLiteral("session/open")); // as M5 kept it
    settings.remove(QStringLiteral("session/current"));
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
            openPath(url.toLocalFile());
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
    QString error;
    if (showFile(path, false, &error))
        return true;
    QMessageBox::warning(this, tr("Cannot Open File"), error);
    return false;
}

bool MainWindow::previewFile(const QString& path)
{
    QString error;
    if (showFile(path, true, &error))
        return true;
    statusBar()->showMessage(error.simplified(), kStatusTimeoutMs);
    return false;
}

bool MainWindow::openFolder(const QString& path)
{
    if (!m_folders->setFolder(path)) {
        QMessageBox::warning(this, tr("Open Folder"),
                             tr("%1 is not a folder, or no longer exists.").arg(QDir::toNativeSeparators(path)));
        return false;
    }
    m_folderDock->show();
    m_folderDock->raise();
    m_lastDirectory = m_folders->folder();
    return true;
}

bool MainWindow::openPath(const QString& path)
{
    return QFileInfo(path).isDir() ? openFolder(path) : openFile(path);
}

DocumentView* MainWindow::showFile(const QString& path, bool preview, QString* error)
{
    const QFileInfo info(path);
    const QString filePath = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (document(i) && document(i)->filePath() == filePath) {
            m_tabs->setCurrentIndex(i);
            if (!preview)
                keepDocument(document(i));
            return document(i);
        }
    }

    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(filePath), ec);
    if (!source) {
        *error = tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(filePath), describeError(ec));
        return nullptr;
    }

    const core::FileTypeInfo type = core::detectFileType(source->bytes());
    const core::TextCodec& codec = core::defaultCodec(type, source->bytes(), preferredLegacyCodec());
    auto* doc = new DocumentView(source, type, codec);
    // A preview takes the place of the one before, unless that one was kept meanwhile.
    DocumentView* replaced = preview && m_previewDoc && !m_previewDoc->isModified() ? m_previewDoc.data() : nullptr;
    addDocument(doc, replaced ? m_tabs->indexOf(replaced) : -1, !preview);
    if (replaced) {
        rememberDocument(replaced, false);
        discardDocument(replaced);
    }
    for (const DocumentView::State& state : std::as_const(m_recentFiles)) {
        if (state.path == doc->filePath()) {
            doc->restoreState(state);
            break;
        }
    }
    if (preview) {
        m_previewDoc = doc;
        updateDocumentTitle(doc);
    } else {
        moveToFrontOfRecent(doc->filePath());
        m_lastDirectory = QFileInfo(filePath).absolutePath();
    }
    return doc;
}

void MainWindow::keepDocument(DocumentView* doc)
{
    if (!doc || doc != m_previewDoc)
        return;
    m_previewDoc = nullptr;
    updateDocumentTitle(doc);
    if (!doc->isUntitled()) {
        moveToFrontOfRecent(doc->filePath());
        m_lastDirectory = QFileInfo(doc->filePath()).absolutePath();
    }
}

void MainWindow::discardDocument(DocumentView* doc)
{
    const int index = m_tabs->indexOf(doc);
    if (index < 0)
        return;
    if (doc == m_previewDoc)
        m_previewDoc = nullptr;
    dropBackup(doc);
    // What it reports while going must not reach this window.
    doc->disconnect(this);
    doc->textView()->verticalScrollBar()->disconnect(this);
    m_tabs->removeTab(index);
    doc->deleteLater();
    if (m_tabs->count() == 0)
        m_stack->setCurrentWidget(m_welcome);
}

void MainWindow::restoreSession(bool reopenTabs)
{
    QList<BackupStore::Entry> orphans = m_backups.takeOrphans();
    QList<DocumentView*> restored;
    QStringList changedOnDisk;
    QStringList gone;
    const auto bringBack = [&](const BackupStore::Entry& entry) {
        if (DocumentView* doc = restoreBackup(entry, &changedOnDisk, &gone))
            restored << doc;
    };

    int current = -1;
    if (reopenTabs) {
        const QSettings settings;
        if (const QString folder = settings.value(QStringLiteral("session/folder")).toString();
            !folder.isEmpty() && QFileInfo(folder).isDir())
            m_folders->setFolder(folder);
        QVariantList tabs = settings.value(QStringLiteral("session/tabs")).toList();
        int currentTab = settings.value(QStringLiteral("session/currentTab"), -1).toInt();
        if (!settings.contains(QStringLiteral("session/tabs"))) { // as M5 kept it: paths only
            const QStringList paths = settings.value(QStringLiteral("session/open")).toStringList();
            for (const QString& path : paths)
                tabs << QVariantMap{{QStringLiteral("path"), path}};
            const QString currentPath = settings.value(QStringLiteral("session/current")).toString();
            currentTab = static_cast<int>(paths.indexOf(currentPath));
        }
        for (qsizetype i = 0; i < tabs.size(); ++i) {
            const QVariantMap tab = tabs[i].toMap();
            const QString id = tab.value(QStringLiteral("backup")).toString();
            const QString path = tab.value(QStringLiteral("path")).toString();
            const auto backup = std::find_if(orphans.begin(), orphans.end(),
                                             [&id](const BackupStore::Entry& entry) { return entry.id == id; });
            if (!id.isEmpty() && backup != orphans.end()) {
                bringBack(*backup);
                orphans.erase(backup);
            } else if (!path.isEmpty() && QFileInfo::exists(path)) { // files gone since are left out
                if (tab.value(QStringLiteral("preview")).toBool()) {
                    QString error;
                    showFile(path, true, &error);
                } else {
                    openFile(path);
                }
            } else {
                continue;
            }
            if (i == currentTab)
                current = m_tabs->currentIndex();
        }
    }
    // Left by a crash, by another window, or by a start with files to open: they come back too.
    for (const BackupStore::Entry& entry : std::as_const(orphans))
        bringBack(entry);

    // What came back is backed up here before the old copies go; if that fails, they stay for
    // another try.
    bool safe = true;
    for (DocumentView* doc : std::as_const(restored)) {
        BackupInfo& backup = m_backupInfo[doc];
        backup.written = m_backups.writeNow(backup.id, backupMeta(doc, m_tabs->indexOf(doc)),
                                            doc->textDocument()->bytes());
        backup.dirty = !backup.written;
        safe = safe && backup.written;
    }
    if (safe)
        m_backups.discardTaken();
    if (current >= 0)
        m_tabs->setCurrentIndex(current);

    QStringList notes;
    if (!restored.isEmpty())
        notes << tr("Unsaved changes are back in %n document(s)", "", static_cast<int>(restored.size()));
    if (!changedOnDisk.isEmpty())
        notes << tr("%1 changed on disk since; saving writes over that")
                     .arg(changedOnDisk.join(QStringLiteral(", ")));
    if (!gone.isEmpty())
        notes << tr("%1 no longer exists; its changes are in a new document")
                     .arg(gone.join(QStringLiteral(", ")));
    if (!notes.isEmpty())
        statusBar()->showMessage(notes.join(QStringLiteral(". ")), kNoticeTimeoutMs);
}

DocumentView* MainWindow::restoreBackup(const BackupStore::Entry& entry, QStringList* changedOnDisk,
                                        QStringList* gone)
{
    QFile file(entry.contentPath);
    if (!file.open(QIODevice::ReadOnly))
        return nullptr;
    const QByteArray content = file.readAll();
    const QJsonObject& meta = entry.meta;
    const QString path = meta.value(QStringLiteral("path")).toString();
    const core::TextCodec* codec = core::findCodec(meta.value(QStringLiteral("codec")).toString().toStdString());
    if (!codec)
        codec = &core::utf8Codec();

    // The file it was made on, if it is still there and can still be edited here.
    DocumentView* doc = nullptr;
    if (!path.isEmpty()) {
        std::error_code ec;
        if (std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec)) {
            auto* candidate = new DocumentView(source, core::detectFileType(source->bytes()), *codec);
            if (candidate->isEditable())
                doc = candidate;
            else
                delete candidate;
        }
    }
    const qint64 baseSize = meta.value(QStringLiteral("baseSize")).toInteger(-1);
    const qint64 baseModified = meta.value(QStringLiteral("baseModified")).toInteger();
    if (doc) {
        const QFileInfo info(path);
        if (baseSize >= 0 && (info.size() != baseSize || info.lastModified().toMSecsSinceEpoch() != baseModified))
            *changedOnDisk << info.fileName();
    } else {
        // A new document, or one whose file is gone: its changes go to a new document.
        std::shared_ptr<const core::FileSource> empty =
            core::FileSource::fromBytes({}, std::make_shared<const std::vector<std::uint8_t>>());
        doc = new DocumentView(empty, core::detectFileType(empty->bytes()), *codec);
        QString title = meta.value(QStringLiteral("title")).toString();
        if (!path.isEmpty()) {
            title = QFileInfo(path).fileName();
            *gone << title;
        }
        if (title.isEmpty())
            title = tr("Untitled %1").arg(++m_untitledCount);
        // Later new documents count on from the restored ones.
        const QString pattern = tr("Untitled %1");
        const qsizetype number = pattern.indexOf(QStringLiteral("%1"));
        const QRegularExpression untitled(QLatin1Char('^') + QRegularExpression::escape(pattern.left(number)) +
                                          QStringLiteral("(\\d+)") +
                                          QRegularExpression::escape(pattern.mid(number + 2)) + QLatin1Char('$'));
        if (const QRegularExpressionMatch match = untitled.match(title); number >= 0 && match.hasMatch())
            m_untitledCount = std::max(m_untitledCount, match.captured(1).toInt());
        doc->setUntitledTitle(title);
    }

    BackupInfo& backup = m_backupInfo[doc];
    backup.id = entry.id;
    if (!doc->isUntitled()) {
        backup.baseSize = baseSize;
        backup.baseModified = baseModified;
    }
    addDocument(doc);
    doc->restoreContent({reinterpret_cast<const std::uint8_t*>(content.constData()),
                         static_cast<std::size_t>(content.size())});
    if (meta.value(QStringLiteral("codecChosen")).toBool())
        doc->setCodec(*codec);
    DocumentView::State state;
    state.path = doc->filePath();
    state.cursor = static_cast<std::uint64_t>(meta.value(QStringLiteral("cursor")).toInteger());
    state.top = static_cast<std::uint64_t>(meta.value(QStringLiteral("top")).toInteger());
    const int mode = std::clamp(meta.value(QStringLiteral("mode")).toInt(), 0, 2);
    state.mode = static_cast<DocumentView::ViewMode>(mode);
    doc->restoreState(state);
    return doc;
}

void MainWindow::noteBackupChange(DocumentView* doc)
{
    BackupInfo& backup = m_backupInfo[doc];
    if (!doc->isModified()) {
        // Saved, undone to what is saved, or reloaded: nothing to keep any more.
        if (backup.written)
            m_backups.remove(backup.id);
        backup.written = false;
        backup.dirty = false;
        backup.baseSize = -1;
        return;
    }
    if (backup.baseSize < 0 && !doc->isUntitled()) {
        const QFileInfo file(doc->filePath());
        backup.baseSize = file.size();
        backup.baseModified = file.lastModified().toMSecsSinceEpoch();
    }
    backup.dirty = true;
    if (!m_backupTimer.isActive())
        m_backupTimer.start(); // not restarted by every keystroke, so typing on does not delay it
}

void MainWindow::writeBackups()
{
    m_backupTimer.stop();
    for (int i = 0; i < m_tabs->count(); ++i) {
        DocumentView* doc = document(i);
        if (!doc)
            continue;
        BackupInfo& backup = m_backupInfo[doc];
        if (!backup.dirty || !doc->isModified())
            continue;
        // The snapshot cannot change under the writer: every edit makes a new one.
        m_backups.write(backup.id, backupMeta(doc, i), doc->textDocument()->source());
        backup.dirty = false;
        backup.written = true;
    }
}

void MainWindow::flushBackups()
{
    writeBackups();
    m_backups.flush();
}

void MainWindow::dropBackup(DocumentView* doc)
{
    // Closed after the question: saved, or the changes were given up.
    const BackupInfo backup = m_backupInfo.take(doc);
    if (backup.written)
        m_backups.remove(backup.id);
}

QJsonObject MainWindow::backupMeta(const DocumentView* doc, int order) const
{
    const DocumentView::State state = doc->state();
    const BackupInfo backup = m_backupInfo.value(doc);
    return {{QStringLiteral("path"), doc->isUntitled() ? QString() : doc->filePath()},
            {QStringLiteral("title"), doc->title()},
            {QStringLiteral("codec"), QString::fromLatin1(doc->textDocument()->codec().name.data())},
            {QStringLiteral("codecChosen"), !state.codec.isEmpty()},
            {QStringLiteral("cursor"), static_cast<qint64>(state.cursor)},
            {QStringLiteral("top"), static_cast<qint64>(state.top)},
            {QStringLiteral("mode"), static_cast<int>(state.mode)},
            {QStringLiteral("baseSize"), backup.baseSize},
            {QStringLiteral("baseModified"), backup.baseModified},
            {QStringLiteral("order"), order}};
}

void MainWindow::rememberDocument(const DocumentView* doc, bool toFront)
{
    if (!doc || doc->isUntitled())
        return;
    const DocumentView::State state = doc->state();
    if (!toFront) {
        for (DocumentView::State& remembered : m_recentFiles) {
            if (remembered.path == state.path) {
                remembered = state;
                saveRecentFiles();
                break;
            }
        }
        return;
    }
    m_recentFiles.removeIf([&state](const DocumentView::State& s) { return s.path == state.path; });
    m_recentFiles.prepend(state);
    if (m_recentFiles.size() > kMaxRememberedFiles)
        m_recentFiles.resize(kMaxRememberedFiles);
    saveRecentFiles();
}

void MainWindow::moveToFrontOfRecent(const QString& path)
{
    DocumentView::State state;
    state.path = path;
    for (qsizetype i = 0; i < m_recentFiles.size(); ++i) {
        if (m_recentFiles[i].path == path) {
            state = m_recentFiles.takeAt(i);
            break;
        }
    }
    m_recentFiles.prepend(state);
    if (m_recentFiles.size() > kMaxRememberedFiles)
        m_recentFiles.resize(kMaxRememberedFiles);
    saveRecentFiles();
}

void MainWindow::saveRecentFiles() const
{
    QVariantList list;
    for (const DocumentView::State& state : m_recentFiles)
        list << toVariant(state);
    QSettings().setValue(QStringLiteral("files/recent"), list);
}

void MainWindow::populateRecentMenu()
{
    m_recentMenu->clear();
    const QString home = QDir::homePath();
    for (qsizetype i = 0; i < std::min(m_recentFiles.size(), kRecentFilesInMenu); ++i) {
        const QString path = m_recentFiles[i].path;
        const QFileInfo info(path);
        QString folder = QDir::toNativeSeparators(info.absolutePath());
        if (folder.startsWith(QDir::toNativeSeparators(home)))
            folder.replace(0, QDir::toNativeSeparators(home).size(), QStringLiteral("~"));
        // 1 to 9, then 0, as the keys to pick them.
        const QString key = i < 9 ? QString::number(i + 1) : QStringLiteral("0");
        QString name = info.fileName();
        name.replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction* action = m_recentMenu->addAction(QStringLiteral("&%1  %2  (%3)").arg(key, name, folder));
        action->setToolTip(QDir::toNativeSeparators(path));
        connect(action, &QAction::triggered, this, [this, path] {
            if (!QFileInfo::exists(path)) {
                m_recentFiles.removeIf([&path](const DocumentView::State& s) { return s.path == path; });
                saveRecentFiles();
                statusBar()->showMessage(tr("%1 no longer exists").arg(QDir::toNativeSeparators(path)),
                                         kStatusTimeoutMs);
                return;
            }
            openFile(path);
        });
    }
    if (m_recentFiles.isEmpty()) {
        m_recentMenu->addAction(tr("No Recent Files"))->setEnabled(false);
        return;
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("&Clear List"), this, [this] {
        m_recentFiles.clear();
        saveRecentFiles();
    });
}

void MainWindow::newDocument()
{
    std::shared_ptr<const core::FileSource> source =
        core::FileSource::fromBytes({}, std::make_shared<const std::vector<std::uint8_t>>());
    auto* doc = new DocumentView(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc->setUntitledTitle(tr("Untitled %1").arg(++m_untitledCount));
    addDocument(doc);
}

void MainWindow::addDocument(DocumentView* doc, int index, bool focus)
{
    applyViewSettings(doc);
    doc->setContextActions({m_aiExplainAction, m_aiTranslateAction, m_aiSummarizeAction, m_aiAskAction},
                           {m_aiExplainAction, m_aiSummarizeAction, m_aiAskAction, m_aiIdentifyAction});
    connect(doc, &DocumentView::stateChanged, this, [this, doc] { documentStateChanged(doc); });
    connect(doc, &DocumentView::modificationChanged, this, [this, doc](bool modified) {
        if (modified)
            keepDocument(doc); // an edited preview stays
        updateDocumentTitle(doc);
    });
    BackupInfo& backup = m_backupInfo[doc];
    if (backup.id.isEmpty())
        backup.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    connect(doc, &DocumentView::modificationChanged, this, [this, doc] { noteBackupChange(doc); });
    connect(doc, &DocumentView::sourceChanged, this, [this, doc] { noteBackupChange(doc); });
    connect(doc, &DocumentView::pathChanged, this, [this, doc] {
        updateDocumentTitle(doc);
        if (doc == currentDocument()) {
            updateFileStatus();
            updateActions(); // a new document saved has a file to reload now
        }
    });
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
    connect(doc, &DocumentView::aiSettingsRequested, this, &MainWindow::showAiSettings);
    connect(doc, &DocumentView::readingModeChanged, this, [this, doc] { applyViewSettings(doc); });
    // How far a book is read shows as it scrolls.
    connect(doc->textView()->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, doc] {
        if (doc == currentDocument() && doc->isReadingMode())
            updateCursorStatus();
    });
    connect(doc, &DocumentView::statusMessage, this,
            [this](const QString& message) { statusBar()->showMessage(message, kStatusTimeoutMs); });

    const int at = index < 0 ? m_tabs->addTab(doc, doc->title()) : m_tabs->insertTab(index, doc, doc->title());
    updateDocumentTitle(doc);
    m_tabs->setCurrentIndex(at);
    m_stack->setCurrentWidget(m_tabs);
    if (focus)
        doc->setFocus();
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
    DocumentView* doc = document(index);
    if (!widget || (doc && !confirmClose(doc)))
        return;
    if (!doc) { // a comparison
        m_tabs->removeTab(index);
        widget->deleteLater();
        if (m_tabs->count() == 0)
            m_stack->setCurrentWidget(m_welcome);
        return;
    }
    rememberDocument(doc, doc != m_previewDoc);
    discardDocument(doc);
}

bool MainWindow::saveDocument(DocumentView* doc)
{
    if (doc->isUntitled())
        return saveDocumentAs(doc);
    QString error;
    if (!doc->save(&error)) {
        QMessageBox::warning(this, tr("Cannot Save"),
                             tr("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(doc->filePath()), error));
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(doc->title()), kStatusTimeoutMs);
    return true;
}

bool MainWindow::saveDocumentAs(DocumentView* doc)
{
    // A new document is named after its title; one recovered from a file that is gone has
    // the file's name, extension included.
    const QString title = doc->title();
    const QString name = title.contains(QLatin1Char('.')) ? title : title + QStringLiteral(".txt");
    const QString suggestion = doc->isUntitled() ? QDir(m_lastDirectory).filePath(name) : doc->filePath();
    const QString path = QFileDialog::getSaveFileName(this, tr("Save As"), suggestion);
    if (path.isEmpty())
        return false;
    QString error;
    if (!doc->saveAs(path, &error)) {
        QMessageBox::warning(this, tr("Cannot Save"),
                             tr("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(path), error));
        return false;
    }
    keepDocument(doc);
    m_lastDirectory = QFileInfo(path).absolutePath();
    moveToFrontOfRecent(doc->filePath());
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), kStatusTimeoutMs);
    return true;
}

bool MainWindow::confirmClose(DocumentView* doc)
{
    if (!doc || !doc->isModified())
        return true;
    m_tabs->setCurrentWidget(doc);
    const auto answer = QMessageBox::question(
        this, tr("Unsaved Changes"),
        tr("Save your changes to %1 before closing it?").arg(doc->title()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return saveDocument(doc);
    return answer == QMessageBox::Discard;
}

void MainWindow::updateDocumentTitle(DocumentView* doc)
{
    const int index = m_tabs->indexOf(doc);
    if (index < 0)
        return;
    const QString name = doc->title();
    const bool preview = doc == m_previewDoc;
    m_tabs->setTabText(index, doc->isModified() ? QStringLiteral("*") + name : name);
    QString tip = doc->isUntitled() ? name : QDir::toNativeSeparators(doc->filePath());
    if (preview)
        tip += QLatin1Char('\n') + tr("Preview: the next file clicked in the folder takes this tab. Double-click the "
                                      "tab to keep it.");
    m_tabs->setTabToolTip(index, tip);
    // Dimmed while it is a preview; an invalid colour is the usual one.
    m_tabs->tabBar()->setTabTextColor(index, preview ? palette().color(QPalette::PlaceholderText) : QColor());
    if (doc == currentDocument()) {
        setWindowTitle(tr("%1[*] — AIEdit").arg(name));
        setWindowModified(doc->isModified());
    }
}

void MainWindow::currentTabChanged()
{
    DocumentView* doc = currentDocument();
    m_inspector->setSource(doc ? doc->source() : nullptr);
    m_contents->setDocument(doc);
    if (doc) {
        updateDocumentTitle(doc);
    } else {
        const CompareTab* compareTab = currentCompareTab();
        setWindowTitle(compareTab ? tr("%1 — AIEdit").arg(compareTab->title()) : QStringLiteral("AIEdit"));
        setWindowModified(false);
    }
    m_compareStatus->setVisible(currentCompareTab() != nullptr);
    if (const CompareTab* compareTab = currentCompareTab())
        m_compareStatus->setText(compareTab->statusText());
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
        m_readingAction->setChecked(doc->isReadingMode());
    }
    updateEditActions(doc);
    // Explaining, translating and summarizing need a selection; asking and identifying do not.
    const bool selected = doc && doc->hasSelection();
    for (QAction* action : {m_aiExplainAction, m_aiTranslateAction, m_aiSummarizeAction})
        action->setEnabled(selected);
    updateCursorStatus();
    updateTextStatus();
    m_aiPanel->refreshContext();
}

void MainWindow::updateEditActions(DocumentView* doc)
{
    // Text needs a selection to copy; the hex view always has the byte at its cursor selected.
    const bool canCopy = doc && (doc->isTextActive() ? doc->textView()->hasSelection() : doc->source()->size() > 0);
    for (QAction* action : {m_copyAction, m_copyHexAction, m_copyTextAction, m_copyCArrayAction})
        action->setEnabled(canCopy);
    m_selectAllAction->setEnabled(doc && doc->source()->size() > 0);

    const bool editing = doc && doc->isEditable() && doc->isTextActive();
    const QMimeData* clipboard = QGuiApplication::clipboard()->mimeData();
    m_undoAction->setEnabled(editing && doc->textDocument()->canUndo());
    m_redoAction->setEnabled(editing && doc->textDocument()->canRedo());
    m_cutAction->setEnabled(editing && doc->textView()->hasSelection());
    m_pasteAction->setEnabled(editing && clipboard && clipboard->hasText());
    for (QAction* action : std::as_const(m_lineActions))
        action->setEnabled(editing);
}

void MainWindow::updateActions()
{
    DocumentView* doc = currentDocument();
    const bool hasDocument = doc != nullptr;
    for (QAction* action : {m_closeAction, m_saveAction, m_saveAsAction, m_copyOffsetAction, m_goToAction, m_findAction,
                            m_findNextAction, m_findPreviousAction, m_replaceAction, m_filterAction, m_aiAskAction,
                            m_aiIdentifyAction, m_readingAction}) {
        action->setEnabled(hasDocument);
    }
    // A new document has no file on disk to reload or follow yet.
    const bool onDisk = hasDocument && !doc->isUntitled();
    m_reloadAction->setEnabled(onDisk);
    m_followAction->setEnabled(onDisk);
    m_viewModeGroup->setEnabled(hasDocument);
    m_aiFolderAction->setEnabled(!m_folders->folder().isEmpty());
    m_compareSavedAction->setEnabled(onDisk);
    m_compareClipboardAction->setEnabled(hasDocument);
    m_compareFileAction->setEnabled(hasDocument);
}

void MainWindow::setViewMode(DocumentView::ViewMode mode)
{
    if (DocumentView* doc = currentDocument())
        doc->setViewMode(mode);
}

void MainWindow::saveWithCodec(const core::TextCodec& codec)
{
    DocumentView* doc = currentDocument();
    if (!doc)
        return;
    QString error;
    DocumentView::Conversion result = doc->saveWithCodec(codec, false, &error);
    if (result == DocumentView::Conversion::Unreadable) {
        const auto answer = QMessageBox::question(
            this, tr("Save with Encoding"),
            tr("Some bytes in %1 are not valid %2. Saved in %3, each becomes the replacement character �. Save anyway?")
                .arg(doc->title(), codecName(doc->textDocument()->codec()), codecName(codec)),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save)
            return;
        result = doc->saveWithCodec(codec, true, &error);
    }
    switch (result) {
    case DocumentView::Conversion::Saved:
        statusBar()->showMessage(tr("Saved %1 in %2").arg(doc->title(), codecName(codec)), kStatusTimeoutMs);
        break;
    case DocumentView::Conversion::Unwritable:
        QMessageBox::warning(this, tr("Save with Encoding"),
                             tr("%1 cannot hold some of the characters in %2; nothing was changed.")
                                 .arg(codecName(codec), doc->title()));
        break;
    case DocumentView::Conversion::Failed:
        QMessageBox::warning(this, tr("Cannot Save"),
                             tr("Cannot save %1:\n%2").arg(QDir::toNativeSeparators(doc->filePath()), error));
        break;
    case DocumentView::Conversion::Unreadable:
        break;
    }
}

void MainWindow::convertLineEndings(const QString& terminator)
{
    DocumentView* doc = currentDocument();
    if (!doc)
        return;
    const QString name = terminator == QStringLiteral("\r\n") ? QStringLiteral("CRLF")
                         : terminator == QStringLiteral("\r")  ? QStringLiteral("CR")
                                                                : QStringLiteral("LF");
    statusBar()->showMessage(doc->convertLineEndings(terminator) ? tr("Every line now ends with %1").arg(name)
                                                                 : tr("Every line already ends with %1").arg(name),
                             kStatusTimeoutMs);
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
        if (view == doc->textView() && doc->isReadingMode()) {
            view->setFont(readingFont());
            continue;
        }
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (!m_fontFamily.isEmpty())
            font.setFamily(m_fontFamily);
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
        if (DocumentView* doc = document(i))
            doc->hexView()->setBytesPerRow(m_bytesPerRow);
}

void MainWindow::setWordWrap(bool on)
{
    m_wordWrap = on;
    const QSignalBlocker blocker(m_wordWrapAction);
    m_wordWrapAction->setChecked(on);
    for (int i = 0; i < m_tabs->count(); ++i)
        if (DocumentView* doc = document(i))
            doc->textView()->setWordWrap(on);
}

void MainWindow::zoomBy(int steps)
{
    const int pointSize = std::clamp(m_pointSize + steps, kMinPointSize, kMaxPointSize);
    if (pointSize == m_pointSize)
        return;
    m_pointSize = pointSize;
    applyViewSettingsToTabs();
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

void MainWindow::setReadingMode(bool on)
{
    DocumentView* doc = currentDocument();
    if (!doc)
        return;
    doc->setReadingMode(on); // the font follows, through readingModeChanged
    if (on) {
        m_contentsDock->show();
        m_contentsDock->raise();
    }
    doc->setFocus();
}

QFont MainWindow::readingFont() const
{
    // A serif for reading, Chinese ones first; else the font of the interface.
    QFont font = QApplication::font();
    for (const char* family : {"Noto Serif CJK SC", "Source Han Serif SC", "Songti SC", "SimSun", "Noto Serif"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            font.setFamily(QString::fromLatin1(family));
            break;
        }
    }
    font.setPointSize(m_pointSize + 2);
    return font;
}

void MainWindow::askAboutChapter(int action, const QString& title, const QString& text, bool cut)
{
    QString question;
    switch (static_cast<ContentsPanel::ChapterAction>(action)) {
    case ContentsPanel::ChapterAction::Summarize:
        question = tr("Summarize this chapter: what happens, who is in it, and what stands out.");
        break;
    case ContentsPanel::ChapterAction::Translate:
        question = tr("Translate this chapter into %1, paragraph by paragraph. Classical Chinese goes into modern "
                      "vernacular Chinese.")
                       .arg(QLocale().nativeLanguageName());
        break;
    case ContentsPanel::ChapterAction::Explain:
        question =
            tr("Explain this chapter: its background, the difficult words and allusions, and what it means.");
        break;
    }
    const QString size = QLocale().toString(static_cast<qulonglong>(text.size()));
    const QString label = cut ? tr("the first %1 characters of %2").arg(size, title)
                              : tr("%1 (%2 characters)").arg(title, size);
    m_aiDock->show();
    m_aiDock->raise();
    m_aiPanel->askAbout(question, label, text);
}

void MainWindow::showFindInFiles()
{
    if (!m_findInFilesDock->isVisible()) {
        // The open folder, or where the file being read is, and what is selected in it, as a start.
        const QString folder = m_folders->folder();
        if (const DocumentView* doc = currentDocument()) {
            const bool inFolder = !folder.isEmpty() &&
                                  (doc->isUntitled() || doc->filePath().startsWith(folder + QLatin1Char('/')));
            if (inFolder)
                m_findInFiles->setFolder(folder);
            else if (!doc->isUntitled())
                m_findInFiles->setFolder(QFileInfo(doc->filePath()).absolutePath());
            if (doc->isTextActive()) {
                const QString selected = doc->textView()->selectedText();
                if (!selected.isEmpty() && selected.size() <= 256 && !selected.contains(QLatin1Char('\n')))
                    m_findInFiles->setQuery(selected);
            }
        } else if (!folder.isEmpty()) {
            m_findInFiles->setFolder(folder);
        }
        if (m_findInFiles->folder().isEmpty())
            m_findInFiles->setFolder(m_lastDirectory);
    }
    m_findInFilesDock->show();
    m_findInFilesDock->raise();
    m_findInFiles->focusQuery();
}

void MainWindow::openAtLine(const QString& path, std::uint64_t line, std::uint64_t offset)
{
    if (!openFile(path))
        return;
    DocumentView* doc = currentDocument();
    if (doc->viewMode() == DocumentView::ViewMode::Hex)
        doc->setViewMode(DocumentView::ViewMode::Text);
    // In a large file still being indexed, the line may not be there yet: then its offset is,
    // and the document goes there once it is indexed.
    if (!doc->textView()->goToLine(line)) {
        DocumentView::State state = doc->state();
        state.cursor = offset;
        state.top = offset;
        doc->restoreState(state);
    }
    doc->textView()->setFocus();
}

void MainWindow::openAtOffset(const QString& path, std::uint64_t offset)
{
    if (!openFile(path))
        return;
    DocumentView* doc = currentDocument();
    doc->setViewMode(DocumentView::ViewMode::Hex);
    doc->hexView()->goTo(offset);
    doc->hexView()->setFocus();
}

void MainWindow::showFindInFilesIn(const QString& folder)
{
    m_findInFiles->setFolder(folder);
    m_findInFilesDock->show();
    m_findInFilesDock->raise();
    m_findInFiles->focusQuery();
}

void MainWindow::documentsMoved(const QString& from, const QString& to)
{
    const QString prefix = from + QLatin1Char('/');
    const auto moved = [&](const QString& path) -> QString {
        return path == from || path.startsWith(prefix) ? to + path.mid(from.size()) : QString();
    };
    for (int i = 0; i < m_tabs->count(); ++i) {
        DocumentView* doc = document(i);
        if (!doc || doc->isUntitled())
            continue;
        if (const QString path = moved(doc->filePath()); !path.isEmpty() && !doc->followMove(path))
            statusBar()->showMessage(tr("%1 was moved, but cannot be opened where it is now").arg(doc->title()),
                                     kStatusTimeoutMs);
    }
    // What is remembered of the files goes with them.
    for (DocumentView::State& state : m_recentFiles)
        if (const QString path = moved(state.path); !path.isEmpty())
            state.path = path;
    saveRecentFiles();
}

void MainWindow::documentsTrashed(const QString& path)
{
    const QString prefix = path + QLatin1Char('/');
    const auto gone = [&](const QString& file) { return file == path || file.startsWith(prefix); };
    // Unsaved changes stop the trash before it gets here, so the documents can just go.
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        DocumentView* doc = document(i);
        if (doc && !doc->isUntitled() && !doc->isModified() && gone(doc->filePath()))
            discardDocument(doc);
    }
    m_recentFiles.removeIf([&gone](const DocumentView::State& state) { return gone(state.path); });
    saveRecentFiles();
}

void MainWindow::askAboutFolder(const QString& folder, const QString& summary)
{
    QString name = QDir(folder).dirName();
    if (name.isEmpty())
        name = QDir::toNativeSeparators(folder);
    const QString question =
        tr("What is in this folder? Say what it is for, how it is organized and which files matter most, and point "
           "out anything unusual. The summary was made on this computer from the names, sizes and first bytes of the "
           "files; say where it is not enough to tell.");
    const QString label =
        tr("a summary of the folder %1: names, sizes and kinds of files, and the first lines of a few").arg(name);
    m_aiDock->show();
    m_aiDock->raise();
    m_aiPanel->askAbout(question, label, summary);
}

void MainWindow::showOpenFolderDialog()
{
    QString start = m_folders->folder();
    if (start.isEmpty())
        start = m_lastDirectory;
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Open Folder"), start);
    if (!folder.isEmpty())
        openFolder(folder);
}

void MainWindow::populateRecentFoldersMenu()
{
    m_recentFoldersMenu->clear();
    const QString home = QDir::homePath();
    const QStringList folders = m_folders->recentFolders();
    for (qsizetype i = 0; i < std::min(folders.size(), kRecentFilesInMenu); ++i) {
        const QString folder = folders[i];
        QString where = QDir::toNativeSeparators(QFileInfo(folder).absolutePath());
        if (where.startsWith(QDir::toNativeSeparators(home)))
            where.replace(0, QDir::toNativeSeparators(home).size(), QStringLiteral("~"));
        const QString key = i < 9 ? QString::number(i + 1) : QStringLiteral("0");
        QString name = QDir(folder).dirName();
        if (name.isEmpty())
            name = QDir::toNativeSeparators(folder);
        name.replace(QLatin1Char('&'), QStringLiteral("&&"));
        QAction* action = m_recentFoldersMenu->addAction(QStringLiteral("&%1  %2  (%3)").arg(key, name, where));
        action->setToolTip(QDir::toNativeSeparators(folder));
        connect(action, &QAction::triggered, this, [this, folder] {
            if (!openFolder(folder))
                m_folders->forgetFolder(folder);
        });
    }
    if (folders.isEmpty()) {
        m_recentFoldersMenu->addAction(tr("No Recent Folders"))->setEnabled(false);
        return;
    }
    m_recentFoldersMenu->addSeparator();
    m_recentFoldersMenu->addAction(tr("&Clear List"), this, [this] { m_folders->clearRecentFolders(); });
}

// Comparing ------------------------------------------------------------------------------------------

CompareTab* MainWindow::compare(CompareSide left, CompareSide right)
{
    auto* tab = new CompareTab(std::move(left), std::move(right));
    applyCompareSettings(tab);
    connect(tab, &CompareTab::openRequested, this, &MainWindow::openAtLine);
    connect(tab, &CompareTab::openOffsetRequested, this, &MainWindow::openAtOffset);
    connect(tab, &CompareTab::zoomRequested, this, &MainWindow::zoomBy);
    connect(tab, &CompareTab::aiRequested, this, [this](const QString& question, const QString& label, const QString& text) {
        m_aiDock->show();
        m_aiDock->raise();
        m_aiPanel->askAbout(question, label, text);
    });
    connect(tab, &CompareTab::statusChanged, this, [this, tab](const QString& text) {
        if (tab == currentCompareTab())
            m_compareStatus->setText(text);
    });
    connect(tab, &CompareTab::titleChanged, this, [this, tab] {
        const int index = m_tabs->indexOf(tab);
        if (index < 0)
            return;
        m_tabs->setTabText(index, tab->title());
        m_tabs->setTabToolTip(index, tab->toolTip());
        if (tab == currentCompareTab())
            setWindowTitle(tr("%1 — AIEdit").arg(tab->title()));
    });
    const int index = m_tabs->addTab(tab, tab->title());
    m_tabs->setTabToolTip(index, tab->toolTip());
    m_tabs->setCurrentIndex(index);
    m_stack->setCurrentWidget(m_tabs);
    tab->setFocus();
    return tab;
}

CompareTab* MainWindow::compareFiles(const QString& left, const QString& right)
{
    std::optional<CompareSide> sides[2];
    const QString paths[2] = {left, right};
    for (int i = 0; i < 2; ++i) {
        QString error;
        sides[i] = fileSide(paths[i], preferredLegacyCodec(), &error);
        if (!sides[i]) {
            QMessageBox::warning(this, tr("Compare"),
                                 tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(paths[i]), error));
            return nullptr;
        }
    }
    return compare(std::move(*sides[0]), std::move(*sides[1]));
}

CompareSide MainWindow::documentSide(DocumentView* doc) const
{
    const QString label = doc->isModified() && !doc->isUntitled() ? tr("%1 (edited)").arg(doc->title()) : doc->title();
    CompareSide side = bytesSide(label, doc->textDocument()->source(), doc->textDocument()->codec());
    if (!doc->isUntitled())
        side.path = doc->filePath();
    const QPointer<DocumentView> pointer(doc);
    side.reload = [pointer]() -> std::shared_ptr<const core::FileSource> {
        return pointer ? pointer->textDocument()->source() : nullptr;
    };
    return side;
}

CompareTab* MainWindow::compareWithSaved()
{
    DocumentView* doc = currentDocument();
    if (!doc || doc->isUntitled())
        return nullptr;
    QString error;
    std::optional<CompareSide> disk = fileSide(doc->filePath(), preferredLegacyCodec(), &error);
    if (!disk) {
        QMessageBox::warning(this, tr("Compare"),
                             tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(doc->filePath()), error));
        return nullptr;
    }
    disk->label = tr("%1 on disk").arg(doc->title());
    disk->codec = &doc->textDocument()->codec(); // read as the document is
    return compare(std::move(*disk), documentSide(doc));
}

CompareTab* MainWindow::compareWithClipboard()
{
    DocumentView* doc = currentDocument();
    if (!doc)
        return nullptr;
    const auto clipboard = []() -> std::shared_ptr<const core::FileSource> {
        const QByteArray utf8 = QGuiApplication::clipboard()->text().toUtf8();
        return core::FileSource::fromBytes({}, std::make_shared<const std::vector<std::uint8_t>>(utf8.begin(), utf8.end()));
    };
    if (QGuiApplication::clipboard()->text().isEmpty()) {
        statusBar()->showMessage(tr("There is no text on the clipboard to compare with"), kStatusTimeoutMs);
        return nullptr;
    }
    CompareSide side = bytesSide(tr("Clipboard"), clipboard(), core::utf8Codec());
    side.reload = clipboard;
    return compare(documentSide(doc), std::move(side));
}

void MainWindow::compareWithFile()
{
    DocumentView* doc = currentDocument();
    if (!doc)
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Compare %1 With").arg(doc->title()), m_lastDirectory);
    if (path.isEmpty())
        return;
    QString error;
    std::optional<CompareSide> other = fileSide(path, preferredLegacyCodec(), &error);
    if (!other) {
        QMessageBox::warning(this, tr("Compare"), tr("Cannot open %1:\n%2").arg(QDir::toNativeSeparators(path), error));
        return;
    }
    compare(documentSide(doc), std::move(*other));
}

void MainWindow::compareTwoFiles()
{
    const QString first = QFileDialog::getOpenFileName(this, tr("Compare: The First File (Left)"), m_lastDirectory);
    if (first.isEmpty())
        return;
    const QString second =
        QFileDialog::getOpenFileName(this, tr("Compare: The Second File (Right)"), QFileInfo(first).absolutePath());
    if (!second.isEmpty())
        compareFiles(first, second);
}

CompareTab* MainWindow::currentCompareTab() const
{
    return qobject_cast<CompareTab*>(m_tabs->currentWidget());
}

void MainWindow::applyViewSettingsToTabs()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (DocumentView* doc = document(i))
            applyViewSettings(doc);
        else if (auto* tab = qobject_cast<CompareTab*>(m_tabs->widget(i)))
            applyCompareSettings(tab);
    }
}

void MainWindow::applyCompareSettings(CompareTab* tab) const
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!m_fontFamily.isEmpty())
        font.setFamily(m_fontFamily);
    font.setPointSize(m_pointSize);
    tab->view()->setFont(font);
    tab->hexView()->setFont(font);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    // The preview tab's dimmed text follows the colour scheme.
    if (event->type() == QEvent::PaletteChange && m_previewDoc)
        updateDocumentTitle(m_previewDoc);
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
    m_lineEndingButton->setVisible(textual);
    m_linesLabel->setVisible(textual);
    if (!doc)
        return;

    const TextDocument* text = doc->textDocument();
    const QString codec = codecName(text->codec());
    m_codecButton->setText(codec);
    for (QAction* action : m_codecGroup->actions())
        action->setChecked(action->text() == codec);
    m_lineEndingButton->setText(lineEndingName(text->lineEndings()));
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
    if (doc->isReadingMode()) {
        // A book: how far it is read, by the text at the top.
        const std::uint64_t size = std::max<std::uint64_t>(1, doc->source()->size());
        const auto top = static_cast<double>(doc->textView()->firstVisibleOffset());
        const double read = 100.0 * top / static_cast<double>(size);
        m_positionLabel->setText(tr("%1% read").arg(locale.toString(read, 'f', read < 10 ? 1 : 0)));
        return;
    }
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
