#include "core/FileSource.h"
#include "ui/DataInspector.h"
#include "ui/DocumentView.h"
#include "ui/HexView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"

#include <QMenuBar>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>

#include <fstream>

using namespace aiedit;
using ui::HexView;

namespace {

std::shared_ptr<const core::FileSource> openSource(const std::filesystem::path& path)
{
    std::error_code ec;
    return core::FileSource::open(path, ec);
}

std::filesystem::path writeFile(const QTemporaryDir& dir, const char* name, const std::string& content)
{
    const auto path = ui::toPath(dir.filePath(QString::fromLatin1(name)));
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

// 0x00, 0x01, ..., wrapping at 256.
std::string countingBytes(std::size_t n)
{
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>(i & 0xFF);
    return s;
}

} // namespace

class TestHexView : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void keyboardNavigation();
    void shortLastRow();
    void selectionAndCopyFormats();
    void textCopyDecodesUtf8();
    void goToCentresTarget();
    void hugeSparseFileUsesScaledScrollBar();
    void emptyFile();
    void dataInspector();
    void mainWindowOpensAndDeduplicates();
    void renderScreenshot();
    void chineseTranslation();

private:
    QTemporaryDir m_dir;
};

void TestHexView::init()
{
    QVERIFY(m_dir.isValid());
}

