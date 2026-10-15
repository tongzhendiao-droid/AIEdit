#include "core/TextCodec.h"
#include "ui/BackupStore.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDirIterator>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <filesystem>
#include <fstream>

using namespace aiedit;

namespace {

std::string readFile(const QString& path)
{
    std::ifstream in(ui::toPath(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string contents(const ui::DocumentView& doc)
{
    const auto bytes = doc.textDocument()->bytes();
    return {bytes.begin(), bytes.end()};
}

// Clicks `button` in the next message box; with none, nothing happens.
void answerNextQuestion(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, [button] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            box->button(button)->click();
    });
}

// The backups on disk, of any window: their content files.
QStringList backupFiles()
{
    QStringList files;
    QDirIterator it(ui::BackupStore::root(), {QStringLiteral("*.bin")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    return files;
}

} // namespace

class TestSession : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void remembersWhereAFileWasLeft();
    void remembersAChosenEncodingAndView();
    void listsRecentFiles();
    void restoresTheSession();
    void forgetsNewDocuments();

    void keepsUnsavedChangesWithoutAsking();
    void keepsNewDocuments();
    void backsUpWhileEditing();
    void recoversAfterACrash();
    void leavesRunningWindowsAlone();
    void bringsChangesBackWhenOpeningFiles();
    void noticesChangesOnDiskSince();
    void keepsChangesToAFileThatIsGone();
    void asksWhenChangesCannotBeKept();

private:
    QString write(const char* name, const std::string& content);
    static QAction* findAction(const QWidget& window, const QString& text);
    static QMenu* findMenu(const QWidget& window, const QString& title);

    QTemporaryDir m_dir;
};

void TestSession::initTestCase()
{
    // Settings go to a test location, away from the user's own.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_session"));
}

void TestSession::init()
{
    QVERIFY(m_dir.isValid());
    QSettings().clear();
    QDir(ui::BackupStore::root()).removeRecursively(); // left by the test before
}

QString TestSession::write(const char* name, const std::string& content)
{
    const QString path = m_dir.filePath(QString::fromLatin1(name));
    std::ofstream(ui::toPath(path), std::ios::binary) << content;
    return path;
}

QAction* TestSession::findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

QMenu* TestSession::findMenu(const QWidget& window, const QString& title)
{
    for (QMenu* menu : window.findChildren<QMenu*>())
        if (menu->title() == title)
            return menu;
    return nullptr;
}

void TestSession::remembersWhereAFileWasLeft()
{
    std::string book;
    for (int i = 0; i < 3000; ++i)
        book += "第" + std::to_string(i) + "行：天下大势，分久必合，合久必分。\n";
    const QString path = write("book.txt", book);

    std::uint64_t cursor = 0;
    std::uint64_t top = 0;
    {
        ui::MainWindow window;
        window.resize(900, 500);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openFile(path));
        ui::TextView* view = window.currentDocument()->textView();
        view->setCursorOffset(5000);
        view->scrollToOffset(40000); // reading on, with the caret left behind
        cursor = view->cursorOffset();
        top = view->firstVisibleOffset();
        QVERIFY(top > cursor);
        findAction(window, QStringLiteral("&Close Tab"))->trigger();
        QCOMPARE(window.documentCount(), 0);
    }

    ui::MainWindow window;
    window.resize(900, 500);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(path));
    const ui::TextView* view = window.currentDocument()->textView();
    QCOMPARE(view->cursorOffset(), cursor);
    QCOMPARE(view->firstVisibleOffset(), top); // the same text at the top as when it was closed
}

void TestSession::remembersAChosenEncodingAndView()
{
    // 你好 in GBK, read as Big5 by choice, in the side-by-side view.
    const QString path = write("chosen.txt", "\xC4\xE3\xBA\xC3\n");
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        ui::DocumentView* doc = window.currentDocument();
        doc->setCodec(*core::findCodec("Big5"));
        doc->setViewMode(ui::DocumentView::ViewMode::Split);
        findAction(window, QStringLiteral("&Close Tab"))->trigger();
    }
    ui::MainWindow window;
    QVERIFY(window.openFile(path));
    const ui::DocumentView* doc = window.currentDocument();
    QCOMPARE(doc->textDocument()->codec().name, std::string_view("Big5"));
    QCOMPARE(doc->viewMode(), ui::DocumentView::ViewMode::Split);

    // A codec that was only detected is not kept: detection runs again next time.
    const QString detected = write("detected.txt", "plain\n");
    QVERIFY(window.openFile(detected));
    QVERIFY(window.currentDocument()->state().codec.isEmpty());
}

