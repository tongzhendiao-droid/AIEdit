#include "ui/SyntaxHighlighting.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QPalette>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QStyleHints>

#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>
#endif

#include <algorithm>
#include <optional>
#include <tuple>

namespace aiedit::ui {

namespace {

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

// The scheme chosen: a name, "Auto", or empty for the system's colours; and whether the
// application palette was ever set for one (so that the system's colours need undoing that).
// GUI thread only.
QString& currentScheme()
{
    static QString name;
    return name;
}

// AIEdit's own schemes. Their syntax colours are in themes/*.theme, which KSyntaxHighlighting
// finds among the resources; without it, these colours are all they have.
struct OwnScheme
{
    const char* name;
    const char* displayName; // translated where shown
    QRgb background;
    QRgb text;
    QRgb selection;
    QRgb link;
    bool dark;
};

constexpr OwnScheme kOwnSchemes[] = {
    {"AIEdit Paper", QT_TRANSLATE_NOOP("aiedit::ui::ColorScheme", "Paper"), 0xF6F1E7, 0x3B3530, 0xE3D5B8, 0x076678,
     false},
    {"AIEdit Eye Care", QT_TRANSLATE_NOOP("aiedit::ui::ColorScheme", "Eye Care"), 0xC7EDCC, 0x1F2D21, 0x9FD4A8,
     0x0B4F73, false},
    {"AIEdit Night", QT_TRANSLATE_NOOP("aiedit::ui::ColorScheme", "Night"), 0x23211E, 0xDDD2C0, 0x4A4134, 0x7DAEA3,
     true},
};
constexpr const char* kAutoLight = "AIEdit Paper";
constexpr const char* kAutoDark = "AIEdit Night";

const OwnScheme* ownScheme(const QString& name)
{
    // Also the light and dark ones AIEdit had before, for what was saved then.
    const QString wanted = name == u"Light"  ? QString::fromLatin1(kAutoLight)
                           : name == u"Dark" ? QString::fromLatin1(kAutoDark)
                                             : name;
    for (const OwnScheme& scheme : kOwnSchemes) {
        if (wanted == QLatin1StringView(scheme.name))
            return &scheme;
    }
    return nullptr;
}

QString effectiveScheme()
{
    if (currentScheme() != QLatin1StringView(kAutoColorScheme))
        return currentScheme();
    const bool dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    return QString::fromLatin1(dark ? kAutoDark : kAutoLight);
}

bool& paletteChanged()
{
    static bool changed = false;
    return changed;
}

// An application palette from the few colours a scheme has: the text area's background and
// text, the selection, and a colour for links. Panels sit a little apart from the text.
QPalette paletteFrom(const QColor& background, const QColor& text, const QColor& selection, const QColor& link)
{
    const bool dark = background.lightness() < 128;
    const QColor window = mix(background, text, dark ? 0.07f : 0.05f);
    const QColor button = mix(background, text, dark ? 0.12f : 0.08f);
    const QColor disabled = mix(text, background, 0.5f);
    QPalette palette(button, window); // works out the shades frames are drawn with
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, background);
    palette.setColor(QPalette::AlternateBase, mix(background, text, 0.04f));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, dark ? QColor(Qt::white) : QColor(Qt::black));
    palette.setColor(QPalette::Highlight, selection);
    palette.setColor(QPalette::HighlightedText, text);
    palette.setColor(QPalette::ToolTipBase, background);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Link, link);
    palette.setColor(QPalette::LinkVisited, link);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    // What the Windows 11 style fills a ticked check box and a default button with; unset, it drew
    // ticked boxes black on a dark scheme and white on a light one, the tick unseen.
    palette.setColor(QPalette::Accent, link);
#endif
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, disabled);
    return palette;
}

} // namespace

#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING

