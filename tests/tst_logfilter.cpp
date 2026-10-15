#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/DocumentView.h"
#include "ui/FilterPanel.h"
#include "ui/FilterResultsView.h"
#include "ui/FilterRuleWriter.h"
#include "ui/LogFilter.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include "MockChatServer.h"

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace aiedit;
using ui::FilterCondition;
using ui::FilterMatch;
using ui::FilterRule;

namespace {

FilterCondition include(const QString& pattern)
{
    FilterCondition c;
    c.pattern = pattern;
    return c;
}

FilterCondition exclude(const QString& pattern)
{
    FilterCondition c = include(pattern);
    c.exclude = true;
    return c;
}

FilterCondition compare(const QString& pattern, const QString& field, const QString& op, double value)
{
    FilterCondition c = include(pattern);
    c.field = field;
    c.op = op;
    c.value = value;
    return c;
}

FilterRule rule(QList<FilterCondition> conditions, bool any = false)
{
    FilterRule r;
    r.conditions = std::move(conditions);
    r.anyInclusion = any;
    return r;
}

std::vector<std::uint64_t> lines(const std::vector<FilterMatch>& matches)
{
    std::vector<std::uint64_t> numbers;
    for (const FilterMatch& match : matches)
        numbers.push_back(match.line);
    return numbers;
}

// A streamed answer from the mock service, in pieces as a model writes it.
QList<QByteArray> answer(const QString& text)
{
    return {aiedit::test::chunk(text.left(text.size() / 2)), aiedit::test::chunk(text.mid(text.size() / 2)),
            aiedit::test::chunk({}, QStringLiteral("content"), QStringLiteral("stop")), "data: [DONE]\n\n"};
}

const char* const kLog = "2026-10-15 12:00:00 INFO  server started\n"
                         "2026-10-15 12:00:01 DEBUG heartbeat\n"
                         "2026-10-15 12:00:02 ERROR db query took 1532ms\n"
                         "2026-10-15 12:00:03 WARN  slow request took 870ms\n"
                         "2026-10-15 12:00:04 ERROR heartbeat missed\n"
                         "2026-10-15 12:00:05 error lowercase took 12ms\n"
                         "2026-10-15 12:00:06 INFO  done\n";

} // namespace

class TestLogFilter : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void includesAndExcludes();
    void anyInclusion();
    void comparesNumbers();
    void ignoresCaseUnlessAsked();
    void reportsProblems();
    void roundTripsJson();
    void readsOtherEncodings();
    void findsLinesWhateverTheirEnds();
    void testsTheStartOfLongLines();
    void followsIndexing();
    void resumesFromARow();
    void findsLogLevels();
    void highlightsRotatedLogs();

    void samplesLinesForTheAi();
    void parsesAnswers();
    void writesARuleWithTheAi();
    void retriesARuleThatFails();
    void asksForSettingsWhenNotSetUp();

    void panelFiltersAndGoesToLines();
    void panelKeepsUpWithAGrowingLog();
    void panelReportsBadRules();
    void panelCopiesLines();
    void filterActionOpensThePanel();

private:
    struct Fixture
    {
        std::shared_ptr<const core::FileSource> source;
        std::unique_ptr<ui::TextDocument> document;
        ui::SearchContext context() const { return {source, document->sharedIndex(), document->codec()}; }
    };

    QString write(const char* name, const std::string& content);
    Fixture open(const char* name, const std::string& content, std::string_view codec = "UTF-8");
    static void waitForFilter(ui::FilterPanel* panel);

    QTemporaryDir m_dir;
};

void TestLogFilter::initTestCase()
{
    // The panel remembers the last rule in the settings: keep the user's own out of it.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_logfilter"));
    // The AI settings come from the environment first; tests must not see a real key.
    qunsetenv(ui::AiSettings::kKeyEnvironmentVariable);
    qunsetenv(ui::AiSettings::kDeepSeekKeyEnvironmentVariable);
}

void TestLogFilter::init()
{
    QVERIFY(m_dir.isValid());
    QSettings().clear();
}

QString TestLogFilter::write(const char* name, const std::string& content)
{
    const QString path = m_dir.filePath(QString::fromLatin1(name));
    std::ofstream(ui::toPath(path), std::ios::binary) << content;
    return path;
}

