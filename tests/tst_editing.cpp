#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TabWidget.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QMessageBox>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>

#include <filesystem>
#include <fstream>
#include <iterator>

using namespace aiedit;
using ui::TextView;

namespace {

const core::TextCodec& codec(std::string_view name)
{
    return *core::findCodec(name);
}

std::string contents(const ui::TextDocument& document)
{
    const auto bytes = document.bytes();
    return {bytes.begin(), bytes.end()};
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

// Clicks `button` in the message box that opens next.
void answerNextQuestion(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, [button] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            box->button(button)->click();
    });
}

} // namespace

class TestEditing : public QObject
{
    Q_OBJECT

private slots:
    // Settings and backups go to a test location, away from the user's own.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init();

    void typingUndoAndRedo();
    void keepsTheEncodingAndLineBreaks();
    void refusesCharactersTheEncodingLacks();
    void deletesCharactersWordsAndLineBreaks();
    void enterIndentsAndTabFollowsTheFile();
    void indentsSelectedLines();
    void typesAtTheEndOfTheLastLine();
    void editsAnEmptyFile();
    void inputMethodComposesThenCommits();
    void pasteUsesTheFilesLineBreaks();
    void readOnlyRefusesTyping();

    void savesInPlaceKeepingBomAndPermissions();
    void saveAsShowsTheNewFile();
    void keepsEditsWhenTheFileChangesOnDisk();
    void onlyTextFilesCanBeEdited();
    void asksBeforeClosingUnsavedChanges();
    void editMenuFollowsSelectionAndClipboard();
    void newDocumentsAreUntitledUntilSaved();
    void convertsLineEndings();
    void savesInAnotherEncoding();

    void sortsLines();
    void removesDuplicateAndEmptyLines();
    void trimsTrailingWhitespace();
    void movesAndDuplicatesLines();
    void lineOperationsKeepUnreadableBytes();
    void lineMenuNeedsEditableText();

private:
    std::filesystem::path pathOf(const char* name) const;
    std::shared_ptr<const core::FileSource> write(const char* name, const std::string& content);
    std::unique_ptr<ui::DocumentView> documentWith(const char* name, const std::string& content,
                                                   std::string_view codecName = "UTF-8");

    QTemporaryDir m_dir;
};

void TestEditing::init()
{
    QVERIFY(m_dir.isValid());
}

std::filesystem::path TestEditing::pathOf(const char* name) const
{
    return ui::toPath(m_dir.filePath(QString::fromLatin1(name)));
}

std::shared_ptr<const core::FileSource> TestEditing::write(const char* name, const std::string& content)
{
    const auto path = pathOf(name);
    std::ofstream(path, std::ios::binary) << content;
    std::error_code ec;
    return core::FileSource::open(path, ec);
}

std::unique_ptr<ui::DocumentView> TestEditing::documentWith(const char* name, const std::string& content,
                                                            std::string_view codecName)
{
    const auto source = write(name, content);
    return std::make_unique<ui::DocumentView>(source, core::detectFileType(source->bytes()), codec(codecName));
}

