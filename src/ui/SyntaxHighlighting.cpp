#include "ui/SyntaxHighlighting.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QRegularExpression>

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

// The scheme in use, empty while following the system; and whether the application palette
// was ever set for one (so that following the system needs undoing that). GUI thread only.
QString& currentScheme()
{
    static QString name;
    return name;
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

// The scheme's theme; invalid while following the system.
KSyntaxHighlighting::Theme schemeTheme()
{
    return currentScheme().isEmpty() ? KSyntaxHighlighting::Theme() : repository().theme(currentScheme());
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

QList<ColorScheme> availableColorSchemes()
{
    QList<ColorScheme> schemes;
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    for (const KSyntaxHighlighting::Theme& theme : repository().themes()) {
        if (theme.name() == u"Printing")
            continue; // white paper, for printing
        const bool dark = editorColor(theme, KSyntaxHighlighting::Theme::BackgroundColor).lightness() < 128;
        // Shown by their own names, as other editors do: they are names, not descriptions.
        schemes.append({theme.name(), theme.name(), dark});
    }
#else
    schemes = {{QStringLiteral("Light"), QCoreApplication::translate("aiedit::ui::ColorScheme", "Light"), false},
               {QStringLiteral("Dark"), QCoreApplication::translate("aiedit::ui::ColorScheme", "Dark"), true}};
#endif
    std::sort(schemes.begin(), schemes.end(), [](const ColorScheme& a, const ColorScheme& b) {
        if (a.dark != b.dark)
            return !a.dark;
        return QString::localeAwareCompare(a.displayName, b.displayName) < 0;
    });
    return schemes;
}

QString colorScheme()
{
    return currentScheme();
}

void setColorScheme(const QString& name)
{
    std::optional<QPalette> palette;
#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
    if (const KSyntaxHighlighting::Theme theme = repository().theme(name);
        !name.isEmpty() && theme.isValid() && name != u"Printing") {
        palette = paletteFrom(editorColor(theme, KSyntaxHighlighting::Theme::BackgroundColor),
                              QColor::fromRgba(theme.textColor(KSyntaxHighlighting::Theme::Normal)),
                              editorColor(theme, KSyntaxHighlighting::Theme::TextSelection),
                              QColor::fromRgba(theme.textColor(KSyntaxHighlighting::Theme::Keyword)));
    }
#else
    if (name == u"Light")
        palette = paletteFrom(QColor(0xFF, 0xFF, 0xFF), QColor(0x1F, 0x1F, 0x1F), QColor(0xB3, 0xD7, 0xFF),
                              QColor(0x1A, 0x5F, 0xB4));
    else if (name == u"Dark")
        palette = paletteFrom(QColor(0x1E, 0x1F, 0x22), QColor(0xD4, 0xD4, 0xD4), QColor(0x26, 0x4F, 0x78),
                              QColor(0x6C, 0xB6, 0xFF));
#endif
    currentScheme() = palette ? name : QString();
    if (palette) {
        QApplication::setPalette(*palette);
        paletteChanged() = true;
    } else if (paletteChanged()) {
        QApplication::setPalette(QPalette()); // the system's again
        paletteChanged() = false;
    }
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
