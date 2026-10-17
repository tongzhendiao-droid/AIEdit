#include "ui/BackupStore.h"
#include "ui/DocumentView.h"
#include "ui/FolderModels.h"
#include "ui/FolderPanel.h"
#include "ui/FuzzyMatch.h"
#include "ui/GitIgnore.h"
#include "ui/MainWindow.h"
#include "ui/QuickOpen.h"
#include "ui/TabWidget.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>

#include "HiddenFile.h"

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

QByteArray numberedLines(int count)
{
    QByteArray text;
    for (int i = 1; i <= count; ++i)
        text += "line " + QByteArray::number(i) + '\n';
    return text;
}

// The files a listing of the folder gives, sorted, with git's ignoring when it applies.
QStringList listed(const QString& folder)
{
    ui::GitIgnore ignore(folder);
    ui::FileListHooks hooks;
    QStringList paths;
    hooks.found = [&paths](std::vector<ui::ListedFile>&& files, int) {
        for (const ui::ListedFile& file : files)
            paths << file.relativePath;
    };
    if (ignore.isActive())
        hooks.skipped = [&ignore](const QString& path, bool isDir) { return ignore.isIgnored(path, isDir); };
    ui::listFiles(folder, ui::NameFilter(), false, hooks);
    paths.sort();
    return paths;
}

} // namespace

class TestNavigation : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void fuzzyMatchesInOrder_data();
    void fuzzyMatchesInOrder();
    void fuzzyPrefersWordStartsAndRuns();
    void opensFilesByName();
    void offersOpenAndRecentFilesFirst();
    void leavesOutWhatGitIgnores();
    void quickOpenLeavesOutWhatGitIgnores();
    void runsCommands();
    void goesToALine();
    void goesBackAndForward();
    void bookmarksLines();
    void findsTheSelection();
    void shortcuts();

private:
    QString write(const QString& name, const QByteArray& content = "text\n");
    QString root() const { return QFileInfo(m_dir->path()).canonicalFilePath(); }
    // Shows the window, which the quick open needs to be seen in.
    static void show(ui::MainWindow& window);

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestNavigation::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_navigation"));
}

void TestNavigation::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QSettings().clear();
    QDir(ui::BackupStore::root()).removeRecursively();
}

QString TestNavigation::write(const QString& name, const QByteArray& content)
{
    const QString path = QDir(root()).filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    return path;
}

void TestNavigation::show(ui::MainWindow& window)
{
    window.resize(900, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
}

void TestNavigation::fuzzyMatchesInOrder_data()
{
    QTest::addColumn<QString>("query");
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("matches");
    QTest::newRow("initials") << "mwc" << "MainWindow.cpp" << true;
    QTest::newRow("scattered") << "mw" << "somewhere" << true;
    QTest::newRow("case") << "MAIN" << "main.cpp" << true;
    QTest::newRow("out of order") << "wm" << "MainWindow" << false;
    QTest::newRow("missing") << "xyz" << "MainWindow.cpp" << false;
    QTest::newRow("longer") << "abc" << "ab" << false;
    QTest::newRow("nothing typed") << "" << "abc" << true;
    QTest::newRow("chinese") << "史记" << "史记·项羽本纪.txt" << true;
}

void TestNavigation::fuzzyMatchesInOrder()
{
    QFETCH(QString, query);
    QFETCH(QString, text);
    QFETCH(bool, matches);
    std::vector<int> positions;
    QCOMPARE(ui::fuzzyMatch(query, text, &positions).has_value(), matches);
    if (!matches)
        return;
    QCOMPARE(positions.size(), static_cast<std::size_t>(query.size()));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        QCOMPARE(text[positions[i]].toLower(), query[static_cast<qsizetype>(i)].toLower());
        if (i > 0)
            QVERIFY(positions[i] > positions[i - 1]);
    }
}