namespace {

// Loading all definitions takes a moment, so the whole program shares one repository.
// It is only touched from the GUI thread.
KSyntaxHighlighting::Repository& repository()
{
    static KSyntaxHighlighting::Repository instance;
    return instance;
}

// The scheme's theme; invalid with the system's colours.
KSyntaxHighlighting::Theme schemeTheme()
{
    const QString scheme = effectiveScheme();
    return scheme.isEmpty() ? KSyntaxHighlighting::Theme() : repository().theme(scheme);
}

QColor editorColor(const KSyntaxHighlighting::Theme& theme, KSyntaxHighlighting::Theme::EditorColorRole role)
{
    return QColor::fromRgba(theme.editorColor(role));
}

// Collects the formats KSyntaxHighlighting reports for one line as QTextLayout ranges.
class FormatCollector final : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    QList<QTextLayout::FormatRange> ranges;

    KSyntaxHighlighting::State run(QStringView text, const KSyntaxHighlighting::State& state)
    {
        ranges.clear();
        return highlightLine(text, state);
    }

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format& format) override
    {
        const KSyntaxHighlighting::Theme style = theme();
        if (length <= 0 || format.isDefaultTextStyle(style))
            return;
        QTextCharFormat charFormat;
        if (format.hasTextColor(style))
            charFormat.setForeground(format.textColor(style));
        if (format.hasBackgroundColor(style))
            charFormat.setBackground(format.backgroundColor(style));
        if (format.isBold(style))
            charFormat.setFontWeight(QFont::Bold);
        charFormat.setFontItalic(format.isItalic(style));
        charFormat.setFontUnderline(format.isUnderline(style));
        charFormat.setFontStrikeOut(format.isStrikeThrough(style));
        ranges.append({offset, length, charFormat});
    }
};

} // namespace

struct SyntaxState::Impl
{
    KSyntaxHighlighting::State state;
};

struct SyntaxHighlighter::Impl
{
    FormatCollector collector;
};

#else

struct SyntaxState::Impl
{
};

struct SyntaxHighlighter::Impl
{
};

#endif

bool isSyntaxHighlightingAvailable()
{
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    return true;
#else
    return false;
#endif
}

QList<SyntaxInfo> availableSyntaxes()
{
    QList<SyntaxInfo> result;
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    for (const KSyntaxHighlighting::Definition& definition : repository().definitions()) {
        if (!definition.isHidden())
            result.append({definition.name(), definition.translatedName(), definition.translatedSection()});
    }
    std::sort(result.begin(), result.end(), [](const SyntaxInfo& a, const SyntaxInfo& b) {
        return std::tie(a.translatedSection, a.translatedName) < std::tie(b.translatedSection, b.translatedName);
    });
#endif
    return result;
}

QString syntaxForFileName(const QString& fileName)
{
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    // Rotated logs (app.log.1, app.log.2) are logs, not the man pages that *.1 would suggest.
    static const QRegularExpression rotated(QStringLiteral("^(.+\\.log)(\\.\\d+)+$"));
    const QRegularExpressionMatch match = rotated.match(fileName);
    const KSyntaxHighlighting::Definition definition =
        repository().definitionForFileName(match.hasMatch() ? match.captured(1) : fileName);
    return definition.isValid() ? definition.name() : QString();
#else
    Q_UNUSED(fileName);
    return {};
#endif
}

QString translatedSyntaxName(const QString& name)
{
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    const KSyntaxHighlighting::Definition definition = repository().definitionForName(name);
    return definition.isValid() ? definition.translatedName() : name;
#else
    return name;
#endif
}

