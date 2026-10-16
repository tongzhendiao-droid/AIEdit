#include "ui/BackupStore.h"
#include "ui/DocumentView.h"
#include "ui/FolderPanel.h"
#include "ui/MainWindow.h"
#include "ui/TabWidget.h"
#include "ui/TextView.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDockWidget>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <memory>

using namespace aiedit;

namespace {

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

// The action of a menu, or of its submenus, with this text (before the shortcut it shows).
QAction* menuAction(const QMenu& menu, const QString& text)
{
    for (QAction* action : menu.actions()) {
        if (action->text().section(QLatin1Char('\t'), 0, 0) == text)
            return action;
        if (action->menu())
            if (QAction* found = menuAction(*action->menu(), text))
                return found;
    }
    return nullptr;
}

// Whether right-clicking the tab offers the action, and can do it now.
bool offers(ui::MainWindow& window, int tab, const QString& text)
{
    const std::unique_ptr<QMenu> menu(window.tabMenu(tab));
    const QAction* action = menuAction(*menu, text);
    return action && action->isEnabled();
}

// Right-clicks the tab and picks the action.
void pick(ui::MainWindow& window, int tab, const QString& text)
{
    const std::unique_ptr<QMenu> menu(window.tabMenu(tab));
    QAction* action = menuAction(*menu, text);
    QVERIFY2(action && action->isEnabled(), qPrintable(text));
    action->trigger();
}

QStringList tabTexts(const QTabWidget* tabs)
{
    QStringList texts;
    for (int i = 0; i < tabs->count(); ++i)
        texts << tabs->tabText(i);
    return texts;
}

QTabBar::ButtonPosition closeSide(const QTabBar* bar)
{
    return static_cast<QTabBar::ButtonPosition>(
        bar->style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, bar));
}

// Clicks `button` in the next message box; with none, nothing happens.
void answerNextQuestion(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, [button] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            box->button(button)->click();
    });
}

} // namespace

class TestTabs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void newTabButtonFollowsTheLastTab();
    void middleButtonCloses();
    void pinnedTabsComeFirst();
    void droppedTabsTakeTheirSide();
    void closesTabsTogether();
    void stopsWhenAChangeIsKept();
    void reopensClosedTabs();
    void switchesAndMovesTabs();
    void copiesPathsAndShowsFiles();
    void comparesTabs();
    void renamesFromTheTab();
    void savesAll();
    void pinningKeepsAPreview();
    void pinnedTabsComeBack();
    void windowCommands();
    void secondWindowsLeaveTheSession();
    void shortcuts();

private:
    QString write(const QString& name, const QByteArray& content = "text\n");
    QString root() const { return QFileInfo(m_dir->path()).canonicalFilePath(); }

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestTabs::initTestCase()
{
    // Settings and backups go to a test location, away from the user's own.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_tabs"));
}

void TestTabs::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QSettings().clear();
    QDir(ui::BackupStore::root()).removeRecursively();
}

QString TestTabs::write(const QString& name, const QByteArray& content)
{
    const QString path = QDir(root()).filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    return path;
}