TestLogFilter::Fixture TestLogFilter::open(const char* name, const std::string& content,
                                           std::string_view codec)
{
    std::error_code ec;
    Fixture fixture;
    fixture.source = core::FileSource::open(ui::toPath(write(name, content)), ec);
    fixture.document = std::make_unique<ui::TextDocument>(fixture.source, *core::findCodec(codec));
    return fixture;
}

void TestLogFilter::waitForFilter(ui::FilterPanel* panel)
{
    QTRY_VERIFY_WITH_TIMEOUT(!panel->isFiltering(), 20000);
    QCoreApplication::processEvents(); // the last results
}

// The engine ----------------------------------------------------------------------------------------

void TestLogFilter::includesAndExcludes()
{
    const Fixture f = open("app.log", kLog);
    const FilterRule errors = rule({include(QStringLiteral("ERROR")), exclude(QStringLiteral("heartbeat"))});
    const std::vector<FilterMatch> matches = ui::filterLines(errors, f.context());
    QCOMPARE(lines(matches), (std::vector<std::uint64_t>{2, 5}));
    // Where each line starts, to show and go to it.
    QCOMPARE(matches[0].start, std::string_view(kLog).find("2026-10-15 12:00:02"));

    // Exclusions alone keep everything else.
    QCOMPARE(lines(ui::filterLines(rule({exclude(QStringLiteral("INFO|DEBUG"))}), f.context())),
             (std::vector<std::uint64_t>{2, 3, 4, 5}));
}

void TestLogFilter::anyInclusion()
{
    const Fixture f = open("any.log", kLog);
    const QList<FilterCondition> conditions{include(QStringLiteral("WARN")), include(QStringLiteral("missed"))};
    QCOMPARE(lines(ui::filterLines(rule(conditions), f.context())), std::vector<std::uint64_t>{});
    QCOMPARE(lines(ui::filterLines(rule(conditions, true), f.context())), (std::vector<std::uint64_t>{3, 4}));
}

void TestLogFilter::comparesNumbers()
{
    const Fixture f = open("numbers.log", kLog);
    const QString took = QStringLiteral("took (?<ms>\\d+)ms");
    const FilterRule slower = rule({compare(took, QStringLiteral("ms"), QStringLiteral(">"), 1000)});
    QCOMPARE(lines(ui::filterLines(slower, f.context())), (std::vector<std::uint64_t>{2}));
    const FilterRule faster = rule({compare(took, QStringLiteral("ms"), QStringLiteral("<="), 870)});
    QCOMPARE(lines(ui::filterLines(faster, f.context())), (std::vector<std::uint64_t>{3, 5}));
    // A group by number, and the whole match as group 0.
    QCOMPARE(lines(ui::filterLines(rule({compare(QStringLiteral("took (\\d+)"), QStringLiteral("1"),
                                                 QStringLiteral("=="), 12)}),
                                   f.context())),
             (std::vector<std::uint64_t>{5}));
    QCOMPARE(lines(ui::filterLines(rule({compare(QStringLiteral("\\d+(?=ms)"), QStringLiteral("0"),
                                                 QStringLiteral("!="), 870)}),
                                   f.context())),
             (std::vector<std::uint64_t>{2, 5}));
    // Excluding by a number: everything but the slow ones.
    FilterCondition slow = compare(took, QStringLiteral("ms"), QStringLiteral(">="), 870);
    slow.exclude = true;
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("took")), slow}), f.context())),
             (std::vector<std::uint64_t>{5}));
}

void TestLogFilter::ignoresCaseUnlessAsked()
{
    const Fixture f = open("case.log", kLog);
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("error"))}), f.context())),
             (std::vector<std::uint64_t>{2, 4, 5}));
    FilterCondition exact = include(QStringLiteral("error"));
    exact.caseSensitive = true;
    QCOMPARE(lines(ui::filterLines(rule({exact}), f.context())), (std::vector<std::uint64_t>{5}));
}

