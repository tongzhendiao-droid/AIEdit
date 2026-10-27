#include "ui/Markdown.h"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace aiedit::ui::markdown {

namespace {

bool isWide(char32_t c)
{
    return (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
           (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x2FFFD) || (c >= 0x30000 && c <= 0x3FFFD);
}

// A table row's cells, split at the bars that are not escaped, without the bars at either end.
QStringList cellsOf(QStringView line)
{
    QStringView row = line.trimmed();
    if (row.startsWith(u'|'))
        row = row.sliced(1);
    if (row.endsWith(u'|') && !row.endsWith(u"\\|"))
        row.chop(1);
    QStringList cells;
    QString cell;
    for (qsizetype i = 0; i < row.size(); ++i) {
        if (row[i] == u'\\' && i + 1 < row.size() && row[i + 1] == u'|') {
            cell += QStringLiteral("\\|");
            ++i;
        } else if (row[i] == u'|') {
            cells << cell.trimmed();
            cell.clear();
        } else {
            cell += row[i];
        }
    }
    cells << cell.trimmed();
    return cells;
}

enum class Align
{
    None,
    Left,
    Center,
    Right,
};

std::optional<Align> delimiterAlign(const QString& cell)
{
    static const QRegularExpression dashes(QStringLiteral("^:?-+:?$"));
    if (!dashes.match(cell).hasMatch())
        return std::nullopt;
    const bool left = cell.startsWith(u':');
    const bool right = cell.size() > 1 && cell.endsWith(u':');
    return left && right ? Align::Center : left ? Align::Left : right ? Align::Right : Align::None;
}

QString padded(const QString& cell, int width, Align align)
{
    const int room = std::max(0, width - displayWidth(cell));
    switch (align) {
    case Align::Right:
        return QString(room, u' ') + cell;
    case Align::Center:
        return QString(room / 2, u' ') + cell + QString(room - room / 2, u' ');
    default:
        return cell + QString(room, u' ');
    }
}

// A line's heading or list mark, and the indentation before it.
const QRegularExpression& blockMark()
{
    static const QRegularExpression mark(
        QStringLiteral(R"(^(\s*)(#{1,6}\s+|[-*+]\s+\[[ xX]\]\s+|[-*+]\s+|\d{1,9}[.)]\s+))"));
    return mark;
}

} // namespace

bool isMarkdownFile(const QString& fileName)
{
    static const QStringList suffixes{QStringLiteral("md"),   QStringLiteral("markdown"), QStringLiteral("mkd"),
                                      QStringLiteral("mdown"), QStringLiteral("mkdn"),    QStringLiteral("mdwn")};
    return suffixes.contains(QFileInfo(fileName).suffix().toLower());
}

int displayWidth(QStringView text)
{
    int width = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text[i].unicode();
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            c = QChar::surrogateToUcs4(text[i].unicode(), text[i + 1].unicode());
            ++i;
        }
        const QChar::Category category = QChar::category(c);
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing)
            continue;
        width += isWide(c) ? 2 : 1;
    }
    return width;
}

std::optional<ListBreak> listBreak(QStringView lineBeforeCaret)
{
    static const QRegularExpression item(
        QStringLiteral(R"(^(\s*)(?:([-*+])|(\d{1,9})([.)]))(\s+)(\[[ xX]\]\s+)?(.*)$)"));
    static const QRegularExpression quote(QStringLiteral(R"(^(\s*>\s?)(.*)$)"));
    if (const QRegularExpressionMatch match = item.matchView(lineBeforeCaret); match.hasMatch()) {
        if (match.capturedView(7).trimmed().isEmpty())
            return ListBreak{{}, static_cast<int>(lineBeforeCaret.size())}; // an empty item ends the list
        QString marker = match.captured(2);
        if (marker.isEmpty())
            marker = QString::number(match.captured(3).toLongLong() + 1) + match.captured(4);
        QString next = match.captured(1) + marker + match.captured(5);
        if (match.capturedLength(6) > 0)
            next += QStringLiteral("[ ] ");
        return ListBreak{next, 0};
    }
    if (const QRegularExpressionMatch match = quote.matchView(lineBeforeCaret); match.hasMatch()) {
        if (match.capturedView(2).trimmed().isEmpty())
            return ListBreak{{}, static_cast<int>(lineBeforeCaret.size())};
        return ListBreak{match.captured(1), 0};
    }
    return std::nullopt;
}

bool isListItem(QStringView line)
{
    static const QRegularExpression item(QStringLiteral(R"(^\s*(?:[-*+]|\d{1,9}[.)])(?:\s|$))"));
    return item.matchView(line).hasMatch();
}

