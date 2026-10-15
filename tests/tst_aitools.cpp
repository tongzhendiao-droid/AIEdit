#include "core/FileSource.h"
#include "ui/AiTools.h"
#include "ui/Paths.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace aiedit;
using namespace aiedit::ui;

class TestAiTools : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void fileInfo();
    void readBytes();
    void readLines();
    void searchText();
    void searchHexAndLimits();
    void filterLines();
    void markFields();
    void runnerDispatchesAndCancels();

private:
    ToolTarget target(const char* name, const std::string& content);

    QTemporaryDir m_dir;
    QList<FieldMark> m_marked;
};

void TestAiTools::init()
{
    QVERIFY(m_dir.isValid());
    m_marked.clear();
}

ToolTarget TestAiTools::target(const char* name, const std::string& content)
{
    const auto path = toPath(m_dir.filePath(QString::fromLatin1(name)));
    std::ofstream(path, std::ios::binary) << content;
    std::error_code ec;
    ToolTarget t;
    t.fileName = QString::fromLatin1(name);
    t.source = core::FileSource::open(path, ec);
    t.type = core::detectFileType(t.source->bytes());
    t.codec = core::defaultCodec(t.type, t.source->bytes());
    auto index = std::make_shared<core::LineIndex>(t.source->bytes(), t.codec.scheme, core::LineBreak::Lf,
                                                   core::bomLength(t.source->bytes(), t.codec.scheme));
    index->build();
    t.index = index;
    t.markFields = [this](const QList<FieldMark>& marks) { m_marked = marks; };
    return t;
}

void TestAiTools::fileInfo()
{
    const ToolTarget t = target("info.log", "alpha\nbeta\ngamma\n");
    const QString info = tools::fileInfo(t);
    QVERIFY(info.contains(QStringLiteral("Name: info.log")));
    QVERIFY(info.contains(QStringLiteral("Size: 17 bytes")));
    QVERIFY(info.contains(QStringLiteral("Detected: text")));
    QVERIFY(info.contains(QStringLiteral("Lines: 3")));
    QVERIFY(info.contains(QStringLiteral("00000000  61 6C 70 68 61")));
}

void TestAiTools::readBytes()
{
    std::string content;
    for (int i = 0; i < 10000; ++i)
        content += static_cast<char>(i & 0xFF);
    const ToolTarget t = target("data.bin", content);

    QString result = tools::readBytes(t, {{QStringLiteral("offset"), 16}, {QStringLiteral("length"), 4}});
    QVERIFY2(result.contains(QStringLiteral("00000010  10 11 12 13")), qPrintable(result));
    // Offsets written as hex strings work too.
    result = tools::readBytes(t, {{QStringLiteral("offset"), QStringLiteral("0x20")}, {QStringLiteral("length"), 2}});
    QVERIFY(result.contains(QStringLiteral("00000020  20 21")));
    // Large reads are capped at 4096 bytes and say so.
    result = tools::readBytes(t, {{QStringLiteral("offset"), 0}, {QStringLiteral("length"), 100000}});
    QVERIFY(result.contains(QStringLiteral("(4096 bytes)")));
    QVERIFY(result.contains(QStringLiteral("At most 4096")));
    QVERIFY(tools::readBytes(t, {{QStringLiteral("offset"), 20000}}).startsWith(QStringLiteral("Error")));
    QVERIFY(tools::readBytes(t, {}).startsWith(QStringLiteral("Error")));
}

void TestAiTools::readLines()
{
    const ToolTarget t = target("lines.txt", "one\ntwo\nthree\n" + std::string(9000, 'x') + "\nlast\n");
    QString result = tools::readLines(t, {{QStringLiteral("start_line"), 2}, {QStringLiteral("count"), 2}});
    QCOMPARE(result, QStringLiteral("2: two\n3: three\n"));
    // A line split into rows comes back whole, but cut to a readable length.
    result = tools::readLines(t, {{QStringLiteral("start_line"), 4}, {QStringLiteral("count"), 2}});
    QVERIFY(result.contains(QStringLiteral("[line truncated]")));
    QVERIFY(result.endsWith(QStringLiteral("5: last\n")));
    QVERIFY(tools::readLines(t, {{QStringLiteral("start_line"), 0}}).startsWith(QStringLiteral("Error")));
    QVERIFY(tools::readLines(t, {{QStringLiteral("start_line"), 99}}).contains(QStringLiteral("only 5 lines")));
}

