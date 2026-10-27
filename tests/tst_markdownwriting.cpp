#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/AiEditBar.h"
#include "ui/AiPanel.h"
#include "ui/AiSettings.h"
#include "ui/DocumentView.h"
#include "ui/FindBar.h"
#include "ui/HexView.h"
#include "ui/MainWindow.h"
#include "ui/MarkdownView.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>

#include <fstream>
#include <memory>

using namespace aiedit;
using Edit = ui::DocumentView::MarkdownEdit;
using Mode = ui::DocumentView::ViewMode;

namespace {

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

// Notes long enough to scroll: forty sections under headings.
std::string notes()
{
    std::string text = "# 读书笔记\n\n开头的话。\n\n";
    for (int i = 1; i <= 40; ++i) {
        text += "## 第" + std::to_string(i) + "节\n\n";
        for (int j = 0; j < 6; ++j)
            text += "这一节写的是第" + std::to_string(i) + "件事，写得很长很长，一行接着一行。\n";
        text += "\n";
    }
    return text;
}

QString textOf(const ui::DocumentView& doc)
{
    const auto bytes = doc.textDocument()->bytes();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

} // namespace

class TestMarkdownWriting : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void previewsBesideTheText();
    void previewsOnlyMarkdown();
    void keepsTheFocusWhereItIs();
    void readsMarkdownDrawn();
    void findsAndCopiesWhereItIsDrawn();
    void drawsNothingTooLarge();
    void marksText();
    void marksLines();
    void makesTables();
    void makesLinks();
    void writesIntoAnEmptyDocument();
    void continuesLists();
    void exportsTheDocumentDrawn();
    void suggestsAiEdits();
    void theWindowOffersMarkdown();
    void remembersThePreview();
    void rendersThePreview();

private:
    QString write(const char* name, const std::string& content);
    std::unique_ptr<ui::DocumentView> open(const QString& path);

    QTemporaryDir m_dir;
};

void TestMarkdownWriting::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_markdownwriting"));
    qunsetenv(ui::AiSettings::kKeyEnvironmentVariable);
    qunsetenv(ui::AiSettings::kDeepSeekKeyEnvironmentVariable);
}

void TestMarkdownWriting::init()
{
    QVERIFY(m_dir.isValid());
    QSettings().clear();
}

QString TestMarkdownWriting::write(const char* name, const std::string& content)
{
    const QString path = m_dir.filePath(QString::fromUtf8(name));
    std::ofstream(ui::toPath(path), std::ios::binary) << content;
    return path;
}

