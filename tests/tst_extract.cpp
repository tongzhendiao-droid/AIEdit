#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/Inflate.h"
#include "ui/DocumentView.h"
#include "ui/Extract.h"
#include "ui/Paths.h"

#include <QBuffer>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>

using namespace aiedit::ui;

namespace {

// A ZIP archive with these files, stored without compression.
std::vector<std::uint8_t> makeZip(const QList<std::pair<QByteArray, QByteArray>>& files)
{
    QByteArray out;
    QByteArray directory;
    auto u16 = [](QByteArray& b, unsigned v) {
        b.append(static_cast<char>(v & 0xFF));
        b.append(static_cast<char>((v >> 8) & 0xFF));
    };
    auto u32 = [&](QByteArray& b, std::uint32_t v) {
        u16(b, v & 0xFFFF);
        u16(b, v >> 16);
    };
    for (const auto& [name, content] : files) {
        const auto crc = aiedit::core::crc32(
            std::span(reinterpret_cast<const std::uint8_t*>(content.constData()), static_cast<std::size_t>(content.size())));
        const auto offset = static_cast<std::uint32_t>(out.size());
        u32(out, 0x04034b50);
        u16(out, 20);
        u16(out, 0);
        u16(out, 0); // stored
        u32(out, 0);
        u32(out, crc);
        u32(out, static_cast<std::uint32_t>(content.size()));
        u32(out, static_cast<std::uint32_t>(content.size()));
        u16(out, static_cast<unsigned>(name.size()));
        u16(out, 0);
        out += name + content;

        u32(directory, 0x02014b50);
        u16(directory, 20);
        u16(directory, 20);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, crc);
        u32(directory, static_cast<std::uint32_t>(content.size()));
        u32(directory, static_cast<std::uint32_t>(content.size()));
        u16(directory, static_cast<unsigned>(name.size()));
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u16(directory, 0);
        u32(directory, 0);
        u32(directory, offset);
        directory += name;
    }
    const auto directoryOffset = static_cast<std::uint32_t>(out.size());
    out += directory;
    u32(out, 0x06054b50);
    u16(out, 0);
    u16(out, 0);
    u16(out, static_cast<unsigned>(files.size()));
    u16(out, static_cast<unsigned>(files.size()));
    u32(out, static_cast<std::uint32_t>(directory.size()));
    u32(out, directoryOffset);
    u16(out, 0);
    return {out.begin(), out.end()};
}

const QByteArray kWordNamespace = R"(xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main")";

} // namespace

class TestExtract : public QObject
{
    Q_OBJECT

private slots:
    void detectsFormats();
    void word();
    void wordHeadingsByOutline();
    void excel();
    void epub();
    void cells();
    void brokenFiles();
    void documentShowsText();
    void pdfWrittenByQt();
    void epl();
};

void TestExtract::detectsFormats()
{
    const auto docx = makeZip({{"[Content_Types].xml", "<Types/>"}, {"word/document.xml", "<w:document/>"}});
    QCOMPARE(extractableFormat(docx), ExtractedFormat::Word);
    const auto xlsx = makeZip({{"xl/workbook.xml", "<workbook/>"}});
    QCOMPARE(extractableFormat(xlsx), ExtractedFormat::Excel);
    const auto epub = makeZip({{"mimetype", "application/epub+zip"}, {"META-INF/container.xml", "<container/>"}});
    QCOMPARE(extractableFormat(epub), ExtractedFormat::Epub);
    const auto plainZip = makeZip({{"readme.txt", "hello"}});
    QCOMPARE(extractableFormat(plainZip), ExtractedFormat::None);

    const QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    QCOMPARE(extractableFormat(std::span(reinterpret_cast<const std::uint8_t*>(pdf.data()), pdf.size())),
             ExtractedFormat::Pdf);
    const QByteArray epl = QByteArray("CNWTEPRG") + QByteArray(8, '\0');
    QCOMPARE(extractableFormat(std::span(reinterpret_cast<const std::uint8_t*>(epl.data()), epl.size())),
             ExtractedFormat::Epl);
    const QByteArray text = "just text";
    QCOMPARE(extractableFormat(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())),
             ExtractedFormat::None);
}