void TestAiTools::searchText()
{
    const ToolTarget t = target("app.log",
                                "INFO start\nERROR disk full on /dev/sda1\nINFO retry\nerror: timeout after 30s\n");
    const std::atomic<bool> notCancelled{false};
    QString result = tools::search(t, {{QStringLiteral("query"), QStringLiteral("error")}}, notCancelled);
    QVERIFY2(result.startsWith(QStringLiteral("2 match(es)")), qPrintable(result));
    QVERIFY(result.contains(QStringLiteral("0xB (line 2): ERROR disk full on /dev/sda1")));
    QVERIFY(result.contains(QStringLiteral("(line 4): error: timeout after 30s")));

    result = tools::search(t, {{QStringLiteral("query"), QStringLiteral("\\d+s$")}, {QStringLiteral("mode"), QStringLiteral("regex")}},
                           notCancelled);
    QVERIFY(result.contains(QStringLiteral("(line 4)")));
    QCOMPARE(tools::search(t, {{QStringLiteral("query"), QStringLiteral("nothing like this")}}, notCancelled),
             QStringLiteral("No matches."));
    QVERIFY(tools::search(t, {{QStringLiteral("query"), QStringLiteral("(")}, {QStringLiteral("mode"), QStringLiteral("regex")}},
                          notCancelled)
                .startsWith(QStringLiteral("Error")));
}

void TestAiTools::filterLines()
{
    std::string log;
    for (int i = 1; i <= 60; ++i)
        log += (i % 3 == 0 ? "ERROR request " : "INFO request ") + std::to_string(i) + " took " +
               std::to_string(i * 50) + "ms\n";
    const ToolTarget t = target("filter.log", log);
    const std::atomic<bool> notCancelled{false};
    const auto condition = [](const QString& type, const QString& regex) {
        return QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("regex"), regex}};
    };

    // Counted over the whole file; the first lines listed with their numbers.
    QString result = tools::filterLines(
        t, {{QStringLiteral("conditions"), QJsonArray{condition(QStringLiteral("include"), QStringLiteral("error"))}},
            {QStringLiteral("max_lines"), 2}},
        notCancelled);
    QVERIFY2(result.startsWith(QStringLiteral("20 of 60 lines pass; the first 2:\n3: ERROR request 3 took 150ms\n"
                                              "6: ERROR request 6 took 300ms")),
             qPrintable(result));

    // A number compared, an exclusion, and "any".
    QJsonObject slow = condition(QStringLiteral("include"), QStringLiteral("took (?<ms>\\d+)ms"));
    slow.insert(QStringLiteral("field"), QStringLiteral("ms"));
    slow.insert(QStringLiteral("op"), QStringLiteral(">"));
    slow.insert(QStringLiteral("value"), 2900);
    result = tools::filterLines(
        t, {{QStringLiteral("conditions"), QJsonArray{slow, condition(QStringLiteral("exclude"), QStringLiteral("ERROR"))}}},
        notCancelled);
    QCOMPARE(result, QStringLiteral("1 of 60 lines pass:\n59: INFO request 59 took 2950ms"));
    result = tools::filterLines(t,
                                {{QStringLiteral("conditions"),
                                  QJsonArray{condition(QStringLiteral("include"), QStringLiteral("request 1 ")),
                                             condition(QStringLiteral("include"), QStringLiteral("request 2 "))}},
                                 {QStringLiteral("match"), QStringLiteral("any")},
                                 {QStringLiteral("max_lines"), 0}},
                                notCancelled);
    QCOMPARE(result, QStringLiteral("2 of 60 lines pass"));

    QVERIFY(tools::filterLines(t, {}, notCancelled).startsWith(QStringLiteral("Error: ")));
    QVERIFY(tools::filterLines(
                t, {{QStringLiteral("conditions"), QJsonArray{condition(QStringLiteral("include"), QStringLiteral("("))}}},
                notCancelled)
                .contains(QStringLiteral("invalid regular expression")));
    const std::atomic<bool> cancelled{true};
    QCOMPARE(tools::filterLines(
                 t, {{QStringLiteral("conditions"), QJsonArray{condition(QStringLiteral("include"), QStringLiteral("x"))}}},
                 cancelled),
             QStringLiteral("Cancelled."));
}

