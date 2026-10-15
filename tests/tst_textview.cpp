#include "core/FileSource.h"
#include "core/TextCodec.h"
#include "ui/DocumentView.h"
#include "ui/HexView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDecoder.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace aiedit;
using ui::TextView;

namespace {

std::span<const std::uint8_t> bytesOf(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

const core::TextCodec& codec(std::string_view name)
{
    return *core::findCodec(name);
}

} // namespace

class TestTextView : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void decodesUtf8WithByteMap();
    void decodesInvalidAndControlBytes();
    void decodesGb18030();
    void decodesUtf16SurrogatePairs();

    void indexesLargeFileInBackground();
    void codecSwitchReindexes();
    void snapsToCharacterStarts();

    void horizontalNavigation();
    void verticalNavigationKeepsColumn();
    void selectionAndCopyInLegacyEncoding();
    void goToLine();
    void columnOnSplitLongLine();

    void documentViewSyncsViews();
    void syntaxDetection();
    void reloadsWhenFileGrows();
    void followModeTracksTheEnd();
    void reloadsAfterTruncationAndDeletion();
    void highlightingCarriesStateAcrossCheckpoints();
    void renderHighlightedCode();
    void aiActionsInContextMenus();
    void renderScreenshot();

private:
    std::shared_ptr<const core::FileSource> write(const char* name, const std::string& content);

    QTemporaryDir m_dir;
};

void TestTextView::init()
{
    QVERIFY(m_dir.isValid());
}

std::shared_ptr<const core::FileSource> TestTextView::write(const char* name, const std::string& content)
{
    const auto path = ui::toPath(m_dir.filePath(QString::fromLatin1(name)));
    std::ofstream(path, std::ios::binary) << content;
    std::error_code ec;
    return core::FileSource::open(path, ec);
}

void TestTextView::decodesUtf8WithByteMap()
{
    const ui::TextDecoder decoder(codec("UTF-8"));
    const std::string text = "a中😀b"; // 1 + 3 + 4 + 1 bytes
    const ui::DecodedText decoded = decoder.decode(bytesOf(text));
    QCOMPARE(decoded.text, QStringLiteral("a中😀b"));
    QCOMPARE(decoded.text.size(), 5); // the emoji is a surrogate pair
    const std::vector<std::uint32_t> expected{0, 1, 4, 4, 8, 9};
    QCOMPARE(decoded.byteOffsets, expected);
    QCOMPARE(decoded.positionAt(4), 2);
    QCOMPARE(decoded.positionAt(2), 2); // inside 中: the next boundary
    QCOMPARE(decoded.positionAt(9), 5);
}

void TestTextView::decodesInvalidAndControlBytes()
{
    const ui::TextDecoder decoder(codec("UTF-8"));
    const ui::DecodedText decoded = decoder.decode(bytesOf(std::string("x\xFF\x01\t\x7F", 5)));
    QCOMPARE(decoded.text, QString::fromUtf16(u"x�␁\t␡"));
    QCOMPARE(decoded.byteOffsets.size(), 6u);
    QCOMPARE(decoder.decodePlain(bytesOf("a\tb\n")), QStringLiteral("a\tb\n"));
}

void TestTextView::decodesGb18030()
{
    const ui::TextDecoder decoder(codec("GB18030"));
    const ui::DecodedText decoded = decoder.decode(bytesOf("\xC4\xE3\xBA\xC3!"));
    QCOMPARE(decoded.text, QStringLiteral("你好!"));
    const std::vector<std::uint32_t> expected{0, 2, 4, 5};
    QCOMPARE(decoded.byteOffsets, expected);
    QCOMPARE(decoder.decodePlain(bytesOf("\xC4\xE3\xBA\xC3")), QStringLiteral("你好"));
}

void TestTextView::decodesUtf16SurrogatePairs()
{
    const ui::TextDecoder decoder(codec("UTF-16LE"));
    const std::string bytes("A\0=\xD8\x00\xDE", 6); // "A" + U+1F600
    const ui::DecodedText decoded = decoder.decode(bytesOf(bytes));
    QCOMPARE(decoded.text, QStringLiteral("A😀"));
    const std::vector<std::uint32_t> expected{0, 2, 2, 6};
    QCOMPARE(decoded.byteOffsets, expected);
}