void TestExtract::word()
{
    const QByteArray styles = "<w:styles " + kWordNamespace + R"(>
        <w:style w:type="paragraph" w:styleId="1"><w:name w:val="heading 1"/></w:style>
        <w:style w:type="paragraph" w:styleId="MyHeading"><w:name w:val="My Heading"/><w:basedOn w:val="1"/></w:style>
        <w:style w:type="paragraph" w:styleId="Body"><w:name w:val="Normal"/></w:style>
        </w:styles>)";
    const QByteArray document = "<w:document " + kWordNamespace + R"(><w:body>
        <w:p><w:pPr><w:pStyle w:val="1"/><w:tabs><w:tab w:val="left" w:pos="420"/></w:tabs></w:pPr>
             <w:r><w:t>第一章</w:t></w:r><w:r><w:t xml:space="preserve"> 开始</w:t></w:r></w:p>
        <w:p><w:r><w:rPr><w:b/></w:rPr><w:t>正文</w:t></w:r><w:r><w:tab/><w:t>一段</w:t></w:r>
             <w:r><w:br/><w:t>第二行</w:t></w:r></w:p>
        <w:p><w:r><w:delText>删掉的</w:delText></w:r></w:p>
        <w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="1"/></w:numPr></w:pPr><w:r><w:t>条目</w:t></w:r></w:p>
        <w:p><w:pPr><w:pStyle w:val="MyHeading"/></w:pPr><w:r><w:t>继承的标题</w:t></w:r></w:p>
        <w:sdt><w:sdtContent><w:p><w:r><w:t>控件里的字</w:t></w:r></w:p></w:sdtContent></w:sdt>
        <w:tbl><w:tblPr/>
          <w:tr><w:tc><w:p><w:r><w:t>名称</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>数量</w:t></w:r></w:p></w:tc></w:tr>
          <w:tr><w:tc><w:tcPr><w:gridSpan w:val="2"/></w:tcPr><w:p><w:r><w:t>a|b</w:t></w:r></w:p></w:tc></w:tr>
        </w:tbl>
        <w:sectPr/>
        </w:body></w:document>)";
    const auto docx = makeZip({{"word/document.xml", document}, {"word/styles.xml", styles}});
    const ExtractedText result = extractWord(docx);
    QVERIFY2(result.note.isEmpty(), qPrintable(result.note));
    QVERIFY(result.markdown);
    const QString expected = QStringLiteral("# 第一章 开始\n\n"
                                            "正文\t一段  \n第二行\n\n"
                                            "- 条目\n\n"
                                            "# 继承的标题\n\n"
                                            "控件里的字\n\n"
                                            "| 名称 | 数量 |\n"
                                            "| --- | --- |\n"
                                            "| a\\|b |  |\n\n");
    QCOMPARE(result.text, expected.trimmed() + QLatin1Char('\n'));
}

void TestExtract::wordHeadingsByOutline()
{
    const QByteArray document = "<w:document " + kWordNamespace + R"(><w:body>
        <w:p><w:pPr><w:outlineLvl w:val="1"/></w:pPr><w:r><w:t>二级</w:t></w:r></w:p>
        <w:p><w:pPr><w:pStyle w:val="Unknown"/></w:pPr><w:r><w:t>普通</w:t></w:r></w:p>
        </w:body></w:document>)";
    const ExtractedText result = extractWord(makeZip({{"word/document.xml", document}}));
    QCOMPARE(result.text, QStringLiteral("## 二级\n\n普通\n"));
}