void TestLogFilter::reportsProblems()
{
    QVERIFY(!rule({}).check().isEmpty());
    QVERIFY(rule({include(QStringLiteral("a"))}).check().isEmpty());
    QVERIFY(rule({include(QStringLiteral("a")), include({})}).check().startsWith(QStringLiteral("Condition 2:")));
    QVERIFY(rule({include(QStringLiteral("(unclosed"))})
                .check()
                .contains(QStringLiteral("invalid regular expression")));
    QVERIFY(rule({compare(QStringLiteral("(?<ms>\\d+)"), QStringLiteral("sec"), QStringLiteral(">"), 1)})
                .check()
                .contains(QStringLiteral("no group named sec")));
    QVERIFY(rule({compare(QStringLiteral("(\\d+)"), QStringLiteral("2"), QStringLiteral(">"), 1)}).check().contains(
        QStringLiteral("no group named 2")));
    QVERIFY(rule({compare(QStringLiteral("(?<ms>\\d+)"), QStringLiteral("ms"), QStringLiteral("~"), 1)})
                .check()
                .contains(QStringLiteral("unknown comparison")));

    // A rule that cannot run finds nothing rather than everything.
    const Fixture f = open("bad.log", kLog);
    QVERIFY(ui::filterLines(rule({include(QStringLiteral("("))}), f.context()).empty());
}

void TestLogFilter::roundTripsJson()
{
    FilterRule r = rule({include(QStringLiteral("ERROR|FATAL")), exclude(QStringLiteral("heartbeat")),
                         compare(QStringLiteral("took (?<ms>\\d+)ms"), QStringLiteral("ms"), QStringLiteral(">"), 1000)},
                        true);
    r.conditions[1].caseSensitive = true;
    r.description = QStringLiteral("慢错误");
    QString error;
    QCOMPARE(FilterRule::fromJson(r.toJson(), &error), std::optional(r));
    QVERIFY(error.isEmpty());

    // As a model may write it: a number as text, fields left out.
    const QJsonObject loose = QJsonDocument::fromJson(R"json({"conditions": [
        {"type": "include", "regex": "took (?<ms>\\d+)ms", "field": "ms", "op": ">=", "value": "250"}]})json")
                                  .object();
    const std::optional<FilterRule> parsed = FilterRule::fromJson(loose);
    QVERIFY(parsed);
    QCOMPARE(parsed->conditions.size(), 1);
    QCOMPARE(parsed->conditions[0].value, 250.0);
    QVERIFY(!parsed->anyInclusion);

    QVERIFY(!FilterRule::fromJson(QJsonObject{}, &error));
    QVERIFY(!error.isEmpty());
}

void TestLogFilter::readsOtherEncodings()
{
    // 错误 (error) and 正常 (normal) in GBK, then in UTF-16LE with a byte order mark.
    const std::string gbk = "\xD5\xFD\xB3\xA3 a\n\xB4\xED\xCE\xF3 b\n\xD5\xFD\xB3\xA3 c\n";
    const Fixture legacy = open("gbk.log", gbk, "GB18030");
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("错误"))}), legacy.context())),
             (std::vector<std::uint64_t>{1}));

    const QString text = QStringLiteral("正常 a\r\n错误 b\r\n错误 c\r\n");
    std::string utf16 = "\xFF\xFE";
    for (const QChar c : text)
        utf16 += {static_cast<char>(c.unicode() & 0xFF), static_cast<char>(c.unicode() >> 8)};
    const Fixture wide = open("utf16.log", utf16, "UTF-16LE");
    // The CR of CRLF is not part of the line: $ works.
    const std::vector<FilterMatch> matches = ui::filterLines(rule({include(QStringLiteral("错误 \\w$"))}), wide.context());
    QCOMPARE(lines(matches), (std::vector<std::uint64_t>{1, 2}));
    QCOMPARE(matches[0].start, std::uint64_t{2 + 2 * 6}); // after the BOM and "正常 a\r\n"
}

void TestLogFilter::findsLinesWhateverTheirEnds()
{
    // Classic Mac OS line breaks.
    const Fixture cr = open("cr.log", "first ERROR\rsecond\rthird ERROR");
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("ERROR$"))}), cr.context())),
             (std::vector<std::uint64_t>{0, 2}));

    // Bytes that are not UTF-8 do not move the lines; a CR before LF is not part of the line,
    // one at the very end with no LF is.
    const Fixture mixed = open("mixed.log", "ok \xFF ERROR\r\nbad \xC3\n\nERROR end\nlast ERROR\r");
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("ERROR"))}), mixed.context())),
             (std::vector<std::uint64_t>{0, 3, 4}));
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("^bad .$"))}), mixed.context())),
             (std::vector<std::uint64_t>{1}));
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("^$"))}), mixed.context())),
             (std::vector<std::uint64_t>{2}));
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("ERROR$"))}), mixed.context())),
             (std::vector<std::uint64_t>{0}));
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("ERROR\\r$"))}), mixed.context())),
             (std::vector<std::uint64_t>{4}));
}