std::unique_ptr<ui::DocumentView> TestMarkdownWriting::open(const QString& path)
{
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(ui::toPath(path), ec);
    if (!source)
        return nullptr;
    auto doc = std::make_unique<ui::DocumentView>(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc->resize(1200, 600);
    doc->show();
    return doc;
}

void TestMarkdownWriting::previewsBesideTheText()
{
    const auto doc = open(write("notes.md", notes()));
    QVERIFY(QTest::qWaitForWindowExposed(doc.get()));
    QVERIFY(doc->isMarkdown());
    QVERIFY(doc->textView()->isMarkdownEditing());
    QVERIFY(!doc->markdownView()); // made when first shown

    doc->setViewMode(Mode::Preview);
    QCOMPARE(doc->viewMode(), Mode::Preview);
    ui::MarkdownView* preview = doc->markdownView();
    QVERIFY(preview && preview->isVisible());
    QVERIFY(doc->textView()->isVisible());
    QVERIFY(!doc->hexView()->isVisible());
    QCOMPARE(preview->headingCount(), 41);
    QVERIFY(doc->isTextActive()); // the text is what is written in
    // Side by side, half each.
    QTRY_VERIFY(qAbs(preview->width() - doc->textView()->width()) < 40);

    // The text scrolls, and the drawing follows heading by heading.
    const core::LineIndex& index = doc->textDocument()->index();
    const auto lineStart = [&](std::uint64_t line) { return index.row(*index.rowIndexOfLine(line)).start; };
    const qsizetype section20 = textOf(*doc).split(u'\n').indexOf(QStringLiteral("## 第20节"));
    QVERIFY(section20 > 0);
    doc->textView()->scrollToOffset(lineStart(static_cast<std::uint64_t>(section20)));
    QCOMPARE(preview->topPlace().heading, 20);
    QVERIFY(preview->topPlace().fraction < 0.05);

    // Written in, it is drawn again after a pause.
    doc->textView()->setCursorOffset(lineStart(static_cast<std::uint64_t>(section20)));
    doc->textView()->insertText(QStringLiteral("## 插进来的一节\n\n"));
    QVERIFY(!preview->markdown().contains(QStringLiteral("插进来的一节")));
    QTRY_VERIFY(preview->markdown().contains(QStringLiteral("插进来的一节")));
    QCOMPARE(preview->headingCount(), 42);

    // Scrolled by hand, the drawing takes the text with it.
    const std::uint64_t top = doc->textView()->firstVisibleOffset();
    for (int i = 0; i < 4; ++i)
        preview->verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepAdd);
    QTRY_VERIFY(doc->textView()->firstVisibleOffset() > top);
    // And the ends meet.
    preview->verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMaximum);
    QTRY_COMPARE(doc->textView()->verticalScrollBar()->value(), doc->textView()->verticalScrollBar()->maximum());
    doc->textView()->scrollToOffset(0);
    QCOMPARE(preview->verticalScrollBar()->value(), 0);

    // Clicked, the drawing is what copying takes from.
    preview->setFocus();
    preview->selectAll();
    QVERIFY(doc->hasSelection());
    doc->copy();
    QVERIFY(QGuiApplication::clipboard()->text().startsWith(QStringLiteral("读书笔记")));
    doc->textView()->setFocus();
    QVERIFY(!doc->hasSelection());

    doc->setViewMode(Mode::Text);
    QVERIFY(preview->isHidden());
}

void TestMarkdownWriting::previewsOnlyMarkdown()
{
    const auto doc = open(write("plain.txt", "# not drawn\n"));
    QVERIFY(!doc->isMarkdown());
    doc->setViewMode(Mode::Preview);
    QCOMPARE(doc->viewMode(), Mode::Text);
    QVERIFY(!doc->markdownView());
    QVERIFY(!doc->textView()->isMarkdownEditing());

    // Markdown by the syntax chosen for it, then not again.
    doc->setSyntax(QStringLiteral("Markdown"));
    QVERIFY(doc->isMarkdown());
    QVERIFY(doc->textView()->isMarkdownEditing());
    doc->setViewMode(Mode::Preview);
    QCOMPARE(doc->viewMode(), Mode::Preview);
    doc->setSyntax(QString());
    QCOMPARE(doc->viewMode(), Mode::Text);
    QVERIFY(doc->markdownView()->isHidden());
}

void TestMarkdownWriting::keepsTheFocusWhereItIs()
{
    const QString path = write("focus.md", "# 标题\n\n正文\n");
    const auto doc = open(path);
    doc->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(doc.get()));
    doc->setViewMode(Mode::Preview);
    doc->showFindBar();
    QWidget* typing = QApplication::focusWidget();
    QVERIFY(typing && doc->findBar()->isAncestorOf(typing));
    // Read again from disk, its syntax chosen again: typing goes on where it was.
    std::ofstream(ui::toPath(path), std::ios::binary) << "# 标题\n\n正文，又写了一句。\n";
    QVERIFY(doc->reload());
    doc->setSyntax(QStringLiteral("Markdown"));
    QCOMPARE(QApplication::focusWidget(), typing);

    // Reading mode takes it from the text to the drawing, and back.
    doc->setViewMode(Mode::Text);
    doc->textView()->setFocus();
    doc->setReadingMode(true);
    QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(doc->markdownView()));
    doc->setReadingMode(false);
    QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(doc->textView()));
}

