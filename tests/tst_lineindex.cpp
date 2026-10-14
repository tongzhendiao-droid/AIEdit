#include "core/LineIndex.h"
#include "core/TextCodec.h"

#include <QStringEncoder>
#include <QTest>

#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace aiedit::core;

Q_DECLARE_METATYPE(CharScheme)

namespace {

std::span<const std::uint8_t> bytesOf(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string utf16le(const std::u16string& text)
{
    std::string out;
    for (const char16_t c : text) {
        out += static_cast<char>(c & 0xFF);
        out += static_cast<char>(c >> 8);
    }
    return out;
}

// Rows by sequential scanning from the start: the reference the index must agree with.
std::vector<Row> scanAll(std::span<const std::uint8_t> data, CharScheme scheme, LineBreak lineBreak,
                         std::uint64_t textStart = 0)
{
    std::vector<Row> rows;
    if (textStart >= data.size())
        return rows;
    Row row = LineIndex::readRow(data, textStart, 0, false, scheme, lineBreak);
    for (;;) {
        rows.push_back(row);
        if (row.end >= data.size())
            return rows;
        const bool terminated = row.end != row.contentEnd;
        row = LineIndex::readRow(data, row.end, terminated ? row.line + 1 : row.line, !terminated, scheme, lineBreak);
    }
}

std::string contentOf(const std::string& text, const Row& row)
{
    return text.substr(row.start, row.contentEnd - row.start);
}

} // namespace

class TestLineIndex : public QObject
{
    Q_OBJECT

private slots:
    void charLengths_data();
    void charLengths();
    void bomLengths();
    void defaultCodecs();
    void charsetDetection();
    void lineEndings();

    void simpleLines();
    void crlfAndCrOnly();
    void trailingNewline();
    void emptyText();
    void longLinesSplitOnCharacterBoundaries();
    void lineOfExactlyMaxLength();
    void legacyCjkSplit();
    void utf16Lines();
    void skipsBom();
    void lookupsMatchSequentialScan();
    void incrementalBuildMatchesFullBuild();
    void readersWhileBuilding();
};

void TestLineIndex::charLengths_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<CharScheme>("scheme");
    QTest::addColumn<int>("length");

    QTest::newRow("ascii") << QByteArray("a") << CharScheme::Utf8 << 1;
    QTest::newRow("utf-8 3 bytes") << QByteArray("中") << CharScheme::Utf8 << 3;
    QTest::newRow("utf-8 truncated") << QByteArray("\xE4\xB8") << CharScheme::Utf8 << 1;
    QTest::newRow("utf-8 stray continuation") << QByteArray("\x80" "a") << CharScheme::Utf8 << 1;
    QTest::newRow("gb18030 2 bytes") << QByteArray("\xC4\xE3") << CharScheme::Gb18030 << 2;
    QTest::newRow("gb18030 4 bytes") << QByteArray("\x81\x30\x81\x30") << CharScheme::Gb18030 << 4;
    QTest::newRow("gb18030 bad trail") << QByteArray("\xC4\x20") << CharScheme::Gb18030 << 1;
    QTest::newRow("big5") << QByteArray("\xA4\x40") << CharScheme::Big5 << 2;
    QTest::newRow("sjis kanji") << QByteArray("\x88\x9F") << CharScheme::ShiftJis << 2;
    QTest::newRow("sjis katakana") << QByteArray("\xB1") << CharScheme::ShiftJis << 1;
    QTest::newRow("euc-jp 3 bytes") << QByteArray("\x8F\xA1\xA1") << CharScheme::EucJp << 3;
    QTest::newRow("euc-kr") << QByteArray("\xB0\xA1") << CharScheme::EucKr << 2;
    QTest::newRow("utf-16 surrogate pair") << QByteArray("\x3D\xD8\x00\xDE", 4) << CharScheme::Utf16LE << 4;
    QTest::newRow("utf-16 bmp") << QByteArray("\x2D\x4E", 2) << CharScheme::Utf16LE << 2;
    QTest::newRow("utf-16 odd tail") << QByteArray("a") << CharScheme::Utf16LE << 1;
}

void TestLineIndex::charLengths()
{
    QFETCH(QByteArray, bytes);
    QFETCH(CharScheme, scheme);
    QFETCH(int, length);
    const std::span<const std::uint8_t> span{reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                             static_cast<std::size_t>(bytes.size())};
    QCOMPARE(charLength(span, scheme), static_cast<std::size_t>(length));
}