void TestNavigation::fuzzyPrefersWordStartsAndRuns()
{
    const auto score = [](const char* query, const char* text) {
        return ui::fuzzyMatch(QString::fromUtf8(query), QString::fromUtf8(text)).value_or(-1000);
    };
    QVERIFY(score("mw", "MainWindow.cpp") > score("mw", "somewhere"));
    QVERIFY(score("main", "main.cpp") > score("main", "domain.cpp"));
    QVERIFY(score("tab", "TabWidget.cpp") > score("tab", "tst_hexview_autogen_b.cpp"));

    // The word starts, rather than the first letters that fit.
    std::vector<int> positions;
    QVERIFY(ui::fuzzyMatch(u"mwc", u"MainWindow.cpp", &positions));
    QCOMPARE(positions, (std::vector<int>{0, 4, 11}));
    QVERIFY(ui::fuzzyMatch(u"ab", u"a_xb_ab", &positions));
    QCOMPARE(positions, (std::vector<int>{5, 6})); // together beats apart
    // Long paths are matched on their end, where the name is, and positions say where in all.
    const QString deep = QString(2000, QLatin1Char('d')) + QStringLiteral("/Name.txt");
    QVERIFY(ui::fuzzyMatch(u"name", deep, &positions));
    QCOMPARE(positions.front(), 2001);
}

void TestNavigation::opensFilesByName()
{
    write(QStringLiteral("src/ui/MainWindow.cpp"));
    write(QStringLiteral("src/ui/TabWidget.cpp"));
    write(QStringLiteral("src/core/Diff.cpp"));
    write(QStringLiteral("README.md"));
    const QString guide = write(QStringLiteral("docs/guide.md"), "one\ntwo\nthree\n");
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFolder(root()));
    ui::QuickOpen* quick = window.quickOpen();

    findAction(window, QStringLiteral("Go to F&ile…"))->trigger();
    QVERIFY(quick->isVisible());
    QTRY_VERIFY(!quick->isListing());
    QTest::keyClicks(quick->input(), QStringLiteral("mwc"));
    QCOMPARE(quick->shownTexts().value(0), QStringLiteral("MainWindow.cpp"));
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QVERIFY(!quick->isVisible());
    QCOMPARE(window.currentDocument()->filePath(), QDir(root()).filePath(QStringLiteral("src/ui/MainWindow.cpp")));

    // By folder, with a slash; down and up move through what is shown.
    window.quickOpen()->open();
    QTest::keyClicks(quick->input(), QStringLiteral("ui/"));
    QCOMPARE(quick->shownTexts(), (QStringList{QStringLiteral("MainWindow.cpp"), QStringLiteral("TabWidget.cpp")}));
    QCOMPARE(quick->currentRow(), 0);
    QTest::keyClick(quick->input(), Qt::Key_Down);
    QCOMPARE(quick->currentRow(), 1);
    QTest::keyClick(quick->input(), Qt::Key_Down); // round to the first
    QCOMPARE(quick->currentRow(), 0);
    QTest::keyClick(quick->input(), Qt::Key_Escape);
    QVERIFY(!quick->isVisible());

    // At a line.
    quick->open();
    QTest::keyClicks(quick->input(), QStringLiteral("guide:2"));
    QCOMPARE(quick->shownTexts().value(0), QStringLiteral("guide.md"));
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QCOMPARE(window.currentDocument()->filePath(), guide);
    QCOMPARE(window.currentDocument()->textView()->cursorLine(), 1u);

    quick->open();
    QTest::keyClicks(quick->input(), QStringLiteral("zzzz"));
    QVERIFY(quick->shownTexts().isEmpty());
    QCOMPARE(quick->note(), QStringLiteral("No file matches"));
}

void TestNavigation::offersOpenAndRecentFilesFirst()
{
    const QString a = write(QStringLiteral("a.txt"));
    const QString b = write(QStringLiteral("b.txt"));
    QTemporaryDir elsewhere;
    const QString recent = QDir(QFileInfo(elsewhere.path()).canonicalFilePath()).filePath(QStringLiteral("notes.log"));
    {
        QFile file(recent);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFile(recent));
    findAction(window, QStringLiteral("&Close Tab"))->trigger(); // recent, no longer open
    QVERIFY(window.openFile(a));
    QVERIFY(window.openFile(b));
    ui::QuickOpen* quick = window.quickOpen();

    // With nothing typed: the one shown first, then the one before, which is selected, so that
    // Enter goes back to it.
    quick->open();
    QCOMPARE(quick->shownTexts(),
             (QStringList{QStringLiteral("b.txt"), QStringLiteral("a.txt"), QStringLiteral("notes.log")}));
    QCOMPARE(quick->currentRow(), 1);
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QCOMPARE(window.currentDocument()->filePath(), a);

    quick->open();
    QTest::keyClicks(quick->input(), QStringLiteral("notes"));
    QCOMPARE(quick->shownTexts(), QStringList{QStringLiteral("notes.log")});
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QCOMPARE(window.currentDocument()->filePath(), recent);
}