void TestMarkdownWriting::readsMarkdownDrawn()
{
    const QString path = write("book.md", notes());
    const auto doc = open(path);
    QVERIFY(QTest::qWaitForWindowExposed(doc.get()));
    doc->setFocus();
    doc->setReadingMode(true);
    QVERIFY(doc->showsDrawnMarkdown());
    QVERIFY(doc->textView()->isHidden());
    ui::MarkdownView* drawn = doc->markdownView();
    QVERIFY(drawn && drawn->isVisible());
    QCOMPARE(doc->focusProxy(), static_cast<QWidget*>(drawn));
    QVERIFY(doc->showsContents());

    // A chapter picked in the contents: there in the drawing.
    const qsizetype section10 = textOf(*doc).split(u'\n').indexOf(QStringLiteral("## 第10节"));
    const core::LineIndex& index = doc->textDocument()->index();
    doc->showFromOffset(index.row(*index.rowIndexOfLine(static_cast<std::uint64_t>(section10))).start);
    QCOMPARE(drawn->topPlace().heading, 10);

    // Read on, the place is kept with the file, as the text would keep it.
    const std::uint64_t top = doc->state().top;
    for (int i = 0; i < 3; ++i)
        drawn->verticalScrollBar()->triggerAction(QAbstractSlider::SliderPageStepAdd);
    QTRY_VERIFY(doc->state().top > top);
    QVERIFY(doc->state().reading);

    // Out of reading mode, the text again, from the same place.
    const std::uint64_t place = doc->state().top;
    doc->setReadingMode(false);
    QVERIFY(!doc->showsDrawnMarkdown());
    QVERIFY(doc->textView()->isVisible());
    QVERIFY(drawn->isHidden());
    QCOMPARE(doc->textView()->firstVisibleOffset(), place);
}

void TestMarkdownWriting::findsAndCopiesWhereItIsDrawn()
{
    const auto doc = open(write("find.md", notes()));
    QVERIFY(QTest::qWaitForWindowExposed(doc.get()));
    doc->setReadingMode(true);
    ui::MarkdownView* drawn = doc->markdownView();

    // Found in the text, shown selected in the drawing.
    doc->findBar()->setQuery(QStringLiteral("第33件事"));
    doc->findNext(true);
    QTRY_COMPARE(drawn->textCursor().selectedText(), QStringLiteral("第33件事"));
    const QRect shown = drawn->cursorRect();
    QVERIFY(shown.top() >= 0 && shown.bottom() <= drawn->viewport()->height());

    // What is selected there goes to the AI and the clipboard.
    QTextCursor cursor = drawn->textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    drawn->setTextCursor(cursor);
    QVERIFY(doc->hasSelection());
    const QString selected = cursor.selectedText();
    QVERIFY(selected.contains(QStringLiteral("第33件事")));
    QCOMPARE(doc->aiContext().selectionContent, selected);
    doc->copy();
    QCOMPARE(QGuiApplication::clipboard()->text(), selected);
}

void TestMarkdownWriting::drawsNothingTooLarge()
{
    // Past the limit, reading mode reads the text.
    std::string large = "# 大文件\n";
    large.append(9 * 1024 * 1024, 'a');
    const auto doc = open(write("large.md", large));
    doc->setReadingMode(true);
    QVERIFY(doc->isReadingMode());
    QVERIFY(!doc->showsDrawnMarkdown());
    QVERIFY(doc->textView()->isVisible());
    // The preview says why it is empty.
    doc->setReadingMode(false);
    doc->setViewMode(Mode::Preview);
    QVERIFY(doc->markdownView()->toPlainText().contains(QStringLiteral("Too large")));
}