MarkedText toggleMark(const QString& line, qsizetype start, qsizetype end, const QString& mark)
{
    start = std::clamp<qsizetype>(start, 0, line.size());
    end = std::clamp<qsizetype>(end, start, line.size());
    const QChar c = mark.front();
    const qsizetype size = mark.size();
    const auto marked = [&](qsizetype run) {
        if (c == u'*')
            return size == 1 ? run % 2 == 1 : run >= 2;
        return run >= size;
    };
    // Just outside the text.
    qsizetype before = 0;
    while (start - before > 0 && line[start - before - 1] == c)
        ++before;
    qsizetype after = 0;
    while (end + after < line.size() && line[end + after] == c)
        ++after;
    if (marked(std::min(before, after))) {
        QString out = line;
        out.remove(end, size);
        out.remove(start - size, size);
        return {out, start - size, end - size};
    }
    // At its ends, selected with it.
    qsizetype lead = 0;
    while (start + lead < end && line[start + lead] == c)
        ++lead;
    qsizetype trail = 0;
    while (end - trail > start + lead && line[end - trail - 1] == c)
        ++trail;
    if (marked(std::min(lead, trail))) {
        QString out = line;
        out.remove(end - size, size);
        out.remove(start, size);
        return {out, start, end - 2 * size};
    }
    // Spaces at the ends stay outside: "** text**" would not be bold.
    while (start < end && line[start].isSpace())
        ++start;
    while (end > start && line[end - 1].isSpace())
        --end;
    QString out = line;
    out.insert(end, mark);
    out.insert(start, mark);
    return {out, start + size, end + size};
}

qsizetype cellColumn(const QString& row, qsizetype column, const QString& formattedRow)
{
    // The cell, by the bars before the caret that are not escaped.
    qsizetype bars = 0;
    for (qsizetype i = 0; i < std::min(column, row.size()); ++i) {
        if (row[i] == u'|' && (i == 0 || row[i - 1] != u'\\'))
            ++bars;
    }
    const qsizetype cell = std::max<qsizetype>(0, bars - (row.trimmed().startsWith(u'|') ? 1 : 0));
    // Its start in the row lined up: past as many bars and one, and the space after the last.
    qsizetype seen = 0;
    for (qsizetype i = 0; i < formattedRow.size(); ++i) {
        if (formattedRow[i] == u'|' && (i == 0 || formattedRow[i - 1] != u'\\') && seen++ == cell)
            return std::min(i + 2, formattedRow.size());
    }
    return formattedRow.size();
}

std::optional<QStringList> formatTable(const QStringList& lines)
{
    if (lines.size() < 2)
        return std::nullopt;
    // The header and the row of dashes have bars; the rows after them may do without (GitHub's).
    if (!lines[0].contains(u'|') || !lines[1].contains(u'|'))
        return std::nullopt;
    QList<QStringList> rows;
    for (const QString& line : lines)
        rows << cellsOf(line);
    qsizetype columns = 0;
    for (const QStringList& row : rows)
        columns = std::max(columns, row.size());
    QList<Align> aligns;
    for (qsizetype c = 0; c < columns; ++c) {
        const std::optional<Align> align = delimiterAlign(c < rows[1].size() ? rows[1][c] : QStringLiteral("---"));
        if (!align)
            return std::nullopt; // no row of dashes under the header: not a table
        aligns << *align;
    }
    QList<int> widths(columns, 3);
    for (qsizetype r = 0; r < rows.size(); ++r) {
        if (r == 1)
            continue;
        for (qsizetype c = 0; c < rows[r].size(); ++c)
            widths[c] = std::max(widths[c], displayWidth(rows[r][c]));
    }

    const qsizetype indentEnd =
        std::find_if(lines[0].cbegin(), lines[0].cend(), [](QChar c) { return !c.isSpace(); }) - lines[0].cbegin();
    const QString indent = lines[0].left(indentEnd);
    QStringList out;
    for (qsizetype r = 0; r < rows.size(); ++r) {
        QStringList cells;
        for (qsizetype c = 0; c < columns; ++c) {
            const int w = widths[c];
            if (r == 1) {
                switch (aligns[c]) {
                case Align::Left:
                    cells << QStringLiteral(":") + QString(w - 1, u'-');
                    break;
                case Align::Right:
                    cells << QString(w - 1, u'-') + QStringLiteral(":");
                    break;
                case Align::Center:
                    cells << QStringLiteral(":") + QString(w - 2, u'-') + QStringLiteral(":");
                    break;
                case Align::None:
                    cells << QString(w, u'-');
                    break;
                }
            } else {
                cells << padded(c < rows[r].size() ? rows[r][c] : QString(), w, aligns[c]);
            }
        }
        out << indent + QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |");
    }
    return out;
}