void TestSession::listsRecentFiles()
{
    const QString a = write("a.txt", "a\n");
    const QString b = write("b.txt", "b\n");
    ui::MainWindow window;
    QVERIFY(window.openFile(a));
    QVERIFY(window.openFile(b));
    QMenu* recent = findMenu(window, QStringLiteral("Recen&t Files"));
    QVERIFY(recent);

    emit recent->aboutToShow();
    QStringList texts;
    for (const QAction* action : recent->actions())
        if (!action->isSeparator())
            texts << action->text();
    QCOMPARE(texts.size(), 3);
    QVERIFY(texts[0].startsWith(QStringLiteral("&1  b.txt"))); // the latest first
    QVERIFY(texts[1].startsWith(QStringLiteral("&2  a.txt")));
    QCOMPARE(texts[2], QStringLiteral("&Clear List"));

    // A file that is gone is dropped when picked, instead of failing to open.
    QFile::remove(a);
    recent->actions()[1]->trigger();
    emit recent->aboutToShow();
    QVERIFY(!recent->actions()[1]->text().contains(QStringLiteral("a.txt")));

    findAction(window, QStringLiteral("&Clear List"))->trigger();
    emit recent->aboutToShow();
    QCOMPARE(recent->actions().size(), 1);
    QVERIFY(!recent->actions().first()->isEnabled()); // "No Recent Files"
}

void TestSession::restoresTheSession()
{
    const QString a = write("one.txt", "one\n");
    const QString b = write("two.txt", "two\n");
    const QString c = write("three.txt", "three\n");
    {
        ui::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openFile(a));
        QVERIFY(window.openFile(b));
        QVERIFY(window.openFile(c));
        QVERIFY(window.openFile(b)); // its tab, which is current when closing
        QVERIFY(window.close());
    }
    QFile::remove(c); // gone since: left out, without a question in the way

    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.document(0)->filePath(), ui::toQString(ui::toPath(a)));
    QCOMPARE(window.currentDocument()->filePath(), ui::toQString(ui::toPath(b)));
}

void TestSession::forgetsNewDocuments()
{
    {
        ui::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        findAction(window, QStringLiteral("&New"))->trigger();
        QVERIFY(window.close()); // empty and unmodified: no question
    }
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.documentCount(), 0);
    QVERIFY(QSettings().value(QStringLiteral("files/recent")).toList().isEmpty());
}

void TestSession::keepsUnsavedChangesWithoutAsking()
{
    const QString path = write("notes.txt", "notes\n");
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        window.currentDocument()->textView()->insertText(QStringLiteral("more "));
        answerNextQuestion(QMessageBox::Cancel); // were there a question, closing would fail
        QVERIFY(window.close());
        QTest::qWait(1); // the answer was not needed: it must not reach a later question
    }
    QCOMPARE(readFile(path), std::string("notes\n")); // nothing written to the file itself

    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.documentCount(), 1);
    ui::DocumentView* doc = window.currentDocument();
    QCOMPARE(doc->filePath(), ui::toQString(ui::toPath(path)));
    QVERIFY(doc->isModified());
    QCOMPARE(contents(*doc), std::string("more notes\n"));
    QCOMPARE(window.findChild<QTabWidget*>()->tabText(0), QStringLiteral("*notes.txt"));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Unsaved changes are back")));

    // One undo goes back to the file as it is on disk.
    doc->undo();
    QCOMPARE(contents(*doc), std::string("notes\n"));
    QVERIFY(!doc->isModified());
}

void TestSession::keepsNewDocuments()
{
    {
        ui::MainWindow window;
        findAction(window, QStringLiteral("&New"))->trigger();
        findAction(window, QStringLiteral("&New"))->trigger(); // left empty: forgotten
        window.document(0)->textView()->insertText(QStringLiteral("a draft"));
        QVERIFY(window.close());
    }
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.documentCount(), 1);
    ui::DocumentView* doc = window.currentDocument();
    QVERIFY(doc->isUntitled() && doc->isModified());
    QCOMPARE(doc->title(), QStringLiteral("Untitled 1"));
    QCOMPARE(contents(*doc), std::string("a draft"));
    findAction(window, QStringLiteral("&New"))->trigger();
    QCOMPARE(window.currentDocument()->title(), QStringLiteral("Untitled 2")); // counting on
}

void TestSession::backsUpWhileEditing()
{
    const QString path = write("edited.txt", "text\n");
    ui::MainWindow window;
    QVERIFY(window.openFile(path));
    ui::DocumentView* doc = window.currentDocument();
    window.flushBackups();
    QVERIFY(backupFiles().isEmpty()); // nothing unsaved yet

    doc->textView()->insertText(QStringLiteral("new "));
    window.flushBackups();
    QStringList files = backupFiles();
    QCOMPARE(files.size(), 1);
    QCOMPARE(readFile(files.first()), std::string("new text\n"));
    doc->textView()->insertText(QStringLiteral("more "));
    window.flushBackups();
    QCOMPARE(readFile(backupFiles().first()), std::string("new more text\n"));

    // Undone to what is saved, or saved: nothing to keep.
    doc->undo();
    window.flushBackups();
    QVERIFY(backupFiles().isEmpty());
    doc->textView()->insertText(QStringLiteral("x"));
    window.flushBackups();
    QCOMPARE(backupFiles().size(), 1);
    QVERIFY(doc->save());
    window.flushBackups();
    QVERIFY(backupFiles().isEmpty());

    // Changes given up when closing the tab go too.
    doc->textView()->insertText(QStringLiteral("y"));
    window.flushBackups();
    QCOMPARE(backupFiles().size(), 1);
    answerNextQuestion(QMessageBox::Discard);
    findAction(window, QStringLiteral("&Close Tab"))->trigger();
    QCOMPARE(window.documentCount(), 0);
    window.flushBackups();
    QVERIFY(backupFiles().isEmpty());
}

