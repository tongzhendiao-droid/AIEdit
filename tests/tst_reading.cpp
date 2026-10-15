#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/AiPanel.h"
#include "ui/Contents.h"
#include "ui/ContentsPanel.h"
#include "ui/DocumentView.h"
#include "ui/FilterRuleWriter.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include "MockChatServer.h"

#include <QAction>
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include <fstream>

using namespace aiedit;
using ui::Heading;

namespace {

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

QStringList titles(const std::vector<Heading>& headings)
{
    QStringList result;
    for (const Heading& heading : headings)
        result << heading.title;
    return result;
}

// A streamed answer from the mock service.
QList<QByteArray> answer(const QString& text)
{
    return {aiedit::test::chunk(text), aiedit::test::chunk({}, QStringLiteral("content"), QStringLiteral("stop")),
            "data: [DONE]\n\n"};
}

// A little classical novel: contents first, then chapters whose titles follow their number.
std::string novel()
{
    std::string text = "东周列国志\n\n目录\n第一回 周宣王闻谣轻杀 杜大夫化厉鸣冤\n第二回 褒人赎罪献美女 幽王烽火戏诸侯\n\n";
    text += "第一回\n周宣王闻谣轻杀\n\n杜大夫化厉鸣冤\n\n";
    for (int i = 0; i < 40; ++i)
        text += "　　话说周朝，自武王伐纣，即天子位，成康继之，那都是守成令主。宣王即位，任用贤臣。\n";
    text += "第二回\n褒人赎罪献美女\n幽王烽火戏诸侯\n";
    for (int i = 0; i < 40; ++i)
        text += "　　话说宣王闻褒姒之事，心中不悦，天下第一\n"; // ends with 第一, but a sentence
    return text;
}

} // namespace

class TestReading : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void findsHeadingsOfManyKinds();
    void completesBareNumbersAndDropsTheContentsList();
    void findsChaptersAtOffsets();
    void readingModeLaysOutABook();
    void readingModeIsRemembered();
    void contentsGoesToChaptersAndFollowsReading();
    void theAiFindsHeadingsTheRuleMisses();
    void sendsAChapterToTheAi();
    void theAiMenuFindsTheChapterFirst();

private:
    QString write(const char* name, const std::string& content);
    ui::SearchContext contextOf(const ui::TextDocument& document) const
    {
        return {document.source(), document.sharedIndex(), document.codec()};
    }

    QTemporaryDir m_dir;
};

void TestReading::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_reading"));
    qunsetenv(ui::AiSettings::kKeyEnvironmentVariable);
    qunsetenv(ui::AiSettings::kDeepSeekKeyEnvironmentVariable);
}

void TestReading::init()
{
    QVERIFY(m_dir.isValid());
    QSettings().clear();
}

QString TestReading::write(const char* name, const std::string& content)
{
    const QString path = m_dir.filePath(QString::fromLatin1(name));
    std::ofstream(ui::toPath(path), std::ios::binary) << content;
    return path;
}

void TestReading::findsHeadingsOfManyKinds()
{
    const std::string book = "书名\n"
                             "序\n"
                             "这是一本书的序言，说的是写书的缘由，和书里的内容，以及作者的感想。\n"
                             "卷一\n"
                             "第一章 风起\n"
                             "正文。\n"
                             "第二章　云涌\n"
                             "曲礼上第一\n"
                             "杂阿含经卷第二\n"
                             "正文 五帝本纪\n"
                             "Chapter 7\n"
                             "# A Markdown Title\n"
                             "他说：“天下第一。”\n"   // a sentence, not a heading
                             "第三十二条规定，凡是这样写的长句子都不应该被当成标题，即使它以第字开头，因为它实在太长了一些。\n"
                             "后记\n";
    std::error_code ec;
    ui::TextDocument document(core::FileSource::open(ui::toPath(write("kinds.txt", book)), ec), core::utf8Codec());
    const std::vector<Heading> headings = ui::findHeadings(ui::defaultHeadingRule(), contextOf(document));
    QCOMPARE(titles(headings), (QStringList{QStringLiteral("序"), QStringLiteral("卷一"),
                                            QStringLiteral("第一章 风起"), QStringLiteral("第二章 云涌"),
                                            QStringLiteral("曲礼上第一"), QStringLiteral("杂阿含经卷第二"),
                                            QStringLiteral("五帝本纪"), QStringLiteral("Chapter 7"),
                                            QStringLiteral("# A Markdown Title"), QStringLiteral("后记")}));
    // Volumes above chapters.
    QCOMPARE(headings[1].level, 1);
    QCOMPARE(headings[2].level, 2);
    QCOMPARE(headings[5].level, 1); // 卷第二
    QCOMPARE(headings[2].line, 4u);
}