void TestMarkdownWriting::marksText()
{
    const auto doc = open(write("marks.md", "hello world\nsecond line\n"));
    ui::TextView* view = doc->textView();

    // The word at the caret, made bold and back.
    view->setCursorOffset(2);
    QVERIFY(doc->applyMarkdownEdit(Edit::Bold));
    QCOMPARE(textOf(*doc), QStringLiteral("**hello** world\nsecond line\n"));
    QCOMPARE(view->selectedText(), QStringLiteral("hello"));
    QVERIFY(doc->applyMarkdownEdit(Edit::Bold));
    QCOMPARE(textOf(*doc), QStringLiteral("hello world\nsecond line\n"));

    // A selection, in italics and struck through; code.
    view->select(6, 11);
    QVERIFY(doc->applyMarkdownEdit(Edit::Italic));
    QCOMPARE(textOf(*doc), QStringLiteral("hello *world*\nsecond line\n"));
    QCOMPARE(view->selectedText(), QStringLiteral("world")); // still the text, inside the marks
    QVERIFY(doc->applyMarkdownEdit(Edit::Strikethrough));
    QCOMPARE(textOf(*doc), QStringLiteral("hello *~~world~~*\nsecond line\n"));
    view->select(0, 5);
    QVERIFY(doc->applyMarkdownEdit(Edit::Code));
    QCOMPARE(textOf(*doc), QStringLiteral("`hello` *~~world~~*\nsecond line\n"));
    // Each change is one step to undo.
    doc->undo();
    QCOMPARE(textOf(*doc), QStringLiteral("hello *~~world~~*\nsecond line\n"));

    // Across lines there is nothing to mark.
    view->select(2, 20);
    QVERIFY(!doc->applyMarkdownEdit(Edit::Bold));
    QCOMPARE(textOf(*doc), QStringLiteral("hello *~~world~~*\nsecond line\n"));

    // Between words, the marks go in with the caret between them.
    const std::shared_ptr<const core::FileSource> spaced = core::FileSource::fromBytes(
        {}, std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{'a', ' ', ' ', 'b'}));
    const auto stopped =
        std::make_unique<ui::DocumentView>(spaced, core::detectFileType(spaced->bytes()), core::utf8Codec());
    stopped->setSyntax(QStringLiteral("Markdown"));
    stopped->textView()->setCursorOffset(2);
    QVERIFY(stopped->applyMarkdownEdit(Edit::Bold));
    QCOMPARE(textOf(*stopped), QStringLiteral("a **** b"));
    QCOMPARE(stopped->textView()->cursorOffset(), 4u);
}

void TestMarkdownWriting::marksLines()
{
    const auto doc = open(write("lines.md", "Title\none\ntwo\n\nthree\n"));
    ui::TextView* view = doc->textView();

    // The caret's line, the caret kept in its text.
    view->setCursorOffset(3);
    QVERIFY(doc->applyMarkdownEdit(Edit::Heading2));
    QCOMPARE(textOf(*doc), QStringLiteral("## Title\none\ntwo\n\nthree\n"));
    QCOMPARE(view->cursorOffset(), 6u);
    QVERIFY(doc->applyMarkdownEdit(Edit::Heading1));
    QCOMPARE(textOf(*doc), QStringLiteral("# Title\none\ntwo\n\nthree\n"));
    QVERIFY(doc->applyMarkdownEdit(Edit::Heading1));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\none\ntwo\n\nthree\n"));

    // Selected lines, the blank one left blank, and selected still.
    view->select(6, 20);
    QVERIFY(doc->applyMarkdownEdit(Edit::BulletList));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\n- one\n- two\n\n- three\n"));
    QCOMPARE(view->selectedText(), QStringLiteral("- one\n- two\n\n- three"));
    QVERIFY(doc->applyMarkdownEdit(Edit::NumberedList));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\n1. one\n2. two\n\n3. three\n"));
    QVERIFY(doc->applyMarkdownEdit(Edit::TaskList));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\n- [ ] one\n- [ ] two\n\n- [ ] three\n"));
    QVERIFY(doc->applyMarkdownEdit(Edit::Quote));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\n> - [ ] one\n> - [ ] two\n\n> - [ ] three\n"));
    QVERIFY(doc->applyMarkdownEdit(Edit::Quote));
    QVERIFY(doc->applyMarkdownEdit(Edit::TaskList));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\none\ntwo\n\nthree\n"));

    // On a blank line, the mark to write after.
    view->setCursorOffset(14);
    QVERIFY(doc->applyMarkdownEdit(Edit::BulletList));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\none\ntwo\n- \nthree\n"));
    QCOMPARE(view->cursorOffset(), 16u);
    doc->undo();

    // Code fenced off.
    view->setCursorOffset(7);
    QVERIFY(doc->applyMarkdownEdit(Edit::CodeBlock));
    QCOMPARE(textOf(*doc), QStringLiteral("Title\n```\none\n```\ntwo\n\nthree\n"));
    QCOMPARE(view->cursorOffset(), 11u); // in it, where it was
}

