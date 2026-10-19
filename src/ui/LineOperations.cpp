#include "ui/LineOperations.h"

#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/Collation.h"
#include "ui/TextDecoder.h"
#include "ui/TextDocument.h"

#include <QCollator>
#include <QCollatorSortKey>

#include <algorithm>
#include <numeric>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace aiedit::ui {

namespace {

struct Line
{
    std::uint64_t start = 0;
    std::uint64_t contentEnd = 0; // where the line break starts
    std::uint64_t end = 0;        // after the line break
};

// The lines first to last as the index has them, long lines joined from their rows.
std::vector<Line> linesOf(const core::LineIndex& index, std::uint64_t first, std::uint64_t last)
{
    std::vector<Line> lines;
    const std::optional<std::uint64_t> firstRow = index.rowIndexOfLine(first);
    if (!firstRow)
        return lines;
    constexpr std::size_t kRowsAtOnce = 4096;
    for (std::uint64_t row = *firstRow; row < index.rowCount(); row += kRowsAtOnce) {
        for (const core::Row& r : index.rows(row, kRowsAtOnce)) {
            if (r.line > last)
                return lines;
            if (r.continuation && !lines.empty()) {
                lines.back().contentEnd = r.contentEnd;
                lines.back().end = r.end;
            } else {
                lines.push_back({r.start, r.contentEnd, r.end});
            }
        }
    }
    return lines;
}

bool isBlank(std::uint32_t unit)
{
    return unit == ' ' || unit == '\t';
}

// Where the text of a line starts, after its spaces and tabs (its end, for a blank line), and
// where it ends, before those that trail. Spaces and tabs are single code units in every
// supported encoding, and these values never occur inside a multi-byte character.
std::pair<std::uint64_t, std::uint64_t> textOf(std::span<const std::uint8_t> data, const Line& line,
                                              core::CharScheme scheme)
{
    const std::size_t unit = core::codeUnitSize(scheme);
    std::uint64_t from = line.start;
    while (from + unit <= line.contentEnd && isBlank(core::codeUnitAt(data, static_cast<std::size_t>(from), scheme)))
        from += unit;
    std::uint64_t to = line.contentEnd;
    while (to >= from + unit && isBlank(core::codeUnitAt(data, static_cast<std::size_t>(to - unit), scheme)))
        to -= unit;
    return {from, to};
}

} // namespace