CommentMarkers commentMarkers(const QString& syntaxName, const QString& fileName)
{
    CommentMarkers markers;
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    if (const KSyntaxHighlighting::Definition definition = repository().definitionForName(syntaxName);
        definition.isValid()) {
        markers.line = definition.singleLineCommentMarker();
        markers.lineAtStart =
            definition.singleLineCommentPosition() == KSyntaxHighlighting::CommentPosition::StartOfLine;
        const auto block = definition.multiLineCommentMarker();
        markers.blockStart = block.first;
        markers.blockEnd = block.second;
        if (!markers.isEmpty())
            return markers;
    }
#else
    Q_UNUSED(syntaxName);
#endif
    // The common languages, by name.
    const QFileInfo info(fileName);
    const QString name = info.fileName().toLower();
    const QString suffix = info.suffix().toLower();
    static const QStringList slashes{QStringLiteral("c"),     QStringLiteral("h"),     QStringLiteral("cc"),
                                     QStringLiteral("cpp"),   QStringLiteral("cxx"),   QStringLiteral("hpp"),
                                     QStringLiteral("hh"),    QStringLiteral("hxx"),   QStringLiteral("java"),
                                     QStringLiteral("js"),    QStringLiteral("mjs"),   QStringLiteral("ts"),
                                     QStringLiteral("tsx"),   QStringLiteral("jsx"),   QStringLiteral("cs"),
                                     QStringLiteral("go"),    QStringLiteral("rs"),    QStringLiteral("swift"),
                                     QStringLiteral("kt"),    QStringLiteral("kts"),   QStringLiteral("scala"),
                                     QStringLiteral("dart"),  QStringLiteral("php"),   QStringLiteral("groovy"),
                                     QStringLiteral("gradle"), QStringLiteral("proto"), QStringLiteral("jsonc"),
                                     QStringLiteral("scss"),  QStringLiteral("less"),  QStringLiteral("qml")};
    static const QStringList hashes{QStringLiteral("py"),   QStringLiteral("sh"),     QStringLiteral("bash"),
                                    QStringLiteral("zsh"),  QStringLiteral("rb"),     QStringLiteral("pl"),
                                    QStringLiteral("pm"),   QStringLiteral("r"),      QStringLiteral("yaml"),
                                    QStringLiteral("yml"),  QStringLiteral("toml"),   QStringLiteral("conf"),
                                    QStringLiteral("cfg"),  QStringLiteral("cmake"),  QStringLiteral("mk"),
                                    QStringLiteral("ps1"),  QStringLiteral("tcl"),    QStringLiteral("jl"),
                                    QStringLiteral("nim"),  QStringLiteral("properties")};
    static const QStringList dashes{QStringLiteral("sql"), QStringLiteral("lua"), QStringLiteral("hs"),
                                    QStringLiteral("ada"), QStringLiteral("vhd"), QStringLiteral("vhdl")};
    static const QStringList semicolons{QStringLiteral("ini"), QStringLiteral("asm"), QStringLiteral("s"),
                                        QStringLiteral("lisp"), QStringLiteral("el"), QStringLiteral("clj"),
                                        QStringLiteral("scm")};
    static const QStringList percents{QStringLiteral("tex"), QStringLiteral("sty"), QStringLiteral("m"),
                                      QStringLiteral("erl")};
    static const QStringList markup{QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("xml"),
                                    QStringLiteral("xhtml"), QStringLiteral("svg"), QStringLiteral("md"),
                                    QStringLiteral("vue"), QStringLiteral("ui")};
    if (slashes.contains(suffix)) {
        markers.line = QStringLiteral("//");
        markers.blockStart = QStringLiteral("/*");
        markers.blockEnd = QStringLiteral("*/");
    } else if (hashes.contains(suffix) || name == QLatin1String("makefile") || name == QLatin1String("dockerfile") ||
               name == QLatin1String("cmakelists.txt")) {
        markers.line = QStringLiteral("#");
    } else if (dashes.contains(suffix)) {
        markers.line = QStringLiteral("--");
    } else if (semicolons.contains(suffix)) {
        markers.line = QStringLiteral(";");
    } else if (percents.contains(suffix)) {
        markers.line = QStringLiteral("%");
    } else if (suffix == QLatin1String("bat") || suffix == QLatin1String("cmd")) {
        markers.line = QStringLiteral("REM");
        markers.lineAtStart = true;
    } else if (suffix == QLatin1String("vb") || suffix == QLatin1String("vbs") || suffix == QLatin1String("bas")) {
        markers.line = QStringLiteral("'");
    } else if (suffix == QLatin1String("css")) {
        markers.blockStart = QStringLiteral("/*");
        markers.blockEnd = QStringLiteral("*/");
    } else if (markup.contains(suffix)) {
        markers.blockStart = QStringLiteral("<!--");
        markers.blockEnd = QStringLiteral("-->");
    }
    return markers;
}