void TestExtract::excel()
{
    const QByteArray workbook = R"(<workbook xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
        <sheets><sheet name="销售" sheetId="1" r:id="rId1"/><sheet name="Empty" sheetId="2" r:id="rId2"/></sheets></workbook>)";
    const QByteArray rels = R"(<Relationships>
        <Relationship Id="rId1" Target="worksheets/sheet1.xml"/>
        <Relationship Id="rId2" Target="/xl/worksheets/sheet2.xml"/></Relationships>)";
    const QByteArray strings = R"(<sst><si><t>城市</t></si><si><r><t>销</t></r><r><t>量</t></r><rPh><t>ruby</t></rPh></si>
        <si><t>北京</t></si></sst>)";
    const QByteArray sheet1 = R"(<worksheet><sheetData>
        <row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c></row>
        <row r="2"/>
        <row r="3"><c r="A3" t="s"><v>2</v></c><c r="B3"><v>12.5</v></c><c r="D3" t="b"><v>1</v></c></row>
        <row r="4"><c r="C4" t="inlineStr"><is><t>行内</t></is></c></row>
        </sheetData></worksheet>)";
    const QByteArray sheet2 = "<worksheet><sheetData/></worksheet>";
    const auto xlsx = makeZip({{"xl/workbook.xml", workbook},
                               {"xl/_rels/workbook.xml.rels", rels},
                               {"xl/sharedStrings.xml", strings},
                               {"xl/worksheets/sheet1.xml", sheet1},
                               {"xl/worksheets/sheet2.xml", sheet2}});
    const ExtractedText result = extractExcel(xlsx);
    QVERIFY2(result.note.isEmpty(), qPrintable(result.note));
    QCOMPARE(result.text, QStringLiteral("## 销售\n\n"
                                         "| 城市 | 销量 |  |  |\n"
                                         "| --- | --- | --- | --- |\n"
                                         "| 北京 | 12.5 |  | TRUE |\n"
                                         "|  |  | 行内 |  |\n\n"
                                         "## Empty\n\n(empty)\n"));
}

void TestExtract::epub()
{
    const QByteArray container = R"(<container><rootfiles>
        <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)";
    const QByteArray opf = R"(<package><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>测试书</dc:title></metadata>
        <manifest><item id="c2" href="Text/ch%202.xhtml" media-type="application/xhtml+xml"/>
        <item id="c1" href="Text/ch1.xhtml" media-type="application/xhtml+xml"/>
        <item id="css" href="style.css" media-type="text/css"/></manifest>
        <spine><itemref idref="c1"/><itemref idref="c2"/></spine></package>)";
    const QByteArray ch1 = R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title>x</title>
        <style>p { color: red }</style></head><body>
        <h1>第一章</h1><p>  这是   <b>第一</b>段。&nbsp;</p><p>第二段<br/>换行</p>
        <ul><li>甲</li><li>乙</li></ul><script>alert(1)</script></body></html>)";
    const QByteArray ch2 = R"(<html><body><h2>第二章</h2><div>结束&mdash;</div></body></html>)";
    const auto epub = makeZip({{"mimetype", "application/epub+zip"},
                               {"META-INF/container.xml", container},
                               {"OEBPS/content.opf", opf},
                               {"OEBPS/Text/ch1.xhtml", ch1},
                               {"OEBPS/Text/ch 2.xhtml", ch2}});
    const ExtractedText result = extractEpub(epub);
    QVERIFY2(result.note.isEmpty(), qPrintable(result.note));
    QCOMPARE(result.text, QStringLiteral("# 测试书\n\n"
                                         "# 第一章\n\n"
                                         "这是 第一段。\n\n"
                                         "第二段  \n换行\n\n"
                                         "- 甲\n\n"
                                         "- 乙\n\n"
                                         "## 第二章\n\n"
                                         "结束—\n"));
}

void TestExtract::cells()
{
    QCOMPARE(markdownCell(QStringLiteral(" a|b\nc\\ ")), QStringLiteral("a\\|b<br>c\\\\"));
}

void TestExtract::brokenFiles()
{
    const auto notWord = makeZip({{"word/other.xml", "<x/>"}});
    const ExtractedText word = extractWord(notWord);
    QVERIFY(word.text.isEmpty());
    QVERIFY(!word.note.isEmpty());

    const QByteArray truncated = "<w:document " + kWordNamespace + "><w:body><w:p><w:r><w:t>开头</w:t></w:r></w:p><w:p><w:r>";
    const ExtractedText cut = extractWord(makeZip({{"word/document.xml", truncated}}));
    QCOMPARE(cut.text, QStringLiteral("开头\n"));
    QVERIFY(!cut.note.isEmpty());
}