std::optional<LineEdit> lineOperation(LineOperation operation, const TextDocument& document,
                                      std::uint64_t first, std::uint64_t last, std::uint64_t selectionStart,
                                      std::uint64_t selectionEnd)
{
    const core::LineIndex& index = document.index();
    const std::uint64_t lineCount = index.lineCount();
    if (lineCount == 0 || first > last)
        return std::nullopt;
    // Moving takes in the line it moves past.
    if (operation == LineOperation::MoveUp) {
        if (first == 0)
            return std::nullopt;
        --first;
    } else if (operation == LineOperation::MoveDown || (operation == LineOperation::Join && first == last)) {
        if (last + 1 >= lineCount)
            return std::nullopt;
        ++last;
    }
    const std::vector<Line> lines = linesOf(index, first, std::min(last, lineCount - 1));
    if (lines.empty())
        return std::nullopt;

    const auto data = document.bytes();
    const std::uint64_t rangeStart = lines.front().start;
    const std::uint64_t rangeEnd = lines.back().end;
    const auto bytesOf = [&data](std::uint64_t from, std::uint64_t to) {
        return QByteArrayView(data.data() + from, static_cast<qsizetype>(to - from));
    };
    const auto content = [&](std::size_t i) { return bytesOf(lines[i].start, lines[i].contentEnd); };
    const auto lineBreak = [&](std::size_t i) { return bytesOf(lines[i].contentEnd, lines[i].end); };

    LineEdit edit;
    edit.start = rangeStart;
    edit.end = rangeEnd;
    QByteArray& out = edit.bytes;
    // Where each line's text went, and how long it is now, to carry the selection along.
    std::vector<std::uint64_t> landed(lines.size());
    std::vector<std::uint64_t> length(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i)
        length[i] = lines[i].contentEnd - lines[i].start;

    // Text in a new order; the line breaks stay where they were.
    const auto reorder = [&](const std::vector<std::size_t>& order) {
        for (std::size_t position = 0; position < order.size(); ++position) {
            landed[order[position]] = rangeStart + static_cast<std::uint64_t>(out.size());
            out += content(order[position]);
            out += lineBreak(position);
        }
    };
    // Some lines left out, with their line breaks.
    const auto keep = [&](const std::vector<bool>& kept) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            landed[i] = rangeStart + static_cast<std::uint64_t>(out.size());
            if (!kept[i]) {
                length[i] = 0;
                ++edit.lines;
                continue;
            }
            out += content(i);
            out += lineBreak(i);
        }
    };

    bool wholeSelection = selectionStart != selectionEnd; // select what the edit made
    std::uint64_t copyOffset = 0; // where the copies of Duplicate start, relative to the originals
    switch (operation) {
    case LineOperation::SortAscending:
    case LineOperation::SortDescending: {
        QCollator collator = naturalCollator(); // the user's language
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        const auto text = [&](std::size_t i) {
            const QByteArrayView bytes = content(i);
            return document.decoder().decodePlain(
                {reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::size_t>(bytes.size())});
        };
#ifdef Q_OS_WIN
        // Compared one pair at a time: Qt's sort keys on Windows end in memory it never fills
        // (sized in characters, filled in bytes), so equal lines would come out in any order.
        std::vector<QString> keys;
        keys.reserve(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i)
            keys.push_back(text(i));
        const auto less = [&](std::size_t a, std::size_t b) { return collator.compare(keys[a], keys[b]) < 0; };
#else
        std::vector<QCollatorSortKey> keys;
        keys.reserve(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i)
            keys.push_back(collator.sortKey(text(i)));
        const auto less = [&](std::size_t a, std::size_t b) { return keys[a].compare(keys[b]) < 0; };
#endif
        std::vector<std::size_t> order(lines.size());
        std::iota(order.begin(), order.end(), 0);
        const bool descending = operation == LineOperation::SortDescending;
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a, std::size_t b) { return descending ? less(b, a) : less(a, b); });
        reorder(order);
        edit.lines = lines.size();
        break;
    }
    case LineOperation::RemoveDuplicates: {
        std::unordered_set<std::string_view> seen;
        std::vector<bool> kept(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const QByteArrayView text = content(i);
            kept[i] = seen.emplace(text.data(), static_cast<std::size_t>(text.size())).second;
        }
        keep(kept);
        break;
    }
    case LineOperation::RemoveEmpty: {
        const core::CharScheme scheme = document.codec().scheme;
        const std::size_t unit = core::codeUnitSize(scheme);
        std::vector<bool> kept(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i) {
            for (std::uint64_t at = lines[i].start; at + unit <= lines[i].contentEnd && !kept[i]; at += unit)
                kept[i] = !isBlank(core::codeUnitAt(data, static_cast<std::size_t>(at), scheme));
        }
        keep(kept);
        break;
    }
    case LineOperation::TrimTrailingWhitespace: {
        // Spaces and tabs are single code units in every supported encoding, and these values
        // never occur inside a multi-byte character.
        const core::CharScheme scheme = document.codec().scheme;
        const std::size_t unit = core::codeUnitSize(scheme);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::uint64_t end = lines[i].contentEnd;
            while (end >= lines[i].start + unit &&
                   isBlank(core::codeUnitAt(data, static_cast<std::size_t>(end - unit), scheme)))
                end -= unit;
            landed[i] = rangeStart + static_cast<std::uint64_t>(out.size());
            length[i] = end - lines[i].start;
            if (end != lines[i].contentEnd)
                ++edit.lines;
            out += bytesOf(lines[i].start, end);
            out += lineBreak(i);
        }
        break;
    }
    case LineOperation::MoveUp:
    case LineOperation::MoveDown: {
        // The line moved past goes to the other side of the selected ones.
        std::vector<std::size_t> order(lines.size());
        std::iota(order.begin(), order.end(), 0);
        if (operation == LineOperation::MoveUp)
            std::rotate(order.begin(), order.begin() + 1, order.end());
        else
            std::rotate(order.begin(), order.end() - 1, order.end());
        reorder(order);
        edit.lines = lines.size() - 1;
        wholeSelection = false; // the selection moves with its lines
        break;
    }
    case LineOperation::Duplicate:
    case LineOperation::CopyUp: {
        out = bytesOf(rangeStart, rangeEnd).toByteArray();
        if (lines.back().end == lines.back().contentEnd) { // the end of the file: the copy needs a break
            const std::optional<QByteArray> lineEnd = document.encode(document.lineTerminator());
            if (!lineEnd)
                return std::nullopt;
            out += *lineEnd;
        }
        copyOffset = operation == LineOperation::Duplicate ? static_cast<std::uint64_t>(out.size()) : 0;
        out += bytesOf(rangeStart, rangeEnd);
        for (std::size_t i = 0; i < lines.size(); ++i)
            landed[i] = lines[i].start + copyOffset; // the selection goes to the copy below, or stays
        edit.lines = lines.size();
        wholeSelection = false;
        break;
    }
    case LineOperation::Delete: {
        // The lines go with their breaks. The file's last line takes the break before it, and
        // the caret goes to the start of the line that comes up, or else of the one before.
        edit.lines = lines.size();
        edit.selectionStart = edit.selectionEnd = rangeStart;
        if (lines.back().end == lines.back().contentEnd && first > 0) {
            const std::vector<Line> before = linesOf(index, first - 1, first - 1);
            if (!before.empty()) {
                edit.start = before.front().contentEnd;
                edit.selectionStart = edit.selectionEnd = before.front().start;
            }
        }
        return edit;
    }
    case LineOperation::Join: {
        // Each line break and the indentation after it become one space: none after a line that
        // ends in a blank, nor next to an empty one. With nothing selected the caret goes to
        // where the lines met; a selection takes in the line made.
        const core::CharScheme scheme = document.codec().scheme;
        const std::optional<QByteArray> space = document.encode(QStringLiteral(" "));
        if (!space)
            return std::nullopt;
        std::uint64_t met = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::uint64_t from = i == 0 ? lines[i].start : textOf(data, lines[i], scheme).first;
            const bool empty = from == lines[i].contentEnd;
            const std::size_t unit = core::codeUnitSize(scheme);
            const bool trailingBlank =
                out.size() >= static_cast<qsizetype>(unit) &&
                isBlank(core::codeUnitAt({reinterpret_cast<const std::uint8_t*>(out.constData()),
                                          static_cast<std::size_t>(out.size())},
                                         static_cast<std::size_t>(out.size()) - unit, scheme));
            if (i == 1)
                met = rangeStart + static_cast<std::uint64_t>(out.size());
            if (i > 0 && !empty && !out.isEmpty() && !trailingBlank)
                out += *space;
            out += bytesOf(from, lines[i].contentEnd);
        }
        const std::uint64_t joinedEnd = rangeStart + static_cast<std::uint64_t>(out.size());
        out += lineBreak(lines.size() - 1);
        edit.lines = lines.size();
        edit.selectionStart = selectionStart == selectionEnd ? met : rangeStart;
        edit.selectionEnd = selectionStart == selectionEnd ? met : joinedEnd;
        return edit;
    }
    case LineOperation::InsertBelow:
    case LineOperation::InsertAbove: {
        const std::optional<QByteArray> lineEnd = document.encode(document.lineTerminator());
        if (!lineEnd)
            return std::nullopt;
        const bool below = operation == LineOperation::InsertBelow;
        const Line& next = below ? lines.back() : lines.front();
        const QByteArrayView indent = bytesOf(next.start, textOf(data, next, document.codec().scheme).first);
        const QByteArrayView original = bytesOf(rangeStart, rangeEnd);
        std::uint64_t caret = 0;
        if (!below) {
            out = indent.toByteArray() + *lineEnd + original.toByteArray();
            caret = rangeStart + static_cast<std::uint64_t>(indent.size());
        } else if (lines.back().end == lines.back().contentEnd) {
            // The end of a file whose last line has no line break: past one there would be no line
            // to type on, so the new line gets one too.
            out = original.toByteArray() + *lineEnd + indent.toByteArray();
            caret = rangeStart + static_cast<std::uint64_t>(out.size());
            out += *lineEnd;
        } else {
            out = original.toByteArray() + indent.toByteArray();
            caret = rangeStart + static_cast<std::uint64_t>(out.size());
            out += *lineEnd;
        }
        edit.lines = 1;
        edit.selectionStart = edit.selectionEnd = caret;
        return edit;
    }
    }

    if (out == bytesOf(rangeStart, rangeEnd))
        return std::nullopt; // sorted already, nothing to remove or trim

    // An offset before the range stays; one after it moves with the change in size; one in a
    // line goes where that line's text went, at the same column if the line is still that long.
    const auto carry = [&](std::uint64_t offset) -> std::uint64_t {
        if (offset < rangeStart)
            return offset;
        if (offset >= rangeEnd && !(offset == rangeEnd && lines.back().end == lines.back().contentEnd))
            return offset - rangeEnd + rangeStart + static_cast<std::uint64_t>(out.size());
        const auto line = std::upper_bound(lines.begin(), lines.end(), offset,
                                           [](std::uint64_t o, const Line& l) { return o < l.start; }) -
                          1;
        const std::size_t i = static_cast<std::size_t>(line - lines.begin());
        return landed[i] + std::min(offset - lines[i].start, length[i]);
    };
    if (wholeSelection) {
        edit.selectionStart = rangeStart;
        edit.selectionEnd = rangeStart + static_cast<std::uint64_t>(out.size());
    } else {
        edit.selectionStart = carry(selectionStart);
        edit.selectionEnd = carry(selectionEnd);
    }
    return edit;
}