void TestTextView::indexesLargeFileInBackground()
{
    std::string content;
    content.reserve(12 * 1024 * 1024);
    for (int i = 0; content.size() < 12 * 1024 * 1024; ++i)
        content += "2026-10-14 12:00:00 INFO request " + std::to_string(i) + " served\n";
    const auto source = write("big.log", content);

    ui::TextDocument document(source, codec("UTF-8"));
    QVERIFY(document.isIndexing()); // more than the synchronous part
    QVERIFY(document.index().rowCount() > 0);
    QSignalSpy finished(&document, &ui::TextDocument::indexFinished);
    QVERIFY(finished.wait(10000));
    QVERIFY(document.index().isComplete());
    QCOMPARE(document.index().lineCount(),
             static_cast<std::uint64_t>(std::count(content.begin(), content.end(), '\n')));
}

void TestTextView::codecSwitchReindexes()
{
    const auto source = write("gbk.txt", "\xC4\xE3\xBA\xC3\n\xCA\xC0\xBD\xE7\n");
    ui::TextDocument document(source, codec("UTF-8"));
    QSignalSpy changed(&document, &ui::TextDocument::codecChanged);
    document.setCodec(codec("GB18030"));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(document.codec().name, std::string_view("GB18030"));
    QCOMPARE(document.index().rowCount(), 2u);
    const auto row = document.index().row(1);
    QCOMPARE(document.decoder().decode(document.bytes().subspan(row.start, row.contentEnd - row.start)).text,
             QStringLiteral("世界"));
}

void TestTextView::snapsToCharacterStarts()
{
    ui::TextDocument utf8(write("snap8.txt", "\xEF\xBB\xBF" "a中b"), codec("UTF-8"));
    QCOMPARE(utf8.snapToCharStart(0), 3u); // inside the BOM
    QCOMPARE(utf8.snapToCharStart(5), 4u); // inside 中
    QCOMPARE(utf8.snapToCharStart(7), 7u);

    ui::TextDocument gbk(write("snapgbk.txt", "a\xC4\xE3\xBA\xC3"), codec("GB18030"));
    QCOMPARE(gbk.snapToCharStart(2), 1u);
    QCOMPARE(gbk.snapToCharStart(4), 3u);

    ui::TextDocument utf16(write("snap16.txt", std::string("\xFF\xFE" "A\0=\xD8\x00\xDE", 8)), codec("UTF-16LE"));
    QCOMPARE(utf16.snapToCharStart(3), 2u);
    QCOMPARE(utf16.snapToCharStart(6), 4u); // low surrogate: back to the pair's start
}