void TestTabs::newTabButtonFollowsTheLastTab()
{
    ui::MainWindow window;
    window.resize(800, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.findChild<ui::TabWidget*>();
    QTabBar* bar = tabs->tabBar();
    QToolButton* plus = tabs->newTabButton();
    const auto lastTabEnd = [&] { return bar->x() + bar->tabRect(bar->count() - 1).right(); };

    // Just after the last tab, and in the middle of the bar's height.
    QVERIFY(window.openFile(write(QStringLiteral("a.txt"))));
    QVERIFY(window.openFile(write(QStringLiteral("b.txt"))));
    QTRY_VERIFY(plus->isVisible());
    QVERIFY(plus->x() > lastTabEnd());
    QVERIFY(plus->x() - lastTabEnd() <= 4);
    QVERIFY(qAbs(plus->geometry().center().y() - bar->geometry().center().y()) <= 1);
    const int before = plus->x();
    QVERIFY(window.openFile(write(QStringLiteral("c.txt"))));
    QTRY_VERIFY(plus->x() > before);
    QVERIFY(plus->x() - lastTabEnd() <= 4);
    // A longer name moves it too.
    window.currentDocument()->textView()->insertText(QStringLiteral("x"));
    QTRY_COMPARE(tabs->tabText(2), QStringLiteral("*c.txt"));
    QTRY_VERIFY(plus->x() - lastTabEnd() > 0 && plus->x() - lastTabEnd() <= 4);

    // Tabs that do not fit scroll, and the + stays in sight at the end. Squeezed first, they
    // keep part of their names.
    for (int i = 0; i < 40; ++i)
        QVERIFY(window.openFile(write(QStringLiteral("a-rather-long-name-%1.txt").arg(i))));
    QTRY_VERIFY(plus->geometry().right() < tabs->width());
    QVERIFY(plus->x() > bar->geometry().right());
    QVERIFY(bar->tabRect(5).width() >= bar->fontMetrics().horizontalAdvance(QLatin1Char('x')) * 10);

    const int count = window.documentCount();
    plus->click();
    QCOMPARE(window.documentCount(), count + 1);
    QVERIFY(window.currentDocument()->isUntitled());
}

void TestTabs::middleButtonCloses()
{
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.findChild<ui::TabWidget*>();
    QTabBar* bar = tabs->tabBar();
    for (const char* name : {"a.txt", "b.txt", "c.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name))));

    QTest::mouseClick(bar, Qt::MiddleButton, {}, bar->tabRect(1).center());
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("c.txt")}));
    // Pressed on one tab and let go on another, it closes neither.
    QTest::mousePress(bar, Qt::MiddleButton, {}, bar->tabRect(0).center());
    QTest::mouseRelease(bar, Qt::MiddleButton, {}, bar->tabRect(1).center());
    QCOMPARE(tabs->count(), 2);
    // Pinned tabs stay.
    tabs->setPinned(1, true);
    QTest::mouseClick(bar, Qt::MiddleButton, {}, bar->tabRect(0).center());
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("a.txt")}));
}

void TestTabs::pinnedTabsComeFirst()
{
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.findChild<ui::TabWidget*>();
    QTabBar* bar = tabs->tabBar();
    QSignalSpy pinned(tabs, &ui::TabWidget::pinnedChanged);
    for (const char* name : {"a.txt", "b.txt", "c.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name))));

    // To the front, with a pin where the close button was.
    tabs->setPinned(2, true);
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("a.txt"), QStringLiteral("b.txt")}));
    QVERIFY(tabs->isPinned(0));
    QCOMPARE(tabs->pinnedCount(), 1);
    QCOMPARE(bar->tabButton(0, closeSide(bar))->objectName(), QStringLiteral("pinTabButton"));
    QCOMPARE(pinned.count(), 1);
    // After the pinned ones.
    tabs->setPinned(2, true);
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("b.txt"), QStringLiteral("a.txt")}));
    QCOMPARE(tabs->pinnedCount(), 2);
    QCOMPARE(tabs->moveTab(2, 0), 2); // not in among the pinned ones
    QCOMPARE(tabs->moveTab(0, 2), 1); // nor out of them
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("b.txt"), QStringLiteral("c.txt"), QStringLiteral("a.txt")}));

    // Unpinned, it goes just after them and has its close button back, which closes it.
    tabs->setPinned(0, false);
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("b.txt"), QStringLiteral("a.txt")}));
    QVERIFY(!tabs->isPinned(1));
    auto* close = qobject_cast<QAbstractButton*>(bar->tabButton(1, closeSide(bar)));
    QVERIFY(close && close->objectName() != QStringLiteral("pinTabButton"));
    close->click();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("a.txt")}));

    // Clicking the pin unpins.
    qobject_cast<QAbstractButton*>(bar->tabButton(0, closeSide(bar)))->click();
    QCOMPARE(tabs->pinnedCount(), 0);
    QVERIFY(bar->tabButton(0, closeSide(bar))->objectName() != QStringLiteral("pinTabButton"));
    // A pinned tab can still be closed by itself.
    tabs->setPinned(1, true);
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("c.txt")}));
    tabs->setCurrentIndex(0);
    findAction(window, QStringLiteral("&Close Tab"))->trigger();
    QCOMPARE(tabTexts(tabs), QStringList{QStringLiteral("c.txt")});
}