std::optional<LineEdit> toggleComment(const TextDocument& document, const CommentMarkers& markers,
                                      std::uint64_t first, std::uint64_t last, std::uint64_t selectionStart,
                                      std::uint64_t selectionEnd)
{
    const core::LineIndex& index = document.index();
    if (index.lineCount() == 0 || first > last || markers.isEmpty())
        return std::nullopt;
    const std::vector<Line> lines = linesOf(index, first, std::min(last, index.lineCount() - 1));
    if (lines.empty())
        return std::nullopt;
    const auto data = document.bytes();
    const core::CharScheme scheme = document.codec().scheme;
    const bool block = markers.line.isEmpty();
    const std::optional<QByteArray> open = document.encode(block ? markers.blockStart : markers.line);
    const std::optional<QByteArray> close = block ? document.encode(markers.blockEnd) : QByteArray();
    const std::optional<QByteArray> space = document.encode(QStringLiteral(" "));
    if (!open || !close || !space)
        return std::nullopt;
    const auto bytesOf = [&data](std::uint64_t from, std::uint64_t to) {
        return QByteArrayView(data.data() + from, static_cast<qsizetype>(to - from));
    };
    const auto has = [&](std::uint64_t at, std::uint64_t limit, const QByteArray& what) {
        return at + static_cast<std::uint64_t>(what.size()) <= limit &&
               bytesOf(at, at + static_cast<std::uint64_t>(what.size())) == what;
    };
    const auto size = [](const QByteArray& bytes) { return static_cast<std::uint64_t>(bytes.size()); };

    // Comment out, unless every line with text is so already; at the indentation they share.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> text(lines.size());
    std::uint64_t indent = ~std::uint64_t{0};
    bool commented = true;
    bool anyText = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text[i] = textOf(data, lines[i], scheme);
        const auto [from, to] = text[i];
        if (from == lines[i].contentEnd)
            continue; // blank
        anyText = true;
        indent = std::min(indent, from - lines[i].start);
        commented = commented && has(from, to, *open) &&
                    (!block || (to >= from + size(*open) + size(*close) && has(to - size(*close), to, *close)));
    }
    if (!anyText)
        return std::nullopt;

    struct Change
    {
        std::uint64_t at;
        std::uint64_t removed;
        QByteArray inserted;
    };
    std::vector<Change> changes; // in order
    LineEdit edit;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto [from, to] = text[i];
        if (from == lines[i].contentEnd)
            continue;
        ++edit.lines;
        if (commented) {
            // The marker goes, and a space after it; at the end, the closing marker and a
            // space before it.
            std::uint64_t removed = size(*open);
            if (has(from + removed, to, *space))
                removed += size(*space);
            changes.push_back({from, removed, {}});
            if (block) {
                std::uint64_t closeAt = to - size(*close);
                if (closeAt >= from + removed + size(*space) && has(closeAt - size(*space), to, *space))
                    closeAt -= size(*space);
                if (closeAt >= from + removed)
                    changes.push_back({closeAt, to - closeAt, {}});
            }
        } else {
            changes.push_back({!block && markers.lineAtStart ? lines[i].start : lines[i].start + indent, 0,
                               *open + *space});
            if (block)
                changes.push_back({to, 0, *space + *close});
        }
    }

    const std::uint64_t rangeStart = lines.front().start;
    const std::uint64_t rangeEnd = lines.back().end;
    edit.start = rangeStart;
    edit.end = rangeEnd;
    std::uint64_t at = rangeStart;
    for (const Change& change : changes) {
        edit.bytes += bytesOf(at, change.at);
        edit.bytes += change.inserted;
        at = change.at + change.removed;
    }
    edit.bytes += bytesOf(at, rangeEnd);
    // Offsets move with the text around them. A selection that starts where a marker goes in
    // takes it in; a caret there goes after it; one in what is taken out goes where that was.
    const auto carry = [&](std::uint64_t offset, bool isStart) {
        std::uint64_t shifted = offset;
        for (const Change& change : changes) {
            if (offset < change.at || (offset == change.at && isStart))
                break;
            if (offset < change.at + change.removed)
                return shifted - (offset - change.at);
            shifted = shifted + size(change.inserted) - change.removed;
        }
        return shifted;
    };
    const bool selection = selectionStart != selectionEnd;
    edit.selectionStart = carry(selectionStart, selection);
    edit.selectionEnd = carry(selectionEnd, false);
    return edit;
}

} // namespace aiedit::ui
