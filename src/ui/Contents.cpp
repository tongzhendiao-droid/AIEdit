#include "ui/Contents.h"

#include "core/FileSource.h"
#include "core/LineIndex.h"
#include "ui/Markdown.h"
#include "ui/TextDecoder.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>

namespace aiedit::ui {

namespace {

constexpr std::size_t kMaxHeadings = 5000;   // more is not a book's contents
constexpr qsizetype kMaxTitleLine = 20;       // of a line that can complete a bare 第一回
constexpr std::size_t kMaxTitleLines = 2;
constexpr int kMaxEmptyLines = 2; // between a bare number and its title, or between its parts

// Chinese and Arabic numbers, as chapters are counted.
QString number()
{
    return QStringLiteral("[0-9０-９零〇一二三四五六七八九十百千万两壹贰叁肆伍陆柒捌玖拾佰仟]+");
}

const QRegularExpression& volumePattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^(?:第%1[卷部篇集]|卷之?%1|.*卷第%1|(?:part|book)\\b|#\\s)").arg(number()),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return pattern;
}

// A heading that is only its number, which a title on the next lines completes.
const QRegularExpression& bareNumberPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^第%1[章回节]$").arg(number()),
                                            QRegularExpression::UseUnicodePropertiesOption);
    return pattern;
}

const QRegularExpression& numberedPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^第%1").arg(number()),
                                            QRegularExpression::UseUnicodePropertiesOption);
    return pattern;
}

QString lineText(const TextDecoder& decoder, std::span<const std::uint8_t> data, const core::Row& row)
{
    return decoder.decodePlain(data.subspan(row.start, static_cast<std::size_t>(row.contentEnd - row.start)));
}

} // namespace

FilterRule defaultHeadingRule()
{
    FilterCondition shortLine;
    shortLine.pattern = QStringLiteral("^\\s*\\S.{0,39}$");
    FilterCondition heading;
    heading.pattern =
        QStringLiteral("^\\s*(?:正文\\s*)?(?:"
                       "第%1[章回节卷部篇集讲品幕话折]"  // 第三回, 第十二章 风起
                       "|卷之?%1(?:\\s|$)"                // 卷二, 卷之一
                       // 五帝本纪第一, 杂阿含经卷第一; not a sentence that ends so (作春申君列传第十八)
                       "|[^，。；：！？]*第%1\\s*$"
                       "|(?:序|序言|序章|自序|前言|引言|引子|楔子|后记|跋|尾声|番外|附录|结语)(?:\\s.*)?$"
                       "|(?:chapter|part|book)\\s+(?:\\d+|[ivxlcdm]+|one|two|three|four|five|six|seven|eight|"
                       "nine|ten)\\b"
                       "|(?:prologue|epilogue|preface|afterword)\\s*$"
                       ")"
                       "|^\\s*正文\\s+\\S" // 正文 日者列传
                       "|^#{1,3}\\s+\\S")  // Markdown
            .arg(number());
    FilterRule rule;
    rule.conditions = {shortLine, heading};
    rule.description = QStringLiteral("headings");
    return rule;
}

std::vector<Heading> findHeadings(const FilterRule& rule, const SearchContext& context,
                                  const std::function<bool()>& cancelled)
{
    std::vector<FilterMatch> matches;
    FilterHooks hooks;
    hooks.cancelled = [&] { return (cancelled && cancelled()) || matches.size() >= kMaxHeadings; };
    hooks.found = [&matches](std::vector<FilterMatch>&& batch) {
        matches.insert(matches.end(), batch.begin(), batch.end());
    };
    filterLines(rule, context, 0, hooks);
    if (matches.size() > kMaxHeadings)
        matches.resize(kMaxHeadings);

    const core::LineIndex& index = *context.index;
    const auto data = context.source->bytes();
    const TextDecoder decoder(context.codec);
    const auto readRow = [&](std::uint64_t start, std::uint64_t line) {
        return core::LineIndex::readRow(data, start, line, false, index.scheme(), index.lineBreak());
    };
    static const QRegularExpression prefix(QStringLiteral("^\\s*正文\\s*"),
                                           QRegularExpression::UseUnicodePropertiesOption);

    std::vector<Heading> headings;
    for (std::size_t m = 0; m < matches.size(); ++m) {
        const FilterMatch& match = matches[m];
        const core::Row row = readRow(match.start, match.line);
        Heading heading;
        heading.line = match.line;
        heading.start = match.start;
        heading.title = lineText(decoder, data, row).simplified().remove(prefix);
        // 第一回 alone: its title is on the short lines that follow (东周列国志 and the like).
        if (bareNumberPattern().match(heading.title).hasMatch()) {
            core::Row next = row;
            QStringList parts;
            for (int empty = 0; parts.size() < static_cast<qsizetype>(kMaxTitleLines) && next.end < data.size();) {
                next = readRow(next.end, next.line + 1);
                if (m + 1 < matches.size() && next.start >= matches[m + 1].start)
                    break; // the next heading
                const QString text = lineText(decoder, data, next).simplified();
                if (text.isEmpty()) {
                    if (++empty > kMaxEmptyLines)
                        break;
                    continue;
                }
                if (text.size() > kMaxTitleLine)
                    break;
                parts << text;
            }
            if (!parts.isEmpty())
                heading.title += QLatin1Char(' ') + parts.join(QLatin1Char(' '));
        }
        heading.level = volumePattern().match(heading.title).hasMatch() ? 1 : 2;
        headings.push_back(std::move(heading));
    }

    // A list of contents at the start repeats the numbered headings: the chapters themselves,
    // later on, are the ones to go to.
    QHash<QString, std::size_t> last;
    for (std::size_t i = 0; i < headings.size(); ++i) {
        if (numberedPattern().match(headings[i].title).hasMatch())
            last.insert(headings[i].title, i);
    }
    std::vector<Heading> kept;
    for (std::size_t i = 0; i < headings.size(); ++i) {
        if (!numberedPattern().match(headings[i].title).hasMatch() || last.value(headings[i].title) == i)
            kept.push_back(std::move(headings[i]));
    }
    return kept;
}

std::vector<Heading> findMarkdownHeadings(const SearchContext& context, const std::function<bool()>& cancelled)
{
    std::vector<Heading> headings;
    if (!context.source || !context.index)
        return headings;
    const core::LineIndex& index = *context.index;
    const auto bytes = context.source->bytes();
    const auto start = static_cast<std::size_t>(std::min<std::uint64_t>(index.textStart(), bytes.size()));
    const QString text = TextDecoder(context.codec).decodePlain(bytes.subspan(start));
    if (cancelled && cancelled())
        return headings;
    for (const markdown::Heading& heading : markdown::headings(text)) {
        const std::optional<std::uint64_t> row = index.rowIndexOfLine(static_cast<std::uint64_t>(heading.line));
        if (!row)
            break;
        headings.push_back({static_cast<std::uint64_t>(heading.line), index.row(*row).start,
                            heading.text.isEmpty() ? QStringLiteral("#") : heading.text, heading.level});
    }
    return headings;
}

std::uint64_t headingEnd(const std::vector<Heading>& headings, std::size_t index, std::uint64_t textEnd)
{
    return index + 1 < headings.size() ? headings[index + 1].start : textEnd;
}

int headingAt(const std::vector<Heading>& headings, std::uint64_t offset)
{
    const auto after = std::upper_bound(headings.begin(), headings.end(), offset,
                                        [](std::uint64_t o, const Heading& h) { return o < h.start; });
    return static_cast<int>(after - headings.begin()) - 1;
}

} // namespace aiedit::ui