void TestTabs::droppedTabsTakeTheirSide()
{
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.findChild<ui::TabWidget*>();
    QTabBar* bar = tabs->tabBar();
    for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name))));
    tabs->setPinned(0, true);
    tabs->setPinned(1, true);

    // Dragged in among the pinned tabs, d is pinned when let go.
    bar->moveTab(3, 1); // as dragging it leaves it
    QTest::mouseClick(bar, Qt::LeftButton, {}, bar->tabRect(1).center());
    QCOMPARE(tabs->tabText(1), QStringLiteral("d.txt"));
    QVERIFY(tabs->isPinned(1));
    QCOMPARE(tabs->pinnedCount(), 3);
    // Dragged out of them, a is unpinned.
    bar->moveTab(0, 3);
    QTest::mouseClick(bar, Qt::LeftButton, {}, bar->tabRect(3).center());
    QCOMPARE(tabs->tabText(3), QStringLiteral("a.txt"));
    QVERIFY(!tabs->isPinned(3));
    QCOMPARE(tabs->pinnedCount(), 2);
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("d.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt"),
                                          QStringLiteral("a.txt")}));
}

void TestTabs::closesTabsTogether()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<ui::TabWidget*>();
    for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt", "f.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name))));
    tabs->setPinned(0, true);

    pick(window, 3, QStringLiteral("Close to the &Right"));
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt"),
                                          QStringLiteral("d.txt")}));
    pick(window, 3, QStringLiteral("Close to the &Left")); // but not the pinned one
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("d.txt")}));
    QVERIFY(!offers(window, 0, QStringLiteral("Close to the &Left")));
    QVERIFY(!offers(window, 1, QStringLiteral("Close to the &Right")));

    QVERIFY(window.openFile(write(QStringLiteral("g.txt"))));
    QVERIFY(window.openFile(write(QStringLiteral("h.txt"))));
    pick(window, 2, QStringLiteral("Close &Others"));
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("g.txt")}));
    QCOMPARE(tabs->currentIndex(), 1);

    // Saved ones: those without unsaved changes.
    window.currentDocument()->textView()->insertText(QStringLiteral("x"));
    QVERIFY(window.openFile(write(QStringLiteral("i.txt"))));
    window.compareWithSaved(window.document(1)); // a comparison has nothing to save
    QCOMPARE(tabs->count(), 4);
    pick(window, 0, QStringLiteral("Close &Saved"));
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("*g.txt")}));

    answerNextQuestion(QMessageBox::Discard);
    findAction(window, QStringLiteral("Close All Ta&bs"))->trigger();
    QCOMPARE(tabTexts(tabs), QStringList{QStringLiteral("a.txt")});
    QVERIFY(!offers(window, 0, QStringLiteral("Close &All")));
}

void TestTabs::stopsWhenAChangeIsKept()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<ui::TabWidget*>();
    for (const char* name : {"a.txt", "b.txt", "c.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name))));
    tabs->setCurrentIndex(1);
    window.currentDocument()->textView()->insertText(QStringLiteral("x"));

    answerNextQuestion(QMessageBox::Cancel);
    findAction(window, QStringLiteral("Close All Ta&bs"))->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("*b.txt"), QStringLiteral("c.txt")}));
}

void TestTabs::reopensClosedTabs()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<ui::TabWidget*>();
    QAction* reopen = findAction(window, QStringLiteral("Reo&pen Closed Tab"));
    QVERIFY(!reopen->isEnabled());
    for (const char* name : {"a.txt", "b.txt", "c.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name), "one\ntwo\nthree\nfour\n")));

    // Where it was, as it was left.
    tabs->setCurrentIndex(1);
    QVERIFY(window.currentDocument()->textView()->goToLine(2));
    findAction(window, QStringLiteral("&Close Tab"))->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("c.txt")}));
    QVERIFY(reopen->isEnabled());
    reopen->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt")}));
    QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(window.currentDocument()->textView()->cursorLine(), 2u);
    QVERIFY(!reopen->isEnabled());

    // Pinned, it comes back pinned.
    tabs->setPinned(2, true);
    pick(window, 0, QStringLiteral("&Close"));
    reopen->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("a.txt"), QStringLiteral("b.txt")}));
    QVERIFY(tabs->isPinned(0));

    // Files gone since are passed over; new documents and comparisons are not kept.
    pick(window, 1, QStringLiteral("&Close"));
    pick(window, 1, QStringLiteral("&Close"));
    QVERIFY(QFile::remove(QDir(root()).filePath(QStringLiteral("a.txt"))));
    findAction(window, QStringLiteral("&New"))->trigger();
    window.compareWithSaved(window.document(0));
    pick(window, 2, QStringLiteral("&Close"));
    pick(window, 1, QStringLiteral("&Close"));
    reopen->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("b.txt")}));
    reopen->trigger();
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("No closed tab to reopen"));
    QVERIFY(!reopen->isEnabled());
}

