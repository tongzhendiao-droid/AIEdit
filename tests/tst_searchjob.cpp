#include "core/FileSource.h"
#include "ui/DocumentView.h"
#include "ui/FindBar.h"
#include "ui/HexView.h"
#include "ui/Paths.h"
#include "ui/SearchJob.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace cppedit;
using ui::SearchMode;
using ui::SearchOutcome;
using ui::SearchRequest;

class TestSearchJob : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void exactText();
    void caseInsensitiveAndRegex();
    void invalidQueries();
    void legacyEncodingSkipsTrailBytes();
    void legacyEncodingRegex();
    void utf16Alignment();
    void hexWildcards();
    void backwardAndWrap();
    void followsIndexing();
    void cancellation();
    void findBarFlow();

private:
    struct Fixture
    {
        std::shared_ptr<const core::FileSource> source;
        std::unique_ptr<ui::TextDocument> document;
        ui::SearchContext context() const
        {
            return {source, document->sharedIndex(), document->codec()};
        }
    };

    Fixture open(const char* name, const std::string& content, std::string_view codec = "UTF-8");
    static SearchRequest request(SearchMode mode, const QString& query, std::uint64_t from = 0, bool forward = true,
                                 bool caseSensitive = true);

    QTemporaryDir m_dir;
};

void TestSearchJob::init()
{
    QVERIFY(m_dir.isValid());
}

TestSearchJob::Fixture TestSearchJob::open(const char* name, const std::string& content, std::string_view codec)
{
    const auto path = ui::toPath(m_dir.filePath(QString::fromLatin1(name)));
    std::ofstream(path, std::ios::binary) << content;
    std::error_code ec;
    Fixture fixture;
    fixture.source = core::FileSource::open(path, ec);
    fixture.document = std::make_unique<ui::TextDocument>(fixture.source, *core::findCodec(codec));
    return fixture;
}

SearchRequest TestSearchJob::request(SearchMode mode, const QString& query, std::uint64_t from, bool forward,
                                     bool caseSensitive)
{
    SearchRequest r;
    r.mode = mode;
    r.query = query;
    r.from = from;
    r.forward = forward;
    r.caseSensitive = caseSensitive;
    return r;
}

void TestSearchJob::exactText()
{
    const Fixture f = open("exact.txt", "hello world, hello 世界");
    SearchOutcome out = ui::search(request(SearchMode::Text, QStringLiteral("hello"), 1), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, 13u);
    QCOMPARE(out.end, 18u);
    QVERIFY(!out.wrapped);

    out = ui::search(request(SearchMode::Text, QStringLiteral("世界")), f.context());
    QCOMPARE(out.start, 19u);
    QCOMPARE(out.end, 25u);
}

void TestSearchJob::caseInsensitiveAndRegex()
{
    const Fixture f = open("regex.txt", "Alpha beta\nGAMMA 123 delta 4567\n");
    SearchOutcome out =
        ui::search(request(SearchMode::Text, QStringLiteral("gamma"), 0, true, false), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, 11u);
    QCOMPARE(out.end, 16u);

    out = ui::search(request(SearchMode::Regex, QStringLiteral("\\d{4}")), f.context());
    QCOMPARE(out.start, 27u);
    QCOMPARE(out.end, 31u);

    out = ui::search(request(SearchMode::Regex, QStringLiteral("^gamma"), 0, true, false), f.context());
    QCOMPARE(out.start, 11u); // ^ matches at line starts

    // Empty matches are skipped instead of selecting nothing.
    out = ui::search(request(SearchMode::Regex, QStringLiteral("x*")), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::NotFound);
}

void TestSearchJob::invalidQueries()
{
    const Fixture f = open("invalid.txt", "text");
    QCOMPARE(ui::search(request(SearchMode::Regex, QStringLiteral("(")), f.context()).status,
             SearchOutcome::Status::InvalidQuery);
    QCOMPARE(ui::search(request(SearchMode::Hex, QStringLiteral("4G")), f.context()).status,
             SearchOutcome::Status::InvalidQuery);
    QCOMPARE(ui::search(request(SearchMode::Text, QString()), f.context()).status,
             SearchOutcome::Status::InvalidQuery);

    const Fixture latin = open("latin.txt", "text", "ISO-8859-1");
    const SearchOutcome out = ui::search(request(SearchMode::Text, QStringLiteral("中")), latin.context());
    QCOMPARE(out.status, SearchOutcome::Status::InvalidQuery); // not representable
}

void TestSearchJob::legacyEncodingSkipsTrailBytes()
{
    // 0x81 0x41 is one GBK character whose trail byte is 'A'; only the standalone A counts.
    const Fixture f = open("gbk.txt", "\x81\x41" "xA", "GB18030");
    const SearchOutcome out = ui::search(request(SearchMode::Text, QStringLiteral("A")), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, 3u);

    const Fixture world = open("gbk2.txt", "\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7", "GB18030");
    const SearchOutcome exact = ui::search(request(SearchMode::Text, QStringLiteral("世界")), world.context());
    QCOMPARE(exact.start, 6u);
    QCOMPARE(exact.end, 10u);
}