QList<ColorScheme> availableColorSchemes()
{
    QList<ColorScheme> schemes;
    for (const OwnScheme& scheme : kOwnSchemes)
        schemes.append({QString::fromLatin1(scheme.name),
                        QCoreApplication::translate("aiedit::ui::ColorScheme", scheme.displayName), scheme.dark, true});
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    for (const KSyntaxHighlighting::Theme& theme : repository().themes()) {
        if (theme.name() == u"Printing" || ownScheme(theme.name()))
            continue; // white paper, for printing; and AIEdit's own, listed already
        const bool dark = editorColor(theme, KSyntaxHighlighting::Theme::BackgroundColor).lightness() < 128;
        // Shown by their own names, as other editors do: they are names, not descriptions.
        schemes.append({theme.name(), theme.name(), dark});
    }
#endif
    std::stable_sort(schemes.begin(), schemes.end(), [](const ColorScheme& a, const ColorScheme& b) {
        if (a.dark != b.dark)
            return !a.dark;
        if (a.own != b.own)
            return a.own;
        return !a.own && QString::localeAwareCompare(a.displayName, b.displayName) < 0;
    });
    return schemes;
}

QString colorScheme()
{
    return currentScheme();
}

QString effectiveColorScheme()
{
    return effectiveScheme();
}

namespace {

// The palette of a scheme: AIEdit's own from its few colours, a theme's from its own; none for
// the system's colours or a name nobody knows.
std::optional<QPalette> schemePalette(const QString& name)
{
    if (name.isEmpty())
        return std::nullopt;
    if (const OwnScheme* own = ownScheme(name))
        return paletteFrom(QColor::fromRgb(own->background), QColor::fromRgb(own->text),
                           QColor::fromRgb(own->selection), QColor::fromRgb(own->link));
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    if (const KSyntaxHighlighting::Theme theme = repository().theme(name); theme.isValid() && name != u"Printing")
        return paletteFrom(editorColor(theme, KSyntaxHighlighting::Theme::BackgroundColor),
                           QColor::fromRgba(theme.textColor(KSyntaxHighlighting::Theme::Normal)),
                           editorColor(theme, KSyntaxHighlighting::Theme::TextSelection),
                           QColor::fromRgba(theme.textColor(KSyntaxHighlighting::Theme::Keyword)));
#endif
    return std::nullopt;
}

void applyScheme()
{
    static bool applying = false;
    if (applying)
        return;
    const QScopedValueRollback guard(applying, true);
    const QString chosen = currentScheme();
    const bool followsSystem = chosen.isEmpty() || chosen == QLatin1StringView(kAutoColorScheme);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (followsSystem) // so that Auto sees the system's light or dark, and title bars follow it
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
#endif
    const std::optional<QPalette> palette = schemePalette(effectiveScheme());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Windows draws title bars, and Qt's Windows 11 style its tabs and menus, light or dark as
    // the system is; a dark scheme on a light system left them white under light text.
    if (!followsSystem && palette)
        QGuiApplication::styleHints()->setColorScheme(palette->color(QPalette::Base).lightness() < 128
                                                         ? Qt::ColorScheme::Dark
                                                         : Qt::ColorScheme::Light);
#endif
    if (palette) {
        QApplication::setPalette(*palette);
        paletteChanged() = true;
    } else if (paletteChanged()) {
        QApplication::setPalette(QPalette()); // the system's again
        paletteChanged() = false;
    }
}

} // namespace

