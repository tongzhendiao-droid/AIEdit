#include "core/Search.h"

#include <QTest>

#include <string>

using namespace cppedit::core;

namespace {

std::span<const std::uint8_t> bytesOf(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

BytePattern literal(const std::string& s)
{
    BytePattern pattern;
    pattern.bytes.assign(s.begin(), s.end());
    return pattern;
}

} // namespace

class TestSearch : public QObject
{
    Q_OBJECT

private slots:
    void parseHex_data();
    void parseHex();
    void forwardAndBackward();
    void wildcards();
    void matchAcrossChunkBoundary();
    void cancellationAndProgress();
};

void TestSearch::parseHex_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("bytes"); // empty: parse fails
    QTest::addColumn<QString>("wildcards"); // one char per byte, 'x' marks a wildcard

    QTest::newRow("spaced") << "4D 5A 90 00" << QByteArray("\x4D\x5A\x90\x00", 4) << "....";
    QTest::newRow("packed") << "4d5a9000" << QByteArray("\x4D\x5A\x90\x00", 4) << "....";
    QTest::newRow("wildcards") << "4D ?? ? 00" << QByteArray("\x4D\x00\x00\x00", 4) << ".xx.";
    QTest::newRow("c array") << "{0x4D, 0x5A}" << QByteArray("\x4D\x5A") << "..";
    QTest::newRow("dashes") << "4D-5A" << QByteArray("\x4D\x5A") << "..";
    QTest::newRow("lone digit") << "A 4D" << QByteArray("\x0A\x4D") << "..";
    QTest::newRow("odd tail") << "4D5" << QByteArray("\x4D\x05") << "..";
    QTest::newRow("bad digit") << "4G" << QByteArray() << "";
    QTest::newRow("only wildcards") << "?? ??" << QByteArray() << "";
    QTest::newRow("empty") << "  " << QByteArray() << "";
}

void TestSearch::parseHex()
{
    QFETCH(QString, text);
    QFETCH(QByteArray, bytes);
    QFETCH(QString, wildcards);

    const auto pattern = parseHexPattern(text.toStdString());
    if (bytes.isEmpty()) {
        QVERIFY(!pattern);
        return;
    }
    QVERIFY(pattern);
    QCOMPARE(QByteArray(reinterpret_cast<const char*>(pattern->bytes.data()), static_cast<qsizetype>(pattern->size())),
             bytes);
    QString marks;
    for (std::size_t i = 0; i < pattern->size(); ++i)
        marks += pattern->wildcard[i] ? QLatin1Char('x') : QLatin1Char('.');
    QCOMPARE(marks, wildcards);
}

void TestSearch::forwardAndBackward()
{
    const std::string text = "abc needle def needle ghi";
    const auto data = bytesOf(text);
    const BytePattern needle = literal("needle");
    QCOMPARE(findPattern(data, needle, 0, true), std::optional<std::uint64_t>(4));
    QCOMPARE(findPattern(data, needle, 4, true), std::optional<std::uint64_t>(4)); // inclusive
    QCOMPARE(findPattern(data, needle, 5, true), std::optional<std::uint64_t>(15));
    QCOMPARE(findPattern(data, needle, 16, true), std::nullopt);
    QCOMPARE(findPattern(data, needle, text.size(), false), std::optional<std::uint64_t>(15));
    QCOMPARE(findPattern(data, needle, 15, false), std::optional<std::uint64_t>(4)); // exclusive
    QCOMPARE(findPattern(data, needle, 4, false), std::nullopt);
    QCOMPARE(findPattern(data, literal("missing"), 0, true), std::nullopt);
    QCOMPARE(findPattern(bytesOf("ab"), literal("abc"), 0, true), std::nullopt);
}

void TestSearch::wildcards()
{
    const std::string data("\x00MZ\x90\x00\x03MZ\x91\x00\x03", 11);
    const auto pattern = parseHexPattern("4D 5A ?? 00 03");
    QVERIFY(pattern);
    QCOMPARE(findPattern(bytesOf(data), *pattern, 0, true), std::optional<std::uint64_t>(1));
    QCOMPARE(findPattern(bytesOf(data), *pattern, 2, true), std::optional<std::uint64_t>(6));
    QCOMPARE(findPattern(bytesOf(data), *pattern, data.size(), false), std::optional<std::uint64_t>(6));

    // A leading wildcard anchors on the first fixed byte.
    const auto leading = parseHexPattern("?? 5A");
    QCOMPARE(findPattern(bytesOf(data), *leading, 0, true), std::optional<std::uint64_t>(1));
}

void TestSearch::matchAcrossChunkBoundary()
{
    // The search works in 16 MiB chunks; a match straddling a boundary must still be found.
    std::string data(40u * 1024 * 1024, 'x');
    const std::uint64_t at = 16u * 1024 * 1024 - 3;
    data.replace(at, 6, "needle");
    QCOMPARE(findPattern(bytesOf(data), literal("needle"), 0, true), std::optional<std::uint64_t>(at));
    QCOMPARE(findPattern(bytesOf(data), literal("needle"), data.size(), false), std::optional<std::uint64_t>(at));
    const auto wild = parseHexPattern("6E 65 ?? 64");
    QCOMPARE(findPattern(bytesOf(data), *wild, 0, true), std::optional<std::uint64_t>(at));
}

void TestSearch::cancellationAndProgress()
{
    const std::string data(40u * 1024 * 1024, 'x');
    std::atomic<bool> cancelled{false};
    std::uint64_t reported = 0;
    SearchControl control;
    control.cancelled = &cancelled;
    control.progress = [&](std::uint64_t scanned) {
        reported = scanned;
        cancelled = true; // stop after the first chunk
    };
    QCOMPARE(findPattern(bytesOf(data), literal("needle"), 0, true, control), std::nullopt);
    QCOMPARE(reported, 16u * 1024 * 1024);
}

QTEST_APPLESS_MAIN(TestSearch)
#include "tst_search.moc"
