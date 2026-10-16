#include "core/ByteDiff.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace aiedit::core {

namespace {

constexpr std::size_t kChunk = 64 * 1024;
constexpr std::uint64_t kMergeGap = 8;        // differences closer than this are one range
constexpr std::uint64_t kShiftWindow = 4096;  // how far after the first difference to look
constexpr std::size_t kProbe = 32;            // bytes looked for on the other side
constexpr std::size_t kVerify = 512;          // bytes that must then line up
constexpr std::size_t kMinVerify = 64;

// A probe of one or two byte values (zeros, padding) lines up anywhere and proves nothing.
bool isTelling(std::span<const std::uint8_t> probe)
{
    bool seen[256] = {};
    int distinct = 0;
    for (const std::uint8_t byte : probe)
        if (!seen[byte]) {
            seen[byte] = true;
            ++distinct;
        }
    return distinct >= 4;
}

// How many of n bytes are the same from a[ai] and b[bi] on, as far as both go.
std::size_t sameBytes(std::span<const std::uint8_t> a, std::uint64_t ai, std::span<const std::uint8_t> b,
                      std::uint64_t bi, std::size_t n, std::size_t* compared)
{
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>({n, a.size() - ai, b.size() - bi}));
    std::size_t same = 0;
    for (std::size_t i = 0; i < count; ++i)
        same += a[ai + i] == b[bi + i];
    *compared = count;
    return same;
}

// Where the bytes of `from` at `at` reappear in `in` near the same offset, as a distance (in - from);
// nullopt if they do not, or prove nothing.
std::optional<std::int64_t> findMove(std::span<const std::uint8_t> from, std::span<const std::uint8_t> in, std::uint64_t at)
{
    if (at + kProbe > from.size())
        return std::nullopt;
    const auto probe = from.subspan(static_cast<std::size_t>(at), kProbe);
    if (!isTelling(probe))
        return std::nullopt;
    const std::uint64_t lo = at > kShiftWindow ? at - kShiftWindow : 0;
    const std::uint64_t hi = std::min<std::uint64_t>(in.size(), at + kShiftWindow + kProbe);
    if (lo >= hi)
        return std::nullopt;
    const auto window = in.subspan(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo));
    const std::boyer_moore_horspool_searcher searcher(probe.begin(), probe.end());
    std::optional<std::int64_t> best;
    for (auto it = window.begin();;) {
        const auto found = std::search(it, window.end(), searcher);
        if (found == window.end())
            break;
        const std::uint64_t where = lo + static_cast<std::uint64_t>(found - window.begin());
        const auto distance = static_cast<std::int64_t>(where) - static_cast<std::int64_t>(at);
        std::size_t compared = 0;
        const std::size_t same = sameBytes(from, at, in, where, kVerify, &compared);
        if (distance != 0 && compared >= kMinVerify && same * 10 >= compared * 9 &&
            (!best || std::abs(distance) < std::abs(*best)))
            best = distance;
        it = found + 1;
    }
    return best;
}

} // namespace

ByteComparison compareBytes(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
                            const std::function<bool()>& cancelled, std::size_t maxRanges)
{
    ByteComparison result;
    result.leftSize = left.size();
    result.rightSize = right.size();
    const std::size_t common = std::min(left.size(), right.size());
    std::optional<ByteRange> open;
    const auto add = [&](const ByteRange& range) {
        if (result.ranges.size() < maxRanges)
            result.ranges.push_back(range);
        else
            result.limited = true;
    };
    for (std::size_t chunk = 0; chunk < common; chunk += kChunk) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        const std::size_t n = std::min(kChunk, common - chunk);
        if (std::memcmp(left.data() + chunk, right.data() + chunk, n) == 0)
            continue;
        for (std::size_t i = chunk; i < chunk + n; ++i) {
            if (left[i] == right[i])
                continue;
            ++result.differentBytes;
            if (open && i - open->end() < kMergeGap) {
                open->length = i + 1 - open->start;
            } else {
                if (open)
                    add(*open);
                open = ByteRange{i, 1};
            }
        }
    }
    if (open)
        add(*open);
    if (left.size() != right.size()) {
        const std::uint64_t extra = std::max(left.size(), right.size()) - common;
        result.differentBytes += extra;
        add({common, extra});
    }

    // Bytes inserted or removed near the first difference shift everything after it: most bytes
    // then differ. Where that is so, look for the same bytes nearby on the other side.
    if (!result.ranges.empty() && result.ranges.front().start < common) {
        const std::uint64_t at = result.ranges.front().start;
        const std::uint64_t end = std::min<std::uint64_t>(common, at + kShiftWindow);
        std::uint64_t differing = 0;
        for (std::uint64_t i = at; i < end; ++i)
            differing += left[i] != right[i];
        if (end - at >= kMinVerify && differing * 2 > end - at) {
            const std::optional<std::int64_t> inserted = findMove(left, right, at); // right = left moved on
            std::optional<std::int64_t> removed = findMove(right, left, at);        // left = right moved on
            if (removed)
                removed = -*removed;
            std::optional<std::int64_t> shift = inserted;
            if (removed && (!shift || std::abs(*removed) < std::abs(*shift)))
                shift = removed;
            if (shift) {
                result.shiftAt = at;
                result.shift = *shift;
            }
        }
    }
    return result;
}

} // namespace aiedit::core