void TestAiTools::searchHexAndLimits()
{
    std::string content;
    for (int i = 0; i < 50; ++i)
        content += std::string("\x00\x00MZ\x90\x00", 6);
    const ToolTarget t = target("many.bin", content);
    const std::atomic<bool> notCancelled{false};
    const QString result = tools::search(
        t, {{QStringLiteral("query"), QStringLiteral("4D 5A ?? 00")}, {QStringLiteral("mode"), QStringLiteral("hex")},
            {QStringLiteral("max_results"), 3}},
        notCancelled);
    QVERIFY2(result.startsWith(QStringLiteral("3 match(es) shown; there are more")), qPrintable(result));
    QVERIFY(result.contains(QStringLiteral("0x2: 00000002  4D 5A 90 00")));
    QVERIFY(result.contains(QStringLiteral("0x8:")));

    const std::atomic<bool> cancelled{true};
    QCOMPARE(tools::search(t, {{QStringLiteral("query"), QStringLiteral("MZ")}}, cancelled), QStringLiteral("Cancelled."));
}

void TestAiTools::markFields()
{
    const ToolTarget t = target("header.bin", std::string("MZ\x90\x00\x03\x00\x00\x00", 8));
    const QJsonArray fields{
        QJsonObject{{QStringLiteral("offset"), 0}, {QStringLiteral("length"), 2}, {QStringLiteral("name"), QStringLiteral("e_magic")},
                    {QStringLiteral("value"), QStringLiteral("MZ")}, {QStringLiteral("description"), QStringLiteral("DOS signature")}},
        QJsonObject{{QStringLiteral("offset"), QStringLiteral("0x2")}, {QStringLiteral("length"), 2},
                    {QStringLiteral("name"), QStringLiteral("e_cblp")}, {QStringLiteral("value"), 144}},
        QJsonObject{{QStringLiteral("offset"), 100}, {QStringLiteral("length"), 4}, {QStringLiteral("name"), QStringLiteral("past end")}},
        QJsonObject{{QStringLiteral("offset"), 6}, {QStringLiteral("length"), 99}, {QStringLiteral("name"), QStringLiteral("clipped")}},
    };
    const QString result = tools::markFields(t, {{QStringLiteral("fields"), fields}});
    QVERIFY2(result.startsWith(QStringLiteral("Marked 3 field(s)")), qPrintable(result));
    QVERIFY(result.contains(QStringLiteral("past end")));
    QCOMPARE(m_marked.size(), 3);
    QCOMPARE(m_marked[0].name, QStringLiteral("e_magic"));
    QCOMPARE(m_marked[0].detail, QStringLiteral("MZ — DOS signature"));
    QCOMPARE(m_marked[1].offset, 2u);
    QCOMPARE(m_marked[1].detail, QStringLiteral("144"));
    QCOMPARE(m_marked[2].length, 2u); // clipped to the end of the file

    ToolTarget withoutView = t;
    withoutView.markFields = nullptr;
    QVERIFY(tools::markFields(withoutView, {{QStringLiteral("fields"), fields}}).startsWith(QStringLiteral("Error")));
    QVERIFY(tools::markFields(t, {{QStringLiteral("fields"), QJsonArray{}}}).startsWith(QStringLiteral("Error")));
}

void TestAiTools::runnerDispatchesAndCancels()
{
    const ToolTarget t = target("runner.log", "first line\nsecond line with needle\n");
    ToolRunner runner;
    QString result;
    runner.run({QStringLiteral("1"), QStringLiteral("read_lines"), QStringLiteral(R"({"start_line":2,"count":1})")}, t,
               [&](const QString& text) { result = text; });
    QCOMPARE(result, QStringLiteral("2: second line with needle\n")); // quick tools answer at once

    result.clear();
    runner.run({QStringLiteral("2"), QStringLiteral("search"), QStringLiteral(R"({"query":"needle"})")}, t,
               [&](const QString& text) { result = text; });
    QVERIFY(runner.isRunning() || !result.isEmpty());
    QTRY_VERIFY(result.contains(QStringLiteral("(line 2)")));

    runner.run({QStringLiteral("3"), QStringLiteral("no_such_tool"), QString()}, t, [&](const QString& text) { result = text; });
    QVERIFY(result.contains(QStringLiteral("no tool called")));
    runner.run({QStringLiteral("4"), QStringLiteral("read_bytes"), QStringLiteral("{not json")}, t,
               [&](const QString& text) { result = text; });
    QVERIFY(result.contains(QStringLiteral("not valid JSON")));

    // A cancelled search never reports back.
    bool called = false;
    runner.run({QStringLiteral("5"), QStringLiteral("search"), QStringLiteral(R"({"query":"line"})")}, t,
               [&](const QString&) { called = true; });
    runner.cancel();
    QTest::qWait(200);
    QVERIFY(!called);
}

QTEST_GUILESS_MAIN(TestAiTools)
#include "tst_aitools.moc"