void TestEditing::typingUndoAndRedo()
{
    ui::TextDocument document(write("type.txt", "hello\nworld\n"), codec("UTF-8"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);
    QSignalSpy modified(&document, &ui::TextDocument::modificationChanged);

    view.setCursorOffset(5);
    QTest::keyClicks(&view, " there");
    QCOMPARE(contents(document), std::string("hello there\nworld\n"));
    QCOMPARE(view.cursorOffset(), 11u);
    QVERIFY(document.isModified());
    QCOMPARE(modified.count(), 1);
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(&view, Qt::Key_Backspace);
    QCOMPARE(contents(document), std::string("hello th\nworld\n"));

    // A run of typing, or of deleting, is one step; undoing brings back what went, selected.
    QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(contents(document), std::string("hello there\nworld\n"));
    QCOMPARE(view.selectedText(), QStringLiteral("ere"));
    QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(contents(document), std::string("hello\nworld\n"));
    QCOMPARE(view.cursorOffset(), 5u);
    QVERIFY(!document.isModified()); // as on disk again
    view.redo();
    QCOMPARE(contents(document), std::string("hello there\nworld\n"));
    QCOMPARE(view.cursorOffset(), 11u);

    // Typing after the caret moved is a step of its own.
    view.setCursorOffset(0);
    QTest::keyClicks(&view, ">");
    QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(contents(document), std::string("hello there\nworld\n"));
}

void TestEditing::keepsTheEncodingAndLineBreaks()
{
    // 你好\r\n世界\r\n in GB18030: new text is written in GB18030, new lines end in CRLF.
    const std::string original("\xC4\xE3\xBA\xC3\r\n\xCA\xC0\xBD\xE7\r\n", 12);
    ui::TextDocument document(write("gbk.txt", original), codec("GB18030"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);

    view.setCursorOffset(4);
    QTest::keyClick(&view, Qt::Key_Return);
    view.insertText(QStringLiteral("新"));
    QCOMPARE(contents(document), std::string("\xC4\xE3\xBA\xC3\r\n\xD0\xC2\r\n\xCA\xC0\xBD\xE7\r\n"));
    QCOMPARE(view.cursorLine(), 1u);
    view.undo();
    view.undo();
    QCOMPARE(contents(document), original);
}

void TestEditing::refusesCharactersTheEncodingLacks()
{
    ui::TextDocument document(write("latin1.txt", "abc\n"), codec("ISO-8859-1"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);
    QSignalSpy messages(&view, &TextView::statusMessage);

    view.setCursorOffset(3);
    view.insertText(QStringLiteral("中"));
    QCOMPARE(contents(document), std::string("abc\n")); // nothing written, and a word why
    QCOMPARE(messages.count(), 1);
    view.insertText(QStringLiteral("é"));
    QCOMPARE(contents(document), std::string("abc\xE9\n"));
}

void TestEditing::deletesCharactersWordsAndLineBreaks()
{
    const std::string original = "a中b\r\nxy zz\n";
    ui::TextDocument document(write("delete.txt", original), codec("UTF-8"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);

    view.setCursorOffset(4); // after 中
    QTest::keyClick(&view, Qt::Key_Backspace);
    QCOMPARE(contents(document), std::string("ab\r\nxy zz\n"));
    QCOMPARE(view.cursorOffset(), 1u);

    view.setCursorOffset(4); // at the start of the second line: joins the lines, CRLF and all
    QTest::keyClick(&view, Qt::Key_Backspace);
    QCOMPARE(contents(document), std::string("abxy zz\n"));
    QTest::keyClick(&view, Qt::Key_Delete);
    QCOMPARE(contents(document), std::string("aby zz\n"));
    QTest::keyClick(&view, Qt::Key_End);
    QTest::keyClick(&view, Qt::Key_Backspace, Qt::ControlModifier); // a word
    QCOMPARE(contents(document), std::string("aby \n"));

    for (int i = 0; i < 4; ++i)
        QTest::keyClick(&view, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(contents(document), original);
    QVERIFY(!document.canUndo());
}

void TestEditing::enterIndentsAndTabFollowsTheFile()
{
    // Indented with two spaces: Enter keeps the indent, Tab adds two spaces.
    ui::TextDocument yaml(write("config.yaml", "server:\n  port: 80\n  host: x\n"), codec("UTF-8"));
    TextView view;
    view.setDocument(&yaml);
    view.setReadOnly(false);
    view.setCursorOffset(18); // after "port: 80"
    QTest::keyClick(&view, Qt::Key_Return);
    QTest::keyClicks(&view, "tls: on");
    QCOMPARE(contents(yaml), std::string("server:\n  port: 80\n  tls: on\n  host: x\n"));
    QTest::keyClick(&view, Qt::Key_Home);
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(contents(yaml), std::string("server:\n  port: 80\n    tls: on\n  host: x\n"));
    QVERIFY(view.hasFocus() || !view.isVisible()); // Tab did not move the focus away

    // Indented with tabs: Tab inserts a tab.
    ui::TextDocument tabs(write("make.txt", "all:\n\tcc a.c\n\tcc b.c\n"), codec("UTF-8"));
    view.setDocument(&tabs);
    view.setCursorOffset(5);
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(contents(tabs), std::string("all:\n\t\tcc a.c\n\tcc b.c\n"));
    view.setDocument(nullptr);
}

void TestEditing::indentsSelectedLines()
{
    const std::string original = "x\n  y\nz\n";
    ui::TextDocument document(write("block.txt", original), codec("UTF-8"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);

    view.select(0, 6); // the first two lines, up to the start of the third
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(contents(document), std::string("  x\n    y\nz\n"));
    QCOMPARE(view.selectionStart(), 0u); // still selected, so Tab can go on
    QTest::keyClick(&view, Qt::Key_Backtab);
    QTest::keyClick(&view, Qt::Key_Backtab);
    QCOMPARE(contents(document), std::string("x\ny\nz\n"));
    view.undo();
    view.undo();
    view.undo();
    QCOMPARE(contents(document), original);
}

void TestEditing::typesAtTheEndOfTheLastLine()
{
    // After the final line break there is no line on screen: the caret shows at the end of the
    // last line, and that is where typing goes.
    ui::TextDocument document(write("end.txt", "line\n"), codec("UTF-8"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);
    QTest::keyClick(&view, Qt::Key_End, Qt::ControlModifier);
    QTest::keyClicks(&view, "x");
    QCOMPARE(contents(document), std::string("linex\n"));
    QTest::keyClick(&view, Qt::Key_Return);
    QTest::keyClicks(&view, "y");
    QCOMPARE(contents(document), std::string("linex\ny\n"));
}

void TestEditing::editsAnEmptyFile()
{
    ui::TextDocument document(write("empty.txt", ""), codec("UTF-8"));
    TextView view;
    view.resize(400, 200);
    view.setDocument(&document);
    view.setReadOnly(false);
    QVERIFY(!view.cursorRect().isNull()); // a caret on line 1, though there are no rows yet
    QTest::keyClicks(&view, "abc");
    QTest::keyClick(&view, Qt::Key_Return);
    // Past the last line break there is no line on screen: the new line gets one too, and
    // typing goes on there.
    QCOMPARE(contents(document), std::string("abc\n\n"));
    QTest::keyClicks(&view, "d");
    QCOMPARE(contents(document), std::string("abc\nd\n"));
}

void TestEditing::inputMethodComposesThenCommits()
{
    ui::TextDocument document(write("ime.txt", "ab\n"), codec("UTF-8"));
    TextView view;
    view.resize(400, 200);
    view.setDocument(&document);
    view.setReadOnly(false);
    view.setCursorOffset(1);
    QVERIFY(view.inputMethodQuery(Qt::ImEnabled).toBool());
    QVERIFY(view.inputMethodQuery(Qt::ImCursorRectangle).toRect().isValid());

    QInputMethodEvent composing(QStringLiteral("ni"), {});
    QCoreApplication::sendEvent(&view, &composing);
    QCOMPARE(contents(document), std::string("ab\n")); // shown, not written yet
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("你"));
    QCoreApplication::sendEvent(&view, &commit);
    QCOMPARE(contents(document), std::string("a你b\n"));
    QCOMPARE(view.cursorOffset(), 4u);
}

void TestEditing::pasteUsesTheFilesLineBreaks()
{
    ui::TextDocument document(write("crlf.txt", "a\r\nb\r\n"), codec("UTF-8"));
    TextView view;
    view.setDocument(&document);
    view.setReadOnly(false);
    QGuiApplication::clipboard()->setText(QStringLiteral("x\ny"));
    view.setCursorOffset(1);
    view.paste();
    QCOMPARE(contents(document), std::string("ax\r\ny\r\nb\r\n"));

    view.select(0, 2); // "ax"
    view.cut();
    QCOMPARE(contents(document), std::string("\r\ny\r\nb\r\n"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("ax"));
}

void TestEditing::readOnlyRefusesTyping()
{
    ui::TextDocument document(write("view.txt", "abc\n"), codec("UTF-8"));
    TextView view; // read-only until told otherwise
    view.setDocument(&document);
    QSignalSpy messages(&view, &TextView::statusMessage);
    QTest::keyClicks(&view, "xyz");
    QTest::keyClick(&view, Qt::Key_Backspace);
    view.paste();
    QCOMPARE(contents(document), std::string("abc\n"));
    QVERIFY(!messages.isEmpty());
    QVERIFY(!view.inputMethodQuery(Qt::ImEnabled).toBool());
}

void TestEditing::savesInPlaceKeepingBomAndPermissions()
{
    const auto path = pathOf("bom.txt");
    std::ofstream(path, std::ios::binary) << "\xEF\xBB\xBF" "hello\r\n";
    namespace fs = std::filesystem;
    const auto mode = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
    fs::permissions(path, mode, fs::perm_options::replace);
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    QVERIFY(doc.isEditable());
    QSignalSpy modified(&doc, &ui::DocumentView::modificationChanged);

    doc.textView()->setCursorOffset(8); // after "hello"
    doc.textView()->insertText(QStringLiteral("!"));
    QVERIFY(doc.isModified());
    QString error;
    QVERIFY2(doc.save(&error), qPrintable(error));
    QVERIFY(!doc.isModified());
    QCOMPARE(modified.count(), 2);
    QCOMPARE(readFile(path), std::string("\xEF\xBB\xBF" "hello!\r\n"));
#ifndef Q_OS_WIN
    QCOMPARE(fs::status(path).permissions() & fs::perms::all, mode);
#endif

    // The save itself is no change on disk to reload: the undo history is still there.
    QTest::qWait(400);
    QVERIFY(doc.textDocument()->canUndo());
    doc.textView()->undo();
    QVERIFY(doc.isModified()); // now different from what was saved
}

void TestEditing::saveAsShowsTheNewFile()
{
    const std::shared_ptr<const core::FileSource> source = write("first.txt", "one\n");
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    QSignalSpy moved(&doc, &ui::DocumentView::pathChanged);
    doc.textView()->setCursorOffset(3);
    doc.textView()->insertText(QStringLiteral("!"));

    const auto target = pathOf("second.txt");
    QString error;
    QVERIFY2(doc.saveAs(ui::toQString(target), &error), qPrintable(error));
    QCOMPARE(moved.count(), 1);
    QCOMPARE(doc.filePath(), ui::toQString(target));
    QVERIFY(!doc.isModified());
    QCOMPARE(readFile(target), std::string("one!\n"));
    QCOMPARE(readFile(pathOf("first.txt")), std::string("one\n")); // untouched
}

void TestEditing::keepsEditsWhenTheFileChangesOnDisk()
{
    const auto path = pathOf("busy.log");
    std::ofstream(path, std::ios::binary) << "first\n";
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.textView()->insertText(QStringLiteral("X"));
    QSignalSpy messages(&doc, &ui::DocumentView::statusMessage);

    std::ofstream(path, std::ios::binary | std::ios::app) << "second\n";
    QTRY_VERIFY(!messages.isEmpty()); // told, not reloaded
    QTest::qWait(400);
    QCOMPARE(contents(*doc.textDocument()), std::string("Xfirst\n"));
    QVERIFY(doc.isModified());
}

void TestEditing::onlyTextFilesCanBeEdited()
{
    const auto text = write("plain.txt", "text\n");
    QVERIFY(ui::DocumentView(text, core::detectFileType(text->bytes()), core::utf8Codec()).isEditable());

    const auto binary = write("program.bin", std::string("\x7F" "ELF\x02\x01\x01\0\0\0\0\0", 12));
    ui::DocumentView binaryDoc(binary, core::detectFileType(binary->bytes()), codec("ISO-8859-1"));
    QVERIFY(!binaryDoc.isEditable());
    QVERIFY(binaryDoc.textView()->isReadOnly());

    std::string big;
    while (big.size() <= core::EditBuffer::kMaxEditableSize)
        big += "a line of a large log file, which is only for viewing\n";
    const auto large = write("large.log", big);
    QVERIFY(!ui::DocumentView(large, core::detectFileType(large->bytes()), core::utf8Codec()).isEditable());
}

void TestEditing::asksBeforeClosingUnsavedChanges()
{
    const auto path = pathOf("notes.txt");
    std::ofstream(path, std::ios::binary) << "notes\n";
    ui::MainWindow window;
    QVERIFY(window.openFile(ui::toQString(path)));
    ui::DocumentView* doc = window.currentDocument();
    auto* tabs = window.findChild<QTabWidget*>();
    QAction* closeTab = nullptr;
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == QStringLiteral("&Close Tab"))
            closeTab = action;
    QVERIFY(tabs && closeTab);

    doc->textView()->insertText(QStringLiteral("more "));
    QCOMPARE(tabs->tabText(0), QStringLiteral("*notes.txt"));

    answerNextQuestion(QMessageBox::Cancel);
    closeTab->trigger();
    QCOMPARE(window.documentCount(), 1); // kept

    answerNextQuestion(QMessageBox::Save);
    closeTab->trigger();
    QCOMPARE(window.documentCount(), 0);
    QCOMPARE(readFile(path), std::string("more notes\n"));
}

void TestEditing::editMenuFollowsSelectionAndClipboard()
{
    const auto path = pathOf("menu.txt");
    std::ofstream(path, std::ios::binary) << "some text\n";
    ui::MainWindow window;
    QVERIFY(window.openFile(ui::toQString(path)));
    ui::DocumentView* doc = window.currentDocument();
    QAction* copy = findAction(window, QStringLiteral("&Copy"));
    QAction* copyHex = findAction(window, QStringLiteral("Copy as &Hex"));
    QAction* cut = findAction(window, QStringLiteral("Cu&t"));
    QAction* paste = findAction(window, QStringLiteral("&Paste"));
    QAction* undo = findAction(window, QStringLiteral("&Undo"));
    QVERIFY(copy && copyHex && cut && paste && undo);

    // Nothing selected: nothing to copy or cut.
    QVERIFY(!copy->isEnabled() && !copyHex->isEnabled() && !cut->isEnabled() && !undo->isEnabled());
    doc->textView()->select(0, 4);
    QVERIFY(copy->isEnabled() && copyHex->isEnabled() && cut->isEnabled());
    doc->textView()->setCursorOffset(2);
    QVERIFY(!copy->isEnabled() && !cut->isEnabled());

    // Paste follows what the clipboard holds.
    QGuiApplication::clipboard()->clear();
    QVERIFY(!paste->isEnabled());
    QGuiApplication::clipboard()->setText(QStringLiteral("x"));
    QVERIFY(paste->isEnabled());
    doc->textView()->insertText(QStringLiteral("!"));
    QVERIFY(undo->isEnabled());

    // In the hex view the byte at the cursor is selected, so it can be copied; text cannot be
    // edited there.
    doc->setViewMode(ui::DocumentView::ViewMode::Hex);
    QVERIFY(copy->isEnabled());
    QVERIFY(!paste->isEnabled() && !cut->isEnabled() && !undo->isEnabled());
}

void TestEditing::newDocumentsAreUntitledUntilSaved()
{
    ui::MainWindow window;
    auto* tabs = window.findChild<QTabWidget*>();
    QAction* newAction = findAction(window, QStringLiteral("&New"));
    QAction* reload = findAction(window, QStringLiteral("&Reload"));
    QAction* selectAll = findAction(window, QStringLiteral("Select &All"));
    QVERIFY(tabs && newAction && reload && selectAll);

    newAction->trigger();
    QCOMPARE(window.documentCount(), 1);
    ui::DocumentView* doc = window.currentDocument();
    QVERIFY(doc->isUntitled() && doc->isEditable());
    QCOMPARE(tabs->tabText(0), QStringLiteral("Untitled 1"));
    QVERIFY(!reload->isEnabled());    // nothing on disk to reload
    QVERIFY(!selectAll->isEnabled()); // nor anything to select

    QTest::keyClicks(doc->textView(), "hello");
    QCOMPARE(tabs->tabText(0), QStringLiteral("*Untitled 1"));
    QVERIFY(selectAll->isEnabled());
    QString error;
    QVERIFY(!doc->save(&error)); // it needs a file first
    const auto path = pathOf("hello.txt");
    QVERIFY2(doc->saveAs(ui::toQString(path), &error), qPrintable(error));
    QCOMPARE(readFile(path), std::string("hello"));
    QVERIFY(!doc->isUntitled());
    QCOMPARE(tabs->tabText(0), QStringLiteral("hello.txt"));
    QVERIFY(reload->isEnabled());

    // The + after the tabs, and a double click beside them, open more new documents.
    QToolButton* plus = window.findChild<ui::TabWidget*>()->newTabButton();
    plus->click();
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(tabs->tabText(1), QStringLiteral("Untitled 2"));
    emit tabs->tabBarDoubleClicked(-1);
    QCOMPARE(window.documentCount(), 3);

    // An untouched new document closes without asking.
    emit tabs->tabCloseRequested(2);
    QCOMPARE(window.documentCount(), 2);
}

void TestEditing::convertsLineEndings()
{
    const auto source = write("crlf.txt", "one\r\ntwo\r\nthree");
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.textView()->setCursorOffset(6); // on the second line
    QVERIFY(doc.convertLineEndings(QStringLiteral("\n")));
    QCOMPARE(contents(*doc.textDocument()), std::string("one\ntwo\nthree"));
    QCOMPARE(doc.textView()->cursorLine(), 1u); // the caret kept its line
    QCOMPARE(doc.textDocument()->lineTerminator(), QStringLiteral("\n"));
    QVERIFY(!doc.convertLineEndings(QStringLiteral("\n"))); // nothing left to change
    doc.textView()->undo();
    QCOMPARE(contents(*doc.textDocument()), std::string("one\r\ntwo\r\nthree")); // one step

    const auto mixed = write("mixed.txt", "a\nb\r\nc\n");
    ui::DocumentView mixedDoc(mixed, core::detectFileType(mixed->bytes()), core::utf8Codec());
    QVERIFY(mixedDoc.convertLineEndings(QStringLiteral("\r\n")));
    QCOMPARE(contents(*mixedDoc.textDocument()), std::string("a\r\nb\r\nc\r\n"));

    // In UTF-16 a line break is two bytes per character.
    const auto wide = write("wide.txt", std::string("\xFF\xFE" "a\0\r\0\n\0" "b\0", 10));
    ui::DocumentView wideDoc(wide, core::detectFileType(wide->bytes()), codec("UTF-16LE"));
    QVERIFY(wideDoc.convertLineEndings(QStringLiteral("\n")));
    QCOMPARE(contents(*wideDoc.textDocument()), std::string("\xFF\xFE" "a\0\n\0" "b\0", 8));
}

void TestEditing::savesInAnotherEncoding()
{
    // 你好 in GB18030, saved as UTF-8: the file is rewritten and read as UTF-8 from then on.
    const auto path = pathOf("gbk-to-utf8.txt");
    std::ofstream(path, std::ios::binary) << "\xC4\xE3\xBA\xC3\n";
    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), codec("GB18030"));
    doc.textView()->insertText(QStringLiteral("！")); // unsaved edits go along
    QCOMPARE(doc.saveWithCodec(codec("UTF-8"), false), ui::DocumentView::Conversion::Saved);
    QCOMPARE(readFile(path), std::string("！你好\n"));
    QCOMPARE(doc.textDocument()->codec().name, std::string_view("UTF-8"));
    QVERIFY(!doc.isModified());

    // UTF-16 gets a byte order mark, so that it is recognised.
    QCOMPARE(doc.saveWithCodec(codec("UTF-16LE"), false), ui::DocumentView::Conversion::Saved);
    QCOMPARE(readFile(path), std::string("\xFF\xFE\x01\xFF\x60\x4F\x7D\x59\n\0", 10));

    // A codec that cannot hold the text changes nothing.
    QCOMPARE(doc.saveWithCodec(codec("ISO-8859-1"), false), ui::DocumentView::Conversion::Unwritable);
    QCOMPARE(readFile(path).size(), 10u);

    // Bytes that are not valid in the current codec need a yes before they become U+FFFD.
    const auto broken = pathOf("broken.txt");
    std::ofstream(broken, std::ios::binary) << "a\xFF" "b\n";
    source = core::FileSource::open(broken, ec);
    ui::DocumentView brokenDoc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    QCOMPARE(brokenDoc.saveWithCodec(codec("UTF-16LE"), false), ui::DocumentView::Conversion::Unreadable);
    QCOMPARE(readFile(broken), std::string("a\xFF" "b\n"));
    QCOMPARE(brokenDoc.saveWithCodec(codec("UTF-16LE"), true), ui::DocumentView::Conversion::Saved);
    QCOMPARE(readFile(broken), std::string("\xFF\xFE" "a\0\xFD\xFF" "b\0\n\0", 10));
}

void TestEditing::sortsLines()
{
    const std::string original = "pear\nApple\napple\nitem10\nitem2\nbanana";
    const auto doc = documentWith("sort.txt", original);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::SortAscending));
    // By the locale, case aside (equal lines keep their order), numbers by value. The line
    // breaks stay where they were, so the file still ends without one.
    QCOMPARE(contents(*doc->textDocument()), std::string("Apple\napple\nbanana\nitem2\nitem10\npear"));
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::SortAscending)); // in order already
    QVERIFY(doc->applyLineOperation(ui::LineOperation::SortDescending));
    QCOMPARE(contents(*doc->textDocument()), std::string("pear\nitem10\nitem2\nbanana\nApple\napple"));
    doc->undo();
    doc->undo();
    QCOMPARE(contents(*doc->textDocument()), original); // one step each

    // Only the selected lines; a selection ending where a line starts leaves that line out.
    const auto part = documentWith("part.txt", "z\nc\nb\na\n");
    part->textView()->select(2, 6);
    QVERIFY(part->applyLineOperation(ui::LineOperation::SortAscending));
    QCOMPARE(contents(*part->textDocument()), std::string("z\nb\nc\na\n"));
    QCOMPARE(part->textView()->selectionStart(), 2u); // still selected
    QCOMPARE(part->textView()->selectionEnd(), 6u);

    const auto crlf = documentWith("crlf-sort.txt", "b\r\na\r\n");
    QVERIFY(crlf->applyLineOperation(ui::LineOperation::SortAscending));
    QCOMPARE(contents(*crlf->textDocument()), std::string("a\r\nb\r\n"));
}