void TestNavigation::leavesOutWhatGitIgnores()
{
    QVERIFY(QDir(root()).mkpath(QStringLiteral(".git")));
    write(QStringLiteral(".gitignore"), "# build output\nbuild/\n*.o\n!keep.o\n/notes.txt\n"
                                        "docs/**/draft.md\nlog?.txt\n[Tt]emp/\n  \n");
    write(QStringLiteral("src/.gitignore"), "gen/\n");
    QVERIFY(markHidden(QDir(root()).filePath(QStringLiteral(".gitignore"))));
    QVERIFY(markHidden(QDir(root()).filePath(QStringLiteral("src/.gitignore"))));
    for (const char* name : {"build/x.cpp", "src/a.cpp", "src/a.o", "src/keep.o", "notes.txt", "src/notes.txt",
                             "src/gen/g.cpp", "docs/a/b/draft.md", "docs/draft.md", "docs/final.md", "log1.txt",
                             "log10.txt", "Temp/t.txt", "temp/u.txt", "README.md"})
        write(QString::fromLatin1(name));

    QCOMPARE(listed(root()), (QStringList{QStringLiteral("README.md"), QStringLiteral("docs/final.md"),
                                          QStringLiteral("log10.txt"), QStringLiteral("src/a.cpp"),
                                          QStringLiteral("src/keep.o"), QStringLiteral("src/notes.txt")}));
    // From a folder further down, the files above still count.
    QCOMPARE(listed(QDir(root()).filePath(QStringLiteral("src"))),
             (QStringList{QStringLiteral("a.cpp"), QStringLiteral("keep.o"), QStringLiteral("notes.txt")}));
    // A folder git ignores, opened on purpose, shows all it has.
    QCOMPARE(listed(QDir(root()).filePath(QStringLiteral("build"))), QStringList{QStringLiteral("x.cpp")});

    // Outside a repository a .gitignore means nothing.
    QTemporaryDir plain;
    QFile ignoreFile(QDir(plain.path()).filePath(QStringLiteral(".gitignore")));
    QVERIFY(ignoreFile.open(QIODevice::WriteOnly));
    ignoreFile.write("*.o\n");
    ignoreFile.close();
    QVERIFY(markHidden(ignoreFile.fileName()));
    QFile object(QDir(plain.path()).filePath(QStringLiteral("a.o")));
    QVERIFY(object.open(QIODevice::WriteOnly));
    object.close();
    QVERIFY(!ui::GitIgnore(plain.path()).isActive());
    QCOMPARE(listed(plain.path()), QStringList{QStringLiteral("a.o")});
}

void TestNavigation::quickOpenLeavesOutWhatGitIgnores()
{
    QVERIFY(QDir(root()).mkpath(QStringLiteral(".git")));
    write(QStringLiteral(".gitignore"), "build/\n");
    write(QStringLiteral("build/main.cpp"));
    write(QStringLiteral("src/main.cpp"));
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFolder(root()));
    ui::QuickOpen* quick = window.quickOpen();
    quick->open();
    QTRY_VERIFY(!quick->isListing());
    QTest::keyClicks(quick->input(), QStringLiteral("main"));
    QCOMPARE(quick->shownTexts(), QStringList{QStringLiteral("main.cpp")});
    quick->input()->setText(QStringLiteral("zzzz"));
    QCOMPARE(quick->note(), QStringLiteral("No file matches (what .gitignore names is left out)"));
}

void TestNavigation::runsCommands()
{
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFile(write(QStringLiteral("a.txt"))));
    ui::QuickOpen* quick = window.quickOpen();
    QAction* wrap = findAction(window, QStringLiteral("&Word Wrap"));
    const bool wrapped = wrap->isChecked();

    findAction(window, QStringLiteral("Co&mmand Palette…"))->trigger();
    QCOMPARE(quick->input()->text(), QStringLiteral(">"));
    QVERIFY(quick->shownTexts().contains(QStringLiteral("Save All"))); // without the mnemonic
    QTest::keyClicks(quick->input(), QStringLiteral("word wrap"));
    QCOMPARE(quick->shownTexts().value(0), QStringLiteral("Word Wrap"));
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QVERIFY(!quick->isVisible());
    QCOMPARE(wrap->isChecked(), !wrapped);

    // One word is matched in the names; with more, the menus count too.
    quick->open(QStringLiteral(">wrap"));
    QCOMPARE(quick->shownTexts(), QStringList{QStringLiteral("Word Wrap")});
    quick->input()->setText(QStringLiteral(">view wrap"));
    QCOMPARE(quick->shownTexts().value(0), QStringLiteral("Word Wrap"));

    // Commands that cannot be done now are not offered; submenus' are.
    quick->open(QStringLiteral(">"));
    QTest::keyClicks(quick->input(), QStringLiteral("reopen closed"));
    QVERIFY(quick->shownTexts().isEmpty());
    QCOMPARE(quick->note(), QStringLiteral("No command matches"));
    quick->input()->setText(QStringLiteral(">with the saved"));
    QCOMPARE(quick->shownTexts().value(0), QStringLiteral("With the Saved Version"));
    // Deleting the ">" goes back to files.
    quick->input()->setText(QStringLiteral("a.t"));
    QCOMPARE(quick->shownTexts().value(0), QStringLiteral("a.txt"));
}