void setColorScheme(const QString& name)
{
    // Auto changes with the system's light and dark.
    static const bool following = [] {
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, qApp, [] {
            if (currentScheme() == QLatin1StringView(kAutoColorScheme))
                applyScheme();
        });
        return true;
    }();
    Q_UNUSED(following);
    if (const OwnScheme* own = ownScheme(name))
        currentScheme() = QString::fromLatin1(own->name);
    else if (name == QLatin1StringView(kAutoColorScheme) || schemePalette(name))
        currentScheme() = name;
    else
        currentScheme().clear();
    applyScheme();
}

EditorColors editorColors(const QPalette& palette)
{
    EditorColors colors;
    colors.background = palette.color(QPalette::Base);
    colors.text = palette.color(QPalette::Text);
    colors.dark = colors.background.lightness() < 128;
    colors.selection = palette.color(QPalette::Active, QPalette::Highlight);
    colors.selectionText = palette.color(QPalette::Active, QPalette::HighlightedText);
    colors.lineNumber = mix(colors.text, colors.background, 0.55f);
    colors.currentLineNumber = colors.text;
    colors.gutter = mix(colors.background, colors.text, colors.dark ? 0.06f : 0.035f);
    colors.currentLine = mix(colors.background, colors.selection, colors.dark ? 0.14f : 0.08f);
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    // What a theme says about the editor itself; the rest is in the palette made from it.
    if (const KSyntaxHighlighting::Theme theme = schemeTheme(); theme.isValid()) {
        colors.currentLine = editorColor(theme, KSyntaxHighlighting::Theme::CurrentLine);
        colors.lineNumber = editorColor(theme, KSyntaxHighlighting::Theme::LineNumbers);
        colors.currentLineNumber = editorColor(theme, KSyntaxHighlighting::Theme::CurrentLineNumber);
        colors.gutter = editorColor(theme, KSyntaxHighlighting::Theme::IconBorder);
    }
#endif
    return colors;
}

SyntaxState::SyntaxState() = default;
SyntaxState::~SyntaxState() = default;
SyntaxState::SyntaxState(const SyntaxState&) = default;
SyntaxState& SyntaxState::operator=(const SyntaxState&) = default;

SyntaxHighlighter::SyntaxHighlighter(const QString& syntaxName, const QPalette& palette)
{
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    if (syntaxName.isEmpty())
        return;
    const KSyntaxHighlighting::Definition definition = repository().definitionForName(syntaxName);
    if (!definition.isValid())
        return;
    m_impl = std::make_unique<Impl>();
    m_impl->collector.setDefinition(definition);
    const KSyntaxHighlighting::Theme scheme = schemeTheme();
    m_impl->collector.setTheme(scheme.isValid() ? scheme : repository().themeForPalette(palette));
#else
    Q_UNUSED(syntaxName);
    Q_UNUSED(palette);
#endif
}

SyntaxHighlighter::~SyntaxHighlighter() = default;

bool SyntaxHighlighter::isActive() const
{
    return m_impl != nullptr;
}

QString SyntaxHighlighter::syntaxName() const
{
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    return m_impl ? m_impl->collector.definition().name() : QString();
#else
    return {};
#endif
}

QList<QTextLayout::FormatRange> SyntaxHighlighter::highlightLine(const QString& text, SyntaxState& state)
{
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    if (!m_impl)
        return {};
    const KSyntaxHighlighting::State previous = state.m_impl ? state.m_impl->state : KSyntaxHighlighting::State();
    auto next = std::make_shared<SyntaxState::Impl>();
    next->state = m_impl->collector.run(text, previous);
    state.m_impl = std::move(next);
    return m_impl->collector.ranges;
#else
    Q_UNUSED(text);
    Q_UNUSED(state);
    return {};
#endif
}

} // namespace aiedit::ui