void TestTextView::horizontalNavigation()
{
    ui::TextDocument document(write("nav.txt", "a中b\r\nxy\n"), codec("UTF-8"));
    TextView view;
    view.resize(600, 300);
    view.setDocument(&document);

    QCOMPARE(view.cursorOffset(), 0u);
    QTest::keyClick(&view, Qt::Key_Right);
    QCOMPARE(view.cursorOffset(), 1u);
    QTest::keyClick(&view, Qt::Key_Right); // over 中
    QCOMPARE(view.cursorOffset(), 4u);
    QTest::keyClick(&view, Qt::Key_End);
    QCOMPARE(view.cursorOffset(), 5u); // before the CR
    QTest::keyClick(&view, Qt::Key_Right); // over CRLF to the next line
    QCOMPARE(view.cursorOffset(), 7u);
    QCOMPARE(view.cursorLine(), 1u);
    QTest::keyClick(&view, Qt::Key_Left); // and back to the end of line 1
    QCOMPARE(view.cursorOffset(), 5u);
    QTest::keyClick(&view, Qt::Key_Home);
    QCOMPARE(view.cursorOffset(), 0u);
    QTest::keyClick(&view, Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(view.cursorOffset(), 10u);
}

void TestTextView::verticalNavigationKeepsColumn()
{
    ui::TextDocument document(write("vert.txt", "abcdef\nab\nabcdef\n"), codec("UTF-8"));
    TextView view;
    view.resize(600, 300);
    view.setDocument(&document);

    view.setCursorOffset(5); // line 0, column 5
    QTest::keyClick(&view, Qt::Key_Down); // the short line clamps to its end
    QCOMPARE(view.cursorOffset(), 9u);
    QTest::keyClick(&view, Qt::Key_Down); // the long line restores column 5
    QCOMPARE(view.cursorOffset(), 15u);
    QCOMPARE(view.cursorColumn(), std::optional<std::uint64_t>(5));
    QTest::keyClick(&view, Qt::Key_Up, Qt::ShiftModifier);
    QTest::keyClick(&view, Qt::Key_Up, Qt::ShiftModifier);
    QCOMPARE(view.selectionStart(), 5u);
    QCOMPARE(view.selectionEnd(), 15u);
}

void TestTextView::selectionAndCopyInLegacyEncoding()
{
    ui::TextDocument document(write("copy.txt", "\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7"), codec("GB18030"));
    TextView view;
    view.setDocument(&document);
    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(view.selectionEnd(), 4u);
    QCOMPARE(view.selectedText(), QStringLiteral("你好"));
    view.selectAll();
    QCOMPARE(view.selectedText(), QStringLiteral("你好，世界"));
}

void TestTextView::goToLine()
{
    std::string content;
    for (int i = 1; i <= 1000; ++i)
        content += "line " + std::to_string(i) + "\n";
    ui::TextDocument document(write("lines.txt", content), codec("UTF-8"));
    TextView view;
    view.resize(600, 300);
    view.setDocument(&document);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QVERIFY(view.goToLine(499));
    QCOMPARE(view.cursorLine(), 499u);
    QVERIFY(view.topRow() <= 499 && 499 < view.topRow() + static_cast<std::uint64_t>(view.visibleRows()));
    QVERIFY(!view.goToLine(5000));
}

void TestTextView::columnOnSplitLongLine()
{
    // One 10,000-character line is split into rows; the column still counts from the line start.
    ui::TextDocument document(write("long.txt", std::string(10000, 'x') + "\n"), codec("UTF-8"));
    TextView view;
    view.setDocument(&document);
    view.setCursorOffset(9000);
    QCOMPARE(view.cursorLine(), 0u);
    QCOMPARE(view.cursorColumn(), std::optional<std::uint64_t>(9000));
}

void TestTextView::documentViewSyncsViews()
{
    const auto source = write("sync.txt", "ab中\ncd\n");
    const auto type = core::detectFileType(source->bytes());
    ui::DocumentView doc(source, type, codec("UTF-8"));
    QCOMPARE(doc.viewMode(), ui::DocumentView::ViewMode::Text);

    doc.textView()->select(1, 5); // "b中"
    QCOMPARE(doc.hexView()->selectionStart(), 1u);
    QCOMPARE(doc.hexView()->selectionLength(), 4u);

    // A hex cursor inside 中 puts the text caret at the start of the character.
    doc.hexView()->setCursorOffset(3);
    QCOMPARE(doc.textView()->cursorOffset(), 2u);
    QVERIFY(!doc.textView()->hasSelection());

    doc.setViewMode(ui::DocumentView::ViewMode::Hex);
    QVERIFY(!doc.isTextActive());
    QCOMPARE(doc.cursorOffset(), 3u);

    const auto binary = write("sync.bin", std::string("\x7F" "ELF\0\0\0\0", 8));
    ui::DocumentView binaryDoc(binary, core::detectFileType(binary->bytes()), codec("ISO-8859-1"));
    QCOMPARE(binaryDoc.viewMode(), ui::DocumentView::ViewMode::Hex);
}

void TestTextView::reloadsWhenFileGrows()
{
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("grow.log")));
    std::ofstream(path, std::ios::binary) << "line 1\nline 2\n";
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.textView()->setCursorOffset(3);
    QSignalSpy reloaded(&doc, &ui::DocumentView::sourceChanged);

    std::ofstream(path, std::ios::binary | std::ios::app) << "line 3\n";
    QTRY_COMPARE(doc.textDocument()->index().lineCount(), 3u);
    QVERIFY(reloaded.count() >= 1);
    QCOMPARE(doc.source()->size(), 21u);
    QCOMPARE(doc.textView()->cursorOffset(), 3u); // the caret stays put
    QCOMPARE(doc.hexView()->size(), 21u);
}

void TestTextView::followModeTracksTheEnd()
{
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("follow.log")));
    std::ofstream(path, std::ios::binary) << "start\n";
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.setFollowing(true);
    QCOMPARE(doc.textView()->cursorOffset(), 6u);

    for (int i = 0; i < 3; ++i)
        std::ofstream(path, std::ios::binary | std::ios::app) << "entry " << i << "\n";
    QTRY_COMPARE(doc.source()->size(), 30u);
    QCOMPARE(doc.textView()->cursorOffset(), 30u);
    QCOMPARE(doc.textView()->cursorLine(), 3u); // on the last line

    doc.setFollowing(false);
    doc.textView()->setCursorOffset(0);
    std::ofstream(path, std::ios::binary | std::ios::app) << "more\n";
    QTRY_COMPARE(doc.source()->size(), 35u);
    QCOMPARE(doc.textView()->cursorOffset(), 0u);
}

