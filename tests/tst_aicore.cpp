#include "core/FileSummary.h"
#include "core/Sse.h"

#include <QTest>

#include <random>
#include <string>

using namespace aiedit::core;

namespace {

std::span<const std::uint8_t> bytesOf(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::vector<std::string> dataOf(const std::vector<SseEvent>& events)
{
    std::vector<std::string> out;
    for (const SseEvent& e : events)
        out.push_back(e.data);
    return out;
}

} // namespace

class TestAiCore : public QObject
{
    Q_OBJECT

private slots:
    void sseBasics();
    void sseSplitAnywhere();
    void sseLineEndingsAndFields();
    void hexDumpFormat();
    void entropyBounds();
    void stringsExtraction();
    void fileSummary();
};

void TestAiCore::sseBasics()
{
    SseParser parser;
    const auto events = parser.feed(": keep-alive\n\ndata: {\"a\":1}\n\ndata: [DONE]\n\n");
    QCOMPARE(events.size(), 2u);
    QCOMPARE(events[0].type, std::string("message"));
    QCOMPARE(events[0].data, std::string("{\"a\":1}"));
    QCOMPARE(events[1].data, std::string("[DONE]"));
    QVERIFY(parser.finish().empty());
}

void TestAiCore::sseSplitAnywhere()
{
    // Every split point of the stream, including inside "\r\n" and a UTF-8 character, must
    // give the same events.
    const std::string stream = "data: {\"t\":\"你好\"}\r\n\r\n: ping\r\ndata: second\r\n\r\ndata: [DONE]\r\n\r\n";
    const std::vector<std::string> expected{"{\"t\":\"你好\"}", "second", "[DONE]"};
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        SseParser parser;
        auto events = parser.feed(std::string_view(stream).substr(0, split));
        const auto rest = parser.feed(std::string_view(stream).substr(split));
        events.insert(events.end(), rest.begin(), rest.end());
        QCOMPARE(dataOf(events), expected);
    }
}

void TestAiCore::sseLineEndingsAndFields()
{
    SseParser parser;
    auto events = parser.feed("event: delta\rdata: line 1\rdata:line 2\r\rid: 7\nretry: 100\ndata: x\n");
    QCOMPARE(events.size(), 1u);
    QCOMPARE(events[0].type, std::string("delta"));
    QCOMPARE(events[0].data, std::string("line 1\nline 2"));
    // A last event without the blank line is delivered by finish().
    events = parser.finish();
    QCOMPARE(dataOf(events), std::vector<std::string>{"x"});
}

void TestAiCore::hexDumpFormat()
{
    const std::string dump = hexDump(bytesOf(std::string("MZ\x90\x00", 4) + "0123456789ABCDEFGH"), 0x10);
    const std::string expected =
        "00000010  4D 5A 90 00 30 31 32 33  34 35 36 37 38 39 41 42  |MZ..0123456789AB|\n"
        "00000020  43 44 45 46 47 48                                 |CDEFGH|\n";
    QCOMPARE(dump, expected);
    QVERIFY(hexDump({}).empty());
}

void TestAiCore::entropyBounds()
{
    QCOMPARE(entropy(bytesOf(std::string(1000, 'a'))), 0.0);
    std::string all;
    for (int i = 0; i < 256 * 4; ++i)
        all += static_cast<char>(i & 0xFF);
    QVERIFY(std::abs(entropy(bytesOf(all)) - 8.0) < 1e-9);
    QVERIFY(std::abs(entropy(bytesOf("abab")) - 1.0) < 1e-9);
}

void TestAiCore::stringsExtraction()
{
    const std::string data = std::string("\x00\x01hello world\x00short\x00", 21) + "/usr/lib/libc.so.6";
    const auto strings = extractStrings(bytesOf(data), 6);
    QCOMPARE(strings, (std::vector<std::string>{"hello world", "/usr/lib/libc.so.6"}));
    QCOMPARE(extractStrings(bytesOf(data), 4, 1).size(), 1u);
}

void TestAiCore::fileSummary()
{
    // An ELF-like header followed by random (high-entropy) data.
    std::string data("\x7F" "ELF\x02\x01\x01\x00", 8);
    data += std::string(56, '\0') + "GCC: (Ubuntu) 15.2.0" + std::string(1, '\0'); // C strings end in NUL
    std::mt19937 rng(1);
    while (data.size() < 200 * 1024)
        data += static_cast<char>(rng() & 0xFF);

    const FileSummary summary = summarizeFile(bytesOf(data));
    QCOMPARE(summary.size, data.size());
    QCOMPARE(summary.type.format, std::string_view("ELF"));
    QVERIFY(summary.headDump.starts_with("00000000  7F 45 4C 46"));
    QCOMPARE(summary.blockEntropy.size(), kSummaryBlocks);
    QVERIFY(summary.blockEntropy.back() > 7.9); // random data
    QVERIFY(std::find(summary.strings.begin(), summary.strings.end(), "GCC: (Ubuntu) 15.2.0") != summary.strings.end());

    const FileSummary tiny = summarizeFile(bytesOf("hi"));
    QVERIFY(tiny.blockEntropy.empty());
}

QTEST_APPLESS_MAIN(TestAiCore)
#include "tst_aicore.moc"
