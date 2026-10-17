#include "core/FileSource.h"
#include "core/TextCodec.h"
#include "ui/Desktop.h"
#include "ui/DocumentView.h"
#include "ui/HexView.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"
#include "ui/TextView.h"

#include <QApplication>
#include <QFontDatabase>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>

using namespace aiedit;

namespace {

bool isDark(const QColor& color)
{
    return color.lightness() < 128;
}

QString aDarkScheme()
{
    for (const ui::ColorScheme& scheme : ui::availableColorSchemes())
        if (scheme.dark)
            return scheme.name;
    return {};
}

QString aLightScheme()
{
    for (const ui::ColorScheme& scheme : ui::availableColorSchemes())
        if (!scheme.dark)
            return scheme.name;
    return {};
}

// The colour the view shows at a point of its text area with nothing written there.
QColor backgroundOf(QAbstractScrollArea& view)
{
    const QImage image = view.viewport()->grab().toImage();
    return image.pixelColor(image.width() - 5, image.height() - 5);
}

} // namespace

class TestColorScheme : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void listsSchemes();
    void colorsTheViews();
    void highlightsInTheSchemesColors();
    void followsTheSystemAgain();
    void remembersTheChoice();
    void remembersTheEditorFont();

private:
    QString write(const char* name, const std::string& content);

    QTemporaryDir m_dir;
    QColor m_systemWindow;
};

void TestColorScheme::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_colorscheme"));
    m_systemWindow = QApplication::palette().color(QPalette::Window);
}

void TestColorScheme::init()
{
    QVERIFY(m_dir.isValid());
    QSettings().clear();
}

void TestColorScheme::cleanup()
{
    ui::setColorScheme({}); // the next test starts from the system's colours
}

QString TestColorScheme::write(const char* name, const std::string& content)
{
    const QString path = m_dir.filePath(QString::fromLatin1(name));
    std::ofstream(ui::toPath(path), std::ios::binary) << content;
    return path;
}

void TestColorScheme::listsSchemes()
{
    const QList<ui::ColorScheme> schemes = ui::availableColorSchemes();
    QVERIFY(schemes.size() >= 2);
    // Light ones first.
    const auto firstDark = std::find_if(schemes.begin(), schemes.end(), [](const auto& s) { return s.dark; });
    QVERIFY(firstDark != schemes.begin() && firstDark != schemes.end());
    QVERIFY(std::all_of(firstDark, schemes.end(), [](const auto& s) { return s.dark; }));
    QStringList names;
    for (const ui::ColorScheme& scheme : schemes)
        names << scheme.name;
    if (ui::isSyntaxHighlightingAvailable()) {
        QVERIFY(names.contains(QStringLiteral("Monokai")));
        QVERIFY(names.contains(QStringLiteral("Solarized Light")));
        QVERIFY(!names.contains(QStringLiteral("Printing"))); // for paper
    } else {
        QCOMPARE(names, (QStringList{QStringLiteral("Light"), QStringLiteral("Dark")}));
    }
}

void TestColorScheme::colorsTheViews()
{
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source =
        core::FileSource::open(ui::toPath(write("colors.txt", "some text\n")), ec);
    ui::DocumentView doc(source, core::detectFileType(source->bytes()), core::utf8Codec());
    doc.resize(600, 300);
    doc.setViewMode(ui::DocumentView::ViewMode::Split);
    doc.show();
    QVERIFY(QTest::qWaitForWindowExposed(&doc));

    for (const QString& name : {aDarkScheme(), aLightScheme()}) {
        ui::setColorScheme(name);
        QCOMPARE(ui::colorScheme(), name);
        QCoreApplication::processEvents();
        const ui::EditorColors colors = ui::editorColors(doc.textView()->palette());
        QCOMPARE(isDark(colors.background), name == aDarkScheme());
        // The whole application, and the views painted in the scheme's colours.
        QCOMPARE(isDark(QApplication::palette().color(QPalette::Window)), name == aDarkScheme());
        QCOMPARE(backgroundOf(*doc.textView()), colors.background);
        QCOMPARE(backgroundOf(*doc.hexView()), colors.background);
    }
}