void TestNavigation::goesToALine()
{
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFile(write(QStringLiteral("a.txt"), numberedLines(20))));
    ui::QuickOpen* quick = window.quickOpen();
    ui::TextView* view = window.currentDocument()->textView();

    quick->open(QStringLiteral(":"));
    QCOMPARE(quick->shownTexts(), QStringList{QStringLiteral("Type a line number, or an offset such as 0x1F")});
    QTest::keyClick(quick->input(), Qt::Key_Return); // nothing to go to yet
    QVERIFY(quick->isVisible());
    QTest::keyClicks(quick->input(), QStringLiteral("12"));
    QCOMPARE(quick->shownTexts(), QStringList{QStringLiteral("Go to line 12")});
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QCOMPARE(view->cursorLine(), 11u);
    quick->open(QStringLiteral(":0x7"));
    QTest::keyClick(quick->input(), Qt::Key_Return);
    QCOMPARE(view->cursorOffset(), 7u);
}

void TestNavigation::goesBackAndForward()
{
    const QString a = write(QStringLiteral("a.txt"), numberedLines(100));
    const QString b = write(QStringLiteral("b.txt"), numberedLines(10));
    ui::MainWindow window;
    show(window);
    QAction* back = findAction(window, QStringLiteral("&Back"));
    QAction* forward = findAction(window, QStringLiteral("&Forward"));
    QVERIFY(window.openFile(a));
    ui::TextView* view = window.currentDocument()->textView();
    QVERIFY(!back->isEnabled());

    // A jump leaves a place behind; a few lines do not.
    QVERIFY(view->goToLine(49));
    QVERIFY(back->isEnabled());
    view->setCursorOffset(view->cursorOffset() + 8); // the next line
    back->trigger();
    QCOMPARE(view->cursorLine(), 0u);
    QVERIFY(forward->isEnabled());
    forward->trigger();
    QCOMPARE(view->cursorLine(), 50u);
    QVERIFY(!forward->isEnabled());

    // Across documents, and back into one closed meanwhile.
    QVERIFY(window.openFile(b));
    back->trigger();
    QCOMPARE(window.currentDocument()->filePath(), a);
    QCOMPARE(window.currentDocument()->textView()->cursorLine(), 50u);
    forward->trigger();
    QCOMPARE(window.currentDocument()->filePath(), b);
    window.findChild<ui::TabWidget*>()->setCurrentIndex(0);
    findAction(window, QStringLiteral("&Close Tab"))->trigger(); // a.txt
    QCOMPARE(window.documentCount(), 1);
    back->trigger();
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.currentDocument()->filePath(), a);

    // The mouse's back button.
    QVERIFY(window.openFile(b));
    QTest::mouseClick(window.currentDocument()->textView()->viewport(), Qt::BackButton);
    QCOMPARE(window.currentDocument()->filePath(), a);
}