void TestTabs::switchesAndMovesTabs()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<ui::TabWidget*>();
    for (const char* name : {"a.txt", "b.txt", "c.txt", "d.txt"})
        QVERIFY(window.openFile(write(QString::fromLatin1(name))));

    findAction(window, QStringLiteral("Go to Tab 2"))->trigger();
    QCOMPARE(tabs->currentIndex(), 1);
    findAction(window, QStringLiteral("Go to the Last Tab"))->trigger();
    QCOMPARE(tabs->currentIndex(), 3);
    findAction(window, QStringLiteral("Go to Tab 8"))->trigger(); // there is none
    QCOMPARE(tabs->currentIndex(), 3);
    findAction(window, QStringLiteral("&Next Tab"))->trigger();
    QCOMPARE(tabs->currentIndex(), 0);
    findAction(window, QStringLiteral("&Previous Tab"))->trigger();
    QCOMPARE(tabs->currentIndex(), 3);

    findAction(window, QStringLiteral("Go to Tab 1"))->trigger();
    findAction(window, QStringLiteral("Move Tab &Right"))->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("b.txt"), QStringLiteral("a.txt"), QStringLiteral("c.txt"),
                                          QStringLiteral("d.txt")}));
    QCOMPARE(window.currentDocument()->title(), QStringLiteral("a.txt"));
    // Not past a pinned tab.
    tabs->setPinned(2, true);
    QCOMPARE(window.currentDocument()->title(), QStringLiteral("a.txt"));
    QAction* pin = findAction(window, QStringLiteral("P&in Tab"));
    QVERIFY(!pin->isChecked());
    findAction(window, QStringLiteral("Move Tab &Left"))->trigger();
    findAction(window, QStringLiteral("Move Tab &Left"))->trigger();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("a.txt"), QStringLiteral("b.txt"),
                                          QStringLiteral("d.txt")}));
    pin->trigger();
    QVERIFY(pin->isChecked());
    QCOMPARE(tabs->pinnedCount(), 2);
}

void TestTabs::copiesPathsAndShowsFiles()
{
    const QString file = write(QStringLiteral("sub/a.txt"));
    QTemporaryDir elsewhere;
    const QString outside = QDir(QFileInfo(elsewhere.path()).canonicalFilePath()).filePath(QStringLiteral("x.txt"));
    {
        QFile out(outside);
        QVERIFY(out.open(QIODevice::WriteOnly));
    }
    ui::MainWindow window;
    auto* panel = window.findChild<ui::FolderPanel*>();
    QVERIFY(window.openFile(file));
    QVERIFY(window.openFile(outside));

    pick(window, 0, QStringLiteral("Copy &Path"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QDir::toNativeSeparators(file));
    pick(window, 0, QStringLiteral("Copy File &Name"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("a.txt"));
    QVERIFY(!offers(window, 0, QStringLiteral("Copy Relative Pa&th"))); // no folder open
    QVERIFY(window.openFolder(root()));
    pick(window, 0, QStringLiteral("Copy Relative Pa&th"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QDir::toNativeSeparators(QStringLiteral("sub/a.txt")));
    QVERIFY(!offers(window, 1, QStringLiteral("Copy Relative Pa&th"))); // not in the folder

    // In the folder open, or in its own.
    pick(window, 0, QStringLiteral("Show in &Folder Panel"));
    QCOMPARE(panel->folder(), root());
    QTRY_COMPARE(panel->selectedPaths(), QStringList{file});
    pick(window, 1, QStringLiteral("Show in &Folder Panel"));
    QCOMPARE(panel->folder(), QFileInfo(outside).absolutePath());
    QTRY_COMPARE(panel->selectedPaths(), QStringList{outside});

    // A new document has no file for any of it.
    findAction(window, QStringLiteral("&New"))->trigger();
    for (const char* text : {"Copy &Path", "Copy File &Name", "Show in File &Manager", "Show in &Folder Panel",
                             "Compare with the Sa&ved Version", "Re&name…", "Rel&oad"})
        QVERIFY2(!offers(window, 2, QString::fromUtf8(text)), text);
    QVERIFY(offers(window, 0, QStringLiteral("Show in File &Manager"))); // not clicked: it would open one
}

void TestTabs::comparesTabs()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<ui::TabWidget*>();
    QVERIFY(window.openFile(write(QStringLiteral("a.txt"), "one\ntwo\n")));
    QVERIFY(window.openFile(write(QStringLiteral("b.txt"), "one\nthree\n")));
    QVERIFY(window.openFile(write(QStringLiteral("c.txt"))));

    // Each other tab, by name.
    const std::unique_ptr<QMenu> menu(window.tabMenu(0));
    QVERIFY(menuAction(*menu, QStringLiteral("b.txt")));
    QVERIFY(menuAction(*menu, QStringLiteral("c.txt")));
    QVERIFY(!menuAction(*menu, QStringLiteral("a.txt")));
    menuAction(*menu, QStringLiteral("b.txt"))->trigger();
    QVERIFY(window.currentCompareTab());
    QCOMPARE(tabs->count(), 4);

    // A comparison's tab can be closed and pinned, but has no file.
    QVERIFY(!offers(window, 3, QStringLiteral("Copy &Path")));
    QVERIFY(!offers(window, 3, QStringLiteral("a.txt")));
    pick(window, 3, QStringLiteral("&Pin Tab"));
    QVERIFY(tabs->isPinned(0));
    QVERIFY(window.currentCompareTab());
    pick(window, 0, QStringLiteral("&Close"));
    QCOMPARE(tabs->count(), 3);
}

void TestTabs::renamesFromTheTab()
{
    const QString old = write(QStringLiteral("old.txt"));
    ui::MainWindow window;
    QVERIFY(window.openFile(old));
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("new.txt"));
            dialog->accept();
        }
    });
    pick(window, 0, QStringLiteral("Re&name…"));
    const QString renamed = QDir(root()).filePath(QStringLiteral("new.txt"));
    QVERIFY(QFileInfo::exists(renamed));
    QVERIFY(!QFileInfo::exists(old));
    QCOMPARE(window.currentDocument()->filePath(), renamed);
    QCOMPARE(window.findChild<QTabWidget*>()->tabText(0), QStringLiteral("new.txt"));
}

