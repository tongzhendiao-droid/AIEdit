#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/BackupStore.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace aiedit;

namespace {

std::string contents(const ui::DocumentView& doc)
{
    const auto bytes = doc.textDocument()->bytes();
    return {bytes.begin(), bytes.end()};
}

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

} // namespace

class TestEditCommands : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void copiesLinesUpAndDeletesThem();
    void joinsLines();
    void insertsLinesAboveAndBelow();
    void togglesComments_data();
    void togglesComments();
    void commentsNeedALanguage();
    void convertsCase();
    void selectsLines();
    void homeGoesToTheTextFirst();
    void ctrlArrowsScroll();
    void shortcuts();

private:
    // An editable document on a file with this name and content.
    std::unique_ptr<ui::DocumentView> documentWith(const QString& name, const std::string& content);

    QTemporaryDir m_dir;
};

void TestEditCommands::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_editcommands"));
    QSettings().clear();
    QDir(ui::BackupStore::root()).removeRecursively();
}

std::unique_ptr<ui::DocumentView> TestEditCommands::documentWith(const QString& name, const std::string& content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return nullptr;
    file.write(content.data(), static_cast<qint64>(content.size()));
    file.close();
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(ui::toPath(path), ec);
    auto doc = std::make_unique<ui::DocumentView>(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc->setViewMode(ui::DocumentView::ViewMode::Text);
    return doc;
}

void TestEditCommands::copiesLinesUpAndDeletesThem()
{
    const auto doc = documentWith(QStringLiteral("lines.txt"), "one\ntwo\nthree");
    QVERIFY(doc->isEditable());
    ui::TextView* view = doc->textView();
    view->setCursorOffset(5); // on "two"
    QVERIFY(doc->applyLineOperation(ui::LineOperation::CopyUp));
    QCOMPARE(contents(*doc), std::string("one\ntwo\ntwo\nthree"));
    QCOMPARE(view->cursorOffset(), 5u); // on the upper one

    QVERIFY(doc->applyLineOperation(ui::LineOperation::Delete));
    QCOMPARE(contents(*doc), std::string("one\ntwo\nthree"));
    QCOMPARE(view->cursorOffset(), 4u); // at the start of the line that came up
    // The last line takes the line break before it.
    view->setCursorOffset(10);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::Delete));
    QCOMPARE(contents(*doc), std::string("one\ntwo"));
    QCOMPARE(view->cursorOffset(), 4u);
    // Selected lines go together.
    view->select(1, 5);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::Delete));
    QCOMPARE(contents(*doc), std::string(""));

    for (int step = 0; step < 4; ++step)
        doc->undo();
    QCOMPARE(contents(*doc), std::string("one\ntwo\nthree")); // a step each
}

void TestEditCommands::joinsLines()
{
    const auto doc = documentWith(QStringLiteral("join.txt"), "a\n   b\nc\n");
    ui::TextView* view = doc->textView();
    view->setCursorOffset(0);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::Join));
    QCOMPARE(contents(*doc), std::string("a b\nc\n")); // the indentation went
    QCOMPARE(view->cursorOffset(), 1u);                 // where the lines met
    view->select(0, 6);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::Join));
    QCOMPARE(contents(*doc), std::string("a b c\n"));
    QCOMPARE(view->selectionStart(), 0u);
    QCOMPARE(view->selectionEnd(), 5u);
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::Join)); // nothing after it

    // No space next to an empty line, nor a second one after a blank.
    const auto other = documentWith(QStringLiteral("join2.txt"), "a\n\nb \nc");
    other->textView()->setCursorOffset(0);
    QVERIFY(other->applyLineOperation(ui::LineOperation::Join));
    QCOMPARE(contents(*other), std::string("a\nb \nc"));
    other->textView()->setCursorOffset(2);
    QVERIFY(other->applyLineOperation(ui::LineOperation::Join));
    QCOMPARE(contents(*other), std::string("a\nb c"));
}

