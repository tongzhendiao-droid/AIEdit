#include "ui/DocumentView.h"
#include "ui/FileSearch.h"
#include "ui/FindInFilesPanel.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextView.h"

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <fstream>

using namespace aiedit;
using ui::FileMatches;
using ui::FileSearchRequest;

namespace {

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

// file name → its lines that passed (0-based), relative to the folder searched.
QMap<QString, QList<std::uint64_t>> linesOf(const std::vector<FileMatches>& files, const QString& folder)
{
    QMap<QString, QList<std::uint64_t>> result;
    for (const FileMatches& file : files) {
        for (const ui::FilterMatch& match : file.matches)
            result[QDir(folder).relativeFilePath(file.path)] << match.line;
    }
    return result;
}

} // namespace

class TestFindInFiles : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void searchesTheFilesOfAFolder();
    void readsEachFileInItsEncoding();
    void regexCaseAndFilterRules();
    void stopsAtTheLimitOrWhenCancelled();
    void opensResultsAtTheirLine();
    void shortcuts();

private:
    void write(const QString& name, const std::string& content);
    std::vector<FileMatches> search(const ui::FilterRule& rule, bool subfolders = true,
                                    const QStringList& filters = {QStringLiteral("*.log"),
                                                                  QStringLiteral("*.txt")},
                                    ui::FileSearchSummary* summary = nullptr, std::size_t maxMatches = 10'000);

    QTemporaryDir m_dir;
};

void TestFindInFiles::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_findinfiles"));
}

void TestFindInFiles::init()
{
    QVERIFY(m_dir.isValid());
    QSettings().clear();
    write(QStringLiteral("a.log"), "start\nERROR one\nok\nERROR two\n");
    write(QStringLiteral("b.txt"), "\xD5\xFD\xB3\xA3\n\xB4\xED\xCE\xF3 \xD4\xDA\xD5\xE2\xC0\xEF\n"); // 正常 / 错误 在这里, GB18030
    write(QStringLiteral("c.log"), std::string("\x7F" "ELF\0\0\0\0ERROR in a binary", 25)); // left out
    write(QStringLiteral("sub/d.log"), "ERROR below\n");
    write(QStringLiteral(".hidden/e.log"), "ERROR hidden\n"); // hidden folders are left out
    write(QStringLiteral(".f.log"), "ERROR hidden too\n");
    write(QStringLiteral("g.md"), "ERROR in another kind of file\n");
}

void TestFindInFiles::write(const QString& name, const std::string& content)
{
    const QString path = m_dir.filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    std::ofstream(ui::toPath(path), std::ios::binary) << content;
}

std::vector<FileMatches> TestFindInFiles::search(const ui::FilterRule& rule, bool subfolders,
                                                 const QStringList& filters, ui::FileSearchSummary* summary,
                                                 std::size_t maxMatches)
{
    FileSearchRequest request;
    request.folder = m_dir.path();
    request.nameFilters = filters;
    request.subfolders = subfolders;
    request.rule = rule;
    request.legacyCodec = "GB18030";
    request.maxMatches = maxMatches;
    std::vector<FileMatches> files;
    ui::FileSearchHooks hooks;
    hooks.found = [&files](FileMatches&& file) { files.push_back(std::move(file)); };
    const ui::FileSearchSummary result = ui::searchFiles(request, hooks);
    if (summary)
        *summary = result;
    return files;
}

void TestFindInFiles::searchesTheFilesOfAFolder()
{
    ui::FileSearchSummary summary;
    const auto files = search(ui::queryRule(QStringLiteral("error"), false, false), true,
                              {QStringLiteral("*.log"), QStringLiteral("*.txt")}, &summary);
    QCOMPARE(linesOf(files, m_dir.path()), (QMap<QString, QList<std::uint64_t>>{
                                               {QStringLiteral("a.log"), {1, 3}},
                                               {QStringLiteral("sub/d.log"), {0}}}));
    QCOMPARE(files.front().texts, (QStringList{QStringLiteral("ERROR one"), QStringLiteral("ERROR two")}));
    QCOMPARE(summary.matches, 3u);
    QCOMPARE(summary.filesSearched, 3); // a.log, b.txt, sub/d.log
    QCOMPARE(summary.filesSkipped, 1);  // the binary one

    // Without subfolders; and with every kind of file.
    QCOMPARE(linesOf(search(ui::queryRule(QStringLiteral("ERROR"), false, true), false), m_dir.path()).keys(),
             QStringList{QStringLiteral("a.log")});
    QVERIFY(linesOf(search(ui::queryRule(QStringLiteral("ERROR"), false, true), true, {}), m_dir.path())
                .contains(QStringLiteral("g.md")));
}