void TestNavigation::bookmarksLines()
{
    const QString path = write(QStringLiteral("a.txt"), numberedLines(20));
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFile(path));
    ui::DocumentView* doc = window.currentDocument();
    ui::TextView* view = doc->textView();
    QAction* toggle = findAction(window, QStringLiteral("Toggle Book&mark"));
    QAction* next = findAction(window, QStringLiteral("&Next Bookmark"));
    QAction* previous = findAction(window, QStringLiteral("&Previous Bookmark"));

    next->trigger();
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("No line of a.txt is bookmarked"));
    QVERIFY(view->goToLine(2));
    view->setCursorOffset(view->cursorOffset() + 3); // anywhere on the line
    toggle->trigger();
    QVERIFY(view->goToLine(9));
    toggle->trigger();
    const std::vector<std::uint64_t> marks = view->bookmarks();
    QCOMPARE(marks.size(), 2u);
    QCOMPARE(marks[0], 14u); // where "line 3" starts

    QVERIFY(view->goToLine(0));
    next->trigger();
    QCOMPARE(view->cursorLine(), 2u);
    next->trigger();
    QCOMPARE(view->cursorLine(), 9u);
    next->trigger(); // round to the first
    QCOMPARE(view->cursorLine(), 2u);
    previous->trigger(); // round to the last
    QCOMPARE(view->cursorLine(), 9u);

    // They move with the text.
    view->setCursorOffset(0);
    view->insertText(QStringLiteral("new\n"));
    QCOMPARE(view->bookmarks(), (std::vector<std::uint64_t>{marks[0] + 4, marks[1] + 4}));
    view->undo();
    QCOMPARE(view->bookmarks(), marks);

    // A bookmarked line's number is drawn on a mark: left of the digits, the gutter differs.
    QVERIFY(view->goToLine(2));
    const QColor marked = view->viewport()->grab().toImage().pixelColor(4, view->cursorRect().center().y());
    QVERIFY(view->goToLine(4));
    const QColor plain = view->viewport()->grab().toImage().pixelColor(4, view->cursorRect().center().y());
    QVERIFY(marked != plain);

    // Remembered with the file.
    view->goToLine(9);
    toggle->trigger(); // off again
    QCOMPARE(view->bookmarks().size(), 1u);
    findAction(window, QStringLiteral("&Close Tab"))->trigger();
    QVERIFY(window.openFile(path));
    QCOMPARE(window.currentDocument()->textView()->bookmarks(), std::vector<std::uint64_t>{marks[0]});
    findAction(window, QStringLiteral("&Clear Bookmarks"))->trigger();
    QVERIFY(window.currentDocument()->textView()->bookmarks().empty());
}

void TestNavigation::findsTheSelection()
{
    ui::MainWindow window;
    show(window);
    QVERIFY(window.openFile(write(QStringLiteral("a.txt"), "alpha beta alpha gamma alpha\n")));
    ui::TextView* view = window.currentDocument()->textView();
    QAction* next = findAction(window, QStringLiteral("Fin&d Next Selection"));
    QAction* previous = findAction(window, QStringLiteral("Find Previous Selection"));

    // The word at the caret, further on.
    view->setCursorOffset(2);
    next->trigger();
    QTRY_COMPARE(view->selectionStart(), 11u);
    QCOMPARE(view->selectionEnd(), 16u);
    next->trigger();
    QTRY_COMPARE(view->selectionStart(), 23u);
    previous->trigger();
    QTRY_COMPARE(view->selectionStart(), 11u);

    // A selection, as it is.
    view->select(2, 4); // "ph"
    next->trigger();
    QTRY_COMPARE(view->selectionStart(), 13u);
    QCOMPARE(view->selectedText(), QStringLiteral("ph"));
    // Between words there is nothing to look for.
    view->setCursorOffset(5);
    previous->trigger();
    QTest::qWait(50);
    QCOMPARE(view->cursorOffset(), 5u);
    QVERIFY(!view->hasSelection());
}

void TestNavigation::shortcuts()
{
    ui::MainWindow window;
    QCOMPARE(findAction(window, QStringLiteral("Go to F&ile…"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_P));
    QCOMPARE(findAction(window, QStringLiteral("Co&mmand Palette…"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    QCOMPARE(findAction(window, QStringLiteral("&Back"))->shortcut(), QKeySequence(Qt::ALT | Qt::Key_Left));
    QCOMPARE(findAction(window, QStringLiteral("&Forward"))->shortcut(), QKeySequence(Qt::ALT | Qt::Key_Right));
    QCOMPARE(findAction(window, QStringLiteral("Toggle Book&mark"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_F2));
    QCOMPARE(findAction(window, QStringLiteral("&Next Bookmark"))->shortcut(), QKeySequence(Qt::Key_F2));
    QCOMPARE(findAction(window, QStringLiteral("&Previous Bookmark"))->shortcut(), QKeySequence(Qt::SHIFT | Qt::Key_F2));
    QCOMPARE(findAction(window, QStringLiteral("Fin&d Next Selection"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::Key_F3));
}

QTEST_MAIN(TestNavigation)
#include "tst_navigation.moc"
