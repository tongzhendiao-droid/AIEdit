#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/AiEdit.h"
#include "ui/AiEditBar.h"
#include "ui/BackupStore.h"
#include "ui/Compare.h"
#include "ui/CompareTab.h"
#include "ui/CompareView.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "MockChatServer.h"

#include <memory>

using namespace aiedit;

namespace {

QList<QByteArray> answer(const QString& text)
{
    return {aiedit::test::chunk(text.left(text.size() / 2)), aiedit::test::chunk(text.mid(text.size() / 2)),
            aiedit::test::chunk({}, QStringLiteral("content"), QStringLiteral("stop")), "data: [DONE]\n\n"};
}

std::string contents(const ui::DocumentView& doc)
{
    const auto bytes = doc.textDocument()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::shared_ptr<const core::FileSource> sourceOf(const std::string& text)
{
    return core::FileSource::fromBytes({}, std::make_shared<const std::vector<std::uint8_t>>(text.begin(), text.end()));
}

// A text in memory, indexed, as a rule runs on it.
struct Text
{
    explicit Text(const std::string& text, const core::TextCodec& codec = core::utf8Codec())
        : document(sourceOf(text), codec)
    {
    }
    ui::SearchContext context() const
    {
        return {document.source(), document.sharedIndex(), document.codec()};
    }
    ui::TextDocument document;
};

// The user message of a request the mock server received.
QString userMessage(const QByteArray& body)
{
    const QJsonArray messages = QJsonDocument::fromJson(body).object().value(QStringLiteral("messages")).toArray();
    return messages.size() >= 2 ? messages[1].toObject().value(QStringLiteral("content")).toString() : QString();
}

// Clicks OK in the message box that opens next.
void answerNextMessage()
{
    QTimer::singleShot(0, [] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            box->button(box->standardButtons() & QMessageBox::Ok ? QMessageBox::Ok : QMessageBox::Close)->click();
    });
}

} // namespace

class TestAiEdit : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void parsesAnswers();
    void readsRules();
    void runsRulesOnLines();
    void keepsOnlyAcceptedChanges();
    void editsASelection();
    void reviewsChangeByChange();
    void refusesWhenTheDocumentChanged();
    void writesARuleForLongText();
    void keepsLineBreaksAndTheEnd();
    void saysWhatTheAiSaid();
    void asksForSettingsFirst();

private:
    QString write(const QString& name, const QByteArray& content);
    // A window with the file open, its AI pointed at the server.
    ui::DocumentView* open(ui::MainWindow& window, const QString& name, const QByteArray& content,
                           aiedit::test::MockServer& server);

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestAiEdit::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_aiedit"));
}

void TestAiEdit::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QSettings().clear();
    QDir(ui::BackupStore::root()).removeRecursively();
}

QString TestAiEdit::write(const QString& name, const QByteArray& content)
{
    const QString path = m_dir->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    return path;
}

ui::DocumentView* TestAiEdit::open(ui::MainWindow& window, const QString& name, const QByteArray& content,
                                   aiedit::test::MockServer& server)
{
    if (!window.openFile(write(name, content)))
        return nullptr;
    ui::DocumentView* doc = window.currentDocument();
    doc->aiEditor()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    return doc;
}

void TestAiEdit::parsesAnswers()
{
    QString note;
    std::optional<QString> text =
        ui::AiEditor::parseText(QStringLiteral("Renamed it.\n```cpp\nint x;\nint y;\n```\nThat is all."), &note);
    QVERIFY(text);
    QCOMPARE(*text, QStringLiteral("int x;\nint y;"));
    QCOMPARE(note, QStringLiteral("Renamed it.\n\nThat is all."));
    // A longer fence holds a text with one inside.
    text = ui::AiEditor::parseText(QStringLiteral("````md\nsee:\n```\ncode\n```\n````"), &note);
    QVERIFY(text);
    QCOMPARE(*text, QStringLiteral("see:\n```\ncode\n```"));
    QCOMPARE(ui::codeFence(QStringLiteral("a ``` b")), QStringLiteral("````"));
    QCOMPARE(ui::codeFence(QStringLiteral("plain")), QStringLiteral("```"));
    // No block, or one cut off before its end: no text, and what was said is the note.
    QVERIFY(!ui::AiEditor::parseText(QStringLiteral("I cannot do that here."), &note));
    QCOMPARE(note, QStringLiteral("I cannot do that here."));
    QVERIFY(!ui::AiEditor::parseText(QStringLiteral("```\nint x;\nint")));
}

