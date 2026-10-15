#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/DocumentView.h"
#include "ui/FindBar.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/Replace.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace aiedit;
using ui::ReplaceRequest;
using ui::SearchMode;

namespace {

const core::TextCodec& codec(std::string_view name)
{
    return *core::findCodec(name);
}

std::string contents(const ui::TextDocument& document)
{
    const auto bytes = document.bytes();
    return {bytes.begin(), bytes.end()};
}

ReplaceRequest request(SearchMode mode, const QString& query, const QString& replacement, bool caseSensitive = true)
{
    ReplaceRequest r;
    r.mode = mode;
    r.query = query;
    r.replacement = replacement;
    r.caseSensitive = caseSensitive;
    return r;
}

} // namespace

class TestReplace : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void replacesAllAsOneStep();
    void ignoresCaseWhenAsked();
    void expandsGroupsAndEscapes();
    void emptyMatchesInsertAtLineStartsAndEnds();
    void keepsTheBytesBetweenMatches();
    void writesInTheFilesEncoding();
    void reportsWhatCannotBeReplaced();
    void replacementForOneMatch();

    void replaceThenFindNext();
    void replaceAllInTheDocumentView();
    void unavailableWhereNothingCanBeEdited();
    void replaceActionOpensTheReplaceRow();

private:
    std::shared_ptr<const core::FileSource> write(const char* name, const std::string& content);
    // Replace All applied to the document, as the document view does it.
    std::size_t replaceAll(ui::TextDocument& document, const ReplaceRequest& request);

    QTemporaryDir m_dir;
};

void TestReplace::init()
{
    QVERIFY(m_dir.isValid());
}

std::shared_ptr<const core::FileSource> TestReplace::write(const char* name, const std::string& content)
{
    const auto path = ui::toPath(m_dir.filePath(QString::fromLatin1(name)));
    std::ofstream(path, std::ios::binary) << content;
    std::error_code ec;
    return core::FileSource::open(path, ec);
}

std::size_t TestReplace::replaceAll(ui::TextDocument& document, const ReplaceRequest& r)
{
    const ui::ReplaceAllResult result =
        ui::replaceAll(r, {document.source(), document.sharedIndex(), document.codec()});
    if (!result.error.isEmpty() || result.count == 0)
        return 0;
    document.replaceBytes(result.start, result.end, result.bytes);
    return result.count;
}

void TestReplace::replacesAllAsOneStep()
{
    const std::string original = "foo bar foo\nfoo\n";
    ui::TextDocument document(write("all.txt", original), codec("UTF-8"));
    QCOMPARE(replaceAll(document, request(SearchMode::Text, QStringLiteral("foo"), QStringLiteral("baz"))), 3u);
    QCOMPARE(contents(document), std::string("baz bar baz\nbaz\n"));
    document.undo();
    QCOMPARE(contents(document), original); // one step
    QVERIFY(!document.canUndo());
}

void TestReplace::ignoresCaseWhenAsked()
{
    ui::TextDocument document(write("case.txt", "Foo FOO foo fo\n"), codec("UTF-8"));
    QCOMPARE(replaceAll(document, request(SearchMode::Text, QStringLiteral("foo"), QStringLiteral("x"), false)), 3u);
    QCOMPARE(contents(document), std::string("x x x fo\n"));
}

