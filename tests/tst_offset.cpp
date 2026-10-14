#include "core/Offset.h"

#include <QTest>

using cppedit::core::parseOffset;

class TestOffset : public QObject
{
    Q_OBJECT

private slots:
    void parse_data();
    void parse();
};

void TestOffset::parse_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<quint64>("current");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<quint64>("expected");

    QTest::newRow("decimal") << "31" << 0ull << true << 31ull;
    QTest::newRow("0x prefix") << "0x1F" << 0ull << true << 31ull;
    QTest::newRow("h suffix") << "1Fh" << 0ull << true << 31ull;
    QTest::newRow("bare hex letters") << "ff" << 0ull << true << 255ull;
    QTest::newRow("whitespace") << "  42 \t" << 0ull << true << 42ull;
    QTest::newRow("relative forward") << "+0x10" << 100ull << true << 116ull;
    QTest::newRow("relative back") << "-16" << 100ull << true << 84ull;
    QTest::newRow("relative back clamps") << "-1000" << 100ull << true << 0ull;
    QTest::newRow("max") << "0xFFFFFFFFFFFFFFFF" << 0ull << true << 0xFFFFFFFFFFFFFFFFull;
    QTest::newRow("overflow") << "0x10000000000000000" << 0ull << false << 0ull;
    QTest::newRow("relative overflow") << "+1" << 0xFFFFFFFFFFFFFFFFull << false << 0ull;
    QTest::newRow("empty") << "" << 0ull << false << 0ull;
    QTest::newRow("sign only") << "+" << 0ull << false << 0ull;
    QTest::newRow("bare 0x") << "0x" << 0ull << false << 0ull;
    QTest::newRow("garbage") << "12z" << 0ull << false << 0ull;
}

void TestOffset::parse()
{
    QFETCH(QString, text);
    QFETCH(quint64, current);
    QFETCH(bool, ok);
    QFETCH(quint64, expected);

    const auto result = parseOffset(text.toStdString(), current);
    QCOMPARE(result.has_value(), ok);
    if (ok)
        QCOMPARE(*result, expected);
}

QTEST_APPLESS_MAIN(TestOffset)
#include "tst_offset.moc"