void TestLineIndex::bomLengths()
{
    QCOMPARE(bomLength(bytesOf("\xEF\xBB\xBFhi"), CharScheme::Utf8), 3u);
    QCOMPARE(bomLength(bytesOf("hi"), CharScheme::Utf8), 0u);
    QCOMPARE(bomLength(bytesOf(std::string("\xFF\xFEh\0", 4)), CharScheme::Utf16LE), 2u);
    QCOMPARE(bomLength(bytesOf(std::string("\xFF\xFE\0\0", 4)), CharScheme::Utf32LE), 4u);
    QCOMPARE(bomLength(bytesOf("\xEF\xBB\xBF"), CharScheme::Gb18030), 0u);
}

void TestLineIndex::defaultCodecs()
{
    const std::string gbk = "\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7\n"; // "你好，世界" in GBK
    const FileTypeInfo legacy{ContentKind::Text, Encoding::Legacy, false, {}};
    QCOMPARE(defaultCodec(legacy, bytesOf(gbk), "GB18030").name, std::string_view("GB18030"));
    // Valid in Big5 too, so a Big5 preference wins...
    QCOMPARE(defaultCodec(legacy, bytesOf(gbk), "Big5").name, std::string_view("Big5"));
    // ...while a single-byte preference loses to a structurally valid CJK encoding.
    QCOMPARE(defaultCodec(legacy, bytesOf(gbk), "windows-1252").name, std::string_view("GB18030"));
    // Latin-1 text fits no CJK encoding.
    QCOMPARE(defaultCodec(legacy, bytesOf("caf\xE9 au lait\n"), "GB18030").name, std::string_view("windows-1252"));

    QCOMPARE(defaultCodec({ContentKind::Text, Encoding::Ascii, false, {}}, {}).name, std::string_view("UTF-8"));
    QCOMPARE(defaultCodec({ContentKind::Text, Encoding::Utf16BE, true, {}}, {}).name, std::string_view("UTF-16BE"));
    QCOMPARE(defaultCodec({ContentKind::Binary, Encoding::Unknown, false, "ELF"}, {}).name,
             std::string_view("ISO-8859-1"));
    QCOMPARE(findCodec("utf-8"), &utf8Codec());
    QCOMPARE(findCodec("no-such-codec"), nullptr);
}

void TestLineIndex::charsetDetection()
{
#ifndef AIEDIT_HAVE_UCHARDET
    QSKIP("Built without uchardet");
#else
    // Longer samples, where uchardet knows better than byte structure and locale.
    const auto detect = [](const QString& text, const char* codecName, std::string_view preferred) {
        QStringEncoder encoder(codecName);
        const QByteArray bytes = encoder.encode(text.repeated(5));
        const std::span<const std::uint8_t> span{reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                                 static_cast<std::size_t>(bytes.size())};
        return std::string(defaultCodec({ContentKind::Text, Encoding::Legacy, false, {}}, span, preferred).name);
    };
    const QString traditional = QStringLiteral("這是一個用於測試編碼檢測的繁體中文文本，包含常見的漢字。\n");
    const QString simplified = QStringLiteral("这是一个用于测试编码检测的较长的中文文本，包含常见的汉字和标点符号。\n");
    const QString russian = QStringLiteral("Это текст на русском языке для проверки определения кодировки.\n");
    const QString japanese = QStringLiteral("これは文字コードの検出をテストするための日本語のテキストです。\n");

    QCOMPARE(detect(traditional, "Big5", "GB18030"), std::string("Big5")); // despite a GB18030 locale
    QCOMPARE(detect(simplified, "GB18030", "Big5"), std::string("GB18030"));
    QCOMPARE(detect(russian, "KOI8-R", "windows-1251"), std::string("KOI8-R"));
    QCOMPARE(detect(russian, "windows-1251", "KOI8-R"), std::string("windows-1251"));
    QCOMPARE(detect(japanese, "Shift_JIS", "GB18030"), std::string("Shift_JIS"));
#endif
}

void TestLineIndex::lineEndings()
{
    const LineEndings mixed = countLineEndings(bytesOf("a\r\nb\nc\rd\r\r\n"), CharScheme::Utf8);
    QCOMPARE(mixed.crlf, 2u);
    QCOMPARE(mixed.lf, 1u);
    QCOMPARE(mixed.cr, 2u);
    QCOMPARE(mixed.lineBreak(), LineBreak::Lf);
    QCOMPARE(countLineEndings(bytesOf("a\rb\r"), CharScheme::Utf8).lineBreak(), LineBreak::Cr);
    QCOMPARE(countLineEndings(bytesOf(utf16le(u"a\r\nb\r\n")), CharScheme::Utf16LE).crlf, 2u);
}

