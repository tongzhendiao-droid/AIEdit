#include "core/Disassembler.h"
#include "core/FileSource.h"
#include "ui/AiPanel.h"
#include "ui/Compare.h"
#include "ui/CompareTab.h"
#include "ui/CompareView.h"
#include "ui/DocumentView.h"
#include "ui/FolderPanel.h"
#include "ui/HexCompareView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextView.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringEncoder>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include "MockChatServer.h"

#include <memory>

using namespace aiedit;
using Kind = ui::CompareRows::Kind;
using Hunks = std::vector<core::DiffHunk>;

namespace {

QList<QByteArray> answer(const QString& text)
{
    return {aiedit::test::chunk(text), aiedit::test::chunk({}, QStringLiteral("content"), QStringLiteral("stop")),
            "data: [DONE]\n\n"};
}

ui::CompareSide side(const QByteArray& bytes, const char* codec = "UTF-8", const QString& label = QStringLiteral("x"))
{
    auto data = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    return ui::bytesSide(label, core::FileSource::fromBytes({}, data), *core::findCodec(codec));
}

Hunks compare(const QByteArray& left, const QByteArray& right, const ui::CompareOptions& options = {})
{
    return ui::compareText(side(left), side(right), options).hunks;
}

// A small 64-bit Linux program: its .text holds "push rbp; mov rbp, rsp; mov eax, <value>; pop rbp;
// ret" at offset 0x1000, address 0x401000.
QByteArray elfProgram(std::uint8_t value)
{
    QByteArray data(0x2000, '\0');
    const auto put = [&data](int offset, std::uint64_t number, int size) {
        for (int i = 0; i < size; ++i)
            data[offset + i] = static_cast<char>(number >> (8 * i));
    };
    data.replace(0, 4, QByteArray("\x7F" "ELF", 4));
    data[4] = 2;
    data[5] = 1;
    data[6] = 1;
    put(16, 2, 2);
    put(18, 62, 2);
    put(40, 0x1800, 8);
    put(58, 64, 2);
    put(60, 3, 2);
    put(62, 2, 2);
    const QByteArray code("\x55\x48\x89\xE5\xB8\x01\x00\x00\x00\x5D\xC3", 11);
    data.replace(0x1000, code.size(), code);
    data[0x1005] = static_cast<char>(value);
    const QByteArray names("\0.text\0.shstrtab\0", 17);
    data.replace(0x1100, names.size(), names);
    put(0x1800 + 64, 1, 4);
    put(0x1800 + 64 + 4, 1, 4);
    put(0x1800 + 64 + 8, 0x6, 8);
    put(0x1800 + 64 + 16, 0x401000, 8);
    put(0x1800 + 64 + 24, 0x1000, 8);
    put(0x1800 + 64 + 32, 11, 8);
    put(0x1800 + 128, 7, 4);
    put(0x1800 + 128 + 4, 3, 4);
    put(0x1800 + 128 + 24, 0x1100, 8);
    put(0x1800 + 128 + 32, 17, 8);
    return data;
}

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

} // namespace

class TestCompare : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void comparesLines();
    void ignoresWhatItIsTold();
    void comparesTextInDifferentCodecs();
    void laysOutRows();
    void foldsWhatIsTheSame();
    void writesAUnifiedDiff();

    void comparesInATab();
    void movesFromChangeToChange();
    void copiesAndOpensLines();
    void comparesADocumentWithItsFile();
    void comparesWithTheClipboard();
    void comparesTwoFilesFromTheFolder();
    void asksTheAiWhatChanged();
    void comparesBinariesByteByByte();
    void notesBytesShiftedByAnInsertion();
    void comparesTextAsBytesWhenAsked();
    void asksTheAiAboutABinary();

private:
    QString write(const QString& name, const QByteArray& content);
    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestCompare::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_compare"));
}

void TestCompare::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QSettings().clear();
}

