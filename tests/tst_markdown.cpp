#include "ui/Desktop.h"
#include "ui/Markdown.h"
#include "ui/MarkdownView.h"
#include "ui/SyntaxHighlighting.h"

#include <QApplication>
#include <QFileInfo>
#include <QFontInfo>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QWheelEvent>

using namespace aiedit;
namespace md = aiedit::ui::markdown;


class TestMarkdown : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void knowsMarkdownFiles();
    void measuresWideCharacters();
    void continuesLists_data();
    void continuesLists();
    void formatsTables();
    void togglesMarks_data();
    void togglesMarks();
    void keepsTheCaretInItsCell();
    void togglesLinePrefixes();
    void findsHeadings();
    void drawsTheDocument();
    void keepsThePlaceInLongDocuments();
    void exportsHtmlAndPdf();

private:
    QTemporaryDir m_dir;
};

void TestMarkdown::knowsMarkdownFiles()
{
    QVERIFY(md::isMarkdownFile(QStringLiteral("README.md")));
    QVERIFY(md::isMarkdownFile(QStringLiteral("/notes/plan.MARKDOWN")));
    QVERIFY(!md::isMarkdownFile(QStringLiteral("main.cpp")));
    QVERIFY(!md::isMarkdownFile(QStringLiteral("md")));
}

void TestMarkdown::measuresWideCharacters()
{
    QCOMPARE(md::displayWidth(u"abc"), 3);
    QCOMPARE(md::displayWidth(u"中文"), 4);
    QCOMPARE(md::displayWidth(u"ｱＡ"), 3); // half-width katakana, full-width A
    QCOMPARE(md::displayWidth(u"😀"), 2);
    QCOMPARE(md::displayWidth(u"é"), 1); // a combining accent takes no room
}

void TestMarkdown::continuesLists_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("insert");
    QTest::addColumn<int>("remove");
    QTest::addColumn<bool>("listed");

    QTest::newRow("bullet") << QStringLiteral("- item") << QStringLiteral("- ") << 0 << true;
    QTest::newRow("indented star") << QStringLiteral("  * x") << QStringLiteral("  * ") << 0 << true;
    QTest::newRow("numbered") << QStringLiteral("3. step") << QStringLiteral("4. ") << 0 << true;
    QTest::newRow("paren") << QStringLiteral("9) a") << QStringLiteral("10) ") << 0 << true;
    QTest::newRow("task") << QStringLiteral("- [x] done") << QStringLiteral("- [ ] ") << 0 << true;
    QTest::newRow("quote") << QStringLiteral("> quoted") << QStringLiteral("> ") << 0 << true;
    QTest::newRow("empty bullet ends") << QStringLiteral("- ") << QString() << 2 << true;
    QTest::newRow("empty number ends") << QStringLiteral("  1. ") << QString() << 5 << true;
    QTest::newRow("empty task ends") << QStringLiteral("- [ ] ") << QString() << 6 << true;
    QTest::newRow("empty quote ends") << QStringLiteral("> ") << QString() << 2 << true;
    QTest::newRow("plain") << QStringLiteral("plain text") << QString() << 0 << false;
    QTest::newRow("no space") << QStringLiteral("-x") << QString() << 0 << false;
}

void TestMarkdown::continuesLists()
{
    QFETCH(QString, line);
    QFETCH(QString, insert);
    QFETCH(int, remove);
    QFETCH(bool, listed);
    const std::optional<md::ListBreak> result = md::listBreak(line);
    QCOMPARE(result.has_value(), listed);
    if (result) {
        QCOMPARE(result->insert, insert);
        QCOMPARE(result->remove, remove);
    }
    // A list item, unless it is a quote or not listed at all.
    QCOMPARE(md::isListItem(line), listed && !line.startsWith(u'>'));
}

void TestMarkdown::formatsTables()
{
    // CJK characters count two columns; each column keeps its alignment.
    const QStringList table{QStringLiteral("|人物|篇目|"), QStringLiteral("|:--|--:|"),
                            QStringLiteral("| 项羽 | 项羽本纪 |"), QStringLiteral("|刘邦|高祖本纪")};
    const std::optional<QStringList> formatted = md::formatTable(table);
    QVERIFY(formatted);
    QCOMPARE(*formatted, (QStringList{QStringLiteral("| 人物 |     篇目 |"), QStringLiteral("| :--- | -------: |"),
                                      QStringLiteral("| 项羽 | 项羽本纪 |"), QStringLiteral("| 刘邦 | 高祖本纪 |")}));
    // Short cells make three-dash columns; a missing cell is padded; centred stays centred.
    QCOMPARE(*md::formatTable({QStringLiteral("a|b"), QStringLiteral(":-:|-"), QStringLiteral("c")}),
             (QStringList{QStringLiteral("|  a  | b   |"), QStringLiteral("| :-: | --- |"),
                          QStringLiteral("|  c  |     |")}));
    // Escaped bars stay in their cells.
    QCOMPARE(md::formatTable({QStringLiteral("|x|"), QStringLiteral("|-|"), QStringLiteral("|a \\| b|")})->last(),
             QStringLiteral("| a \\| b |"));
    QVERIFY(!md::formatTable({QStringLiteral("| a | b |"), QStringLiteral("| c | d |")})); // no dashes
    QVERIFY(!md::formatTable({QStringLiteral("just text")}));
}