void TestReplace::expandsGroupsAndEscapes()
{
    ui::TextDocument dates(write("dates.txt", "on 2026-10-15 and 2027-01-02.\n"), codec("UTF-8"));
    QCOMPARE(replaceAll(dates, request(SearchMode::Regex, QStringLiteral("(\\d+)-(\\d+)-(\\d+)"),
                                       QStringLiteral("$3/$2/\\1"))),
             2u);
    QCOMPARE(contents(dates), std::string("on 15/10/2026 and 02/01/2027.\n"));

    // \n becomes the file's own line break (CRLF here), \t a tab, $$ and \\ themselves.
    ui::TextDocument crlf(write("list.txt", "a,b\r\n"), codec("UTF-8"));
    ReplaceRequest split = request(SearchMode::Regex, QStringLiteral(","), QStringLiteral("\\n\\t$$\\\\"));
    split.lineTerminator = crlf.lineTerminator();
    QCOMPARE(replaceAll(crlf, split), 1u);
    QCOMPARE(contents(crlf), std::string("a\r\n\t$\\b\r\n"));

    // In text mode the replacement is taken as typed.
    ui::TextDocument literal(write("literal.txt", "x\n"), codec("UTF-8"));
    QCOMPARE(replaceAll(literal, request(SearchMode::Text, QStringLiteral("x"), QStringLiteral("$1\\n"))), 1u);
    QCOMPARE(contents(literal), std::string("$1\\n\n"));
}

void TestReplace::emptyMatchesInsertAtLineStartsAndEnds()
{
    ui::TextDocument starts(write("starts.txt", "a\nb\n"), codec("UTF-8"));
    QCOMPARE(replaceAll(starts, request(SearchMode::Regex, QStringLiteral("^"), QStringLiteral("// "))), 2u);
    QCOMPARE(contents(starts), std::string("// a\n// b\n"));

    // $ is the end of each line, not also the end of the file after its last line break.
    ui::TextDocument ends(write("ends.txt", "a\nb\n"), codec("UTF-8"));
    QCOMPARE(replaceAll(ends, request(SearchMode::Regex, QStringLiteral("$"), QStringLiteral(";"))), 2u);
    QCOMPARE(contents(ends), std::string("a;\nb;\n"));
    ui::TextDocument noBreak(write("nobreak.txt", "a\nb"), codec("UTF-8"));
    QCOMPARE(replaceAll(noBreak, request(SearchMode::Regex, QStringLiteral("$"), QStringLiteral(";"))), 2u);
    QCOMPARE(contents(noBreak), std::string("a;\nb;"));
}

void TestReplace::keepsTheBytesBetweenMatches()
{
    // A byte that is not UTF-8 between the matches stays exactly as it was.
    ui::TextDocument document(write("mixed.txt", std::string("a\xFF a\n", 5)), codec("UTF-8"));
    QCOMPARE(replaceAll(document, request(SearchMode::Text, QStringLiteral("a"), QStringLiteral("bb"))), 2u);
    QCOMPARE(contents(document), std::string("bb\xFF bb\n", 7));
}

void TestReplace::writesInTheFilesEncoding()
{
    // 你好，世界 in GB18030: 世界 becomes 朋友, written in GB18030 too.
    ui::TextDocument document(write("gbk.txt", "\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7\n"), codec("GB18030"));
    QCOMPARE(replaceAll(document, request(SearchMode::Text, QStringLiteral("世界"), QStringLiteral("朋友"))), 1u);
    QCOMPARE(contents(document), std::string("\xC4\xE3\xBA\xC3\xA3\xAC\xC5\xF3\xD3\xD1\n"));
}

void TestReplace::reportsWhatCannotBeReplaced()
{
    ui::TextDocument latin1(write("latin1.txt", "abc\n"), codec("ISO-8859-1"));
    const ui::SearchContext context{latin1.source(), latin1.sharedIndex(), latin1.codec()};
    ui::ReplaceAllResult result =
        ui::replaceAll(request(SearchMode::Text, QStringLiteral("b"), QStringLiteral("中")), context);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.count, 0u);

    result = ui::replaceAll(request(SearchMode::Regex, QStringLiteral("("), QStringLiteral("x")), context);
    QVERIFY(!result.error.isEmpty()); // an invalid regular expression

    result = ui::replaceAll(request(SearchMode::Text, QStringLiteral("zzz"), QStringLiteral("x")), context);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.count, 0u); // nothing to replace is not an error
}