void TestMarkdownWriting::makesTables()
{
    const auto doc = open(write("table.md", "名单\n|人物|篇目|\n|---|:-:|\n|项羽|项羽本纪|\n\nafter\n"));
    ui::TextView* view = doc->textView();

    // The table at the caret, lined up; the caret stays in its cell.
    const auto bytesOf = [](const QString& text) { return static_cast<std::uint64_t>(text.toUtf8().size()); };
    view->setCursorOffset(bytesOf(QStringLiteral("名单\n|人物|篇目|\n|---|:-:|\n|项羽|项")));
    QVERIFY(doc->applyMarkdownEdit(Edit::FormatTable));
    const QString formatted = QStringLiteral("名单\n| 人物 |   篇目   |\n| ---- | :------: |\n| 项羽 | 项羽本纪 |\n");
    QCOMPARE(textOf(*doc), formatted + QStringLiteral("\nafter\n"));
    QCOMPARE(view->cursorOffset(), bytesOf(formatted.left(formatted.indexOf(QStringLiteral("项羽本纪")))));
    QVERIFY(!doc->applyMarkdownEdit(Edit::FormatTable)); // lined up already
    doc->undo();
    QCOMPARE(textOf(*doc), QStringLiteral("名单\n|人物|篇目|\n|---|:-:|\n|项羽|项羽本纪|\n\nafter\n"));

    // Outside a table, nothing.
    view->setCursorOffset(textOf(*doc).toUtf8().size() - 2);
    QVERIFY(!doc->applyMarkdownEdit(Edit::FormatTable));

    // A new one under the caret's line, after a blank line, its first heading selected to type over.
    QVERIFY(doc->applyMarkdownEdit(Edit::Table));
    QVERIFY(textOf(*doc).endsWith(QStringLiteral("after\n\n| Column 1 | Column 2 | Column 3 |\n"
                                                 "| -------- | -------- | -------- |\n"
                                                 "|          |          |          |\n")));
    QCOMPARE(view->selectedText(), QStringLiteral("Column 1"));
}

void TestMarkdownWriting::makesLinks()
{
    const auto doc = open(write("links.md", "see AIEdit here\nhttps://example.com\n"));
    ui::TextView* view = doc->textView();

    // Text: the address goes in, selected to type over.
    view->select(4, 10);
    QVERIFY(doc->applyMarkdownEdit(Edit::Link));
    QCOMPARE(textOf(*doc), QStringLiteral("see [AIEdit](https://) here\nhttps://example.com\n"));
    QCOMPARE(view->selectedText(), QStringLiteral("https://"));

    // An address: the text to type, in the brackets.
    const std::uint64_t start = static_cast<std::uint64_t>(QStringLiteral("see [AIEdit](https://) here\n").size());
    view->select(start, start + 19);
    QVERIFY(doc->applyMarkdownEdit(Edit::Link));
    QCOMPARE(textOf(*doc), QStringLiteral("see [AIEdit](https://) here\n[](https://example.com)\n"));
    QCOMPARE(view->cursorOffset(), start + 1);
    QVERIFY(!view->hasSelection());
}

void TestMarkdownWriting::writesIntoAnEmptyDocument()
{
    const auto empty = [] {
        const std::shared_ptr<const core::FileSource> source =
            core::FileSource::fromBytes({}, std::make_shared<const std::vector<std::uint8_t>>());
        auto doc = std::make_unique<ui::DocumentView>(source, core::detectFileType(source->bytes()), core::utf8Codec());
        doc->setSyntax(QStringLiteral("Markdown"));
        return doc;
    };
    auto doc = empty();
    QVERIFY(doc->applyMarkdownEdit(Edit::Heading1));
    QCOMPARE(textOf(*doc), QStringLiteral("# "));
    QCOMPARE(doc->textView()->cursorOffset(), 2u);

    doc = empty();
    QVERIFY(doc->applyMarkdownEdit(Edit::Bold));
    QCOMPARE(textOf(*doc), QStringLiteral("****"));
    QCOMPARE(doc->textView()->cursorOffset(), 2u);

    doc = empty();
    QVERIFY(doc->applyMarkdownEdit(Edit::Table));
    QVERIFY(textOf(*doc).startsWith(QStringLiteral("| Column 1 |")));
    QVERIFY(!empty()->applyMarkdownEdit(Edit::FormatTable));
}