void TestExtract::documentShowsText()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("report.docx"));
    const QByteArray document =
        "<w:document " + kWordNamespace + "><w:body><w:p><w:r><w:t>报告正文</w:t></w:r></w:p></w:body></w:document>";
    const auto docx = makeZip({{"word/document.xml", document}});
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(reinterpret_cast<const char*>(docx.data()), static_cast<qint64>(docx.size()));
    }
    std::error_code ec;
    std::shared_ptr<const aiedit::core::FileSource> source = aiedit::core::FileSource::open(toPath(path), ec);
    QVERIFY(source);
    DocumentView doc(source, aiedit::core::detectFileType(source->bytes()), aiedit::core::utf8Codec());
    auto shown = [&] {
        const auto bytes = doc.source()->bytes();
        return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    };

    QCOMPARE(doc.extractedFormat(), ExtractedFormat::Word);
    QVERIFY(doc.showsExtractedText());
    QCOMPARE(shown(), QStringLiteral("报告正文\n").toUtf8());
    QCOMPARE(doc.viewMode(), DocumentView::ViewMode::Text);
    QVERIFY(!doc.isEditable());
    QVERIFY(doc.isMarkdown());
    QCOMPARE(doc.filePath(), QFileInfo(path).absoluteFilePath());

    doc.setShowsOriginal(true);
    QVERIFY(!doc.showsExtractedText());
    QCOMPARE(shown().size(), static_cast<qsizetype>(docx.size()));
    QCOMPARE(doc.viewMode(), DocumentView::ViewMode::Hex);
    QVERIFY(!doc.isMarkdown());

    doc.setShowsOriginal(false);
    QVERIFY(doc.showsExtractedText());
    QCOMPARE(shown(), QStringLiteral("报告正文\n").toUtf8());
    QCOMPARE(doc.viewMode(), DocumentView::ViewMode::Text);
}

void TestExtract::pdfWrittenByQt()
{
    // Qt embeds its fonts as CID fonts with ToUnicode maps, as most writers do.
    const bool chinese = !QFontDatabase::families(QFontDatabase::SimplifiedChinese).isEmpty();
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        QPainter painter(&writer);
        QFont font = painter.font();
        font.setPointSize(12);
        painter.setFont(font);
        const int line = painter.fontMetrics().height() * 2;
        painter.drawText(200, line, QStringLiteral("First line of text"));
        painter.drawText(200, line * 2, QStringLiteral("Second line"));
        if (chinese) {
            QFont cjk(QFontDatabase::families(QFontDatabase::SimplifiedChinese).first());
            cjk.setPointSize(12);
            painter.setFont(cjk);
            painter.drawText(200, line * 3, QStringLiteral("中文的一行"));
        }
        writer.newPage();
        painter.setFont(font);
        painter.drawText(200, line, QStringLiteral("Page two"));
    }
    const QByteArray pdf = buffer.data();
    const auto data = std::span(reinterpret_cast<const std::uint8_t*>(pdf.constData()), static_cast<std::size_t>(pdf.size()));
    QCOMPARE(extractableFormat(data), ExtractedFormat::Pdf);
    const ExtractedText result = extractPdf(data);
    QVERIFY2(result.note.isEmpty(), qPrintable(result.note));
    QCOMPARE(result.text, chinese ? QStringLiteral("First line of text\nSecond line\n中文的一行\n\nPage two\n")
                                  : QStringLiteral("First line of text\nSecond line\n\nPage two\n"));
}

void TestExtract::epl()
{
    auto bytesOf = [](const QByteArray& b) {
        return std::span(reinterpret_cast<const std::uint8_t*>(b.constData()), static_cast<std::size_t>(b.size()));
    };
    const QByteArray locked = QByteArray("WTLE") + QByteArray(12, '\x01');
    QCOMPARE(extractableFormat(bytesOf(locked)), ExtractedFormat::Epl);
    const ExtractedText password = extractEpl(bytesOf(locked));
    QVERIFY(password.text.isEmpty());
    QVERIFY(!password.note.isEmpty());

    // The smallest source file: its header and the section that ends it.
    QByteArray empty = "CNWTEPRG";
    const char section[] = "\x19\x73\x11\x15" "\0\0\0\0" "\x19\x73\x00\x07";
    empty += QByteArray(section, 12) + QByteArray(30 + 2 + 4 + 4 + 4 + 4 + 40, '\0');
    const ExtractedText text = extractEpl(bytesOf(empty));
    QVERIFY2(text.note.isEmpty(), qPrintable(text.note));
    QCOMPARE(text.text, QStringLiteral(".版本 2\n"));
}

QTEST_MAIN(TestExtract)
#include "tst_extract.moc"
