#include "core/ByteDiff.h"

#include "core/Diff.h"

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
// Files lined up again after an insertion are at most this large; larger ones stay compared at
// the same offsets.
constexpr std::uint64_t kMaxAligned = 256ull * 1024 * 1024;
// Middles up to this are diffed byte by byte; larger ones by chunks first.
constexpr std::size_t kMaxByteDiff = 1024 * 1024;
// Chunks: cut where a rolling hash of the last kWindow bytes has its low bits zero, so about
// every 2^kChunkBits bytes, never longer than kMaxChunk.
constexpr std::size_t kWindow = 16;
constexpr unsigned kChunkBits = 6;
constexpr std::size_t kMaxChunk = 1024;

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

struct Hunk
{
    std::uint64_t left, leftLength, right, rightLength;
};

// Byte by byte, with the common start and end set aside by diffSequences.
std::vector<Hunk> diffBytes(std::span<const std::uint8_t> left, std::uint64_t leftBase,
                            std::span<const std::uint8_t> right, std::uint64_t rightBase,
                            const std::function<bool()>& cancelled)
{
    const std::vector<std::uint64_t> a(left.begin(), left.end());
    const std::vector<std::uint64_t> b(right.begin(), right.end());
    std::vector<Hunk> hunks;
    for (const DiffHunk& h : diffSequences(a, b, cancelled))
        hunks.push_back({leftBase + h.leftStart, h.leftCount, rightBase + h.rightStart, h.rightCount});
    return hunks;
}

// Where content-defined chunks end in `data`.
std::vector<std::uint64_t> chunkEnds(std::span<const std::uint8_t> data)
{
    std::vector<std::uint64_t> ends;
    std::uint64_t hash = 0;
    std::size_t start = 0;
    // A rolling sum of the window, mixed: cheap, and the same wherever the bytes are.
    for (std::size_t i = 0; i < data.size(); ++i) {
        hash += data[i] * 0x9E3779B97F4A7C15ull;
        if (i >= kWindow)
            hash -= data[i - kWindow] * 0x9E3779B97F4A7C15ull;
        const std::uint64_t mixed = (hash ^ (hash >> 29)) * 0xBF58476D1CE4E5B9ull;
        if (i + 1 - start >= kMaxChunk || (i + 1 - start >= kWindow && (mixed >> (64 - kChunkBits)) == 0)) {
            ends.push_back(i + 1);
            start = i + 1;
        }
    }
    if (start < data.size())
        ends.push_back(data.size());
    return ends;
}

std::vector<Hunk> diffMiddle(std::span<const std::uint8_t> left, std::uint64_t leftBase,
                             std::span<const std::uint8_t> right, std::uint64_t rightBase,
                             const std::function<bool()>& cancelled)
{
    if (left.size() <= kMaxByteDiff && right.size() <= kMaxByteDiff)
        return diffBytes(left, leftBase, right, rightBase, cancelled);
    const std::vector<std::uint64_t> leftEnds = chunkEnds(left);
    const std::vector<std::uint64_t> rightEnds = chunkEnds(right);
    auto keys = [](std::span<const std::uint8_t> data, const std::vector<std::uint64_t>& ends) {
        std::vector<std::uint64_t> out;
        out.reserve(ends.size());
        std::uint64_t start = 0;
        for (const std::uint64_t end : ends) {
            out.push_back(hashBytes(data.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start))));
            start = end;
        }
        return out;
    };
    auto chunkStart = [](const std::vector<std::uint64_t>& ends, std::uint64_t index) {
        return index == 0 ? 0 : ends[static_cast<std::size_t>(index - 1)];
    };
    std::vector<Hunk> hunks;
    for (const DiffHunk& h : diffSequences(keys(left, leftEnds), keys(right, rightEnds), cancelled)) {
        const std::uint64_t l0 = chunkStart(leftEnds, h.leftStart);
        const std::uint64_t l1 = chunkStart(leftEnds, h.leftStart + h.leftCount);
        const std::uint64_t r0 = chunkStart(rightEnds, h.rightStart);
        const std::uint64_t r1 = chunkStart(rightEnds, h.rightStart + h.rightCount);
        // Inside the changed chunks, byte by byte when small enough; else as one change.
        if (l1 - l0 <= 64 * 1024 && r1 - r0 <= 64 * 1024 && l1 > l0 && r1 > r0) {
            for (const Hunk& inner :
                 diffBytes(left.subspan(static_cast<std::size_t>(l0), static_cast<std::size_t>(l1 - l0)), leftBase + l0,
                           right.subspan(static_cast<std::size_t>(r0), static_cast<std::size_t>(r1 - r0)), rightBase + r0,
                           cancelled))
                hunks.push_back(inner);
        } else {
            hunks.push_back({leftBase + l0, l1 - l0, rightBase + r0, r1 - r0});
        }
    }
    return hunks;
}

} // namespace