void TestLineIndex::simpleLines()
{
    const std::string text = "first\nsecond\n\nfourth";
    LineIndex index(bytesOf(text), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(index.build());
    QVERIFY(index.isComplete());
    QCOMPARE(index.rowCount(), 4u);
    QCOMPARE(index.lineCount(), 4u);
    QCOMPARE(contentOf(text, index.row(0)), std::string("first"));
    QCOMPARE(contentOf(text, index.row(1)), std::string("second"));
    QCOMPARE(contentOf(text, index.row(2)), std::string(""));
    QCOMPARE(contentOf(text, index.row(3)), std::string("fourth"));
    QCOMPARE(index.row(3).line, 3u);
    QCOMPARE(index.rowIndexAt(0), 0u);
    QCOMPARE(index.rowIndexAt(5), 0u); // the '\n' belongs to its row
    QCOMPARE(index.rowIndexAt(6), 1u);
    QCOMPARE(index.rowIndexAt(text.size()), 3u);
    QCOMPARE(index.rowIndexOfLine(2), std::optional<std::uint64_t>(2));
    QCOMPARE(index.rowIndexOfLine(4), std::nullopt);
}

void TestLineIndex::crlfAndCrOnly()
{
    const std::string crlf = "one\r\ntwo\r\n";
    LineIndex windows(bytesOf(crlf), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(windows.build());
    QCOMPARE(windows.rowCount(), 2u);
    QCOMPARE(contentOf(crlf, windows.row(0)), std::string("one"));
    QCOMPARE(windows.row(0).end, 5u);

    const std::string mac = "one\rtwo\rthree";
    LineIndex classic(bytesOf(mac), CharScheme::Utf8, LineBreak::Cr);
    QVERIFY(classic.build());
    QCOMPARE(classic.rowCount(), 3u);
    QCOMPARE(contentOf(mac, classic.row(2)), std::string("three"));
}

void TestLineIndex::trailingNewline()
{
    // A final terminator does not create an extra empty row.
    LineIndex index(bytesOf("a\nb\n"), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(index.build());
    QCOMPARE(index.rowCount(), 2u);
    QCOMPARE(index.lineCount(), 2u);
}

void TestLineIndex::emptyText()
{
    LineIndex index(bytesOf(""), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(index.build());
    QCOMPARE(index.rowCount(), 0u);
    QCOMPARE(index.lineCount(), 0u);
    QVERIFY(index.rows(0, 10).empty());
}

void TestLineIndex::longLinesSplitOnCharacterBoundaries()
{
    // 10,000 three-byte characters on one line, then a short one.
    std::string text;
    for (int i = 0; i < 10000; ++i)
        text += "中";
    text += "\nshort\n";

    LineIndex index(bytesOf(text), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(index.build());
    QCOMPARE(index.lineCount(), 2u);
    QVERIFY(index.rowCount() > 2);

    const auto rows = index.rows(0, static_cast<std::size_t>(index.rowCount()));
    std::uint64_t covered = 0;
    for (std::size_t i = 0; i + 1 < rows.size(); ++i) {
        const Row& row = rows[i];
        QCOMPARE(row.start, covered);
        QVERIFY(row.contentEnd - row.start <= LineIndex::kMaxRowBytes);
        QCOMPARE((row.contentEnd - row.start) % 3, 0u); // never splits a character
        QCOMPARE(row.line, 0u);
        QCOMPARE(row.continuation, i > 0);
        covered = row.end;
    }
    QCOMPARE(contentOf(text, rows.back()), std::string("short"));
    QCOMPARE(rows.back().line, 1u);
    QCOMPARE(index.rowIndexOfLine(1), std::optional<std::uint64_t>(rows.size() - 1));
}

void TestLineIndex::lineOfExactlyMaxLength()
{
    // The terminator right after a full-length row must not produce an empty extra row.
    const std::string text = std::string(LineIndex::kMaxRowBytes, 'x') + "\nnext";
    LineIndex index(bytesOf(text), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(index.build());
    QCOMPARE(index.rowCount(), 2u);
    QCOMPARE(index.row(0).contentEnd, LineIndex::kMaxRowBytes);
    QCOMPARE(contentOf(text, index.row(1)), std::string("next"));
}

void TestLineIndex::legacyCjkSplit()
{
    // "a" then GBK double-byte characters: an odd offset at the 4096 boundary must not split one.
    std::string text = "a";
    for (int i = 0; i < 3000; ++i)
        text += "\xC4\xE3";
    LineIndex index(bytesOf(text), CharScheme::Gb18030, LineBreak::Lf);
    QVERIFY(index.build());
    const Row first = index.row(0);
    QCOMPARE((first.contentEnd - 1) % 2, 0u);
    QCOMPARE(index.row(1).start, first.contentEnd);
}

void TestLineIndex::utf16Lines()
{
    const std::string text = utf16le(u"﻿hello\r\n世界\nend");
    LineIndex index(bytesOf(text), CharScheme::Utf16LE, LineBreak::Lf, 2);
    QVERIFY(index.build());
    QCOMPARE(index.rowCount(), 3u);
    QCOMPARE(index.row(0).start, 2u);
    QCOMPARE(index.row(0).contentEnd - index.row(0).start, 10u); // "hello" without CR
    QCOMPARE(index.row(1).contentEnd - index.row(1).start, 4u);
    QCOMPARE(index.row(2).contentEnd, text.size());
}

void TestLineIndex::skipsBom()
{
    const std::string text = "\xEF\xBB\xBFline";
    LineIndex index(bytesOf(text), CharScheme::Utf8, LineBreak::Lf, bomLength(bytesOf(text), CharScheme::Utf8));
    QVERIFY(index.build());
    QCOMPARE(index.row(0).start, 3u);
    QCOMPARE(index.rowIndexAt(0), 0u);
}

void TestLineIndex::lookupsMatchSequentialScan()
{
    // Random lines (some very long, some empty) spanning many checkpoints.
    std::mt19937 rng(42);
    std::string text;
    for (int line = 0; line < 3000; ++line) {
        const int kind = static_cast<int>(rng() % 20);
        const std::size_t length = kind == 0 ? 9000 + rng() % 5000 : kind == 1 ? 0 : rng() % 120;
        for (std::size_t i = 0; i < length; ++i)
            text += i % 7 == 6 ? std::string("é") : std::string(1, static_cast<char>('a' + i % 26));
        text += (rng() % 3 == 0) ? "\r\n" : "\n";
    }

    const auto data = bytesOf(text);
    const auto reference = scanAll(data, CharScheme::Utf8, LineBreak::Lf);
    LineIndex index(data, CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(index.build());
    QCOMPARE(index.rowCount(), reference.size());
    QCOMPARE(index.lineCount(), reference.back().line + 1);

    for (int probe = 0; probe < 2000; ++probe) {
        const std::size_t i = rng() % reference.size();
        QCOMPARE(index.row(i), reference[i]);
        const std::uint64_t inside = reference[i].start + rng() % (reference[i].end - reference[i].start);
        QCOMPARE(index.rowIndexAt(inside), static_cast<std::uint64_t>(i));
        if (!reference[i].continuation)
            QCOMPARE(index.rowIndexOfLine(reference[i].line), std::optional<std::uint64_t>(i));
    }
    const auto window = index.rows(700, 300);
    QCOMPARE(window.size(), 300u);
    for (std::size_t k = 0; k < window.size(); ++k)
        QCOMPARE(window[k], reference[700 + k]);
}

void TestLineIndex::incrementalBuildMatchesFullBuild()
{
    std::string text;
    for (int i = 0; i < 5000; ++i)
        text += "line " + std::to_string(i) + "\n";
    LineIndex full(bytesOf(text), CharScheme::Utf8, LineBreak::Lf);
    QVERIFY(full.build());

    LineIndex stepwise(bytesOf(text), CharScheme::Utf8, LineBreak::Lf);
    int steps = 0;
    while (!stepwise.buildSome(997)) {
        ++steps;
        QVERIFY(stepwise.rowCount() > 0);
        QVERIFY(stepwise.indexedBytes() < text.size());
    }
    QVERIFY(steps > 10);
    QCOMPARE(stepwise.rowCount(), full.rowCount());
    QCOMPARE(stepwise.row(4321), full.row(4321));
}

void TestLineIndex::readersWhileBuilding()
{
    std::string text;
    for (int i = 0; i < 400000; ++i)
        text += "a log line with some text " + std::to_string(i) + "\n";
    LineIndex index(bytesOf(text), CharScheme::Utf8, LineBreak::Lf);

    std::thread builder([&] { index.build(); });
    std::uint64_t lastSeen = 0;
    while (!index.isComplete()) {
        const std::uint64_t rows = index.rowCount();
        QVERIFY(rows >= lastSeen);
        if (rows > 0) {
            const Row last = index.row(rows - 1);
            QCOMPARE(last.line, rows - 1);
            QCOMPARE(index.rowIndexAt(last.start), rows - 1);
        }
        lastSeen = rows;
    }
    builder.join();
    QCOMPARE(index.rowCount(), 400000u);
}

QTEST_APPLESS_MAIN(TestLineIndex)
#include "tst_lineindex.moc"