void TestTextView::reloadsAfterTruncationAndDeletion()
{
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("rotate.log")));
    std::ofstream(path, std::ios::binary) << std::string(10000, 'x') << "\n";
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.textView()->setCursorOffset(9000);

    std::ofstream(path, std::ios::binary | std::ios::trunc) << "fresh\n"; // like copytruncate + new entry
    QTRY_COMPARE(doc.source()->size(), 6u);
    QVERIFY(doc.textView()->cursorOffset() <= 6u);
    QVERIFY(doc.hexView()->cursorOffset() < 6u);

    QSignalSpy messages(&doc, &ui::DocumentView::statusMessage);
    std::filesystem::remove(path);
    QVERIFY(!doc.reload());
    QVERIFY(!messages.isEmpty());
    QCOMPARE(doc.source()->size(), 6u); // still showing the last version
}

void TestTextView::syntaxDetection()
{
    if (!ui::isSyntaxHighlightingAvailable())
        QSKIP("Built without KSyntaxHighlighting");
    QCOMPARE(ui::syntaxForFileName(QStringLiteral("main.cpp")), QStringLiteral("C++"));
    QCOMPARE(ui::syntaxForFileName(QStringLiteral("setup.py")), QStringLiteral("Python"));
    QCOMPARE(ui::syntaxForFileName(QStringLiteral("CMakeLists.txt")), QStringLiteral("CMake"));
    QVERIFY(ui::syntaxForFileName(QStringLiteral("data.no-such-extension")).isEmpty());

    ui::SyntaxHighlighter highlighter(QStringLiteral("C++"), QPalette());
    QVERIFY(highlighter.isActive());
    ui::SyntaxState state;
    const auto first = highlighter.highlightLine(QStringLiteral("int x; /* open"), state);
    QVERIFY(!first.isEmpty());
    QCOMPARE(first.front().start, 0); // "int"
    // The comment carries over: the next line starts inside it.
    const auto inside = highlighter.highlightLine(QStringLiteral("still a comment */ int y;"), state);
    ui::SyntaxState fresh;
    const auto outside = highlighter.highlightLine(QStringLiteral("still a comment */ int y;"), fresh);
    QVERIFY(!inside.isEmpty());
    QCOMPARE(inside.front().start, 0);
    QVERIFY(outside.isEmpty() || outside.front().start != 0 ||
            outside.front().format.foreground() != inside.front().format.foreground());

    const ui::SyntaxHighlighter none{QString(), QPalette()};
    QVERIFY(!none.isActive());
}

void TestTextView::highlightingCarriesStateAcrossCheckpoints()
{
    if (!ui::isSyntaxHighlightingAvailable())
        QSKIP("Built without KSyntaxHighlighting");
    // A block comment spanning several state checkpoints (one every 64 rows).
    std::string code = "/* start\n";
    for (int i = 0; i < 150; ++i)
        code += "comment line " + std::to_string(i) + "\n";
    code += "end */\nint value = 42;\n";
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("comment.cpp")));
    std::ofstream(path, std::ios::binary) << code;
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    QCOMPARE(doc.syntax(), QStringLiteral("C++"));
    TextView* view = doc.textView();

    // Ask for a deep row first, so the state has to be built through the checkpoints.
    const auto deep = view->rowFormats(140);
    const auto early = view->rowFormats(1);
    QVERIFY(!deep.isEmpty() && !early.isEmpty());
    QCOMPARE(deep.front().start, 0);
    QCOMPARE(deep.front().format.foreground(), early.front().format.foreground()); // both comment
    const auto after = view->rowFormats(152); // "int value = 42;"
    QVERIFY(!after.isEmpty());
    QVERIFY(after.front().format.foreground() != early.front().format.foreground());

    doc.setSyntax(QString());
    QVERIFY(view->rowFormats(140).isEmpty());
}