void TestLogFilter::testsTheStartOfLongLines()
{
    // A 3 MiB line: what is in its first MiB counts, what comes later does not, and the lines
    // after it are read as usual.
    std::string content = "short ERROR\n";
    content += "NEEDLE-AT-START " + std::string(3 * 1024 * 1024, 'x') + " NEEDLE-LATE\n";
    content += "after ERROR NEEDLE-AT-START NEEDLE-LATE\n";
    const Fixture f = open("long.log", content);
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("NEEDLE-AT-START"))}), f.context())),
             (std::vector<std::uint64_t>{1, 2}));
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("NEEDLE-LATE"))}), f.context())),
             (std::vector<std::uint64_t>{2}));
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("ERROR"))}), f.context())),
             (std::vector<std::uint64_t>{0, 2}));
}

void TestLogFilter::followsIndexing()
{
    std::string content;
    while (content.size() < 12u * 1024 * 1024)
        content += "ordinary log line without anything special\n";
    content += "the needle 42\n";
    const Fixture f = open("follow.log", content);
    QVERIFY(f.document->isIndexing());
    const std::vector<FilterMatch> matches = ui::filterLines(rule({include(QStringLiteral("needle \\d+"))}), f.context());
    QCOMPARE(matches.size(), 1u);
    QCOMPARE(matches[0].start, content.size() - 14);

    // Cancelled at once, with progress until then.
    ui::FilterHooks hooks;
    hooks.cancelled = [] { return true; };
    bool found = false;
    hooks.found = [&found](std::vector<FilterMatch>&&) { found = true; };
    ui::filterLines(rule({include(QStringLiteral("o"))}), f.context(), 0, hooks);
    QVERIFY(!found);
}

void TestLogFilter::resumesFromARow()
{
    const Fixture f = open("resume.log", kLog);
    // Row 3 is line 3: only the lines from there on.
    QCOMPARE(lines(ui::filterLines(rule({include(QStringLiteral("ERROR"))}), f.context(), 3)),
             (std::vector<std::uint64_t>{4, 5}));
}

void TestLogFilter::findsLogLevels()
{
    const auto level = [](const char* line) { return ui::findLogLevel(QString::fromUtf8(line)).level; };
    QCOMPARE(level("2026-10-15 ERROR something"), ui::LogLevel::Error);
    QCOMPARE(level("[warn] disk almost full"), ui::LogLevel::Warning);
    QCOMPARE(level("WARNING: x"), ui::LogLevel::Warning);
    QCOMPARE(level("I/INFO user reported an error"), ui::LogLevel::Info); // the first level word
    QCOMPARE(level("level=debug msg=x"), ui::LogLevel::Debug);
    QCOMPARE(level("FATAL boom"), ui::LogLevel::Fatal);
    QCOMPARE(level("Information is not a level"), ui::LogLevel::None);
    QCOMPARE(level("plain line"), ui::LogLevel::None);
    const ui::LogLevelSpan span = ui::findLogLevel(QStringLiteral("12:00 [Error] x"));
    QCOMPARE(span.start, qsizetype(7));
    QCOMPARE(span.length, qsizetype(5));
}

void TestLogFilter::highlightsRotatedLogs()
{
    QCOMPARE(ui::syntaxForFileName(QStringLiteral("app.log.1")), ui::syntaxForFileName(QStringLiteral("app.log")));
    QCOMPARE(ui::syntaxForFileName(QStringLiteral("app.log.2.3")), ui::syntaxForFileName(QStringLiteral("app.log")));
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    QVERIFY(ui::syntaxForFileName(QStringLiteral("app.log")).contains(QStringLiteral("Log")));
#endif
}

// The AI ------------------------------------------------------------------------------------------