void TestSession::recoversAfterACrash()
{
    const QString path = write("crash.txt", "before\n");
    {
        // A window that ends without closing, as in a crash: only its backups are left.
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        window.currentDocument()->textView()->insertText(QStringLiteral("typed "));
        findAction(window, QStringLiteral("&New"))->trigger();
        window.currentDocument()->textView()->insertText(QStringLiteral("draft"));
        window.flushBackups();
    }
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.document(0)->filePath(), ui::toQString(ui::toPath(path))); // in their order
    QCOMPARE(contents(*window.document(0)), std::string("typed before\n"));
    QVERIFY(window.document(1)->isUntitled());
    QCOMPARE(contents(*window.document(1)), std::string("draft"));

    // Backed up again by this window; the old copies are gone.
    QCOMPARE(backupFiles().size(), 2);
}

void TestSession::leavesRunningWindowsAlone()
{
    const QString path = write("running.txt", "a\n");
    auto running = std::make_unique<ui::MainWindow>();
    QVERIFY(running->openFile(path));
    running->currentDocument()->textView()->insertText(QStringLiteral("b"));
    running->flushBackups();
    {
        ui::MainWindow other; // e.g. aiedit --new-window
        other.restoreSession(false);
        QCOMPARE(other.documentCount(), 0);
    }
    running.reset(); // now it has ended
    ui::MainWindow next;
    next.restoreSession(false);
    QCOMPARE(next.documentCount(), 1);
    QCOMPARE(contents(*next.currentDocument()), std::string("ba\n"));
}

void TestSession::bringsChangesBackWhenOpeningFiles()
{
    const QString changed = write("changed.txt", "c\n");
    const QString plain = write("plain.txt", "p\n");
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(changed));
        window.currentDocument()->textView()->insertText(QStringLiteral("x"));
        QVERIFY(window.openFile(plain));
        QVERIFY(window.close());
    }
    // Started with files to open, the other tabs stay closed, but unsaved changes come back.
    ui::MainWindow window;
    window.restoreSession(false);
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.currentDocument()->filePath(), ui::toQString(ui::toPath(changed)));
    QVERIFY(window.currentDocument()->isModified());
}

void TestSession::noticesChangesOnDiskSince()
{
    const QString path = write("shared.txt", "one\n");
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        window.currentDocument()->textView()->insertText(QStringLiteral("mine "));
        QVERIFY(window.close());
    }
    std::ofstream(ui::toPath(path), std::ios::binary | std::ios::app) << "theirs\n";
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(contents(*window.currentDocument()), std::string("mine one\n")); // the changes win
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("shared.txt changed on disk since")));
}

void TestSession::keepsChangesToAFileThatIsGone()
{
    const QString path = write("gone.txt", "old\n");
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        window.currentDocument()->textView()->insertText(QStringLiteral("kept "));
        QVERIFY(window.close());
    }
    QFile::remove(path);
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.documentCount(), 1);
    ui::DocumentView* doc = window.currentDocument();
    QVERIFY(doc->isUntitled() && doc->isModified());
    QCOMPARE(doc->title(), QStringLiteral("gone.txt"));
    QCOMPARE(contents(*doc), std::string("kept old\n"));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("gone.txt no longer exists")));
}

void TestSession::asksWhenChangesCannotBeKept()
{
    // Where the backups would go is a file: nothing can be written there.
    const QString root = ui::BackupStore::root();
    QVERIFY(QDir().mkpath(QFileInfo(root).absolutePath()));
    { QFile blocker(root); QVERIFY(blocker.open(QIODevice::WriteOnly)); }

    const QString path = write("stuck.txt", "s\n");
    ui::MainWindow window;
    QVERIFY(window.openFile(path));
    window.currentDocument()->textView()->insertText(QStringLiteral("t"));
    answerNextQuestion(QMessageBox::Cancel);
    QVERIFY(!window.close()); // asked, and kept open
    answerNextQuestion(QMessageBox::Save);
    QVERIFY(window.close());
    QCOMPARE(readFile(path), std::string("ts\n"));
    QFile::remove(root);
}

QTEST_MAIN(TestSession)
#include "tst_session.moc"