void TestAiEdit::readsRules()
{
    QString note;
    QString error;
    std::optional<ui::EditRule> rule = ui::AiEditor::parseRule(
        QStringLiteral("Deletes the debug lines.\n```json\n{\"description\": \"no debug\", \"lines\": "
                       "{\"conditions\": [{\"type\": \"include\", \"regex\": \"DEBUG\"}]}, \"delete\": true}\n```"),
        &note, &error);
    QVERIFY2(rule, qPrintable(error));
    QCOMPARE(note, QStringLiteral("Deletes the debug lines."));
    QVERIFY(rule->deleteLines);
    QVERIFY(rule->lines);
    QCOMPARE(rule->lines->conditions.size(), 1);
    QCOMPARE(ui::EditRule::fromJson(rule->toJson()), rule); // written as it was read

    // A bare object will do; one that cannot run will not.
    rule = ui::AiEditor::parseRule(QStringLiteral("{\"replace\": [{\"find\": \"(\\\\d+)\", \"with\": \"<\\\\1>\"}]}"));
    QVERIFY(rule);
    QCOMPARE(rule->replacements.size(), 1);
    QCOMPARE(rule->replacements[0].with, QStringLiteral("<\\1>"));
    QVERIFY(!ui::AiEditor::parseRule(QStringLiteral("{\"replace\": [{\"find\": \"(\"}]}"), nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("invalid regular expression")));
    QVERIFY(!ui::AiEditor::parseRule(QStringLiteral("{\"description\": \"nothing\"}"), nullptr, &error));
    QVERIFY(!ui::AiEditor::parseRule(QStringLiteral("{\"delete\": true, \"replace\": [{\"find\": \"a\"}]}")));
}

void TestAiEdit::runsRulesOnLines()
{
    const Text text("DEBUG one\nINFO two 12\nDEBUG three\nbad \xFF byte 7\n");
    ui::EditRule rule;
    rule.lines = ui::FilterRule::fromJson(QJsonDocument::fromJson(
                                              R"({"conditions": [{"type": "include", "regex": "DEBUG"}]})")
                                              .object());
    QVERIFY(rule.lines);
    rule.deleteLines = true;
    const auto size = static_cast<std::uint64_t>(text.document.bytes().size());
    QCOMPARE(ui::applyEditRule(rule, text.context(), 0, size), QByteArray("INFO two 12\nbad \xFF byte 7\n"));

    // Replacements, with groups; lines they leave alone keep their bytes, unreadable ones too.
    ui::EditRule numbers;
    numbers.replacements.append({QStringLiteral("(\\d+)"), QStringLiteral("<\\1>"), false});
    QCOMPARE(ui::applyEditRule(numbers, text.context(), 0, size),
             QByteArray("DEBUG one\nINFO two <12>\nDEBUG three\nbad \xEF\xBF\xBD byte <7>\n"));
    // Only the lines starting in the range.
    QCOMPARE(ui::applyEditRule(numbers, text.context(), 10, 22), QByteArray("INFO two <12>\n"));
    // A character the codec cannot hold stops it, and says where.
    const Text latin("a\nb\n", *core::findCodec("ISO-8859-1"));
    ui::EditRule arrows;
    arrows.replacements.append({QStringLiteral("b"), QStringLiteral("→"), false});
    QString error;
    QVERIFY(!ui::applyEditRule(arrows, latin.context(), 0, 4, &error));
    QVERIFY(error.contains(QStringLiteral("line 2")));
}

void TestAiEdit::keepsOnlyAcceptedChanges()
{
    const std::string left = "a\nb\nc\nd\ne\n";
    const std::string right = "A\nb\nc\nd\nE\nf\n";
    const ui::CompareSide one = ui::bytesSide(QStringLiteral("left"), sourceOf(left), core::utf8Codec());
    const ui::CompareSide two = ui::bytesSide(QStringLiteral("right"), sourceOf(right), core::utf8Codec());
    const ui::TextComparison comparison = ui::compareText(one, two, ui::CompareOptions{});
    QCOMPARE(comparison.hunks.size(), 2u);
    const auto bytes = [](const std::string& s) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };
    QCOMPARE(ui::acceptChanges(comparison, bytes(left), bytes(right), {true, true}), QByteArray(right.c_str()));
    QCOMPARE(ui::acceptChanges(comparison, bytes(left), bytes(right), {false, false}), QByteArray(left.c_str()));
    QCOMPARE(ui::acceptChanges(comparison, bytes(left), bytes(right), {false, true}),
             QByteArray("a\nb\nc\nd\nE\nf\n"));
}

