#include "core/TextCodec.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>

using namespace aiedit;

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

QTEST_MAIN(TestSession)
#include "tst_session.moc"