void TestColorScheme::highlightsInTheSchemesColors()
{
    if (!ui::isSyntaxHighlightingAvailable())
        QSKIP("built without KSyntaxHighlighting");
    std::error_code ec;
    const std::shared_ptr<const core::FileSource> source =
        core::FileSource::open(ui::toPath(write("code.cpp", "int main() { return 0; }\n")), ec);
    ui::TextDocument document(source, core::utf8Codec());
    ui::TextView view;
    view.setDocument(&document);
    view.setSyntax(QStringLiteral("C++"));
    const auto colorsOfLine = [&view] {
        QList<QColor> colors;
        for (const QTextLayout::FormatRange& range : view.rowFormats(0))
            colors << range.format.foreground().color();
        return colors;
    };
    ui::setColorScheme(QStringLiteral("Solarized Light"));
    QCoreApplication::processEvents();
    const QList<QColor> solarized = colorsOfLine();
    ui::setColorScheme(QStringLiteral("Monokai"));
    QCoreApplication::processEvents();
    const QList<QColor> monokai = colorsOfLine();
    QVERIFY(!solarized.isEmpty() && !monokai.isEmpty());
    QVERIFY(solarized != monokai); // the scheme's colours, not one that suits the palette
}

void TestColorScheme::followsTheSystemAgain()
{
    ui::setColorScheme(aDarkScheme());
    QVERIFY(QApplication::palette().color(QPalette::Window) != m_systemWindow);
    ui::setColorScheme({});
    QVERIFY(ui::colorScheme().isEmpty());
    QCOMPARE(QApplication::palette().color(QPalette::Window), m_systemWindow);

    ui::setColorScheme(QStringLiteral("No Such Scheme")); // unknown: the system's
    QVERIFY(ui::colorScheme().isEmpty());
    QCOMPARE(QApplication::palette().color(QPalette::Window), m_systemWindow);
}

void TestColorScheme::remembersTheChoice()
{
    const QString dark = aDarkScheme();
    QSettings().setValue(QStringLiteral("view/colorScheme"), dark);
    {
        ui::MainWindow window; // applies what was chosen before
        QCOMPARE(ui::colorScheme(), dark);
    }

    // Choosing from View > Color Scheme is remembered.
    ui::MainWindow window;
    QMenu* menu = nullptr;
    for (QMenu* m : window.menuBar()->findChildren<QMenu*>())
        if (m->title() == QStringLiteral("&Color Scheme"))
            menu = m;
    QVERIFY(menu);
    emit menu->aboutToShow();
    QAction* light = nullptr;
    QAction* system = nullptr;
    for (QAction* action : menu->actions()) {
        if (action->data().toString() == aLightScheme() && !action->isSeparator())
            light = action;
        if (action->text() == QStringLiteral("Follow the &System"))
            system = action;
    }
    QVERIFY(light && system);
    QVERIFY(!light->isChecked());
    light->trigger();
    QCOMPARE(ui::colorScheme(), aLightScheme());
    QCOMPARE(QSettings().value(QStringLiteral("view/colorScheme")).toString(), aLightScheme());
    emit menu->aboutToShow();
    QVERIFY(light->isChecked());
    system->trigger();
    QVERIFY(ui::colorScheme().isEmpty());
    QVERIFY(QSettings().value(QStringLiteral("view/colorScheme")).toString().isEmpty());
}

void TestColorScheme::remembersTheEditorFont()
{
    const QString standard = ui::fixedFont().family();
    QString other;
    for (const QString& family : QFontDatabase::families()) {
        if (family != standard && QFontDatabase::isFixedPitch(family)) {
            other = family;
            break;
        }
    }
    if (other.isEmpty())
        QSKIP("no other fixed-width font installed");
    QSettings().setValue(QStringLiteral("view/fontFamily"), other);
    ui::MainWindow window;
    QVERIFY(window.openFile(write("font.txt", "text\n")));
    QCOMPARE(window.currentDocument()->textView()->font().family(), other);
    QCOMPARE(window.currentDocument()->hexView()->font().family(), other);
}

QTEST_MAIN(TestColorScheme)
#include "tst_colorscheme.moc"