void TestFindInFiles::readsEachFileInItsEncoding()
{
    const auto files = search(ui::queryRule(QStringLiteral("错误"), false, false));
    QCOMPARE(files.size(), 1u);
    QCOMPARE(QFileInfo(files.front().path).fileName(), QStringLiteral("b.txt"));
    QCOMPARE(files.front().matches.front().line, 1u);
    QCOMPARE(files.front().codec, QStringLiteral("GB18030"));
    QCOMPARE(files.front().texts.front(), QStringLiteral("错误 在这里"));
}

void TestFindInFiles::regexCaseAndFilterRules()
{
    QCOMPARE(linesOf(search(ui::queryRule(QStringLiteral("^error t"), true, false)), m_dir.path()),
             (QMap<QString, QList<std::uint64_t>>{{QStringLiteral("a.log"), {3}}}));
    QVERIFY(search(ui::queryRule(QStringLiteral("error"), false, true)).empty()); // case matters
    // An unfinished expression searches nothing.
    QVERIFY(search(ui::queryRule(QStringLiteral("(error"), true, false)).empty());

    ui::FilterRule rule = ui::queryRule(QStringLiteral("ERROR"), false, true);
    ui::FilterCondition two;
    two.pattern = QStringLiteral("two");
    two.exclude = true;
    rule.conditions << two;
    QCOMPARE(linesOf(search(rule), m_dir.path()), (QMap<QString, QList<std::uint64_t>>{
                                                      {QStringLiteral("a.log"), {1}},
                                                      {QStringLiteral("sub/d.log"), {0}}}));
}

void TestFindInFiles::stopsAtTheLimitOrWhenCancelled()
{
    ui::FileSearchSummary summary;
    const auto files = search(ui::queryRule(QStringLiteral("ERROR"), false, true), true,
                              {QStringLiteral("*.log")}, &summary, 2);
    QVERIFY(summary.limited);
    QCOMPARE(summary.matches, 2u);
    QCOMPARE(files.size(), 1u); // both in a.log, the first file

    FileSearchRequest request;
    request.folder = m_dir.path();
    request.rule = ui::queryRule(QStringLiteral("ERROR"), false, true);
    ui::FileSearchHooks hooks;
    hooks.cancelled = [] { return true; };
    bool found = false;
    hooks.found = [&found](FileMatches&&) { found = true; };
    ui::searchFiles(request, hooks);
    QVERIFY(!found);
}

void TestFindInFiles::opensResultsAtTheirLine()
{
    ui::MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    findAction(window, QStringLiteral("Find in F&iles…"))->trigger();
    auto* panel = window.findChild<ui::FindInFilesPanel*>();
    QVERIFY(panel && panel->isVisible());

    panel->setFolder(m_dir.path());
    panel->setQuery(QStringLiteral("ERROR"));
    panel->setNameFilters(QStringLiteral("*.log"));
    panel->search();
    QTRY_VERIFY(!panel->isSearching());
    QTreeWidget* results = panel->results();
    QCOMPARE(results->topLevelItemCount(), 2);
    QCOMPARE(results->topLevelItem(0)->text(0), QStringLiteral("a.log (2)"));
    QCOMPARE(results->topLevelItem(0)->child(1)->text(0), QStringLiteral("4: ERROR two"));
    QVERIFY(panel->statusText().contains(QStringLiteral("3 matches in 2 of")));

    emit results->itemActivated(results->topLevelItem(0)->child(1), 0);
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(QFileInfo(window.currentDocument()->filePath()).fileName(), QStringLiteral("a.log"));
    QCOMPARE(window.currentDocument()->textView()->cursorLine(), 3u);

    // What was searched is there next time.
    QCOMPARE(QSettings().value(QStringLiteral("findInFiles/files")).toString(), QStringLiteral("*.log"));
}

void TestFindInFiles::shortcuts()
{
    ui::MainWindow window;
    QCOMPARE(findAction(window, QStringLiteral("Find in F&iles…"))->shortcut(),
             QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    // Ctrl+Shift+T reopens closed tabs, as elsewhere; F5 reloads once, Shift+F5 keeps on.
    QCOMPARE(findAction(window, QStringLiteral("&Follow End of File"))->shortcut(),
             QKeySequence(Qt::SHIFT | Qt::Key_F5));
}

QTEST_MAIN(TestFindInFiles)
#include "tst_findinfiles.moc"