void TestEditCommands::insertsLinesAboveAndBelow()
{
    const auto doc = documentWith(QStringLiteral("insert.txt"), "  x\n  y\n");
    ui::TextView* view = doc->textView();
    view->setCursorOffset(1);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::InsertBelow));
    QCOMPARE(contents(*doc), std::string("  x\n  \n  y\n")); // indented as the line before
    QCOMPARE(view->cursorOffset(), 6u);
    view->setCursorOffset(3);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::InsertAbove));
    QCOMPARE(contents(*doc), std::string("  \n  x\n  \n  y\n"));
    QCOMPARE(view->cursorOffset(), 2u);

    // After the last line, without a line break of its own: typing goes on the new line.
    const auto last = documentWith(QStringLiteral("insert2.txt"), "x");
    last->textView()->setReadOnly(false);
    last->textView()->setCursorOffset(0);
    QVERIFY(last->applyLineOperation(ui::LineOperation::InsertBelow));
    QTest::keyClicks(last->textView(), QStringLiteral("y"));
    QCOMPARE(contents(*last), std::string("x\ny\n"));

    // As after Enter at the end of such a file.
    const auto enter = documentWith(QStringLiteral("insert3.txt"), "x");
    enter->textView()->setReadOnly(false);
    enter->textView()->setCursorOffset(1);
    QTest::keyClick(enter->textView(), Qt::Key_Return);
    QTest::keyClicks(enter->textView(), QStringLiteral("y"));
    QCOMPARE(contents(*enter), std::string("x\ny\n"));
}

void TestEditCommands::togglesComments_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("original");
    QTest::addColumn<QByteArray>("commented");
    // At the indentation the lines share; blank lines stay as they are.
    QTest::newRow("c++") << "a.cpp" << QByteArray("int a;\n  int b;\n\n") << QByteArray("// int a;\n//   int b;\n\n");
    QTest::newRow("indented") << "b.cpp" << QByteArray("  a();\n    b();\n") << QByteArray("  // a();\n  //   b();\n");
    QTest::newRow("python") << "c.py" << QByteArray("x = 1\n") << QByteArray("# x = 1\n");
    QTest::newRow("css") << "d.css" << QByteArray("a { }\n") << QByteArray("/* a { } */\n");
}

void TestEditCommands::togglesComments()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, original);
    QFETCH(QByteArray, commented);
    const auto doc = documentWith(name, original.toStdString());
    ui::TextView* view = doc->textView();
    view->select(0, static_cast<std::uint64_t>(original.size()));
    QVERIFY(doc->toggleComment());
    QCOMPARE(contents(*doc), commented.toStdString());
    // Every line with text is commented: the comments go.
    QVERIFY(doc->toggleComment());
    QCOMPARE(contents(*doc), original.toStdString());
}

void TestEditCommands::commentsNeedALanguage()
{
    // Some lines commented, some not: all of them are.
    const auto doc = documentWith(QStringLiteral("e.cpp"), "// a\nb\n");
    ui::TextView* view = doc->textView();
    view->select(0, 7);
    QVERIFY(doc->toggleComment());
    QCOMPARE(contents(*doc), std::string("// // a\n// b\n"));
    // The caret moves with its text.
    const auto one = documentWith(QStringLiteral("f.cpp"), "int a;\n");
    one->textView()->setCursorOffset(2);
    QVERIFY(one->toggleComment());
    QCOMPARE(one->textView()->cursorOffset(), 5u);
    QVERIFY(one->toggleComment());
    QCOMPARE(one->textView()->cursorOffset(), 2u);

    // Plain text has no comments.
    const auto plain = documentWith(QStringLiteral("notes.txt"), "a\n");
    QString message;
    connect(plain.get(), &ui::DocumentView::statusMessage, this, [&message](const QString& text) { message = text; });
    QVERIFY(!plain->toggleComment());
    QVERIFY(message.startsWith(QStringLiteral("Plain text has no comments")));
    QCOMPARE(contents(*plain), std::string("a\n"));
}

void TestEditCommands::convertsCase()
{
    const auto doc = documentWith(QStringLiteral("case.txt"), "hello World stra\xC3\x9F" "e\n");
    ui::TextView* view = doc->textView();
    view->setCursorOffset(2); // in "hello": the word at the caret
    QVERIFY(doc->convertCase(true));
    QCOMPARE(contents(*doc), std::string("HELLO World stra\xC3\x9F" "e\n"));
    QCOMPARE(view->selectedText(), QStringLiteral("HELLO"));
    view->select(0, 11);
    QVERIFY(doc->convertCase(false));
    QCOMPARE(contents(*doc), std::string("hello world stra\xC3\x9F" "e\n"));
    QVERIFY(!doc->convertCase(false)); // lower case already
    // A longer word in upper case stays selected whole.
    view->setCursorOffset(13);
    QVERIFY(doc->convertCase(true));
    QCOMPARE(view->selectedText(), QStringLiteral("STRASSE"));
}