void TestReplace::replacementForOneMatch()
{
    ui::TextDocument document(write("one.txt", "key=value\nname=AIEdit\n"), codec("UTF-8"));
    const ui::SearchContext context{document.source(), document.sharedIndex(), document.codec()};
    const ReplaceRequest swap = request(SearchMode::Regex, QStringLiteral("(\\w+)=(\\w+)"), QStringLiteral("$2=$1"));
    QCOMPARE(ui::replacementFor(swap, context, 10, 21), std::optional<QString>(QStringLiteral("AIEdit=name")));
    QString error;
    QVERIFY(!ui::replacementFor(swap, context, 3, 9, &error)); // "=value": not where a match is
    QVERIFY(!error.isEmpty());
}

void TestReplace::replaceThenFindNext()
{
    const std::shared_ptr<const core::FileSource> source = write("doc.txt", "one two one two\n");
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    ui::FindBar* bar = doc.findBar();
    bar->setMode(SearchMode::Text);
    bar->setQuery(QStringLiteral("one"));
    bar->setReplacement(QStringLiteral("1"));

    // The first Replace finds the first match; each next one replaces it and finds the next.
    doc.replaceCurrent();
    QTRY_COMPARE(doc.textView()->selectionEnd(), 3u);
    QCOMPARE(doc.textView()->selectionStart(), 0u);
    doc.replaceCurrent();
    QCOMPARE(contents(*doc.textDocument()), std::string("1 two one two\n"));
    QTRY_COMPARE(doc.textView()->selectionStart(), 6u);
    doc.replaceCurrent();
    QCOMPARE(contents(*doc.textDocument()), std::string("1 two 1 two\n"));
    doc.textView()->undo();
    QCOMPARE(contents(*doc.textDocument()), std::string("1 two one two\n")); // one at a time
}

void TestReplace::replaceAllInTheDocumentView()
{
    const std::shared_ptr<const core::FileSource> source = write("view.txt", "a-b-c\n");
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.findBar()->setQuery(QStringLiteral("-"));
    doc.findBar()->setReplacement(QStringLiteral("+"));
    doc.replaceAll();
    QCOMPARE(contents(*doc.textDocument()), std::string("a+b+c\n"));
    QVERIFY(doc.isModified());
    QVERIFY(!doc.findBar()->statusText().isEmpty()); // says how many

    doc.findBar()->setQuery(QStringLiteral("?"));
    doc.replaceAll();
    QCOMPARE(contents(*doc.textDocument()), std::string("a+b+c\n"));
}

void TestReplace::unavailableWhereNothingCanBeEdited()
{
    const auto binary = write("program.bin", std::string("\x7F" "ELF\x02\x01\x01\0\0\0\0\0", 12));
    ui::DocumentView binaryDoc(binary, core::detectFileType(binary->bytes()), codec("ISO-8859-1"));
    binaryDoc.findBar()->setMode(SearchMode::Text);
    binaryDoc.findBar()->setQuery(QStringLiteral("ELF"));
    binaryDoc.findBar()->setReplacement(QStringLiteral("XYZ"));
    binaryDoc.replaceAll();
    QCOMPARE(binaryDoc.source()->bytes()[1], std::uint8_t('E')); // untouched

    const auto text = write("text.txt", "abc\n");
    ui::DocumentView textDoc(text, core::detectFileType(text->bytes()), core::utf8Codec());
    textDoc.findBar()->setMode(SearchMode::Hex); // bytes are not replaced
    textDoc.findBar()->setQuery(QStringLiteral("62"));
    textDoc.replaceAll();
    QCOMPARE(contents(*textDoc.textDocument()), std::string("abc\n"));
}

void TestReplace::replaceActionOpensTheReplaceRow()
{
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("menu.txt")));
    std::ofstream(path, std::ios::binary) << "text\n";
    ui::MainWindow window;
    QVERIFY(window.openFile(ui::toQString(path)));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QAction* replace = nullptr;
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == QStringLiteral("R&eplace…"))
            replace = action;
    QVERIFY(replace);
    replace->trigger();
    QVERIFY(window.currentDocument()->findBar()->isReplaceVisible());
}

QTEST_MAIN(TestReplace)
#include "tst_replace.moc"