QString TestCompare::write(const QString& name, const QByteArray& content)
{
    const QString path = QDir(QFileInfo(m_dir->path()).canonicalFilePath()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    return path;
}

void TestCompare::comparesLines()
{
    QVERIFY(compare("a\nb\nc\n", "a\nb\nc\n").empty());
    QVERIFY((compare("a\nb\nc\n", "a\nx\nc\n") == Hunks{{1, 1, 1, 1}}));
    QVERIFY((compare("a\nb\nc\n", "a\nc\n") == Hunks{{1, 1, 1, 0}}));
    QVERIFY((compare("", "a\n") == Hunks{{0, 0, 0, 1}}));
    // Lines longer than an index row are compared whole.
    const QByteArray longLine(10'000, 'x');
    QVERIFY(compare(longLine + "\n", longLine + "\n").empty());
    QVERIFY((compare(longLine + "\n", longLine + "y\n") == Hunks{{0, 1, 0, 1}}));

    const ui::TextComparison same = ui::compareText(side("a\n"), side("a\n"), {});
    QVERIFY(same.identicalBytes);
    QCOMPARE(same.left->lineCount(), 1u);
}

void TestCompare::ignoresWhatItIsTold()
{
    // Line endings are ignored unless asked otherwise, and so is a missing last line break.
    QVERIFY(compare("a\r\nb\r\n", "a\nb").empty());
    ui::CompareOptions endings;
    endings.ignoreLineEndings = false;
    QVERIFY((compare("a\r\nb\n", "a\nb\n", endings) == Hunks{{0, 1, 0, 1}}));

    QVERIFY(!compare("Hello World\n", "hello  world\n").empty());
    ui::CompareOptions loose;
    loose.ignoreCase = true;
    loose.ignoreWhitespace = true;
    QVERIFY(compare("Hello World\n", "hello  world\n", loose).empty());
    QVERIFY(compare("if (a)\n", "if(a)\n", loose).empty());
    QVERIFY(!compare("abc\n", "abd\n", loose).empty());
}

void TestCompare::comparesTextInDifferentCodecs()
{
    // A GBK file and its UTF-8 copy hold the same text.
    const QString text = QStringLiteral("话说天下大势，分久必合，合久必分。\n周末七国分争，并入于秦。\n");
    const QByteArray gbk = QStringEncoder("GB18030").encode(text);
    const ui::TextComparison same = ui::compareText(side(gbk, "GB18030"), side(text.toUtf8()), {});
    QVERIFY(!same.identicalBytes);
    QVERIFY(same.hunks.empty());

    const QByteArray changed = QStringEncoder("GB18030").encode(QString(text).replace(QStringLiteral("秦"), QStringLiteral("汉")));
    QVERIFY((ui::compareText(side(changed, "GB18030"), side(text.toUtf8()), {}).hunks == Hunks{{1, 1, 1, 1}}));
}

void TestCompare::laysOutRows()
{
    ui::CompareRows rows;
    // 10 lines; line 2 changed, 5 and 6 removed, one added after 8.
    rows.build({{2, 1, 2, 1}, {5, 2, 5, 0}, {9, 0, 7, 1}}, 10, 9, false);
    QCOMPARE(rows.count(), 11u); // 10 lines, and a gap row for the line added
    QCOMPARE(rows.hunkCount(), 3);
    QCOMPARE(rows.at(0).kind, Kind::Same);
    QCOMPARE(rows.at(2).kind, Kind::Changed);
    QCOMPARE(*rows.at(2).left, 2u);
    QCOMPARE(*rows.at(2).right, 2u);
    QCOMPARE(rows.at(5).kind, Kind::Removed);
    QVERIFY(!rows.at(5).right);
    QCOMPARE(rows.at(6).kind, Kind::Removed);
    QCOMPARE(*rows.at(7).left, 7u);
    QCOMPARE(*rows.at(7).right, 5u);
    QCOMPARE(rows.at(9).kind, Kind::Added);
    QVERIFY(!rows.at(9).left);
    QCOMPARE(*rows.at(9).right, 7u);
    QCOMPARE(*rows.at(10).left, 9u);

    QCOMPARE(rows.firstRowOf(1), 5u);
    QCOMPARE(rows.hunkAtOrAfter(3), 1);
    QCOMPARE(rows.hunkAtOrAfter(6), 1);
    QCOMPARE(rows.hunkAtOrAfter(10), -1);
    QCOMPARE(rows.hunkBefore(5), 0);
    QCOMPARE(rows.hunkBefore(2), -1);
    QCOMPARE(rows.rowOfLine(true, 7), 7u);
    QCOMPARE(rows.rowOfLine(false, 7), 9u);
}

void TestCompare::foldsWhatIsTheSame()
{
    ui::CompareRows rows;
    // 100 lines, one change at line 50.
    rows.build({{50, 1, 50, 1}}, 100, 100, true);
    // A fold for lines 0-46, three lines, the change, three lines, a fold for the rest.
    QCOMPARE(rows.count(), 9u);
    QCOMPARE(rows.at(0).kind, Kind::Fold);
    QCOMPARE(rows.at(0).folded, 47u);
    QCOMPARE(*rows.at(1).left, 47u);
    QCOMPARE(rows.at(4).kind, Kind::Changed);
    QCOMPARE(rows.at(8).kind, Kind::Fold);
    QCOMPARE(rows.at(8).folded, 46u);
    QCOMPARE(rows.rowOfLine(true, 10), 0u); // inside the first fold
    QVERIFY(!rows.unfold(4));
    QVERIFY(rows.unfold(0));
    QCOMPARE(rows.count(), 55u); // 50 lines, the change, three lines and the last fold
    QCOMPARE(*rows.at(0).left, 0u);
    QCOMPARE(rows.at(54).kind, Kind::Fold);

    // Identical files fold into one row.
    rows.build({}, 1000, 1000, true);
    QCOMPARE(rows.count(), 1u);
    QCOMPARE(rows.at(0).folded, 1000u);
}

void TestCompare::writesAUnifiedDiff()
{
    QByteArray left;
    QByteArray right;
    for (int i = 1; i <= 20; ++i) {
        left += "line " + QByteArray::number(i) + "\n";
        right += (i == 10 ? QByteArray("LINE 10") : "line " + QByteArray::number(i)) + "\n";
    }
    const ui::CompareSide a = side(left, "UTF-8", QStringLiteral("a.txt"));
    const ui::CompareSide b = side(right, "UTF-8", QStringLiteral("b.txt"));
    const ui::TextComparison comparison = ui::compareText(a, b, {});
    bool cut = true;
    const QString diff = ui::unifiedDiff(a, b, comparison, 10'000, &cut);
    QVERIFY(!cut);
    QCOMPARE(diff, QStringLiteral("--- a.txt\n+++ b.txt\n@@ -7,7 +7,7 @@\n line 7\n line 8\n line 9\n-line 10\n+LINE 10\n"
                                  " line 11\n line 12\n line 13\n"));
    QVERIFY(ui::unifiedDiff(a, b, comparison, 40, &cut).size() == 40);
    QVERIFY(cut);
}

void TestCompare::comparesInATab()
{
    const QString a = write(QStringLiteral("a.txt"), "one\ntwo\nthree\n");
    const QString b = write(QStringLiteral("b.txt"), "one\n2\nthree\nfour\n");
    std::optional<ui::CompareSide> left = ui::fileSide(a, "GB18030", nullptr);
    std::optional<ui::CompareSide> right = ui::fileSide(b, "GB18030", nullptr);
    QVERIFY(left && right);
    ui::CompareTab tab(*left, *right);
    tab.resize(900, 500);
    tab.show();
    QSignalSpy compared(&tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    QCOMPARE(tab.title(), QStringLiteral("a.txt ↔ b.txt"));
    QCOMPARE(tab.comparison().hunks.size(), 2u);
    QCOMPARE(tab.statusText(), QStringLiteral("2 change(s): 1 line(s) removed, 2 added"));
    ui::CompareView* view = tab.view();
    QCOMPARE(view->currentRow(), 1u); // at the first change
    QCOMPARE(view->rowText(1, true), QStringLiteral("two"));
    QCOMPARE(view->rowText(1, false), QStringLiteral("2"));
    QVERIFY(view->rowText(3, true).isEmpty()); // a gap across from "four"

    // An option compares again; so does Refresh, with what is on disk now.
    tab.setOptions({true, true, true});
    QTRY_COMPARE(compared.count(), 2);
    write(QStringLiteral("b.txt"), "one\ntwo\nthree\n");
    tab.refresh();
    QTRY_COMPARE(compared.count(), 3);
    QVERIFY(tab.comparison().hunks.empty());
    QCOMPARE(tab.statusText(), QStringLiteral("The two are identical"));

    tab.swapSides();
    QTRY_COMPARE(compared.count(), 4);
    QCOMPARE(tab.title(), QStringLiteral("b.txt ↔ a.txt"));
}

void TestCompare::movesFromChangeToChange()
{
    QByteArray left;
    QByteArray right;
    for (int i = 0; i < 200; ++i) {
        left += "line " + QByteArray::number(i) + "\n";
        right += (i % 50 == 25 ? QByteArray("changed") : "line " + QByteArray::number(i)) + "\n";
    }
    ui::CompareTab tab(side(left), side(right));
    tab.resize(900, 300);
    tab.show();
    QSignalSpy compared(&tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    ui::CompareView* view = tab.view();
    QCOMPARE(view->rows().hunkCount(), 4);
    QCOMPARE(view->currentHunk(), 0);
    QAction* next = findAction(tab, QStringLiteral("Next ▶"));
    QAction* previous = findAction(tab, QStringLiteral("◀ Previous"));
    QVERIFY(next && previous);
    QVERIFY(!previous->isEnabled());
    next->trigger();
    QCOMPARE(view->currentHunk(), 1);
    QCOMPARE(view->rowText(view->currentRow(), false), QStringLiteral("changed"));
    next->trigger();
    next->trigger();
    QCOMPARE(view->currentHunk(), 3);
    QVERIFY(!next->isEnabled());
    QVERIFY(!view->goToNextDifference());
    previous->trigger();
    QCOMPARE(view->currentHunk(), 2);

    // Only the differences: the rest folds, and the change stays current.
    tab.setOnlyDifferences(true);
    QCOMPARE(view->currentHunk(), 2);
    QVERIFY(view->rows().count() < 50);
    QCOMPARE(view->rows().at(0).kind, Kind::Fold);
}

void TestCompare::copiesAndOpensLines()
{
    ui::CompareTab tab(side("same\nold line\n"), side("same\nnew line\n"));
    QSignalSpy compared(&tab, &ui::CompareTab::compared);
    tab.resize(800, 300);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    QTRY_COMPARE(compared.count(), 1);
    ui::CompareView* view = tab.view();
    view->setFocus();
    view->setLeftActive(false);
    view->copy();
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("new line"));
    QTest::keyClick(view, Qt::Key_Left);
    QVERIFY(view->isLeftActive());
    QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("old line"));

    QSignalSpy open(view, &ui::CompareView::openRequested);
    QTest::keyClick(view, Qt::Key_Return);
    QCOMPARE(open.count(), 1);
    QCOMPARE(open.last().at(0).toBool(), true);
    QCOMPARE(open.last().at(1).toULongLong(), 1u);
}

void TestCompare::comparesADocumentWithItsFile()
{
    const QString path = write(QStringLiteral("notes.txt"), "alpha\nbeta\ngamma\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(path));
    ui::DocumentView* doc = window.currentDocument();
    QTRY_VERIFY(doc->textView()->goToLine(1));
    doc->textView()->setFocus();
    QTest::keyClicks(doc->textView(), QStringLiteral("very "));
    QVERIFY(doc->isModified());

    ui::CompareTab* tab = window.compareWithSaved();
    QVERIFY(tab);
    QSignalSpy compared(tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    QVERIFY((tab->comparison().hunks == Hunks{{1, 1, 1, 1}}));
    QCOMPARE(tab->view()->rowText(1, false), QStringLiteral("very beta"));
    QCOMPARE(tab->title(), QStringLiteral("notes.txt on disk ↔ notes.txt (edited)"));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.currentCompareTab(), tab);
    QVERIFY(!window.currentDocument());

    // More edits show on Refresh.
    window.findChild<QTabWidget*>()->setCurrentIndex(0);
    QTest::keyClicks(doc->textView(), QStringLiteral("!"));
    window.findChild<QTabWidget*>()->setCurrentWidget(tab);
    tab->refresh();
    QTRY_COMPARE(compared.count(), 2);
    QCOMPARE(tab->view()->rowText(1, false), QStringLiteral("very !beta"));

    // Opening a line goes to the document, which is the file.
    emit tab->view()->openRequested(false, 2);
    QCOMPARE(window.currentDocument(), doc);
    QCOMPARE(doc->textView()->cursorLine(), 2u);

    // A comparison tab closes without a question and is not part of the session.
    const int index = window.findChild<QTabWidget*>()->indexOf(tab);
    window.findChild<QTabWidget*>()->tabCloseRequested(index);
    QCOMPARE(window.documentCount(), 1);
}

void TestCompare::comparesWithTheClipboard()
{
    const QString path = write(QStringLiteral("list.txt"), "apples\npears\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(path));
    QGuiApplication::clipboard()->setText(QStringLiteral("apples\nplums\n"));
    ui::CompareTab* tab = window.compareWithClipboard();
    QVERIFY(tab);
    QSignalSpy compared(tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    QVERIFY((tab->comparison().hunks == Hunks{{1, 1, 1, 1}}));
    QCOMPARE(tab->title(), QStringLiteral("list.txt ↔ Clipboard"));
    QCOMPARE(tab->view()->rowText(1, false), QStringLiteral("plums"));
}

void TestCompare::comparesTwoFilesFromTheFolder()
{
    const QString a = write(QStringLiteral("v1.txt"), "x\n");
    const QString b = write(QStringLiteral("v2.txt"), "y\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFolder(QFileInfo(a).absolutePath()));
    auto* panel = window.findChild<ui::FolderPanel*>();
    QTRY_COMPARE(panel->model()->rowCount(panel->tree()->rootIndex()), 2);
    QAction* compareAction = findAction(*panel, QStringLiteral("Co&mpare These Two Files"));
    QVERIFY(compareAction);
    panel->select(b);
    QVERIFY(!compareAction->isEnabled());
    panel->tree()->selectionModel()->select(panel->model()->index(a),
                                             QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QVERIFY(compareAction->isEnabled());
    compareAction->trigger();
    ui::CompareTab* tab = window.currentCompareTab();
    QVERIFY(tab);
    QCOMPARE(tab->title(), QStringLiteral("v1.txt ↔ v2.txt")); // in name order, whichever was picked first
}

void TestCompare::asksTheAiWhatChanged()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("把 count 改名为 total。"));
    const QString a = write(QStringLiteral("old.c"), "int count = 1;\nreturn count;\n");
    const QString b = write(QStringLiteral("new.c"), "int total = 1;\nreturn total;\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.findChild<ui::AiPanel*>()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    ui::CompareTab* tab = window.compareFiles(a, b);
    QVERIFY(tab);
    QSignalSpy compared(tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    findAction(*tab, QStringLiteral("Ask AI What Changed"))->trigger();
    QTRY_COMPARE(server.requests, 1);
    const QString body = QString::fromUtf8(server.body);
    QVERIFY(body.contains(QStringLiteral("What changed between these two versions?")));
    QVERIFY(body.contains(QStringLiteral("-int count = 1;")));
    QVERIFY(body.contains(QStringLiteral("+int total = 1;")));
    QTRY_VERIFY(window.findChild<ui::AiPanel*>()->transcript().contains(QStringLiteral("改名为 total")));
}

void TestCompare::comparesBinariesByteByByte()
{
    const QString a = write(QStringLiteral("one"), elfProgram(1));
    const QString b = write(QStringLiteral("two"), elfProgram(2));
    std::optional<ui::CompareSide> left = ui::fileSide(a, "GB18030", nullptr);
    std::optional<ui::CompareSide> right = ui::fileSide(b, "GB18030", nullptr);
    ui::CompareTab tab(*left, *right);
    QSignalSpy compared(&tab, &ui::CompareTab::compared);
    tab.resize(1000, 600);
    tab.show();
    QTRY_COMPARE(compared.count(), 1);
    QVERIFY(tab.isBinary());
    QVERIFY((tab.byteComparison().ranges == std::vector<core::ByteRange>{{0x1005, 1}}));
    QCOMPARE(tab.statusText(), QStringLiteral("1 difference(s), 1 bytes in all · ELF64 x86-64"));
    QCOMPARE(tab.hexView()->currentRange(), 0);
    QCOMPARE(tab.hexView()->cursorOffset(), 0x1005u);
    const QString details = tab.detailsText();
    QVERIFY2(details.contains(QStringLiteral("in .text, address 0x401005, code")), qPrintable(details));
    if (core::canDisassemble(core::Architecture::X86_64)) {
        QVERIFY(details.contains(QStringLiteral("mov eax, 1")));
        QVERIFY(details.contains(QStringLiteral("mov eax, 2")));
    }
    // Options about lines mean nothing here; bytes are compared whatever is asked.
    int checked = 0;
    for (QCheckBox* box : tab.findChildren<QCheckBox*>()) {
        if (box->text() == QStringLiteral("Ignore whitespace")) {
            QVERIFY(!box->isEnabled());
            ++checked;
        } else if (box->text() == QStringLiteral("Compare bytes")) {
            QVERIFY(box->isChecked() && !box->isEnabled());
            ++checked;
        }
    }
    QCOMPARE(checked, 2);
}

void TestCompare::notesBytesShiftedByAnInsertion()
{
    QByteArray bytes(20'000, '\0');
    std::uint32_t seed = 5;
    for (char& byte : bytes) {
        seed = seed * 1103515245u + 12345u;
        byte = static_cast<char>(seed >> 16);
    }
    bytes[0] = '\0'; // binary, even by its first bytes
    QByteArray inserted = bytes;
    inserted.insert(6'000, QByteArray(8, '\x90'));
    ui::CompareTab tab(side(bytes), side(inserted));
    QSignalSpy compared(&tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    QVERIFY(tab.isBinary());
    QVERIFY(tab.statusText().contains(QStringLiteral("shifted by 8 bytes from 0x1770")));
    QVERIFY(tab.detailsText().contains(QStringLiteral("shifted by 8 bytes")));
}

void TestCompare::comparesTextAsBytesWhenAsked()
{
    ui::CompareTab tab(side("abc\n"), side("abd\n"));
    QSignalSpy compared(&tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    QVERIFY(!tab.isBinary());
    tab.setCompareBytes(true);
    QTRY_COMPARE(compared.count(), 2);
    QVERIFY(tab.isBinary());
    QVERIFY((tab.byteComparison().ranges == std::vector<core::ByteRange>{{2, 1}}));
    QVERIFY(tab.detailsText().contains(QStringLiteral("64 0A"))); // "d\n" on the right
}

void TestCompare::asksTheAiAboutABinary()
{
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("返回值从 1 改成了 2。"));
    const QString a = write(QStringLiteral("one"), elfProgram(1));
    const QString b = write(QStringLiteral("two"), elfProgram(2));
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.findChild<ui::AiPanel*>()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    ui::CompareTab* tab = window.compareFiles(a, b);
    QSignalSpy compared(tab, &ui::CompareTab::compared);
    QTRY_COMPARE(compared.count(), 1);
    findAction(*tab, QStringLiteral("Ask AI What Changed"))->trigger();
    QTRY_COMPARE(server.requests, 1);
    const QString body = QString::fromUtf8(server.body);
    QVERIFY(body.contains(QStringLiteral("differences between two binary files")));
    QVERIFY(body.contains(QStringLiteral("Difference 1: offset 0x1005-0x1005 (1 bytes)")));
    QVERIFY(body.contains(QStringLiteral("in .text, address 0x401005, code")));
    QVERIFY(body.contains(QStringLiteral("left : 01")));
    QVERIFY(body.contains(QStringLiteral("right: 02")));
    if (core::canDisassemble(core::Architecture::X86_64))
        QVERIFY(body.contains(QStringLiteral("mov eax, 2   <-- differs")));

    // Opening a byte goes to the file, in the hex view, there.
    emit tab->hexView()->openRequested(false, 0x1005);
    QVERIFY(window.currentDocument());
    QCOMPARE(window.currentDocument()->filePath(), b);
    QCOMPARE(window.currentDocument()->viewMode(), ui::DocumentView::ViewMode::Hex);
    QCOMPARE(window.currentDocument()->cursorOffset(), 0x1005u);
}

QTEST_MAIN(TestCompare)
#include "tst_compare.moc"