void TestHexView::keyboardNavigation()
{
    HexView view;
    view.resize(800, 400);
    view.setSource(openSource(writeFile(m_dir, "nav.bin", countingBytes(1000))));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QSignalSpy moved(&view, &HexView::cursorMoved);
    QTest::keyClick(&view, Qt::Key_Right);
    QCOMPARE(view.cursorOffset(), 1u);
    QCOMPARE(moved.count(), 1);
    QTest::keyClick(&view, Qt::Key_Down);
    QCOMPARE(view.cursorOffset(), 17u);
    QTest::keyClick(&view, Qt::Key_End);
    QCOMPARE(view.cursorOffset(), 31u);
    QTest::keyClick(&view, Qt::Key_Home);
    QCOMPARE(view.cursorOffset(), 16u);
    QTest::keyClick(&view, Qt::Key_Up);
    QCOMPARE(view.cursorOffset(), 0u);
    QTest::keyClick(&view, Qt::Key_Up); // already on the first row
    QCOMPARE(view.cursorOffset(), 0u);
    QTest::keyClick(&view, Qt::Key_Left);
    QCOMPARE(view.cursorOffset(), 0u);

    QTest::keyClick(&view, Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(view.cursorOffset(), 999u);
    QVERIFY(view.topRow() > 0); // scrolled to keep the cursor visible
    QTest::keyClick(&view, Qt::Key_Home, Qt::ControlModifier);
    QCOMPARE(view.cursorOffset(), 0u);
    QCOMPARE(view.topRow(), 0u);

    QTest::keyClick(&view, Qt::Key_PageDown);
    QCOMPARE(view.cursorOffset(), static_cast<std::uint64_t>(16 * view.visibleRows()));

    QCOMPARE(view.activePane(), HexView::Pane::Hex);
    QTest::keyClick(&view, Qt::Key_Tab);
    QCOMPARE(view.activePane(), HexView::Pane::Text);
    QVERIFY(view.hasFocus()); // Tab switched panes instead of moving focus
}

void TestHexView::shortLastRow()
{
    HexView view;
    view.setSource(openSource(writeFile(m_dir, "short.bin", countingBytes(20))));
    view.setCursorOffset(10);
    QTest::keyClick(&view, Qt::Key_Down); // row 1 only has 4 bytes: land on the last one
    QCOMPARE(view.cursorOffset(), 19u);
    QTest::keyClick(&view, Qt::Key_Down);
    QCOMPARE(view.cursorOffset(), 19u);
    QTest::keyClick(&view, Qt::Key_End);
    QCOMPARE(view.cursorOffset(), 19u);
    view.setCursorOffset(1000); // clamped
    QCOMPARE(view.cursorOffset(), 19u);
}

void TestHexView::selectionAndCopyFormats()
{
    HexView view;
    view.setSource(openSource(writeFile(m_dir, "copy.bin", std::string("MZ\x90\x00" "AB", 6))));
    QCOMPARE(view.selectionLength(), 1u); // the cursor byte

    QSignalSpy selection(&view, &HexView::selectionChanged);
    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(view.selectionStart(), 0u);
    QCOMPARE(view.selectionLength(), 4u);
    QCOMPARE(selection.count(), 3);
    QCOMPARE(view.selectionAsHex(), QStringLiteral("4D 5A 90 00"));
    QCOMPARE(view.selectionAsText(), QStringLiteral("MZ.."));
    QCOMPARE(view.selectionAsCArray(), QStringLiteral("const unsigned char data[4] = {\n    0x4D, 0x5A, 0x90, 0x00\n};\n"));

    // Extending backwards past the anchor flips the range.
    view.setCursorOffset(4);
    QTest::keyClick(&view, Qt::Key_Left, Qt::ShiftModifier);
    QTest::keyClick(&view, Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(view.selectionStart(), 2u);
    QCOMPARE(view.selectionLength(), 3u);

    view.select(4, 100); // clamped to the end
    QCOMPARE(view.selectionStart(), 4u);
    QCOMPARE(view.selectionLength(), 2u);
    QCOMPARE(view.cursorOffset(), 5u);

    view.selectAll();
    QCOMPARE(view.selectionLength(), 6u);
}

void TestHexView::textCopyDecodesUtf8()
{
    HexView view;
    view.setSource(openSource(writeFile(m_dir, "utf8.txt", "你好\n")));
    view.selectAll();
    QCOMPARE(view.selectionAsText(), QStringLiteral("你好\n"));
}

void TestHexView::goToCentresTarget()
{
    HexView view;
    view.resize(800, 400);
    view.setSource(openSource(writeFile(m_dir, "goto.bin", countingBytes(100000))));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    view.goTo(50000);
    QCOMPARE(view.cursorOffset(), 50000u);
    const std::uint64_t row = 50000 / 16;
    QVERIFY(view.topRow() <= row && row < view.topRow() + static_cast<std::uint64_t>(view.visibleRows()));
    QVERIFY(row - view.topRow() >= static_cast<std::uint64_t>(view.visibleRows() / 2 - 1)); // roughly centred
}

void TestHexView::hugeSparseFileUsesScaledScrollBar()
{
    // 1 TiB, sparse: 2^36 rows, far more than a QScrollBar can count.
    const auto path = ui::toPath(m_dir.filePath(QStringLiteral("huge.bin")));
    std::ofstream(path, std::ios::binary).close();
    std::error_code ec;
    std::filesystem::resize_file(path, 1ull << 40, ec);
    if (ec)
        QSKIP("This file system cannot create a 1 TiB sparse file");

    HexView view;
    view.resize(800, 400);
    view.setSource(openSource(path));
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QCOMPARE(view.size(), 1ull << 40);

    QScrollBar* bar = view.verticalScrollBar();
    QCOMPARE(bar->maximum(), 1 << 30);

    QTest::keyClick(&view, Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(view.cursorOffset(), (1ull << 40) - 1);
    QCOMPARE(bar->value(), bar->maximum());

    // Dragging the scroll bar to the middle lands in the middle of the file.
    bar->setValue(bar->maximum() / 2);
    const double fraction = static_cast<double>(view.topRow()) / static_cast<double>((1ull << 36));
    QVERIFY(fraction > 0.49 && fraction < 0.51);

    view.goTo(0x123456789A);
    QCOMPARE(view.cursorOffset(), 0x123456789Aull);
    const std::uint64_t row = 0x123456789Aull / 16;
    QVERIFY(view.topRow() <= row && row < view.topRow() + static_cast<std::uint64_t>(view.visibleRows()));
}

void TestHexView::emptyFile()
{
    HexView view;
    view.setSource(openSource(writeFile(m_dir, "empty.bin", "")));
    QCOMPARE(view.selectionLength(), 0u);
    QTest::keyClick(&view, Qt::Key_Right);
    QCOMPARE(view.cursorOffset(), 0u);
    QVERIFY(view.selectionAsHex().isEmpty());
    view.grab(); // painting an empty file must not crash
}

void TestHexView::dataInspector()
{
    ui::DataInspector inspector;
    // 0x01 0x02 0x03 0x04, then 1.5f little-endian, then "中".
    inspector.setSource(openSource(writeFile(m_dir, "inspect.bin", std::string("\x01\x02\x03\x04\x00\x00\xC0\x3F\xE4\xB8\xAD", 11))));

    QCOMPARE(inspector.valueText(QStringLiteral("UInt8")), QStringLiteral("1"));
    QCOMPARE(inspector.valueText(QStringLiteral("Binary")), QStringLiteral("00000001"));
    QCOMPARE(inspector.valueText(QStringLiteral("UInt16")), QStringLiteral("513"));
    QCOMPARE(inspector.valueText(QStringLiteral("UInt32")), QStringLiteral("67305985"));
    inspector.setBigEndian(true);
    QCOMPARE(inspector.valueText(QStringLiteral("UInt16")), QStringLiteral("258"));
    QCOMPARE(inspector.valueText(QStringLiteral("UInt32")), QStringLiteral("16909060"));
    inspector.setBigEndian(false);

    inspector.setOffset(4);
    QCOMPARE(inspector.valueText(QStringLiteral("Float32")), QStringLiteral("1.5"));
    inspector.setOffset(8);
    QCOMPARE(inspector.valueText(QStringLiteral("UTF-8")), QStringLiteral("中  U+4E2D"));
    QCOMPARE(inspector.valueText(QStringLiteral("UInt64")), QStringLiteral("—")); // only 3 bytes left
    inspector.setOffset(9); // inside the character
    QCOMPARE(inspector.valueText(QStringLiteral("UTF-8")), QStringLiteral("—"));
}

void TestHexView::mainWindowOpensAndDeduplicates()
{
    const auto path = writeFile(m_dir, "window.bin", countingBytes(64));
    ui::MainWindow window;
    QCOMPARE(window.documentCount(), 0);
    QVERIFY(window.openFile(ui::toQString(path)));
    QVERIFY(window.openFile(ui::toQString(path)));
    QCOMPARE(window.documentCount(), 1);

    ui::DocumentView* doc = window.currentDocument();
    QVERIFY(doc);
    QCOMPARE(doc->fileType().kind, core::ContentKind::Binary);

    doc->hexView()->setCursorOffset(0x10);
    QCOMPARE(window.dataInspector()->valueText(QStringLiteral("UInt8")), QStringLiteral("16"));
}

void TestHexView::renderScreenshot()
{
    // Set AIEDIT_SCREENSHOT_DIR to look at the rendering; otherwise this only checks that it paints.
    std::string content = "#!/bin/sh\necho \"hello, AIEdit\"\n";
    content += std::string("\x00\x00\x00\x00\x7F" "ELF\x02\x01\x01\x00\xFF\xFE\x80\x1B\x01\x02\t\r", 22);
    content += "UTF-8: 你好，世界\n";
    content += countingBytes(200);

    ui::MainWindow window;
    window.resize(1100, 520);
    QVERIFY(window.openFile(ui::toQString(writeFile(m_dir, "sample.bin", content))));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    HexView* view = window.currentDocument()->hexView();
    view->setFocus();
    view->select(0x22, 5);
    QCoreApplication::processEvents(); // let the status bar lay out its updated labels

    const QPixmap shot = window.grab();
    QVERIFY(!shot.isNull());
    if (const QByteArray dir = qgetenv("AIEDIT_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(shot.save(QString::fromLocal8Bit(dir) + QStringLiteral("/hexview.png")));
}

void TestHexView::chineseTranslation()
{
    // Catches .ts contexts that do not match the runtime class names (e.g. a missing namespace).
    QTranslator translator;
    if (!translator.load(QStringLiteral(AIEDIT_QM_DIR "/aiedit_zh_CN.qm")))
        QSKIP("Translations were not built (lrelease missing)");
    QCoreApplication::installTranslator(&translator);

    ui::MainWindow window;
    window.resize(1100, 520);
    QCOMPARE(window.menuBar()->actions().first()->text(), QStringLiteral("文件(&F)"));
    QVERIFY(window.openFile(ui::toQString(writeFile(m_dir, "zh.txt", "你好，AIEdit\n"))));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.currentDocument()->hexView()->setFocus();
    QCoreApplication::processEvents();
    if (const QByteArray dir = qgetenv("AIEDIT_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(window.grab().save(QString::fromLocal8Bit(dir) + QStringLiteral("/hexview_zh.png")));

    QCoreApplication::removeTranslator(&translator);
}

QTEST_MAIN(TestHexView)
#include "tst_hexview.moc"