void TestAiEdit::editsASelection()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("Renamed b to x.\n```cpp\nint x;\n```"));
    ui::MainWindow window;
    ui::DocumentView* doc = open(window, QStringLiteral("a.cpp"), "int a;\nint b;\nint c;\n", server);
    QVERIFY(doc);
    doc->textView()->select(7, 14); // "int b;\n"
    doc->showAiEditBar();
    QVERIFY(doc->aiEditBar()->isVisible() || !window.isVisible());
    doc->requestAiEdit(QStringLiteral("rename b to x"));
    QVERIFY(doc->aiEditBar()->isBusy());

    QTRY_VERIFY(window.currentCompareTab());
    ui::CompareTab* tab = window.currentCompareTab();
    QVERIFY(tab->isReview());
    QTRY_VERIFY(!tab->isComparing());
    QCOMPARE(tab->accepted(), std::vector<bool>{true});
    QVERIFY(tab->title().contains(QStringLiteral("line 2")));

    // What went out: the instruction, the text, and around it the lines to read.
    const QString sent = userMessage(server.body);
    QVERIFY(sent.contains(QStringLiteral("rename b to x")));
    QVERIFY(sent.contains(QStringLiteral("a.cpp (C++)")));
    QVERIFY(sent.contains(QStringLiteral("The text to change:\n```\nint b;\n\n```")));
    QVERIFY(sent.contains(QStringLiteral("int a;")));
    QVERIFY(sent.contains(QStringLiteral("int c;")));

    // Nothing is written until it is applied, and then as one step.
    QCOMPARE(contents(*doc), std::string("int a;\nint b;\nint c;\n"));
    QVERIFY(tab->applyAccepted());
    QCOMPARE(contents(*doc), std::string("int a;\nint x;\nint c;\n"));
    QTRY_VERIFY(!window.currentCompareTab()); // the review closed, back to the document
    QCOMPARE(window.currentDocument(), doc);
    QVERIFY(doc->aiEditBar()->isHidden()); // and the bar is done with
    doc->undo();
    QCOMPARE(contents(*doc), std::string("int a;\nint b;\nint c;\n"));
}

void TestAiEdit::reviewsChangeByChange()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("```\nA\nb\nC\nd\n```"));
    ui::MainWindow window;
    window.show();
    window.activateWindow(); // for the review's keys
    QVERIFY(QTest::qWaitForWindowActive(&window));
    ui::DocumentView* doc = open(window, QStringLiteral("letters.txt"), "a\nb\nc\nd\n", server);
    doc->requestAiEdit(QStringLiteral("capitalize a and c")); // nothing selected: all of it
    QTRY_VERIFY(window.currentCompareTab());
    ui::CompareTab* tab = window.currentCompareTab();
    QTRY_VERIFY(!tab->isComparing());
    QCOMPARE(tab->accepted(), (std::vector<bool>{true, true}));

    // The review starts at the first change; Space takes it out, and the view strikes it through.
    QCOMPARE(tab->view()->currentHunk(), 0);
    tab->view()->setFocus();
    QTest::keyClick(tab->view(), Qt::Key_Space);
    QCOMPARE(tab->accepted(), (std::vector<bool>{false, true}));
    QVERIFY(tab->view()->isRejected(0));
    tab->rejectAll();
    QCOMPARE(tab->accepted(), (std::vector<bool>{false, false}));
    QVERIFY(!tab->applyAccepted()); // nothing to apply
    tab->acceptAll();
    tab->setAccepted(0, false);
    QTest::keyClick(tab->view(), Qt::Key_Return, Qt::ControlModifier); // applies
    QCOMPARE(contents(*doc), std::string("a\nb\nC\nd\n"));
}

void TestAiEdit::refusesWhenTheDocumentChanged()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("```\nhello world\n```"));
    ui::MainWindow window;
    ui::DocumentView* doc = open(window, QStringLiteral("hello.txt"), "hello\n", server);
    doc->requestAiEdit(QStringLiteral("add world"));
    QTRY_VERIFY(window.currentCompareTab());
    ui::CompareTab* tab = window.currentCompareTab();
    QTRY_VERIFY(!tab->isComparing());
    doc->textView()->setCursorOffset(0);
    doc->textView()->insertText(QStringLiteral("> "));
    answerNextMessage();
    QVERIFY(!tab->applyAccepted());
    QCOMPARE(contents(*doc), std::string("> hello\n")); // left alone
    QCOMPARE(window.currentCompareTab(), tab);         // and the review stays
}

