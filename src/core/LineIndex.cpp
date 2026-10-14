#include "core/LineIndex.h"

#include <algorithm>
#include <cstring>

namespace cppedit::core {

namespace {

constexpr std::uint64_t kNotFound = ~std::uint64_t{0};
// How often the indexing thread makes its progress visible to readers.
constexpr std::size_t kPublishEveryCheckpoints = 64;
constexpr std::uint64_t kPublishEveryBytes = 8ull * 1024 * 1024;
constexpr std::uint64_t kBuildChunkBytes = 64ull * 1024 * 1024;

// Offset of the first code unit equal to `value` in [from, to), or kNotFound.
std::uint64_t findUnit(std::span<const std::uint8_t> data, std::uint64_t from, std::uint64_t to, std::uint32_t value,
                       CharScheme scheme)
{
    const std::size_t unit = codeUnitSize(scheme);
    if (unit == 1) {
        const void* hit = std::memchr(data.data() + from, static_cast<int>(value), static_cast<std::size_t>(to - from));
        return hit ? static_cast<std::uint64_t>(static_cast<const std::uint8_t*>(hit) - data.data()) : kNotFound;
    }
    for (std::uint64_t p = from; p + unit <= to; p += unit) {
        if (codeUnitAt(data, static_cast<std::size_t>(p), scheme) == value)
            return p;
    }
    return kNotFound;
}

bool isHighSurrogate(std::uint32_t unit)
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

// Where to cut a row that would exceed kMaxRowBytes: a character boundary in (start, limit].
std::uint64_t splitPoint(std::span<const std::uint8_t> data, std::uint64_t start, std::uint64_t limit,
                         CharScheme scheme)
{
    switch (scheme) {
    case CharScheme::SingleByte:
        return limit;
    case CharScheme::Utf8: {
        // Back off over at most three continuation bytes to the start of the character.
        std::uint64_t p = limit;
        while (p > start && limit - p < 3 && (data[p] & 0xC0) == 0x80)
            --p;
        return p > start ? p : limit;
    }
    case CharScheme::Utf16LE:
    case CharScheme::Utf16BE: {
        std::uint64_t p = start + (limit - start) / 2 * 2;
        if (p >= start + 4 && isHighSurrogate(codeUnitAt(data, static_cast<std::size_t>(p - 2), scheme)))
            p -= 2; // keep surrogate pairs together
        return p;
    }
    case CharScheme::Utf32LE:
    case CharScheme::Utf32BE:
        return start + (limit - start) / 4 * 4;
    default: {
        // Lead and trail bytes of legacy CJK encodings overlap, so walk forward from a known boundary.
        std::uint64_t p = start;
        while (p < limit) {
            const std::size_t length = charLength(data.subspan(static_cast<std::size_t>(p)), scheme);
            if (p + length > limit)
                break;
            p += length;
        }
        return p > start ? p : limit;
    }
    }
}

} // namespace

LineEndings countLineEndings(std::span<const std::uint8_t> sample, CharScheme scheme)
{
    LineEndings result;
    const std::size_t unit = codeUnitSize(scheme);
    bool afterCr = false;
    for (std::size_t p = 0; p + unit <= sample.size(); p += unit) {
        const std::uint32_t value = codeUnitAt(sample, p, scheme);
        if (value == '\n') {
            if (afterCr) {
                --result.cr;
                ++result.crlf;
            } else {
                ++result.lf;
            }
        } else if (value == '\r') {
            ++result.cr;
        }
        afterCr = value == '\r';
    }
    return result;
}

LineIndex::LineIndex(std::span<const std::uint8_t> data, CharScheme scheme, LineBreak lineBreak,
                     std::uint64_t textStart)
    : m_data(data), m_scheme(scheme), m_lineBreak(lineBreak), m_textStart(std::min<std::uint64_t>(textStart, data.size()))
{
    m_progress.bytes = m_textStart;
    m_lastPublishedBytes = m_textStart;
}

Row LineIndex::readRow(std::span<const std::uint8_t> data, std::uint64_t start, std::uint64_t line, bool continuation,
                       CharScheme scheme, LineBreak lineBreak)
{
    const std::uint64_t size = data.size();
    const std::size_t unit = codeUnitSize(scheme);
    const std::uint64_t limit = std::min<std::uint64_t>(size, start + kMaxRowBytes);
    const std::uint32_t breakUnit = lineBreak == LineBreak::Lf ? '\n' : '\r';

    // A terminator directly after a full-length row still belongs to that row.
    const std::uint64_t searchEnd = std::min<std::uint64_t>(size, limit + unit);
    if (const std::uint64_t at = findUnit(data, start, searchEnd, breakUnit, scheme); at != kNotFound) {
        std::uint64_t contentEnd = at;
        if (lineBreak == LineBreak::Lf && contentEnd >= start + unit &&
            codeUnitAt(data, static_cast<std::size_t>(contentEnd - unit), scheme) == '\r')
            contentEnd -= unit;
        return {start, contentEnd, at + unit, line, continuation};
    }
    if (limit >= size)
        return {start, size, size, line, continuation};
    const std::uint64_t split = splitPoint(data, start, limit, scheme);
    return {start, split, split, line, continuation};
}

Row LineIndex::nextRow(std::span<const std::uint8_t> data, const Row& previous, CharScheme scheme,
                       LineBreak lineBreak)
{
    // A split row ends where its text ends; a terminated one has the terminator in between.
    const bool terminated = previous.end != previous.contentEnd;
    return readRow(data, previous.end, terminated ? previous.line + 1 : previous.line, !terminated, scheme,
                   lineBreak);
}

Row LineIndex::readRowAt(const Checkpoint& at) const
{
    return readRow(m_data, at.offset, at.line, at.continuation, m_scheme, m_lineBreak);
}

bool LineIndex::build()
{
    for (;;) {
        if (buildSome(kBuildChunkBytes))
            return true;
        if (m_cancelled.load(std::memory_order_relaxed))
            return false;
    }
}

bool LineIndex::buildSome(std::uint64_t maxBytes)
{
    const std::uint64_t size = m_data.size();
    std::uint64_t next = m_lastBuilt ? m_lastBuilt->end : m_textStart;
    const std::uint64_t stop = next + std::min(maxBytes, size - next);

    while (next < size && next < stop) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            publish(false);
            return false;
        }
        const Row row = m_lastBuilt ? nextRow(m_data, *m_lastBuilt, m_scheme, m_lineBreak)
                                    : readRow(m_data, m_textStart, 0, false, m_scheme, m_lineBreak);
        if (m_builtRows % kRowsPerCheckpoint == 0)
            m_pending.push_back({row.start, row.line, row.continuation});
        ++m_builtRows;
        m_lastBuilt = row;
        next = row.end;
        if (m_pending.size() >= kPublishEveryCheckpoints || next - m_lastPublishedBytes >= kPublishEveryBytes)
            publish(false);
    }

    const bool complete = next >= size;
    publish(complete);
    return complete;
}