void TestReading::completesBareNumbersAndDropsTheContentsList()
{
    std::error_code ec;
    ui::TextDocument document(core::FileSource::open(ui::toPath(write("novel.txt", novel())), ec),
                              core::utf8Codec());
    const std::vector<Heading> headings = ui::findHeadings(ui::defaultHeadingRule(), contextOf(document));
    // The list of contents at the start repeats the chapters: only the chapters stay. Their
    // titles come from the lines after the number, empty lines between them or not.
    QCOMPARE(titles(headings), (QStringList{QStringLiteral("第一回 周宣王闻谣轻杀 杜大夫化厉鸣冤"),
                                            QStringLiteral("第二回 褒人赎罪献美女 幽王烽火戏诸侯")}));
    QCOMPARE(headings[0].line, 6u);
}

void TestReading::findsChaptersAtOffsets()
{
    std::vector<Heading> headings(3);
    headings[0].start = 10;
    headings[1].start = 100;
    headings[2].start = 500;
    QCOMPARE(ui::headingAt(headings, 0), -1);
    QCOMPARE(ui::headingAt(headings, 10), 0);
    QCOMPARE(ui::headingAt(headings, 99), 0);
    QCOMPARE(ui::headingAt(headings, 100), 1);
    QCOMPARE(ui::headingAt(headings, 9999), 2);
    QCOMPARE(ui::headingEnd(headings, 1, 800), 500u);
    QCOMPARE(ui::headingEnd(headings, 2, 800), 800u);
}

void TestReading::readingModeLaysOutABook()
{
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source =
        core::FileSource::open(ui::toPath(write("layout.txt", novel())), ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(1200, 600);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));
    ui::TextView* view = doc.textView();
    view->setWordWrap(false);
    const QRect normalCaret = view->cursorRect();

    doc.setViewMode(ui::DocumentView::ViewMode::Split);
    doc.setReadingMode(true);
    QVERIFY(doc.isReadingMode());
    QCOMPARE(doc.viewMode(), ui::DocumentView::ViewMode::Text); // a book is read as text
    QVERIFY(!view->wordWrap());                                   // the choice is kept …
    const QRect readingCaret = view->cursorRect();
    QVERIFY(readingCaret.height() > normalCaret.height()); // … but lines are farther apart,
    QVERIFY(readingCaret.left() > 200);                    // the text in a column in the middle,
    QVERIFY(!view->horizontalScrollBar()->isVisible());    // and lines wrap

    doc.setReadingMode(false);
    QCOMPARE(view->cursorRect().height(), normalCaret.height());
    QCOMPARE(view->cursorRect().left(), normalCaret.left());
}

void TestReading::readingModeIsRemembered()
{
    const QString path = write("remembered.txt", novel());
    {
        ui::MainWindow window;
        QVERIFY(window.openFile(path));
        findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
        QVERIFY(window.currentDocument()->isReadingMode());
        QVERIFY(window.findChild<ui::ContentsPanel*>()->parentWidget()->isVisibleTo(&window)); // with its contents
        findAction(window, QStringLiteral("&Close Tab"))->trigger();
    }
    ui::MainWindow window;
    QVERIFY(window.openFile(path));
    QVERIFY(window.currentDocument()->isReadingMode());
    QCOMPARE(findAction(window, QStringLiteral("Rea&ding Mode"))->shortcut(), QKeySequence(Qt::Key_F9));
    QVERIFY(findAction(window, QStringLiteral("Rea&ding Mode"))->isChecked());
}

void TestReading::contentsGoesToChaptersAndFollowsReading()
{
    ui::MainWindow window;
    window.resize(1000, 600);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(write("follow.txt", novel())));
    findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
    auto* contents = window.findChild<ui::ContentsPanel*>();
    QTRY_COMPARE(contents->headings().size(), 2u);
    QVERIFY(contents->statusText().contains(QStringLiteral("2 heading")));

    // A click goes to the chapter: its heading at the top.
    QTreeWidget* tree = contents->tree();
    emit tree->itemClicked(tree->topLevelItem(1), 0);
    ui::TextView* view = window.currentDocument()->textView();
    QCOMPARE(view->firstVisibleOffset(), contents->headings()[1].start);
    QTRY_COMPARE(tree->currentItem(), tree->topLevelItem(1));

    // Reading back up marks the chapter there.
    view->scrollToOffset(contents->headings()[0].start + 10);
    QTRY_COMPARE(tree->currentItem(), tree->topLevelItem(0));
}