std::vector<ByteSegment> alignBytes(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
                                    const std::function<bool()>& cancelled)
{
    // The common start and end, set aside first: most of two versions of a binary.
    std::size_t prefix = 0;
    const std::size_t shorter = std::min(left.size(), right.size());
    while (prefix < shorter && left[prefix] == right[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < shorter - prefix && left[left.size() - 1 - suffix] == right[right.size() - 1 - suffix])
        ++suffix;
    std::vector<Hunk> hunks =
        diffMiddle(left.subspan(prefix, left.size() - prefix - suffix), prefix,
                   right.subspan(prefix, right.size() - prefix - suffix), prefix, cancelled);
    if (cancelled && cancelled())
        return {};

    // Same bytes between hunks fewer than kMergeGap: one change with them.
    std::vector<Hunk> merged;
    for (const Hunk& h : hunks) {
        if (!merged.empty() && h.left - (merged.back().left + merged.back().leftLength) < kMergeGap &&
            h.right - (merged.back().right + merged.back().rightLength) < kMergeGap) {
            Hunk& last = merged.back();
            last.leftLength = h.left + h.leftLength - last.left;
            last.rightLength = h.right + h.rightLength - last.right;
        } else {
            merged.push_back(h);
        }
    }

    std::vector<ByteSegment> segments;
    std::uint64_t position = 0;
    std::uint64_t l = 0;
    std::uint64_t r = 0;
    auto add = [&](std::uint64_t leftLength, std::uint64_t rightLength, bool same) {
        if (leftLength == 0 && rightLength == 0)
            return;
        ByteSegment s{position, l, leftLength, r, rightLength, same};
        segments.push_back(s);
        position += s.length();
        l += leftLength;
        r += rightLength;
    };
    for (const Hunk& h : merged) {
        add(h.left - l, h.right - r, true); // the two runs of same bytes have one length
        add(h.leftLength, h.rightLength, false);
    }
    add(left.size() - l, right.size() - r, left.size() - l == right.size() - r);
    return segments;
}

std::uint64_t positionCount(const ByteComparison& comparison)
{
    if (comparison.segments.empty())
        return std::max(comparison.leftSize, comparison.rightSize);
    const ByteSegment& last = comparison.segments.back();
    return last.position + last.length();
}

BytesAt bytesAt(const ByteComparison& comparison, std::span<const std::uint8_t> left,
                std::span<const std::uint8_t> right, std::uint64_t position)
{
    BytesAt at;
    if (comparison.segments.empty()) {
        if (position < left.size())
            at.left = position;
        if (position < right.size())
            at.right = position;
        at.same = at.left && at.right && left[static_cast<std::size_t>(position)] == right[static_cast<std::size_t>(position)];
        return at;
    }
    const auto& segments = comparison.segments;
    const auto it = std::upper_bound(segments.begin(), segments.end(), position,
                                     [](std::uint64_t p, const ByteSegment& s) { return p < s.position; });
    if (it == segments.begin())
        return at;
    const ByteSegment& s = *(it - 1);
    const std::uint64_t into = position - s.position;
    if (into < s.leftLength)
        at.left = s.left + into;
    if (into < s.rightLength)
        at.right = s.right + into;
    at.same = s.same;
    return at;
}

SideRanges sideRanges(const ByteComparison& comparison, const ByteRange& positions)
{
    if (comparison.segments.empty()) {
        auto clip = [&](std::uint64_t size) {
            const std::uint64_t start = std::min(positions.start, size);
            return ByteRange{start, std::min(positions.end(), size) - start};
        };
        return {clip(comparison.leftSize), clip(comparison.rightSize)};
    }
    const auto& segments = comparison.segments;
    auto it = std::upper_bound(segments.begin(), segments.end(), positions.start,
                               [](std::uint64_t p, const ByteSegment& s) { return p < s.position; });
    if (it != segments.begin())
        --it;
    std::optional<ByteRange> left;
    std::optional<ByteRange> right;
    std::uint64_t leftAt = it == segments.end() ? comparison.leftSize : it->left;
    std::uint64_t rightAt = it == segments.end() ? comparison.rightSize : it->right;
    for (; it != segments.end() && it->position < positions.end(); ++it) {
        const std::uint64_t from = std::max(positions.start, it->position) - it->position;
        const std::uint64_t to = std::min(positions.end(), it->position + it->length()) - it->position;
        auto take = [&](std::uint64_t base, std::uint64_t length, std::optional<ByteRange>& range, std::uint64_t& at) {
            const std::uint64_t a = std::min(from, length);
            const std::uint64_t b = std::min(to, length);
            if (a < b) {
                if (!range)
                    range = ByteRange{base + a, 0};
                range->length = base + b - range->start;
            } else if (!range) {
                at = base + a;
            }
        };
        take(it->left, it->leftLength, left, leftAt);
        take(it->right, it->rightLength, right, rightAt);
    }
    return {left.value_or(ByteRange{leftAt, 0}), right.value_or(ByteRange{rightAt, 0})};
}

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

    // Bytes inserted or removed (the sizes differ, or bytes look moved): line the files up again
    // after them, and the differences are of positions. Unless they share little: lining up
    // files that differ everywhere shows nothing.
    const bool moved = result.shiftAt || left.size() != right.size();
    if (moved && !result.ranges.empty() && left.size() <= kMaxAligned && right.size() <= kMaxAligned) {
        std::vector<ByteSegment> segments = alignBytes(left, right, cancelled);
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        std::uint64_t same = 0;
        for (const ByteSegment& s : segments)
            same += s.same ? s.length() : 0;
        if (same * 2 < std::max(left.size(), right.size()))
            return result;
        result.segments = std::move(segments);
        result.ranges.clear();
        result.limited = false;
        result.differentBytes = 0;
        for (const ByteSegment& s : result.segments) {
            if (s.same)
                continue;
            result.differentBytes += s.length();
            add({s.position, s.length()});
        }
    }
    return result;
}

} // namespace aiedit::core