void TestMarkdownWriting::continuesLists()
{
    const auto doc = open(write("list.md", "- one\n"));
    ui::TextView* view = doc->textView();
    view->setFocus();
    view->setCursorOffset(5);

    // Enter goes on with the list; on an empty item, it ends it.
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(textOf(*doc), QStringLiteral("- one\n- \n"));
    QTest::keyClicks(view, QStringLiteral("two"));
    QTest::keyClick(view, Qt::Key_Return);
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(textOf(*doc), QStringLiteral("- one\n- two\n\n"));
    QCOMPARE(view->cursorOffset(), 12u);

    // Numbers count on; a task's box comes empty.
    QTest::keyClicks(view, QStringLiteral("9. nine"));
    QTest::keyClick(view, Qt::Key_Return);
    QTest::keyClicks(view, QStringLiteral("ten"));
    QTest::keyClick(view, Qt::Key_Return);
    QTest::keyClick(view, Qt::Key_Return);
    QTest::keyClicks(view, QStringLiteral("- [x] done"));
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(textOf(*doc), QStringLiteral("- one\n- two\n9. nine\n10. ten\n- [x] done\n- [ ] \n"));

    // Shift+Enter breaks the line only; Tab moves an item in.
    QTest::keyClicks(view, QStringLiteral("task"));
    QTest::keyClick(view, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(textOf(*doc), QStringLiteral("- one\n- two\n9. nine\n10. ten\n- [x] done\n- [ ] task\n\n"));
    view->setCursorOffset(3);
    QTest::keyClick(view, Qt::Key_Tab);
    QVERIFY(textOf(*doc).startsWith(QStringLiteral("\t- one\n")));
    QCOMPARE(view->cursorOffset(), 4u);
    QVERIFY(!view->hasSelection());

    // Elsewhere Enter keeps to the indentation.
    const auto plain = open(write("list.txt", "- one\n"));
    plain->textView()->setFocus();
    plain->textView()->setCursorOffset(5);
    QTest::keyClick(plain->textView(), Qt::Key_Return);
    QCOMPARE(textOf(*plain), QStringLiteral("- one\n\n"));
}

void TestMarkdownWriting::exportsTheDocumentDrawn()
{
    const auto doc = open(write("export.md", "# 标题\n\n正文，**加粗**。\n"));
    doc->textView()->setCursorOffset(textOf(*doc).toUtf8().size());
    doc->textView()->insertText(QStringLiteral("没存的一句。")); // as it is now, not as saved
    QString error;
    const QString html = m_dir.filePath(QStringLiteral("export.html"));
    QVERIFY2(doc->exportMarkdown(html, ui::DocumentView::ExportFormat::Html, &error), qPrintable(error));
    QFile page(html);
    QVERIFY(page.open(QIODevice::ReadOnly));
    const QString written = QString::fromUtf8(page.readAll());
    QVERIFY(written.contains(QStringLiteral("标题")) && written.contains(QStringLiteral("没存的一句")));
    const QString pdf = m_dir.filePath(QStringLiteral("export.pdf"));
    QVERIFY2(doc->exportMarkdown(pdf, ui::DocumentView::ExportFormat::Pdf, &error), qPrintable(error));
    QFile paper(pdf);
    QVERIFY(paper.open(QIODevice::ReadOnly));
    QCOMPARE(paper.read(5), QByteArray("%PDF-"));
}

void TestMarkdownWriting::suggestsAiEdits()
{
    const auto markdown = open(write("suggest.md", "a, b, c\n"));
    markdown->showAiEditBar();
    QCOMPARE(markdown->aiEditBar()->suggestions().size(), 4);
    const auto plain = open(write("suggest.txt", "a, b, c\n"));
    plain->showAiEditBar();
    QCOMPARE(plain->aiEditBar()->suggestions().size(), 1);
}

void TestMarkdownWriting::theWindowOffersMarkdown()
{
    const QString notesPath = write("window.md", "# 笔记\n\n[另一篇](other.md)\n");
    const QString otherPath = write("other.md", "# 另一篇\n");
    const QString plainPath = write("window.txt", "text\n");
    ui::MainWindow window;
    window.resize(1200, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QAction* preview = findAction(window, QStringLiteral("Markdown &Preview"));
    QAction* bold = findAction(window, QStringLiteral("&Bold"));
    QAction* exportPdf = findAction(window, QStringLiteral("Export as P&DF…"));
    QVERIFY(preview && bold && exportPdf);

    QVERIFY(window.openFile(plainPath));
    QVERIFY(!preview->isEnabled());
    QVERIFY(!bold->isEnabled());
    QVERIFY(!exportPdf->isEnabled());

    QVERIFY(window.openFile(notesPath));
    ui::DocumentView* doc = window.currentDocument();
    QVERIFY(preview->isEnabled());
    QVERIFY(bold->isEnabled());
    QVERIFY(exportPdf->isEnabled());
    preview->trigger();
    QCOMPARE(doc->viewMode(), Mode::Preview);
    QVERIFY(preview->isChecked());
    // Drawn as large as the window's other text is, a size up.
    QCOMPARE(doc->markdownView()->font().pointSize(), doc->textView()->font().pointSize() + 1);

    // Read drawn, there is nothing to write in.
    doc->setViewMode(Mode::Text);
    findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
    QVERIFY(doc->showsDrawnMarkdown());
    QVERIFY(!bold->isEnabled());
    QVERIFY(!findAction(window, QStringLiteral("&Undo"))->isEnabled());

    // A link to another note opens it.
    emit doc->markdownView()->fileLinkActivated(otherPath);
    QCOMPARE(window.currentDocument()->filePath(), otherPath);
}

void TestMarkdownWriting::remembersThePreview()
{
    const QString path = write("remembered.md", "# 记住\n");
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        window.currentDocument()->setViewMode(Mode::Preview);
        findAction(window, QStringLiteral("&Close Tab"))->trigger();
    }
    ui::MainWindow window;
    QVERIFY(window.openFile(path));
    QCOMPARE(window.currentDocument()->viewMode(), Mode::Preview);
}

void TestMarkdownWriting::rendersThePreview()
{
    // Set AIEDIT_SCREENSHOT_DIR to look at it; otherwise this only checks that it paints.
    const QString dir = qEnvironmentVariable("AIEDIT_SCREENSHOT_DIR");
    const QString path = write("史记笔记.md",
                               "# 史记读书笔记\n\n> 究天人之际，通古今之变，成一家之言。\n\n"
                               "## 本纪\n\n读了**项羽本纪**和*高祖本纪*，两篇对照着读最有意思。\n\n"
                               "| 人物 | 篇目 | 读完 |\n|------|------|:----:|\n| 项羽 | 项羽本纪 | 是 |\n"
                               "| 刘邦 | 高祖本纪 | 否 |\n\n## 待办\n\n- [x] 读完本纪\n- [ ] 整理`年表`\n"
                               "- [ ] 查[三家注](https://example.com)\n\n```python\n# 按篇目数人物\n"
                               "count = {\"本纪\": 12, \"世家\": 30}\nprint(sum(count.values()))\n```\n");
    ui::MainWindow window;
    window.resize(1200, 640);
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ui::DocumentView* doc = window.currentDocument();
    doc->setViewMode(Mode::Preview);
    const auto shoot = [&](const QString& name) {
        QTest::qWait(100);
        const QImage image = window.grab().toImage();
        QVERIFY(!image.isNull());
        if (!dir.isEmpty())
            image.save(dir + QStringLiteral("/markdown-") + name + QStringLiteral(".png"));
    };
    for (const char* scheme : {"AIEdit Paper", "AIEdit Night"}) {
        ui::setColorScheme(QString::fromLatin1(scheme));
        shoot(QStringLiteral("preview-") + QString::fromLatin1(scheme).section(u' ', 1).toLower());
    }
    ui::setColorScheme(QStringLiteral("AIEdit Paper"));
    doc->setViewMode(Mode::Text);
    findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
    shoot(QStringLiteral("reading"));
    findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
    doc->textView()->select(0, 0);
    doc->showAiEditBar();
    shoot(QStringLiteral("ai-edit"));
}

QTEST_MAIN(TestMarkdownWriting)
#include "tst_markdownwriting.moc"