void TestLogFilter::samplesLinesForTheAi()
{
    std::string content;
    for (int i = 0; i < 1000; ++i)
        content += "line " + std::to_string(i) + (i % 3 == 0 ? " " + std::string(300, 'x') : "") + "\n";
    content += "\n\n"; // empty lines are left out
    const Fixture f = open("sample.log", content);
    const QStringList samples = ui::FilterRuleWriter::sampleLines(f.context(), 10);
    QCOMPARE(samples.size(), 10);
    QCOMPARE(samples[0], QStringLiteral("line 0 ") + QString(193, u'x')); // cut to 200 characters
    QCOMPARE(samples[4], QStringLiteral("line 4"));                         // the first half from the start
    QVERIFY(samples[5].startsWith(QStringLiteral("line 1"))); // then spread over the rest
    QVERIFY(samples[9].startsWith(QStringLiteral("line 9")));
    for (const QString& sample : samples)
        QVERIFY(sample.size() <= ui::FilterRuleWriter::kSampleLineLength);

    const Fixture small = open("small.log", "a\nb\n");
    QCOMPARE(ui::FilterRuleWriter::sampleLines(small.context()), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
}

void TestLogFilter::parsesAnswers()
{
    QString error;
    const std::optional<FilterRule> fenced = ui::FilterRuleWriter::parseAnswer(
        QStringLiteral("Here is the rule:\n```json\n{\"description\": \"errors\", \"conditions\": [{\"type\": "
                       "\"include\", \"regex\": \"ERROR\"}]}\n```"),
        &error);
    QVERIFY2(fenced, qPrintable(error));
    QCOMPARE(fenced->description, QStringLiteral("errors"));
    QCOMPARE(fenced->conditions.size(), 1);

    QVERIFY(!ui::FilterRuleWriter::parseAnswer(QStringLiteral("I cannot do that."), &error));
    QVERIFY(error.contains(QStringLiteral("no rule")));
    QVERIFY(!ui::FilterRuleWriter::parseAnswer(QStringLiteral("{\"conditions\": [}"), &error));
    QVERIFY(error.contains(QStringLiteral("JSON")));
    // (Raw strings with unbalanced parentheses stay out of macros, which moc would misread.)
    const QString unclosed = QString::fromUtf8(R"json({"conditions": [{"type": "include", "regex": "(oops"}]})json");
    QVERIFY(!ui::FilterRuleWriter::parseAnswer(unclosed, &error));
    QVERIFY(error.contains(QStringLiteral("invalid regular expression")));
}

void TestLogFilter::writesARuleWithTheAi()
{
    std::string content = kLog;
    for (int i = 0; i < 500; ++i)
        content += "2026-10-15 12:01:00 INFO  filler " + std::to_string(i) + "\n";
    content += "2026-10-15 12:59:59 INFO  SECRET-NOT-SAMPLED\n";
    const Fixture f = open("ai.log", content);

    aiedit::test::MockServer server;
    const QString reply = QString::fromUtf8(R"json(```json
{"description": "slow errors", "match": "all", "conditions": [
  {"type": "include", "regex": "ERROR"},
  {"type": "include", "regex": "took (?<ms>\\d+)ms", "field": "ms", "op": ">", "value": 1000}]}
```)json");
    server.chunks = answer(reply);
    ui::FilterRuleWriter writer;
    writer.setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    QSignalSpy written(&writer, &ui::FilterRuleWriter::ruleWritten);
    QSignalSpy failed(&writer, &ui::FilterRuleWriter::failed);
    writer.write(QStringLiteral("errors slower than a second"), ui::FilterRuleWriter::sampleLines(f.context(), 20));
    QVERIFY(writer.isBusy());
    QTRY_COMPARE(written.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(server.requests, 1);

    const FilterRule r = written.first().first().value<FilterRule>();
    QCOMPARE(r.description, QStringLiteral("slow errors"));
    QCOMPARE(lines(ui::filterLines(r, f.context())), (std::vector<std::uint64_t>{2}));

    // What went out: the description and the sample, not the rest of the log.
    const QJsonObject body = QJsonDocument::fromJson(server.body).object();
    const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 2);
    const QString user = messages[1].toObject().value(QStringLiteral("content")).toString();
    QVERIFY(user.contains(QStringLiteral("errors slower than a second")));
    QVERIFY(user.contains(QStringLiteral("db query took 1532ms")));
    QVERIFY(!server.body.contains("SECRET-NOT-SAMPLED"));
    QCOMPARE(writer.previewRequest(QStringLiteral("errors slower than a second"),
                                   ui::FilterRuleWriter::sampleLines(f.context(), 20)),
             server.body);
}

void TestLogFilter::retriesARuleThatFails()
{
    aiedit::test::MockServer server;
    const QString broken = QString::fromUtf8(R"json({"conditions": [{"type": "include", "regex": "took (\\d+"}]})json");
    const QString fixed = QString::fromUtf8(R"json({"conditions": [{"type": "include", "regex": "took (\\d+)"}]})json");
    server.queue = {answer(broken), answer(fixed)};
    ui::FilterRuleWriter writer;
    writer.setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    QSignalSpy written(&writer, &ui::FilterRuleWriter::ruleWritten);
    writer.write(QStringLiteral("timings"), {QStringLiteral("took 5ms")});
    QTRY_COMPARE(written.count(), 1);
    QCOMPARE(server.requests, 2);
    // The second request carries the first answer and what was wrong with it.
    const QJsonArray messages =
        QJsonDocument::fromJson(server.bodies[1]).object().value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 4);
    QVERIFY(messages[3].toObject().value(QStringLiteral("content")).toString().contains(
        QStringLiteral("invalid regular expression")));

    // Twice wrong: it gives up with the reason.
    server.queue = {answer(QStringLiteral("no idea")), answer(QStringLiteral("still no idea"))};
    QSignalSpy failed(&writer, &ui::FilterRuleWriter::failed);
    writer.write(QStringLiteral("timings"), {QStringLiteral("took 5ms")});
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(server.requests, 4);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("no rule")));
}

void TestLogFilter::asksForSettingsWhenNotSetUp()
{
    ui::FilterRuleWriter writer;
    writer.setSettingsProvider([] { return ui::AiSettings(); }); // no key
    QSignalSpy settings(&writer, &ui::FilterRuleWriter::settingsRequested);
    QSignalSpy failed(&writer, &ui::FilterRuleWriter::failed);
    writer.write(QStringLiteral("errors"), {QStringLiteral("x")});
    QCOMPARE(settings.count(), 1);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!writer.isBusy());
}

// The panel ---------------------------------------------------------------------------------------

void TestLogFilter::panelFiltersAndGoesToLines()
{
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(ui::toPath(write("panel.log", kLog)), ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(900, 600);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));

    doc.showFilterPanel();
    ui::FilterPanel* panel = doc.filterPanel();
    QVERIFY(panel && panel->isVisible());
    QCOMPARE(panel->conditionCount(), 1);
    QCOMPARE(doc.focusWidget(), panel->descriptionEdit()); // no rule yet: the AI field first

    panel->setRule(rule({include(QStringLiteral("ERROR|WARN")), exclude(QStringLiteral("heartbeat"))}));
    waitForFilter(panel);
    QCOMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{2, 3, 5}));
    QVERIFY(panel->statusText().contains(QStringLiteral("3 of 7 lines")));
    ui::FilterResultsView* results = panel->resultsView();
    QCOMPARE(results->displayText(0), QStringLiteral("2026-10-15 12:00:02 ERROR db query took 1532ms"));

    // Moving through the results shows each line in the document; Enter goes there.
    results->setFocus();
    QTest::keyClick(results, Qt::Key_Down);
    QCOMPARE(results->currentResult(), 0);
    QCOMPARE(doc.textView()->cursorLine(), 2u);
    QTest::keyClick(results, Qt::Key_Down);
    QCOMPARE(doc.textView()->cursorLine(), 3u);
    QCOMPARE(doc.focusWidget(), results);
    QTest::keyClick(results, Qt::Key_Return);
    QCOMPARE(doc.focusWidget(), doc.textView());

    // The rule is remembered for the next panel, anywhere.
    const QJsonObject saved =
        QJsonDocument::fromJson(QSettings().value(QStringLiteral("filter/lastRule")).toByteArray()).object();
    QCOMPARE(FilterRule::fromJson(saved)->conditions, panel->rule().conditions);

    // Escape closes the panel; the filter stays for when it opens again.
    panel->descriptionEdit()->setFocus();
    QTest::keyClick(panel->descriptionEdit(), Qt::Key_Escape);
    QVERIFY(!panel->isVisible());
    doc.showFilterPanel();
    QCOMPARE(panel->matches().size(), 3u);
}

void TestLogFilter::panelKeepsUpWithAGrowingLog()
{
    const QString path = write("growing.log", "a ERROR 1\nb\nc ERROR 2 (partial");
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(ui::toPath(path), ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(900, 600);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));
    doc.showFilterPanel();
    ui::FilterPanel* panel = doc.filterPanel();
    panel->setRule(rule({include(QStringLiteral("ERROR \\d+$"))}));
    waitForFilter(panel);
    QCOMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{0}));

    // The last line goes on and new ones come: what was found stays, and only the rest is read.
    // (The file watcher may reload once more on its own; the results come out the same.)
    std::ofstream(ui::toPath(path), std::ios::binary | std::ios::app) << ")\nd ERROR 3\ne\n";
    QVERIFY(doc.reload());
    QCOMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{0}));
    QTRY_COMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{0, 3}));

    // Rewritten: everything again.
    std::ofstream(ui::toPath(path), std::ios::binary | std::ios::trunc) << "x ERROR 9\n";
    QVERIFY(doc.reload());
    QVERIFY(panel->matches().empty());
    QTRY_COMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{0}));
    QCOMPARE(panel->resultsView()->displayText(0), QStringLiteral("x ERROR 9"));

    // An edit moves lines: the results follow once the typing pauses. A reload the watcher
    // still had pending must not throw the edit away.
    doc.textView()->setCursorOffset(0);
    doc.textView()->insertText(QStringLiteral("new ERROR 1\n"));
    QVERIFY(panel->matches().empty());
    QTRY_COMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{0, 1}));
    QTest::qWait(500);
    QVERIFY(doc.isModified());
}

void TestLogFilter::panelReportsBadRules()
{
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(ui::toPath(write("bad-panel.log", kLog)), ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(900, 600);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));
    doc.showFilterPanel();
    ui::FilterPanel* panel = doc.filterPanel();
    panel->setRule(rule({include(QStringLiteral("ERROR"))}));
    waitForFilter(panel);
    QCOMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{2, 4, 5}));

    // Typing an unfinished pattern shows what is wrong and keeps the last results.
    QLineEdit* pattern = panel->patternEdit(0);
    QCOMPARE(pattern->text(), QStringLiteral("ERROR"));
    pattern->setFocus();
    pattern->end(false);
    QTest::keyClicks(pattern, QStringLiteral("|(started"));
    QTRY_VERIFY(panel->statusText().contains(QStringLiteral("invalid regular expression")));
    QCOMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{2, 4, 5}));
    QTest::keyClick(pattern, Qt::Key_ParenRight);
    QTest::keyClick(pattern, Qt::Key_Return);
    waitForFilter(panel);
    QCOMPARE(lines(panel->matches()), (std::vector<std::uint64_t>{0, 2, 4, 5}));

    // A number test that does not parse.
    QLineEdit* comparison = panel->comparisonEdit(0);
    comparison->setFocus();
    QTest::keyClicks(comparison, QStringLiteral("ms >"));
    QTest::keyClick(comparison, Qt::Key_Return);
    QVERIFY(panel->statusText().startsWith(QStringLiteral("Condition 1: write the number test")));
}

void TestLogFilter::panelCopiesLines()
{
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(ui::toPath(write("copy.log", kLog)), ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(900, 600);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));
    doc.showFilterPanel();
    ui::FilterPanel* panel = doc.filterPanel();
    panel->setRule(rule({include(QStringLiteral("took"))}));
    waitForFilter(panel);
    ui::FilterResultsView* results = panel->resultsView();
    results->setFocus();
    results->setCurrentResult(0);
    QTest::keyClick(results, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(results, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QApplication::clipboard()->text(),
             QStringLiteral("2026-10-15 12:00:02 ERROR db query took 1532ms\n"
                            "2026-10-15 12:00:03 WARN  slow request took 870ms\n"));
    QTest::keyClick(results, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(results->selection(), std::optional(std::pair(0, 2)));
}

void TestLogFilter::filterActionOpensThePanel()
{
    ui::MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(write("menu.log", kLog)));
    QAction* filter = nullptr;
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == QStringLiteral("Fi&lter Lines…"))
            filter = action;
    QVERIFY(filter && filter->isEnabled());
    QCOMPARE(filter->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    filter->trigger();
    QVERIFY(window.currentDocument()->isFilterPanelVisible());
}

QTEST_MAIN(TestLogFilter)
#include "tst_logfilter.moc"