void TestSearchJob::legacyEncodingRegex()
{
    const Fixture f = open("gbk3.txt", "abc\n\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7\n", "GB18030");
    const SearchOutcome out = ui::search(request(SearchMode::Regex, QStringLiteral("世.")), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, 10u);
    QCOMPARE(out.end, 14u);
}

void TestSearchJob::utf16Alignment()
{
    // BOM, U+6200 (bytes 00 62), 'A', then 'b': "62 00" appears misaligned at offset 3 first.
    const Fixture f = open("utf16.txt", std::string("\xFF\xFE\x00\x62\x41\x00\x62\x00", 8), "UTF-16LE");
    const SearchOutcome out = ui::search(request(SearchMode::Text, QStringLiteral("b")), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, 6u);
    QCOMPARE(out.end, 8u);
}

void TestSearchJob::hexWildcards()
{
    const Fixture f = open("bin.dat", std::string("\x00MZ\x90\x00\x03MZ\x91\x00\x03", 11), "ISO-8859-1");
    const SearchOutcome out = ui::search(request(SearchMode::Hex, QStringLiteral("4D 5A ?? 00"), 2), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, 6u);
    QCOMPARE(out.end, 10u);
}

void TestSearchJob::backwardAndWrap()
{
    const Fixture f = open("wrap.txt", "one two one two one");
    // "two" is at 4 and 12; backward from 12 excludes the match starting there.
    SearchOutcome out = ui::search(request(SearchMode::Text, QStringLiteral("two"), 12, false), f.context());
    QCOMPARE(out.start, 4u);
    QVERIFY(!out.wrapped);

    out = ui::search(request(SearchMode::Text, QStringLiteral("two"), 13), f.context());
    QCOMPARE(out.start, 4u);
    QVERIFY(out.wrapped);

    out = ui::search(request(SearchMode::Regex, QStringLiteral("t\\w+"), 4, false), f.context());
    QCOMPARE(out.start, 12u); // nothing before 4: wraps to the last match
    QVERIFY(out.wrapped);

    SearchRequest noWrap = request(SearchMode::Text, QStringLiteral("two"), 13);
    noWrap.wrap = false;
    QCOMPARE(ui::search(noWrap, f.context()).status, SearchOutcome::Status::NotFound);
}

void TestSearchJob::followsIndexing()
{
    // The regex search waits for the background indexer to reach the end.
    std::string content;
    while (content.size() < 12u * 1024 * 1024)
        content += "ordinary log line without anything special\n";
    content += "the needle 42\n";
    const Fixture f = open("follow.log", content);
    QVERIFY(f.document->isIndexing());
    const SearchOutcome out = ui::search(request(SearchMode::Regex, QStringLiteral("needle \\d+")), f.context());
    QCOMPARE(out.status, SearchOutcome::Status::Found);
    QCOMPARE(out.start, content.size() - 10);
}

void TestSearchJob::cancellation()
{
    const Fixture f = open("cancel.txt", std::string(1024 * 1024, 'x'));
    ui::SearchHooks hooks;
    hooks.cancelled = [] { return true; };
    QCOMPARE(ui::search(request(SearchMode::Regex, QStringLiteral("y")), f.context(), hooks).status,
             SearchOutcome::Status::Cancelled);
}

void TestSearchJob::findBarFlow()
{
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("flow.txt")));
    std::ofstream(path, std::ios::binary) << "foo bar\nbar baz\nbar\n";
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source = core::FileSource::open(path, ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(800, 400);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));

    doc.showFindBar();
    QVERIFY(doc.findBar()->isVisible());
    doc.findBar()->setQuery(QStringLiteral("bar"));
    doc.findBar()->setCaseSensitive(true);
    QSignalSpy found(doc.findBar(), &ui::FindBar::matchFound);

    doc.findNext();
    QVERIFY(found.wait());
    QCOMPARE(doc.textView()->selectionStart(), 4u);
    QCOMPARE(doc.textView()->selectionEnd(), 7u);
    QCOMPARE(doc.hexView()->selectionStart(), 4u); // the hex view follows

    doc.findNext();
    QVERIFY(found.wait());
    QCOMPARE(doc.textView()->selectionStart(), 8u);
    doc.findNext();
    QVERIFY(found.wait());
    QCOMPARE(doc.textView()->selectionStart(), 16u);
    doc.findNext(); // wraps to the first one
    QVERIFY(found.wait());
    QCOMPARE(doc.textView()->selectionStart(), 4u);
    QCOMPARE(found.last().at(2).toBool(), true);
    doc.findNext(false); // and back across the start
    QVERIFY(found.wait());
    QCOMPARE(doc.textView()->selectionStart(), 16u);

    doc.findBar()->setQuery(QStringLiteral("nothing"));
    doc.findNext();
    QTRY_COMPARE(doc.findBar()->statusText(), QStringLiteral("Not found"));
}

QTEST_MAIN(TestSearchJob)
#include "tst_searchjob.moc"