void TestEditCommands::selectsLines()
{
    const auto doc = documentWith(QStringLiteral("select.txt"), "one\ntwo\nthree");
    ui::TextView* view = doc->textView();
    view->setCursorOffset(1);
    view->selectLines();
    QCOMPARE(view->selectionStart(), 0u);
    QCOMPARE(view->selectionEnd(), 4u); // the line break too
    view->selectLines();
    QCOMPARE(view->selectionEnd(), 8u); // and the next line
    view->selectLines();
    QCOMPARE(view->selectionEnd(), 13u);
    // Part of two lines: both, but not the line after a selection ending at its start.
    view->select(2, 8);
    view->selectLines();
    QCOMPARE(view->selectionStart(), 0u);
    QCOMPARE(view->selectionEnd(), 8u);

    // A triple click.
    doc->resize(400, 200);
    doc->show();
    QVERIFY(QTest::qWaitForWindowExposed(doc.get()));
    view->setCursorOffset(5);
    const QPoint point = view->cursorRect().center();
    QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, point);
    QCOMPARE(view->selectedText(), QStringLiteral("two"));
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, point);
    QCOMPARE(view->selectionStart(), 4u);
    QCOMPARE(view->selectionEnd(), 8u);
}

void TestEditCommands::homeGoesToTheTextFirst()
{
    const auto doc = documentWith(QStringLiteral("home.txt"), "    indented\nflat\n");
    ui::TextView* view = doc->textView();
    view->setReadOnly(false);
    view->setCursorOffset(10);
    QTest::keyClick(view, Qt::Key_Home);
    QCOMPARE(view->cursorOffset(), 4u); // where the text starts
    QTest::keyClick(view, Qt::Key_Home);
    QCOMPARE(view->cursorOffset(), 0u); // the very start
    QTest::keyClick(view, Qt::Key_Home);
    QCOMPARE(view->cursorOffset(), 4u);
    view->setCursorOffset(12);
    QTest::keyClick(view, Qt::Key_Home, Qt::ShiftModifier);
    QCOMPARE(view->selectedText(), QStringLiteral("indented"));
}

void TestEditCommands::ctrlArrowsScroll()
{
    std::string text;
    for (int i = 0; i < 200; ++i)
        text += "line\n";
    const auto doc = documentWith(QStringLiteral("scroll.txt"), text);
    ui::TextView* view = doc->textView();
    doc->resize(300, 200);
    doc->show();
    QVERIFY(QTest::qWaitForWindowExposed(doc.get()));
    view->setCursorOffset(0);
    for (int i = 0; i < 5; ++i)
        QTest::keyClick(view, Qt::Key_Down, Qt::ControlModifier);
    QCOMPARE(view->topRow(), 5u);
    QCOMPARE(view->cursorLine(), 0u); // the caret stayed
    QTest::keyClick(view, Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(view->topRow(), 4u);
    QTest::keyClick(view, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier); // selects, as before
    QCOMPARE(view->cursorLine(), 1u);
}

void TestEditCommands::shortcuts()
{
    ui::MainWindow window;
    QCOMPARE(findAction(window, QStringLiteral("Toggle Co&mment"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Slash));
    QCOMPARE(findAction(window, QStringLiteral("Select Line"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_L));
    QCOMPARE(findAction(window, QStringLiteral("Delete L&ine"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
    QCOMPARE(findAction(window, QStringLiteral("&Join Lines"))->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_J));
    QCOMPARE(findAction(window, QStringLiteral("&Copy Line Up"))->shortcut(),
             QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_Up));
    QVERIFY(findAction(window, QStringLiteral("Dup&licate Line"))
                ->shortcuts()
                .contains(QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_Down)));
    QVERIFY(findAction(window, QStringLiteral("Insert Line &Below"))
                ->shortcuts()
                .contains(QKeySequence(Qt::CTRL | Qt::Key_Return)));
    QVERIFY(findAction(window, QStringLiteral("Insert Line Abo&ve"))
                ->shortcuts()
                .contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return)));
}

QTEST_MAIN(TestEditCommands)
#include "tst_editcommands.moc"