void TestReading::theAiFindsHeadingsTheRuleMisses()
{
    // 左传: reigns and years, which no built-in rule knows.
    std::string chronicle;
    for (const char* year : {"隐公元年", "隐公二年", "桓公元年"}) {
        chronicle += std::string(year) + "\n";
        for (int i = 0; i < 5; ++i)
            chronicle += "春，王正月。三月，公及邾仪父盟于蔑。夏，五月，郑伯克段于鄢。\n";
    }
    const QString path = write("zuozhuan.txt", chronicle);
    aiedit::test::MockServer server;
    server.chunks = answer(QString::fromUtf8(
        R"json({"description": "公年", "conditions": [{"type": "include", "regex": "^\\S{1,2}公(元|[一二三四五六七八九十]+)年$"}]})json"));

    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(path));
    findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
    auto* contents = window.findChild<ui::ContentsPanel*>();
    QTRY_VERIFY(!contents->isScanning());
    QVERIFY(contents->headings().empty());

    contents->ruleWriter()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    contents->findWithAi();
    QTRY_COMPARE(contents->headings().size(), 3u);
    QCOMPARE(contents->headings()[2].title, QStringLiteral("桓公元年"));
    // What went out: lines with their numbers, the first ones and short ones.
    const QString sent = QJsonDocument::fromJson(server.body)
                             .object()
                             .value(QStringLiteral("messages"))
                             .toArray()[1]
                             .toObject()
                             .value(QStringLiteral("content"))
                             .toString();
    QVERIFY(sent.contains(QStringLiteral("1: 隐公元年")));

    // Remembered for the file; the built-in rule can come back.
    const QVariantMap rules = QSettings().value(QStringLiteral("contents/rules")).toMap();
    QVERIFY(rules.contains(window.currentDocument()->filePath()));
    contents->useBuiltInRule();
    QTRY_VERIFY(!contents->isScanning());
    QVERIFY(contents->headings().empty());
}

void TestReading::sendsAChapterToTheAi()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("这一回讲的是周宣王。"));
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.findChild<ui::AiPanel*>()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    QVERIFY(window.openFile(write("chapter.txt", novel())));
    findAction(window, QStringLiteral("Rea&ding Mode"))->trigger();
    auto* contents = window.findChild<ui::ContentsPanel*>();
    QTRY_COMPARE(contents->headings().size(), 2u);

    window.currentDocument()->showFromOffset(contents->headings()[0].start);
    contents->askAboutCurrentChapter(ui::ContentsPanel::ChapterAction::Summarize);
    QTRY_COMPARE(server.requests, 1);
    const QString body = QString::fromUtf8(server.body);
    QVERIFY(body.contains(QStringLiteral("Summarize this chapter")));
    QVERIFY(body.contains(QStringLiteral("那都是守成令主")));  // the chapter …
    QVERIFY(!body.contains(QStringLiteral("褒姒")));          // … and not the next one

    bool cut = true;
    const QString text = contents->chapterText(0, &cut);
    QVERIFY(!cut);
    QVERIFY(text.startsWith(QStringLiteral("第一回")));
}

void TestReading::theAiMenuFindsTheChapterFirst()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("译文。"));
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.findChild<ui::AiPanel*>()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    QVERIFY(window.openFile(write("menu.txt", novel())));
    auto* contents = window.findChild<ui::ContentsPanel*>();
    QVERIFY(!contents->isVisible()); // not looked for yet
    window.currentDocument()->showFromOffset(std::string_view(novel()).find("第二回\n褒人"));

    findAction(window, QStringLiteral("Translate This C&hapter"))->trigger();
    QTRY_COMPARE(server.requests, 1); // once the chapters were found
    QVERIFY(contents->isVisible());
    const QString body = QString::fromUtf8(server.body);
    QVERIFY(body.contains(QStringLiteral("Translate this chapter")));
    QVERIFY(body.contains(QStringLiteral("褒姒")));
}

QTEST_MAIN(TestReading)
#include "tst_reading.moc"
