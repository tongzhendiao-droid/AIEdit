#include "ui/SyntaxHighlighting.h"

#include <QPalette>

#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>
#endif

#include <algorithm>
#include <tuple>

namespace aiedit::ui {

#ifdef AIEDIT_HAVE_KSYNTAXHIGHLIGHTING

namespace {

// Loading all definitions takes a moment, so the whole program shares one repository.
// It is only touched from the GUI thread.
KSyntaxHighlighting::Repository& repository()
{
    static KSyntaxHighlighting::Repository instance;
    return instance;
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
    const KSyntaxHighlighting::Definition definition = repository().definitionForFileName(fileName);
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
    m_impl->collector.setTheme(repository().themeForPalette(palette));
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