void LineIndex::publish(bool complete)
{
    const std::lock_guard lock(m_mutex);
    m_checkpoints.insert(m_checkpoints.end(), m_pending.begin(), m_pending.end());
    m_pending.clear();
    m_progress.rows = m_builtRows;
    m_progress.lines = m_lastBuilt ? m_lastBuilt->line + 1 : 0;
    m_progress.bytes = m_lastBuilt ? m_lastBuilt->end : m_textStart;
    m_progress.complete = complete;
    m_lastPublishedBytes = m_progress.bytes;
}

bool LineIndex::isComplete() const
{
    const std::lock_guard lock(m_mutex);
    return m_progress.complete;
}

std::uint64_t LineIndex::indexedBytes() const
{
    const std::lock_guard lock(m_mutex);
    return m_progress.bytes;
}

std::uint64_t LineIndex::rowCount() const
{
    const std::lock_guard lock(m_mutex);
    return m_progress.rows;
}

std::uint64_t LineIndex::lineCount() const
{
    const std::lock_guard lock(m_mutex);
    return m_progress.lines;
}

Row LineIndex::row(std::uint64_t index) const
{
    const std::vector<Row> found = rows(index, 1);
    return found.empty() ? Row{} : found.front();
}

std::vector<Row> LineIndex::rows(std::uint64_t first, std::size_t count) const
{
    Checkpoint checkpoint{};
    std::uint64_t total = 0;
    {
        const std::lock_guard lock(m_mutex);
        total = m_progress.rows;
        if (first >= total || count == 0)
            return {};
        checkpoint = m_checkpoints[static_cast<std::size_t>(first / kRowsPerCheckpoint)];
    }

    const std::uint64_t last = std::min<std::uint64_t>(total, first + count); // exclusive
    std::vector<Row> result;
    result.reserve(static_cast<std::size_t>(last - first));
    Row row = readRowAt(checkpoint);
    for (std::uint64_t i = first - first % kRowsPerCheckpoint;; ++i) {
        if (i >= first)
            result.push_back(row);
        if (i + 1 >= last)
            break;
        row = nextRow(m_data, row, m_scheme, m_lineBreak);
    }
    return result;
}