void TestEditing::removesDuplicateAndEmptyLines()
{
    const auto doc = documentWith("dups.txt", "a\nb\na\n\n  \nb\nc");
    QVERIFY(doc->applyLineOperation(ui::LineOperation::RemoveDuplicates));
    QCOMPARE(contents(*doc->textDocument()), std::string("a\nb\n\n  \nc")); // the first of each stays
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::RemoveDuplicates));
    QVERIFY(doc->applyLineOperation(ui::LineOperation::RemoveEmpty)); // also one of only spaces
    QCOMPARE(contents(*doc->textDocument()), std::string("a\nb\nc"));
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::RemoveEmpty));
}

void TestEditing::trimsTrailingWhitespace()
{
    const auto doc = documentWith("trim.txt", "a  \nb\t\n c \nd");
    QVERIFY(doc->applyLineOperation(ui::LineOperation::TrimTrailingWhitespace));
    QCOMPARE(contents(*doc->textDocument()), std::string("a\nb\n c\nd")); // leading space kept
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::TrimTrailingWhitespace));

    // In UTF-16 a space is two bytes.
    const auto wide = documentWith("wide-trim.txt", std::string("\xFF\xFE" "a\0 \0\n\0", 8), "UTF-16LE");
    QVERIFY(wide->applyLineOperation(ui::LineOperation::TrimTrailingWhitespace));
    QCOMPARE(contents(*wide->textDocument()), std::string("\xFF\xFE" "a\0\n\0", 6));
}

