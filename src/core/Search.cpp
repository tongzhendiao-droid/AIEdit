#include "core/Search.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace aiedit::core {

namespace {

constexpr std::uint64_t kChunkBytes = 16ull * 1024 * 1024;

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool isSeparator(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == '-' || c == ':' || c == '{' ||
           c == '}';
}

bool isCancelled(const SearchControl& control)
{
    return control.cancelled && control.cancelled->load(std::memory_order_relaxed);
}

class Matcher
{
public:
    explicit Matcher(const BytePattern& pattern)
        : m_pattern(pattern), m_searcher(pattern.bytes.begin(), pattern.bytes.end())
    {
        const auto anchor = std::find(pattern.wildcard.begin(), pattern.wildcard.end(), false);
        m_anchor = pattern.wildcard.empty() ? 0 : static_cast<std::size_t>(anchor - pattern.wildcard.begin());
    }

    // The first match whose start lies in [first, last); the caller guarantees that a match
    // starting before `last` fits in the data.
    std::optional<std::uint64_t> find(std::span<const std::uint8_t> data, std::uint64_t first, std::uint64_t last) const
    {
        const std::size_t n = m_pattern.size();
        if (!m_pattern.hasWildcards()) {
            const auto begin = data.begin() + static_cast<std::ptrdiff_t>(first);
            const auto end = data.begin() + static_cast<std::ptrdiff_t>(last + n - 1);
            const auto it = std::search(begin, end, m_searcher);
            if (it == end)
                return std::nullopt;
            return static_cast<std::uint64_t>(it - data.begin());
        }

        // Jump between occurrences of the first fixed byte, then compare the rest.
        const std::uint8_t anchorByte = m_pattern.bytes[m_anchor];
        std::uint64_t candidate = first;
        while (candidate < last) {
            const void* hit = std::memchr(data.data() + candidate + m_anchor, anchorByte,
                                          static_cast<std::size_t>(last - candidate));
            if (!hit)
                return std::nullopt;
            candidate = static_cast<std::uint64_t>(static_cast<const std::uint8_t*>(hit) - data.data()) - m_anchor;
            if (matchesAt(data, candidate))
                return candidate;
            ++candidate;
        }
        return std::nullopt;
    }

private:
    bool matchesAt(std::span<const std::uint8_t> data, std::uint64_t position) const
    {
        for (std::size_t k = 0; k < m_pattern.size(); ++k) {
            if (!m_pattern.wildcard[k] && data[position + k] != m_pattern.bytes[k])
                return false;
        }
        return true;
    }

    const BytePattern& m_pattern;
    std::boyer_moore_horspool_searcher<std::vector<std::uint8_t>::const_iterator> m_searcher;
    std::size_t m_anchor = 0;
};

} // namespace

bool BytePattern::hasWildcards() const
{
    return std::find(wildcard.begin(), wildcard.end(), true) != wildcard.end();
}

std::optional<BytePattern> parseHexPattern(std::string_view text)
{
    BytePattern pattern;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (isSeparator(c)) {
            ++i;
            continue;
        }
        if (c == '0' && i + 2 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && hexDigit(text[i + 2]) >= 0) {
            i += 2; // C-style 0x prefix
            continue;
        }
        if (c == '?') {
            i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
            pattern.bytes.push_back(0);
            pattern.wildcard.push_back(true);
            continue;
        }
        const int high = hexDigit(c);
        if (high < 0)
            return std::nullopt;
        if (i + 1 < text.size() && hexDigit(text[i + 1]) >= 0) {
            pattern.bytes.push_back(static_cast<std::uint8_t>(high * 16 + hexDigit(text[i + 1])));
            i += 2;
        } else if (i + 1 >= text.size() || isSeparator(text[i + 1])) {
            pattern.bytes.push_back(static_cast<std::uint8_t>(high)); // a lone digit: "A" is 0x0A
            i += 1;
        } else {
            return std::nullopt;
        }
        pattern.wildcard.push_back(false);
    }

    if (pattern.bytes.empty() || std::all_of(pattern.wildcard.begin(), pattern.wildcard.end(), [](bool w) { return w; }))
        return std::nullopt;
    if (!pattern.hasWildcards())
        pattern.wildcard.assign(pattern.bytes.size(), false);
    return pattern;
}

std::optional<std::uint64_t> findPattern(std::span<const std::uint8_t> data, const BytePattern& pattern,
                                         std::uint64_t from, bool forward, const SearchControl& control)
{
    const std::uint64_t size = data.size();
    const std::uint64_t n = pattern.size();
    if (n == 0 || n > size)
        return std::nullopt;
    BytePattern normalized = pattern;
    if (normalized.wildcard.size() != normalized.bytes.size())
        normalized.wildcard.assign(normalized.bytes.size(), false);
    const Matcher matcher(normalized);
    const std::uint64_t starts = size - n + 1; // valid match starts are [0, starts)

    if (forward) {
        for (std::uint64_t first = from; first < starts;) {
            if (isCancelled(control))
                return std::nullopt;
            const std::uint64_t last = std::min(starts, first + kChunkBytes);
            if (const auto match = matcher.find(data, first, last))
                return match;
            if (control.progress)
                control.progress(last - from);
            first = last;
        }
        return std::nullopt;
    }

    for (std::uint64_t last = std::min(from, starts); last > 0;) {
        if (isCancelled(control))
            return std::nullopt;
        const std::uint64_t first = last > kChunkBytes ? last - kChunkBytes : 0;
        std::optional<std::uint64_t> latest;
        for (std::uint64_t at = first; const auto match = matcher.find(data, at, last); at = *match + 1)
            latest = match;
        if (latest)
            return latest;
        if (control.progress)
            control.progress(from - first);
        last = first;
    }
    return std::nullopt;
}

} // namespace aiedit::core
