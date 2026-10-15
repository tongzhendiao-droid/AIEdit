#include "ui/LineOperations.h"

#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/TextDecoder.h"
#include "ui/TextDocument.h"

#include <QCollator>
#include <QCollatorSortKey>

#include <algorithm>
#include <numeric>
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
    } else if (operation == LineOperation::MoveDown) {
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
        QCollator collator; // the user's locale
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::vector<QCollatorSortKey> keys;
        keys.reserve(lines.size());
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const QByteArrayView text = content(i);
            keys.push_back(collator.sortKey(document.decoder().decodePlain(
                {reinterpret_cast<const std::uint8_t*>(text.data()), static_cast<std::size_t>(text.size())})));
        }
        std::vector<std::size_t> order(lines.size());
        std::iota(order.begin(), order.end(), 0);
        const bool descending = operation == LineOperation::SortDescending;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return descending ? keys[b].compare(keys[a]) < 0 : keys[a].compare(keys[b]) < 0;
        });
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
    case LineOperation::Duplicate: {
        out = bytesOf(rangeStart, rangeEnd).toByteArray();
        if (lines.back().end == lines.back().contentEnd) { // the end of the file: the copy needs a break
            const std::optional<QByteArray> lineEnd = document.encode(document.lineTerminator());
            if (!lineEnd)
                return std::nullopt;
            out += *lineEnd;
        }
        copyOffset = static_cast<std::uint64_t>(out.size());
        out += bytesOf(rangeStart, rangeEnd);
        for (std::size_t i = 0; i < lines.size(); ++i)
            landed[i] = lines[i].start + copyOffset; // the selection goes to the copy
        edit.lines = lines.size();
        wholeSelection = false;
        break;
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

} // namespace aiedit::ui