void TestEditing::movesAndDuplicatesLines()
{
    const std::string original = "one\ntwo\nthree";
    const auto doc = documentWith("move.txt", original);
    TextView* view = doc->textView();
    view->setCursorOffset(5); // on "two", after the t
    QVERIFY(doc->applyLineOperation(ui::LineOperation::MoveUp));
    QCOMPARE(contents(*doc->textDocument()), std::string("two\none\nthree"));
    QCOMPARE(view->cursorOffset(), 1u); // the caret went along
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::MoveUp)); // at the top
    QVERIFY(doc->applyLineOperation(ui::LineOperation::MoveDown));
    QVERIFY(doc->applyLineOperation(ui::LineOperation::MoveDown));
    // The last line has no line break: the line moved there does not get one either.
    QCOMPARE(contents(*doc->textDocument()), std::string("one\nthree\ntwo"));
    QCOMPARE(view->cursorOffset(), 11u);
    QVERIFY(!doc->applyLineOperation(ui::LineOperation::MoveDown)); // at the bottom

    QVERIFY(doc->applyLineOperation(ui::LineOperation::Duplicate));
    QCOMPARE(contents(*doc->textDocument()), std::string("one\nthree\ntwo\ntwo"));
    QCOMPARE(view->cursorOffset(), 15u); // on the copy

    // Several lines move together, and stay selected.
    view->select(0, 9);
    QVERIFY(doc->applyLineOperation(ui::LineOperation::MoveDown));
    QCOMPARE(contents(*doc->textDocument()), std::string("two\none\nthree\ntwo"));
    QCOMPARE(view->selectionStart(), 4u);
    QCOMPARE(view->selectionEnd(), 13u);

    for (int step = 0; step < 5; ++step)
        doc->undo();
    QCOMPARE(contents(*doc->textDocument()), original); // one step each
}