void TestAiEdit::writesARuleForLongText()
{
    QByteArray text;
    for (int i = 0; i < 1500; ++i)
        text += (i % 3 == 0 ? "DEBUG step " : "INFO step ") + QByteArray::number(i) + '\n';
    text += "INFO SECRET-NOT-SAMPLED\n";
    QVERIFY(text.size() > ui::AiEditor::kMaxText);
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral(
        "Deletes the debug lines.\n```json\n{\"description\": \"no debug\", \"lines\": {\"conditions\": "
        "[{\"type\": \"include\", \"regex\": \"^DEBUG\"}]}, \"delete\": true}\n```"));
    ui::MainWindow window;
    ui::DocumentView* doc = open(window, QStringLiteral("long.log"), text, server);
    doc->requestAiEdit(QStringLiteral("delete the debug lines"));
    QTRY_VERIFY(window.currentCompareTab());
    ui::CompareTab* tab = window.currentCompareTab();
    QTRY_VERIFY(!tab->isComparing());
    QCOMPARE(tab->accepted().size(), 500u);

    // The AI saw samples, not the text; the rule ran here, and the review shows it.
    const QString sent = userMessage(server.body);
    QVERIFY(sent.contains(QStringLiteral("Sample lines")));
    QVERIFY(!server.body.contains("SECRET-NOT-SAMPLED"));
    QVERIFY(QJsonDocument::fromJson(server.body).object().value(QStringLiteral("messages")).toArray()[0]
                .toObject()
                .value(QStringLiteral("content"))
                .toString()
                .contains(QStringLiteral("rules that change files too long to send")));
    const QLabel* note = nullptr;
    for (const QLabel* label : tab->findChildren<QLabel*>())
        if (label->text().contains(QStringLiteral("delete the debug lines")))
            note = label;
    QVERIFY(note);
    QVERIFY(note->text().contains(QStringLiteral("\"delete\": true")));

    QVERIFY(tab->applyAccepted());
    const std::string after = contents(*doc);
    QVERIFY(after.find("DEBUG") == std::string::npos);
    QVERIFY(after.find("INFO step 1\n") != std::string::npos);
}

void TestAiEdit::keepsLineBreaksAndTheEnd()
{
    // The AI writes LF and leaves off the last line break: the document keeps CRLF, and its end.
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("```\nA\nb\n```"));
    ui::MainWindow window;
    ui::DocumentView* doc = open(window, QStringLiteral("crlf.txt"), "a\r\nb\r\n", server);
    QSignalSpy proposed(doc, &ui::DocumentView::aiEditProposed);
    doc->requestAiEdit(QStringLiteral("capitalize a"));
    QTRY_COMPARE(proposed.count(), 1);
    QCOMPARE(proposed.first().first().value<ui::DocumentView::AiEditProposal>().after, QByteArray("A\r\nb\r\n"));

    // A word selected: no line break comes in with the change.
    server.chunks = answer(QStringLiteral("```\nB\n```"));
    doc->textView()->select(3, 4);
    doc->requestAiEdit(QStringLiteral("capitalize"));
    QTRY_COMPARE(proposed.count(), 2);
    const auto word = proposed.last().first().value<ui::DocumentView::AiEditProposal>();
    QCOMPARE(word.before, QByteArray("b"));
    QCOMPARE(word.after, QByteArray("B"));
}

void TestAiEdit::saysWhatTheAiSaid()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("I cannot tell which variable you mean."));
    ui::MainWindow window;
    ui::DocumentView* doc = open(window, QStringLiteral("x.txt"), "x = 1\n", server);
    doc->requestAiEdit(QStringLiteral("rename it"));
    QTRY_VERIFY(!doc->aiEditBar()->isBusy());
    QVERIFY(!window.currentCompareTab());
    bool said = false;
    for (const QLabel* label : doc->aiEditBar()->findChildren<QLabel*>())
        said = said || label->text().contains(QStringLiteral("I cannot tell which variable you mean."));
    QVERIFY(said);
}

void TestAiEdit::asksForSettingsFirst()
{
    ui::AiEditor editor;
    editor.setSettingsProvider([] { return ui::AiSettings(); }); // no key
    QSignalSpy settings(&editor, &ui::AiEditor::settingsRequested);
    QSignalSpy failed(&editor, &ui::AiEditor::failed);
    ui::AiEditRequest request;
    request.instruction = QStringLiteral("anything");
    request.text = QStringLiteral("x");
    editor.edit(request);
    QCOMPARE(settings.count(), 1);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!editor.isBusy());
}

QTEST_MAIN(TestAiEdit)
#include "tst_aiedit.moc"