QStringList toggleLinePrefix(const QStringList& lines, LinePrefix prefix)
{
    static const QRegularExpression heading1(QStringLiteral(R"(^\s*#\s)"));
    static const QRegularExpression heading2(QStringLiteral(R"(^\s*##\s)"));
    static const QRegularExpression heading3(QStringLiteral(R"(^\s*###\s)"));
    static const QRegularExpression task(QStringLiteral(R"(^\s*[-*+]\s+\[[ xX]\]\s)"));
    static const QRegularExpression bullet(QStringLiteral(R"(^\s*[-*+]\s+(?!\[[ xX]\]\s))"));
    static const QRegularExpression numbered(QStringLiteral(R"(^\s*\d{1,9}[.)]\s)"));
    static const QRegularExpression quote(QStringLiteral(R"(^\s*>\s?)"));
    const QRegularExpression* const has[] = {&heading1, &heading2, &heading3, &bullet, &numbered, &task, &quote};
    const QRegularExpression& marked = *has[static_cast<int>(prefix)];

    bool all = true;
    bool anyText = false;
    for (const QString& line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        anyText = true;
        all = all && marked.match(line).hasMatch();
    }
    if (!anyText)
        return lines;

    QStringList out;
    int number = 0;
    for (const QString& line : lines) {
        if (line.trimmed().isEmpty()) {
            out << line;
            continue;
        }
        if (prefix == LinePrefix::Quote) {
            out << (all ? QString(line).remove(quote) : QStringLiteral("> ") + line);
            continue;
        }
        // Without its own mark (or any other heading or list mark), keeping the indentation.
        const QRegularExpressionMatch match = blockMark().match(line);
        const QString indent = match.hasMatch() ? match.captured(1)
                                                : line.left(std::find_if(line.cbegin(), line.cend(), [](QChar c) {
                                                                return !c.isSpace();
                                                            }) - line.cbegin());
        const QString text = match.hasMatch() ? line.mid(match.capturedEnd(0)) : line.mid(indent.size());
        if (all) {
            out << indent + text;
            continue;
        }
        switch (prefix) {
        case LinePrefix::Heading1:
            out << QStringLiteral("# ") + text;
            break;
        case LinePrefix::Heading2:
            out << QStringLiteral("## ") + text;
            break;
        case LinePrefix::Heading3:
            out << QStringLiteral("### ") + text;
            break;
        case LinePrefix::Bullet:
            out << indent + QStringLiteral("- ") + text;
            break;
        case LinePrefix::Numbered:
            out << indent + QString::number(++number) + QStringLiteral(". ") + text;
            break;
        case LinePrefix::Task:
            out << indent + QStringLiteral("- [ ] ") + text;
            break;
        case LinePrefix::Quote:
            break;
        }
    }
    return out;
}

QList<Heading> headings(QStringView text)
{
    static const QRegularExpression atx(QStringLiteral(R"(^ {0,3}(#{1,6})(?:\s+(.*?))?\s*#*\s*$)"));
    static const QRegularExpression fence(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,}))"));
    static const QRegularExpression underline(QStringLiteral(R"(^ {0,3}(=+|-+)\s*$)"));
    QList<Heading> found;
    const QList<QStringView> lines = text.split(u'\n');
    QString openFence; // the fence of the code block we are in
    for (qsizetype i = 0; i < lines.size(); ++i) {
        QStringView line = lines[i];
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (const QRegularExpressionMatch match = fence.matchView(line); match.hasMatch()) {
            const QString marks = match.captured(1);
            if (openFence.isEmpty())
                openFence = marks;
            else if (marks.front() == openFence.front() && marks.size() >= openFence.size())
                openFence.clear();
            continue;
        }
        if (!openFence.isEmpty())
            continue;
        if (const QRegularExpressionMatch match = atx.matchView(line); match.hasMatch()) {
            found.append({static_cast<int>(i), static_cast<int>(match.capturedLength(1)), match.captured(2)});
            continue;
        }
        // Text over a line of = or - (not a list item, quote or table).
        if (i + 1 < lines.size() && !line.trimmed().isEmpty() && !blockMark().matchView(line).hasMatch() &&
            !line.trimmed().startsWith(u'>') && !line.contains(u'|')) {
            QStringView next = lines[i + 1];
            if (next.endsWith(u'\r'))
                next.chop(1);
            if (const QRegularExpressionMatch match = underline.matchView(next); match.hasMatch()) {
                found.append({static_cast<int>(i), match.captured(1).front() == u'=' ? 1 : 2,
                              line.trimmed().toString()});
                ++i;
            }
        }
    }
    return found;
}

} // namespace aiedit::ui::markdown