void TestEditing::lineOperationsKeepUnreadableBytes()
{
    // Bytes that are not UTF-8 go with their lines, as they are.
    const auto doc = documentWith("bytes.txt", "b\xFF\na\xFE\n");
    QVERIFY(doc->isEditable());
    QVERIFY(doc->applyLineOperation(ui::LineOperation::SortAscending));
    QCOMPARE(contents(*doc->textDocument()), std::string("a\xFE\nb\xFF\n"));
}

void TestEditing::lineMenuNeedsEditableText()
{
    const auto path = pathOf("menu-lines.txt");
    std::ofstream(path, std::ios::binary) << "b\na\n";
    ui::MainWindow window;
    QVERIFY(window.openFile(ui::toQString(path)));
    QAction* sort = findAction(window, QStringLiteral("Sort &Ascending"));
    QAction* moveUp = findAction(window, QStringLiteral("Move Line U&p"));
    QVERIFY(sort && moveUp && sort->isEnabled());
    QCOMPARE(moveUp->shortcut(), QKeySequence(Qt::ALT | Qt::Key_Up));
    sort->trigger();
    QCOMPARE(contents(*window.currentDocument()->textDocument()), std::string("a\nb\n"));
    QVERIFY(window.statusBar()->currentMessage().startsWith(QStringLiteral("Sorted 2 line")));

    window.currentDocument()->setViewMode(ui::DocumentView::ViewMode::Hex); // no lines to edit there
    QVERIFY(!sort->isEnabled() && !moveUp->isEnabled());
}

QTEST_MAIN(TestEditing)
#include "tst_editing.moc"