std::uint64_t LineIndex::rowIndexAt(std::uint64_t offset) const
{
    Checkpoint checkpoint{};
    std::uint64_t index = 0;
    std::uint64_t total = 0;
    {
        const std::lock_guard lock(m_mutex);
        total = m_progress.rows;
        if (total == 0)
            return 0;
        const auto it = std::upper_bound(m_checkpoints.begin(), m_checkpoints.end(), offset,
                                         [](std::uint64_t value, const Checkpoint& c) { return value < c.offset; });
        const auto position = it == m_checkpoints.begin() ? 0 : static_cast<std::size_t>(it - m_checkpoints.begin() - 1);
        checkpoint = m_checkpoints[position];
        index = position * kRowsPerCheckpoint;
    }

    Row row = readRowAt(checkpoint);
    while (row.end <= offset && index + 1 < total) {
        row = nextRow(m_data, row, m_scheme, m_lineBreak);
        ++index;
    }
    return index;
}

std::optional<std::uint64_t> LineIndex::rowIndexOfLine(std::uint64_t line) const
{
    Checkpoint checkpoint{};
    std::uint64_t index = 0;
    std::uint64_t total = 0;
    {
        const std::lock_guard lock(m_mutex);
        total = m_progress.rows;
        if (line >= m_progress.lines)
            return std::nullopt;
        // Checkpoints before or at the line's first row; its continuation rows come after it.
        const auto it = std::partition_point(m_checkpoints.begin(), m_checkpoints.end(), [line](const Checkpoint& c) {
            return c.line < line || (c.line == line && !c.continuation);
        });
        const auto position = it == m_checkpoints.begin() ? 0 : static_cast<std::size_t>(it - m_checkpoints.begin() - 1);
        checkpoint = m_checkpoints[position];
        index = position * kRowsPerCheckpoint;
    }

    Row row = readRowAt(checkpoint);
    while (row.line != line || row.continuation) {
        if (row.line > line || index + 1 >= total)
            return std::nullopt;
        row = nextRow(m_data, row, m_scheme, m_lineBreak);
        ++index;
    }
    return index;
}

std::uint64_t snapToCharStart(const LineIndex& index, std::uint64_t offset)
{
    const auto data = index.data();
    const CharScheme scheme = index.scheme();
    const std::uint64_t start = index.textStart();
    offset = std::clamp<std::uint64_t>(offset, start, data.size());
    if (offset == data.size())
        return offset;

    switch (scheme) {
    case CharScheme::SingleByte:
        return offset;
    case CharScheme::Utf8:
        // Back off to a lead byte if the character it starts covers offset; a stray
        // continuation byte is a character of its own.
        for (std::uint64_t back = 1; back <= 3 && back <= offset - start; ++back) {
            const std::uint64_t p = offset - back;
            if ((data[p] & 0xC0) != 0x80)
                return p + charLength(data.subspan(p), CharScheme::Utf8) > offset ? p : offset;
        }
        return offset;
    case CharScheme::Utf16LE:
    case CharScheme::Utf16BE:
    case CharScheme::Utf32LE:
    case CharScheme::Utf32BE: {
        const std::size_t unit = codeUnitSize(scheme);
        std::uint64_t p = start + (offset - start) / unit * unit;
        if (unit == 2 && p >= start + 2 && p + 2 <= data.size() &&
            codeUnitAt(data, static_cast<std::size_t>(p), scheme) >= 0xDC00 &&
            codeUnitAt(data, static_cast<std::size_t>(p), scheme) <= 0xDFFF &&
            isHighSurrogate(codeUnitAt(data, static_cast<std::size_t>(p - 2), scheme)))
            p -= 2;
        return p;
    }
    default: {
        // Legacy CJK: walk forward from the start of the row, a known character boundary.
        if (index.rowCount() == 0 || offset >= index.indexedBytes())
            return offset;
        std::uint64_t p = index.row(index.rowIndexAt(offset)).start;
        while (p < offset) {
            const std::size_t length = charLength(data.subspan(static_cast<std::size_t>(p)), scheme);
            if (p + length > offset)
                return p;
            p += length;
        }
        return p;
    }
    }
}

} // namespace cppedit::core