void TestTabs::savesAll()
{
    ui::MainWindow window;
    QStringList paths;
    for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
        paths << write(QString::fromLatin1(name));
        QVERIFY(window.openFile(paths.constLast()));
        if (paths.size() != 2)
            window.currentDocument()->textView()->insertText(QStringLiteral("x"));
    }
    findAction(window, QStringLiteral("Save A&ll"))->trigger();
    for (int i = 0; i < 3; ++i)
        QVERIFY(!window.document(i)->isModified());
    QFile a(paths[0]);
    QVERIFY(a.open(QIODevice::ReadOnly));
    QCOMPARE(a.readAll(), QByteArray("xtext\n"));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Saved 2")));
}

void TestTabs::pinningKeepsAPreview()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<ui::TabWidget*>();
    QVERIFY(window.previewFile(write(QStringLiteral("a.txt"))));
    QVERIFY(!offers(window, 0, QStringLiteral("Un&pin Tab")));
    pick(window, 0, QStringLiteral("&Pin Tab"));
    QVERIFY(tabs->isPinned(0));
    QVERIFY(!window.previewDocument());
    QVERIFY(window.previewFile(write(QStringLiteral("b.txt"))));
    QCOMPARE(window.documentCount(), 2); // a new preview, beside it
    pick(window, 1, QStringLiteral("&Keep Open"));
    QVERIFY(!window.previewDocument());
    QVERIFY(!offers(window, 1, QStringLiteral("&Keep Open")));
    QVERIFY(offers(window, 0, QStringLiteral("Un&pin Tab")));
}

void TestTabs::pinnedTabsComeBack()
{
    {
        ui::MainWindow window;
        auto* tabs = window.findChild<ui::TabWidget*>();
        for (const char* name : {"a.txt", "b.txt", "c.txt"})
            QVERIFY(window.openFile(write(QString::fromLatin1(name))));
        tabs->setPinned(2, true);
        tabs->setCurrentIndex(1);
        QVERIFY(window.close());
    }
    ui::MainWindow window;
    window.restoreSession();
    auto* tabs = window.findChild<ui::TabWidget*>();
    QCOMPARE(tabTexts(tabs), (QStringList{QStringLiteral("c.txt"), QStringLiteral("a.txt"), QStringLiteral("b.txt")}));
    QVERIFY(tabs->isPinned(0));
    QCOMPARE(tabs->pinnedCount(), 1);
    QCOMPARE(tabs->currentIndex(), 1);
}