void TestMarkdown::togglesMarks_data()
{
    // The line with the selection in [brackets]; as it becomes, the text in brackets again.
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("mark");
    QTest::addColumn<QString>("result");

    QTest::newRow("bold") << QStringLiteral("a [word] b") << QStringLiteral("**") << QStringLiteral("a **[word]** b");
    QTest::newRow("unbold outside") << QStringLiteral("a **[word]** b") << QStringLiteral("**")
                                    << QStringLiteral("a [word] b");
    QTest::newRow("unbold inside") << QStringLiteral("a [**word**] b") << QStringLiteral("**")
                                   << QStringLiteral("a [word] b");
    QTest::newRow("italic in bold") << QStringLiteral("**[word]**") << QStringLiteral("*")
                                    << QStringLiteral("***[word]***");
    QTest::newRow("italic out of both") << QStringLiteral("***[word]***") << QStringLiteral("*")
                                        << QStringLiteral("**[word]**");
    QTest::newRow("bold out of both") << QStringLiteral("***[word]***") << QStringLiteral("**")
                                      << QStringLiteral("*[word]*");
    QTest::newRow("spaces stay out") << QStringLiteral("a[ word ]b") << QStringLiteral("~~")
                                     << QStringLiteral("a ~~[word]~~ b");
    QTest::newRow("code") << QStringLiteral("call [f()] now") << QStringLiteral("`")
                          << QStringLiteral("call `[f()]` now");
    QTest::newRow("uncode") << QStringLiteral("call [`f()`] now") << QStringLiteral("`")
                            << QStringLiteral("call [f()] now");
    QTest::newRow("empty") << QStringLiteral("中[]文") << QStringLiteral("**") << QStringLiteral("中**[]**文");
}

void TestMarkdown::togglesMarks()
{
    QFETCH(QString, line);
    QFETCH(QString, mark);
    QFETCH(QString, result);
    const qsizetype start = line.indexOf(u'[');
    const qsizetype end = line.indexOf(u']') - 1;
    const QString plain = QString(line).remove(u'[').remove(u']');
    const md::MarkedText marked = md::toggleMark(plain, start, end, mark);
    QString shown = marked.line;
    shown.insert(marked.end, u']');
    shown.insert(marked.start, u'[');
    QCOMPARE(shown, result);
}

void TestMarkdown::keepsTheCaretInItsCell()
{
    // In the second cell, before and after: just inside it.
    QCOMPARE(md::cellColumn(QStringLiteral("|a|bc|"), 4, QStringLiteral("| a   | bc  |")), 8);
    QCOMPARE(md::cellColumn(QStringLiteral("a|bc"), 3, QStringLiteral("| a   | bc  |")), 8);
    QCOMPARE(md::cellColumn(QStringLiteral("|a|bc|"), 1, QStringLiteral("| a   | bc  |")), 2);
    // An escaped bar is no cell's edge.
    QCOMPARE(md::cellColumn(QStringLiteral("|a\\|b|c|"), 7, QStringLiteral("| a\\|b | c   |")), 9);
}