void TestTextView::renderHighlightedCode()
{
    const std::string code =
        "#include <iostream>\n"
        "\n"
        "// A tiny example\n"
        "int main(int argc, char* argv[])\n"
        "{\n"
        "    const char* text = \"你好，AIEdit\";  /* 中文注释 */\n"
        "    for (int i = 0; i < 3; ++i)\n"
        "        std::cout << text << i << '\\n';\n"
        "    return argc > 1 ? 0x1F : 42;\n"
        "}\n";
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("example.cpp")));
    std::ofstream(path, std::ios::binary) << code;

    ui::MainWindow window;
    window.resize(1000, 420);
    QVERIFY(window.openFile(ui::toQString(path)));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.currentDocument()->textView()->setFocus();
    window.currentDocument()->textView()->setCursorOffset(60);
    QCoreApplication::processEvents();
    const QPixmap shot = window.grab();
    QVERIFY(!shot.isNull());
    if (const QByteArray dir = qgetenv("AIEDIT_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(shot.save(QString::fromLocal8Bit(dir) + QStringLiteral("/highlight.png")));
}

void TestTextView::aiActionsInContextMenus()
{
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("book.txt")));
    std::ofstream(path, std::ios::binary) << "第一章\n很久以前，有一座山。\nOnce upon a time there was a mountain.\n";
    ui::MainWindow window;
    QVERIFY(window.openFile(ui::toQString(path)));
    ui::DocumentView* doc = window.currentDocument();

    const auto actionTexts = [](QMenu* menu) {
        QStringList texts;
        for (const QAction* action : menu->actions())
            if (!action->isSeparator())
                texts << action->text();
        return texts;
    };
    const auto find = [](QMenu* menu, const QString& text) -> QAction* {
        for (QAction* action : menu->actions())
            if (action->text() == text)
                return action;
        return nullptr;
    };

    // Without a selection, the selection actions are there but disabled; asking is not.
    auto menu = doc->textView()->createContextMenu();
    QCOMPARE(actionTexts(menu.get()), (QStringList{QStringLiteral("Copy"), QStringLiteral("AI &Explain"),
                                                   QStringLiteral("AI &Translate"), QStringLiteral("AI &Summarize"),
                                                   QStringLiteral("&Ask AI…"), QStringLiteral("Select All")}));
    QVERIFY(!find(menu.get(), QStringLiteral("AI &Explain"))->isEnabled());
    QVERIFY(find(menu.get(), QStringLiteral("&Ask AI…"))->isEnabled());

    // Selecting a sentence enables them, as a right-click on it would see.
    doc->textView()->select(10, 40);
    menu = doc->textView()->createContextMenu();
    QVERIFY(find(menu.get(), QStringLiteral("AI &Explain"))->isEnabled());
    QVERIFY(find(menu.get(), QStringLiteral("AI &Translate"))->isEnabled());

    // The hex view offers the byte-oriented set, including identifying the file.
    doc->setViewMode(ui::DocumentView::ViewMode::Hex);
    menu = doc->hexView()->createContextMenu();
    const QStringList hexTexts = actionTexts(menu.get());
    QVERIFY(hexTexts.contains(QStringLiteral("What Is This &File?")));
    QVERIFY(!hexTexts.contains(QStringLiteral("AI &Translate")));
    QVERIFY(find(menu.get(), QStringLiteral("AI &Explain"))->isEnabled()); // the selection carried over
}

void TestTextView::renderScreenshot()
{
    // Set AIEDIT_SCREENSHOT_DIR to look at the rendering; otherwise this only checks that it paints.
    std::string content = "// AIEdit 文本视图\n";
    content += "int main() {\n\tconst char* greeting = \"你好，世界 👋\";\n\treturn 0;\n}\n";
    content += "2026-10-14 09:30:01 \x1B[31mERROR\x1B[0m connection reset by peer\n";
    content += "混合内容：English, 中文, 日本語, 한국어, emoji 🎉\n";
    content += std::string(300, '=') + "\n";
    for (int i = 0; i < 40; ++i)
        content += "line " + std::to_string(i + 9) + "\n";

    ui::MainWindow window;
    window.resize(1200, 560);
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("render.txt")));
    std::ofstream(path, std::ios::binary) << content;
    window.openFile(ui::toQString(path));
    QCOMPARE(window.documentCount(), 1);
    ui::DocumentView* doc = window.currentDocument();
    doc->setViewMode(ui::DocumentView::ViewMode::Split);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    doc->textView()->setFocus();
    doc->textView()->select(53, 71); // inside the string literal on line 3
    QCoreApplication::processEvents();

    const QPixmap shot = window.grab();
    QVERIFY(!shot.isNull());
    if (const QByteArray dir = qgetenv("AIEDIT_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(shot.save(QString::fromLocal8Bit(dir) + QStringLiteral("/textview.png")));
}

QTEST_MAIN(TestTextView)
#include "tst_textview.moc"