void TestTabs::windowCommands()
{
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(write(QStringLiteral("a.txt"), "one\ntwo\nthree\n")));

    // F11.
    QAction* fullScreen = findAction(window, QStringLiteral("F&ull Screen"));
    fullScreen->trigger();
    QTRY_VERIFY(window.isFullScreen());
    QVERIFY(fullScreen->isChecked());
    fullScreen->trigger();
    QTRY_VERIFY(!window.isFullScreen());
    QVERIFY(!fullScreen->isChecked());

    // Ctrl+B hides the panels on the left, and brings back the same ones.
    QAction* sideBar = findAction(window, QStringLiteral("Side &Bar"));
    auto* folders = window.findChild<QDockWidget*>(QStringLiteral("folderDock"));
    QVERIFY(!sideBar->isChecked());
    sideBar->trigger(); // none was open: the folder
    QVERIFY(folders->isVisible());
    QVERIFY(sideBar->isChecked());
    sideBar->trigger();
    QVERIFY(!folders->isVisible());
    QVERIFY(!sideBar->isChecked());
    sideBar->trigger();
    QVERIFY(folders->isVisible());
    folders->toggleViewAction()->trigger(); // closed by itself, the toggle follows
    QVERIFY(!sideBar->isChecked());

    // Clicking the line and column goes to a line.
    QToolButton* position = nullptr;
    for (QToolButton* button : window.statusBar()->findChildren<QToolButton*>())
        if (button->text().startsWith(QStringLiteral("Ln ")))
            position = button;
    QVERIFY(position);
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("3"));
            dialog->accept();
        }
    });
    position->click();
    QCOMPARE(window.currentDocument()->textView()->cursorLine(), 2u);
    // Another window is a process of its own: only its key is looked at here.
    QCOMPARE(findAction(window, QStringLiteral("New &Window"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
}

void TestTabs::secondWindowsLeaveTheSession()
{
    {
        ui::MainWindow first;
        QVERIFY(first.openFile(write(QStringLiteral("a.txt"))));
        QVERIFY(first.close());
    }
    {
        ui::MainWindow second;
        second.setSecondary(true);
        QVERIFY(second.openFile(write(QStringLiteral("b.txt"))));
        QVERIFY(second.close());
    }
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(tabTexts(window.findChild<QTabWidget*>()), QStringList{QStringLiteral("a.txt")});
}

void TestTabs::shortcuts()
{
    ui::MainWindow window;
    QCOMPARE(findAction(window, QStringLiteral("&Close Tab"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_W));
    QCOMPARE(findAction(window, QStringLiteral("Close All Ta&bs"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    QCOMPARE(findAction(window, QStringLiteral("Reo&pen Closed Tab"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    QVERIFY(findAction(window, QStringLiteral("&Next Tab"))
                ->shortcuts()
                .contains(QKeySequence(Qt::CTRL | Qt::Key_PageDown)));
    QVERIFY(findAction(window, QStringLiteral("&Previous Tab"))
                ->shortcuts()
                .contains(QKeySequence(Qt::CTRL | Qt::Key_PageUp)));
    QCOMPARE(findAction(window, QStringLiteral("Move Tab &Left"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_PageUp));
    QCOMPARE(findAction(window, QStringLiteral("Move Tab &Right"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_PageDown));
    QCOMPARE(findAction(window, QStringLiteral("Go to Tab 1"))->shortcut(), QKeySequence(Qt::ALT | Qt::Key_1));
    QCOMPARE(findAction(window, QStringLiteral("Go to the Last Tab"))->shortcut(), QKeySequence(Qt::ALT | Qt::Key_9));

    // No key does two things in the window: the window's own actions, and those of the
    // documents, which work while one has the focus. (The panels' actions are their own.)
    QHash<QKeySequence, QString> taken;
    for (const QAction* action : window.findChildren<QAction*>()) {
        const QObject* owner = action->parent();
        if (owner != &window && !qobject_cast<const QDockWidget*>(owner))
            continue;
        for (const QKeySequence& key : action->shortcuts()) {
            QVERIFY2(!taken.contains(key), qPrintable(key.toString() + QStringLiteral(": ") + taken.value(key) +
                                                       QStringLiteral(", ") + action->text()));
            taken.insert(key, action->text());
        }
    }
}

QTEST_MAIN(TestTabs)
#include "tst_tabs.moc"
