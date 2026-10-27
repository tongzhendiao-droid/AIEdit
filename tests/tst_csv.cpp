#include "core/Csv.h"

#include <QTest>

using namespace aiedit::core;

namespace {

std::span<const std::uint8_t> bytes(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// Every row as its cells' text joined by '|'.
QStringList rows(const std::string& data, char separator)
{
    const CsvTable table = readCsv(bytes(data), separator);
    QStringList out;
    for (std::size_t r = 0; r < table.rowCount(); ++r) {
        QStringList cells;
        for (const auto& cell : table.row(r))
            cells << QString::fromStdString(csvCellBytes(bytes(data), cell));
        out << cells.join(u'|');
    }
    return out;
}

} // namespace

class TestCsv : public QObject
{
    Q_OBJECT

private slots:
    void plain();
    void quoted();
    void lineEndings();
    void separators();
    void offsetsAndLimits();
};

void TestCsv::plain()
{
    QCOMPARE(rows("a,b,c\n1,2,3\n", ','), QStringList({"a|b|c", "1|2|3"}));
    QCOMPARE(rows("a,,c\n,\n", ','), QStringList({"a||c", "|"}));
    QCOMPARE(rows("only", ','), QStringList({"only"}));
    QCOMPARE(rows("", ','), QStringList());
    // A UTF-8 byte order mark is not text of the first cell; GBK bytes pass through untouched.
    const std::string gbk = "\xEF\xBB\xBFx,\xB2\xE2\xCA\xD4\n";
    const CsvTable withBom = readCsv(bytes(gbk), ',');
    QCOMPARE(withBom.rowCount(), 1u);
    QCOMPARE(csvCellBytes(bytes(gbk), withBom.row(0)[0]), std::string("x"));
    QCOMPARE(csvCellBytes(bytes(gbk), withBom.row(0)[1]), std::string("\xB2\xE2\xCA\xD4"));
    const CsvTable table = readCsv(bytes("a,b\n1,2,3\n"), ',');
    QCOMPARE(table.columns, 3u);
}

void TestCsv::quoted()
{
    QCOMPARE(rows("\"a,b\",\"say \"\"hi\"\"\"\n", ','), QStringList({"a,b|say \"hi\""}));
    QCOMPARE(rows("\"two\nlines\",x\ny,z\n", ','), QStringList({"two\nlines|x", "y|z"}));
    QCOMPARE(rows("\"\",\"\"\n", ','), QStringList({"|"}));
    // Junk after a closing quote is dropped; an unclosed quote runs to the end.
    QCOMPARE(rows("\"a\"junk,b\n", ','), QStringList({"a|b"}));
    QCOMPARE(rows("\"open,x\ny", ','), QStringList({"open,x\ny"}));
}

void TestCsv::lineEndings()
{
    QCOMPARE(rows("a,b\r\nc,d\r\n", ','), QStringList({"a|b", "c|d"}));
    QCOMPARE(rows("a,b\rc,d", ','), QStringList({"a|b", "c|d"}));
}

void TestCsv::separators()
{
    QCOMPARE(detectCsvSeparator(bytes("a,b,c\n1,2,3\n4,5,6\n")), ',');
    QCOMPARE(detectCsvSeparator(bytes("a\tb\tc\n1\t2,5\t3\n")), '\t');
    QCOMPARE(detectCsvSeparator(bytes("name;price\n\"x;y\";1,50\nz;2,00\n")), ';');
    QCOMPARE(detectCsvSeparator(bytes("a|b\n1|2\n")), '|');
    QCOMPARE(detectCsvSeparator(bytes("no separators here\n")), ',');
    QCOMPARE(rows("a\tb,c\n", '\t'), QStringList({"a|b,c"}));
}

void TestCsv::offsetsAndLimits()
{
    const std::string data = "h1,h2\nr1,x\n\"q\nq\",y\nlast,z";
    const CsvTable table = readCsv(bytes(data), ',');
    QCOMPARE(table.rowCount(), 4u);
    QCOMPARE(table.rowOffsets[1], 6u);
    QCOMPARE(table.rowOffsets[2], 11u);
    QCOMPARE(table.rowOffsets[3], 19u);
    QVERIFY(!table.truncated);

    const CsvTable limited = readCsv(bytes(data), ',', 2);
    QCOMPARE(limited.rowCount(), 2u);
    QVERIFY(limited.truncated);
    const CsvTable fewCells = readCsv(bytes(data), ',', 100, 3);
    QCOMPARE(fewCells.rowCount(), 2u);
    QVERIFY(fewCells.truncated);
}

QTEST_GUILESS_MAIN(TestCsv)
#include "tst_csv.moc"