void TestMarkdown::togglesLinePrefixes()
{
    using md::LinePrefix;
    const QStringList lines{QStringLiteral("a"), QString(), QStringLiteral("  b")};
    const QStringList bullets = md::toggleLinePrefix(lines, LinePrefix::Bullet);
    QCOMPARE(bullets, (QStringList{QStringLiteral("- a"), QString(), QStringLiteral("  - b")}));
    QCOMPARE(md::toggleLinePrefix(bullets, LinePrefix::Bullet), lines); // and back
    QCOMPARE(md::toggleLinePrefix(bullets, LinePrefix::Numbered),
             (QStringList{QStringLiteral("1. a"), QString(), QStringLiteral("  2. b")}));
    QCOMPARE(md::toggleLinePrefix(bullets, LinePrefix::Task),
             (QStringList{QStringLiteral("- [ ] a"), QString(), QStringLiteral("  - [ ] b")}));
    QCOMPARE(md::toggleLinePrefix({QStringLiteral("# Title")}, LinePrefix::Heading2),
             QStringList{QStringLiteral("## Title")});
    QCOMPARE(md::toggleLinePrefix({QStringLiteral("## Title")}, LinePrefix::Heading2),
             QStringList{QStringLiteral("Title")});
    QCOMPARE(md::toggleLinePrefix({QStringLiteral("- a"), QStringLiteral("b")}, LinePrefix::Quote),
             (QStringList{QStringLiteral("> - a"), QStringLiteral("> b")}));
    QCOMPARE(md::toggleLinePrefix({QStringLiteral("> a"), QStringLiteral(">b")}, LinePrefix::Quote),
             (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
}

void TestMarkdown::findsHeadings()
{
    const QString text = QStringLiteral("# A\n```\n# not a heading\n```\nB\n===\n\n## C ##\nlist:\n- x\n---\n");
    const QList<md::Heading> found = md::headings(text);
    QCOMPARE(found.size(), 3);
    QCOMPARE(found[0].line, 0);
    QCOMPARE(found[0].text, QStringLiteral("A"));
    QCOMPARE(found[1].line, 4); // "B" over "==="
    QCOMPARE(found[1].level, 1);
    QCOMPARE(found[2].line, 7);
    QCOMPARE(found[2].level, 2);
    QCOMPARE(found[2].text, QStringLiteral("C"));
}

void TestMarkdown::drawsTheDocument()
{
    QVERIFY(m_dir.isValid());
    const QString text = QStringLiteral(
        "# 读书笔记\n\n> 究天人之际，通古今之变。\n\n## 人物\n\n| 人物 | 篇目 |\n|---|---|\n| 项羽 | 项羽本纪 |\n\n"
        "- [x] 读完\n- [ ] 整理`年表`\n\n```cpp\nint main() { return 0; }\n```\n\n"
        "[链接](https://example.com) ![远处的图](https://example.com/a.png)\n");
    ui::MarkdownView view;
    view.resize(700, 240);
    view.setMarkdown(text, m_dir.path());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCOMPARE(view.headingCount(), 2);

    QTextDocument* doc = view.document();
    QFont h1;
    QColor codeBackground;
    bool linked = false;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (block.blockFormat().headingLevel() == 1)
            h1 = block.begin().fragment().charFormat().font();
        if (block.blockFormat().hasProperty(QTextFormat::BlockCodeFence)) {
            codeBackground = block.blockFormat().background().color();
            // Code in columns: in the editor's fixed-width font. (Whether the platform calls that
            // font fixed-pitch differs: Windows' font database does not say so of every one.)
            const QTextCharFormat chars = block.begin().fragment().charFormat();
            QVERIFY(chars.fontFixedPitch());
            QCOMPARE(chars.fontFamilies().toStringList().value(0), ui::withCjkFallback(ui::fixedFont()).families().value(0));
            qInfo() << "code drawn in" << QFontInfo(chars.font()).family() << "fixed pitch:" << QFontInfo(chars.font()).fixedPitch();
        }
        for (auto it = block.begin(); !it.atEnd(); ++it)
            linked = linked || (it.fragment().charFormat().isAnchor() &&
                                it.fragment().charFormat().foreground().color() == view.palette().color(QPalette::Link));
    }
    QVERIFY(h1.pointSizeF() > view.font().pointSizeF() * 1.5); // a big heading
    QVERIFY(codeBackground.isValid() && codeBackground != view.palette().color(QPalette::Base)); // code shaded
    QVERIFY(linked);
    bool table = false;
    for (QTextFrame* frame : doc->rootFrame()->childFrames())
        if (auto* t = qobject_cast<QTextTable*>(frame))
            table = t->format().borderCollapse() && t->format().cellPadding() > 0;
    QVERIFY(table);
    // In a column in the middle of a wide window.
    QVERIFY(doc->rootFrame()->frameFormat().leftMargin() > 0);

    // Nothing comes from the network: the picture is not loaded.
    QVERIFY(doc->resource(QTextDocument::ImageResource, QUrl(QStringLiteral("https://example.com/a.png"))).isNull());

    // Drawn again as typed, it stays where it was read.
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
    const int scrolled = view.verticalScrollBar()->value();
    view.setMarkdown(text + QStringLiteral("\nmore\n"), m_dir.path());
    QCOMPARE(view.verticalScrollBar()->value(), scrolled);
    view.scrollToPlace({1, 0});
    QVERIFY(view.verticalScrollBar()->value() < scrolled);
    QCOMPARE(view.topPlace().heading, 1);

    if (const QString dir = qEnvironmentVariable("AIEDIT_SCREENSHOT_DIR"); !dir.isEmpty()) {
        view.resize(700, 760);
        view.verticalScrollBar()->setValue(0);
        view.grab().save(dir + QStringLiteral("/markdown.png"));
    }
}

void TestMarkdown::keepsThePlaceInLongDocuments()
{
    // Long enough that Qt would lay it out a piece at a time.
    QString text;
    for (int i = 1; i <= 300; ++i)
        text += QStringLiteral("## 第%1节\n\n").arg(i) + QStringLiteral("这一节说的是第%1件事。").arg(i).repeated(12) +
                QStringLiteral("\n\n- 一\n- 二\n\n");
    ui::MarkdownView view;
    view.resize(600, 400);
    view.setMarkdown(text, m_dir.path());
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCOMPARE(view.headingCount(), 300);
    QScrollBar* bar = view.verticalScrollBar();

    // The far end can be reached at once, and stays reached as it is drawn again.
    view.scrollToPlace({250, 0.5});
    const int far = bar->value();
    QVERIFY(far > bar->maximum() / 2);
    QCOMPARE(view.topPlace().heading, 250);
    QVERIFY(qAbs(view.topPlace().fraction - 0.5) < 0.05);
    view.setMarkdown(text + QStringLiteral("\n末尾\n"), m_dir.path());
    QCOMPARE(bar->value(), far);

    // Narrower, the lines wrap more: the same heading stays on top.
    view.resize(400, 400);
    QCOMPARE(view.topPlace().heading, 250);

    // Before the first heading.
    view.scrollToPlace({-1, 0});
    QCOMPARE(bar->value(), 0);
    QCOMPARE(view.topPlace().heading, -1);

    // What the editor found, found here too: the nearest one to where it was in the text.
    QVERIFY(view.selectText(QStringLiteral("第120件事"), {119, 0.2}));
    QCOMPARE(view.textCursor().selectedText(), QStringLiteral("第120件事"));
    const QRect found = view.cursorRect();
    QVERIFY(found.top() >= 0 && found.bottom() <= view.viewport()->height());
    QVERIFY(!view.selectText(QStringLiteral("**不在这里**"), {10, 0}));

    // Only what the user does says so.
    QSignalSpy scrolled(&view, &ui::MarkdownView::scrolledByUser);
    view.scrollToPlace({20, 0});
    QCoreApplication::processEvents();
    QCOMPARE(scrolled.count(), 0);
    bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
    QTRY_COMPARE(scrolled.count(), 1);

    QSignalSpy zoomed(&view, &ui::MarkdownView::zoomRequested);
    QWheelEvent wheel(QPointF(10, 10), view.mapToGlobal(QPointF(10, 10)), {}, QPoint(0, 120), Qt::NoButton,
                      Qt::ControlModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view.viewport(), &wheel);
    QCOMPARE(zoomed.count(), 1);
    QCOMPARE(zoomed.first().first().toInt(), 1);
}

void TestMarkdown::exportsHtmlAndPdf()
{
    const QString text = QStringLiteral("# 标题\n\n正文，**加粗**。\n");
    const QString html = m_dir.filePath(QStringLiteral("out.html"));
    const QString pdf = m_dir.filePath(QStringLiteral("out.pdf"));
    QString error;
    QVERIFY2(ui::MarkdownView::exportHtml(text, m_dir.path(), QApplication::font(), html, &error), qPrintable(error));
    QVERIFY2(ui::MarkdownView::exportPdf(text, m_dir.path(), QApplication::font(), pdf, &error), qPrintable(error));
    QFile file(html);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString written = QString::fromUtf8(file.readAll());
    QVERIFY(written.contains(QStringLiteral("标题")) && written.contains(QStringLiteral("加粗")));
    QVERIFY(written.contains(QStringLiteral("<title>out</title>")));
    QVERIFY(QFileInfo(pdf).size() > 1000);
    QFile pdfFile(pdf);
    QVERIFY(pdfFile.open(QIODevice::ReadOnly));
    QVERIFY(pdfFile.read(5) == "%PDF-");
    QVERIFY(!ui::MarkdownView::exportHtml(text, m_dir.path(), QApplication::font(),
                                          m_dir.filePath(QStringLiteral("no/such/folder/out.html")), &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestMarkdown)
#include "tst_markdown.moc"
