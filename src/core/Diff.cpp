#include "core/Diff.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace aiedit::core {

namespace {

using Index = std::int64_t;

constexpr Index kIndexMax = std::numeric_limits<Index>::max();
// Below this many steps the search always finds the shortest script; above it, the more
// the inputs have, the longer it may look (about their square root), as in GNU diff.
constexpr Index kMinTooExpensive = 4096;
// Lines longer than this are marked whole rather than compared word by word.
constexpr std::size_t kMaxInlineLength = 10'000;
// Below this share of the text in common, a changed line is marked whole.
constexpr double kMinInlineSimilarity = 0.3;

// Finds which elements of x and y are not in a longest common subsequence, marking them in
// xChanged and yChanged (which must start all clear).
class Myers
{
public:
    Myers(std::span<const std::uint64_t> x, std::span<const std::uint64_t> y, std::vector<char>& xChanged,
          std::vector<char>& yChanged, const std::function<bool()>& cancelled)
        : m_x(x), m_y(y), m_xChanged(xChanged), m_yChanged(yChanged), m_cancelled(cancelled),
          m_offset(static_cast<Index>(y.size()) + 1),
          m_forward(x.size() + y.size() + 3), m_backward(x.size() + y.size() + 3)
    {
        Index diagonals = static_cast<Index>(x.size() + y.size()) + 3;
        m_tooExpensive = 1;
        for (; diagonals != 0; diagonals >>= 2)
            m_tooExpensive <<= 1;
        m_tooExpensive = std::max(m_tooExpensive, kMinTooExpensive);
    }

    // False if cancelled.
    bool run()
    {
        struct Work
        {
            Index xoff, xlim, yoff, ylim;
            bool minimal;
        };
        std::vector<Work> work{{0, static_cast<Index>(m_x.size()), 0, static_cast<Index>(m_y.size()), false}};
        while (!work.empty()) {
            Work w = work.back();
            work.pop_back();
            while (w.xoff < w.xlim && w.yoff < w.ylim && at(m_x, w.xoff) == at(m_y, w.yoff)) {
                ++w.xoff;
                ++w.yoff;
            }
            while (w.xoff < w.xlim && w.yoff < w.ylim && at(m_x, w.xlim - 1) == at(m_y, w.ylim - 1)) {
                --w.xlim;
                --w.ylim;
            }
            if (w.xoff == w.xlim || w.yoff == w.ylim) {
                markAll(w.xoff, w.xlim, w.yoff, w.ylim);
                continue;
            }
            const std::optional<Split> split = middle(w.xoff, w.xlim, w.yoff, w.ylim, w.minimal);
            if (!split)
                return false;
            const bool corner = (split->x == w.xoff && split->y == w.yoff) || (split->x == w.xlim && split->y == w.ylim);
            if (corner) { // no progress possible this way: all of it differs
                markAll(w.xoff, w.xlim, w.yoff, w.ylim);
                continue;
            }
            work.push_back({split->x, w.xlim, split->y, w.ylim, split->hiMinimal});
            work.push_back({w.xoff, split->x, w.yoff, split->y, split->loMinimal});
        }
        return true;
    }

private:
    struct Split
    {
        Index x, y;
        bool loMinimal, hiMinimal;
    };

    static std::uint64_t at(std::span<const std::uint64_t> v, Index i) { return v[static_cast<std::size_t>(i)]; }
    Index& forward(Index diagonal) { return m_forward[static_cast<std::size_t>(diagonal + m_offset)]; }
    Index& backward(Index diagonal) { return m_backward[static_cast<std::size_t>(diagonal + m_offset)]; }

    void markAll(Index xoff, Index xlim, Index yoff, Index ylim)
    {
        for (Index i = xoff; i < xlim; ++i)
            m_xChanged[static_cast<std::size_t>(i)] = 1;
        for (Index j = yoff; j < ylim; ++j)
            m_yChanged[static_cast<std::size_t>(j)] = 1;
    }

    // Where the shortest edit script from (xoff, yoff) to (xlim, ylim) crosses its middle:
    // searched from both ends, one more edit at a time, until the two searches meet. Past
    // m_tooExpensive edits, unless `minimal`, the furthest point either search reached is taken.
    // Nullopt if cancelled.
    std::optional<Split> middle(Index xoff, Index xlim, Index yoff, Index ylim, bool minimal)
    {
        const Index dmin = xoff - ylim;
        const Index dmax = xlim - yoff;
        const Index fmid = xoff - yoff;
        const Index bmid = xlim - ylim;
        Index fmin = fmid;
        Index fmax = fmid;
        Index bmin = bmid;
        Index bmax = bmid;
        const bool odd = ((fmid - bmid) & 1) != 0;
        forward(fmid) = xoff;
        backward(bmid) = xlim;

        for (Index cost = 1;; ++cost) {
            if ((cost & 255) == 0 && m_cancelled && m_cancelled())
                return std::nullopt;

            if (fmin > dmin)
                forward(--fmin - 1) = -1;
            else
                ++fmin;
            if (fmax < dmax)
                forward(++fmax + 1) = -1;
            else
                --fmax;
            for (Index d = fmax; d >= fmin; d -= 2) {
                const Index lower = forward(d - 1);
                const Index upper = forward(d + 1);
                Index x = lower < upper ? upper : lower + 1;
                Index y = x - d;
                while (x < xlim && y < ylim && at(m_x, x) == at(m_y, y)) {
                    ++x;
                    ++y;
                }
                forward(d) = x;
                if (odd && bmin <= d && d <= bmax && backward(d) <= x)
                    return Split{x, y, true, true};
            }

            if (bmin > dmin)
                backward(--bmin - 1) = kIndexMax;
            else
                ++bmin;
            if (bmax < dmax)
                backward(++bmax + 1) = kIndexMax;
            else
                --bmax;
            for (Index d = bmax; d >= bmin; d -= 2) {
                const Index lower = backward(d - 1);
                const Index upper = backward(d + 1);
                Index x = lower < upper ? lower : upper - 1;
                Index y = x - d;
                while (x > xoff && y > yoff && at(m_x, x - 1) == at(m_y, y - 1)) {
                    --x;
                    --y;
                }
                backward(d) = x;
                if (!odd && fmin <= d && d <= fmax && x <= forward(d))
                    return Split{x, y, true, true};
            }

            if (minimal || cost < m_tooExpensive)
                continue;

            // Gone far enough: split where one of the searches got furthest.
            Index forwardBest = -1;
            Index forwardX = 0;
            for (Index d = fmax; d >= fmin; d -= 2) {
                Index x = std::min(forward(d), xlim);
                Index y = x - d;
                if (y > ylim) {
                    x = ylim + d;
                    y = ylim;
                }
                if (x + y > forwardBest) {
                    forwardBest = x + y;
                    forwardX = x;
                }
            }
            Index backwardBest = kIndexMax;
            Index backwardX = 0;
            for (Index d = bmax; d >= bmin; d -= 2) {
                Index x = std::max(xoff, backward(d));
                Index y = x - d;
                if (y < yoff) {
                    x = yoff + d;
                    y = yoff;
                }
                if (x + y < backwardBest) {
                    backwardBest = x + y;
                    backwardX = x;
                }
            }
            if ((xlim + ylim) - backwardBest < forwardBest - (xoff + yoff))
                return Split{forwardX, forwardBest - forwardX, true, false};
            return Split{backwardX, backwardBest - backwardX, false, true};
        }
    }

    std::span<const std::uint64_t> m_x;
    std::span<const std::uint64_t> m_y;
    std::vector<char>& m_xChanged;
    std::vector<char>& m_yChanged;
    const std::function<bool()>& m_cancelled;
    Index m_offset;
    std::vector<Index> m_forward;
    std::vector<Index> m_backward;
    Index m_tooExpensive = kMinTooExpensive;
};

// The keys that occur on one side, sorted, to look up whether a key occurs there at all.
std::vector<std::uint64_t> sortedKeys(std::span<const std::uint64_t> keys)
{
    std::vector<std::uint64_t> sorted(keys.begin(), keys.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    return sorted;
}

// Runs of changed elements as hunks.
std::vector<DiffHunk> hunksFrom(const std::vector<char>& leftChanged, const std::vector<char>& rightChanged)
{
    std::vector<DiffHunk> hunks;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < leftChanged.size() || j < rightChanged.size()) {
        const bool leftEqual = i < leftChanged.size() && !leftChanged[i];
        const bool rightEqual = j < rightChanged.size() && !rightChanged[j];
        if (leftEqual && rightEqual) {
            ++i;
            ++j;
            continue;
        }
        DiffHunk hunk{i, 0, j, 0};
        while (i < leftChanged.size() && leftChanged[i])
            ++i;
        while (j < rightChanged.size() && rightChanged[j])
            ++j;
        hunk.leftCount = i - hunk.leftStart;
        hunk.rightCount = j - hunk.rightStart;
        hunks.push_back(hunk);
    }
    return hunks;
}

// Moves blocks that were only added or only removed as far down as the same lines allow: of
// the equally short scripts, the one people read best puts an added function after the "}"
// before it rather than before the "}" after it. Touching hunks are merged.
void slideDown(std::vector<DiffHunk>& hunks, std::span<const std::uint64_t> left, std::span<const std::uint64_t> right)
{
    // A change with more lines on one side shows them after the lines it pairs. Where those extra
    // lines start like the same lines that follow, they move down past them: "}" that closes a
    // changed function then stays, and the function removed after it goes with its own "}".
    std::vector<DiffHunk> split;
    for (std::size_t h = 0; h < hunks.size(); ++h) {
        const DiffHunk hunk = hunks[h];
        const std::uint64_t pairs = std::min(hunk.leftCount, hunk.rightCount);
        const bool leftLonger = hunk.leftCount > hunk.rightCount;
        if (pairs == 0 || hunk.leftCount == hunk.rightCount) {
            split.push_back(hunk);
            continue;
        }
        const std::span<const std::uint64_t> side = leftLonger ? left : right;
        const std::uint64_t start = leftLonger ? hunk.leftStart : hunk.rightStart;
        const std::uint64_t count = leftLonger ? hunk.leftCount : hunk.rightCount;
        const std::uint64_t limit =
            h + 1 < hunks.size() ? (leftLonger ? hunks[h + 1].leftStart : hunks[h + 1].rightStart) : side.size();
        std::uint64_t moved = 0;
        while (start + count + moved < limit && side[start + pairs + moved] == side[start + count + moved])
            ++moved;
        if (moved == 0) {
            split.push_back(hunk);
            continue;
        }
        split.push_back({hunk.leftStart, pairs, hunk.rightStart, pairs});
        const std::uint64_t extra = count - pairs;
        split.push_back({hunk.leftStart + pairs + moved, leftLonger ? extra : 0, hunk.rightStart + pairs + moved,
                         leftLonger ? 0 : extra});
    }
    hunks.swap(split);

    for (std::size_t h = 0; h < hunks.size(); ++h) {
        DiffHunk& hunk = hunks[h];
        if ((hunk.leftCount == 0) == (hunk.rightCount == 0))
            continue; // a change on both sides stays where it is
        const bool removed = hunk.rightCount == 0;
        const std::span<const std::uint64_t> side = removed ? left : right;
        std::uint64_t& start = removed ? hunk.leftStart : hunk.rightStart;
        const std::uint64_t count = removed ? hunk.leftCount : hunk.rightCount;
        // The next hunk on this side, which the block must not run into.
        const std::uint64_t limit =
            h + 1 < hunks.size() ? (removed ? hunks[h + 1].leftStart : hunks[h + 1].rightStart) : side.size();
        while (start + count < limit && side[start] == side[start + count]) {
            ++hunk.leftStart;
            ++hunk.rightStart;
        }
    }
    std::vector<DiffHunk> merged;
    for (const DiffHunk& hunk : hunks) {
        if (!merged.empty()) {
            DiffHunk& last = merged.back();
            if (last.leftStart + last.leftCount == hunk.leftStart && last.rightStart + last.rightCount == hunk.rightStart) {
                last.leftCount += hunk.leftCount;
                last.rightCount += hunk.rightCount;
                continue;
            }
        }
        merged.push_back(hunk);
    }
    hunks.swap(merged);
}

bool isWordUnit(char16_t c)
{
    if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'_')
        return true;
    // Latin with accents, Greek, Cyrillic: alphabets that write words with spaces between.
    return (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) || (c >= 0x0370 && c <= 0x052F);
}

bool isBlank(char16_t c)
{
    return c == u' ' || c == u'\t';
}

struct Token
{
    std::uint32_t start;
    std::uint32_t length;
};

std::vector<Token> tokenize(std::u16string_view text)
{
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i + 1;
        if (isWordUnit(text[i])) {
            while (j < text.size() && isWordUnit(text[j]))
                ++j;
        } else if (isBlank(text[i])) {
            while (j < text.size() && isBlank(text[j]))
                ++j;
        } else if (text[i] >= 0xD800 && text[i] < 0xDC00 && j < text.size() && text[j] >= 0xDC00 && text[j] < 0xE000) {
            ++j; // a surrogate pair is one character
        }
        tokens.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j - i)});
        i = j;
    }
    return tokens;
}

std::vector<std::uint64_t> tokenKeys(std::u16string_view text, const std::vector<Token>& tokens)
{
    std::vector<std::uint64_t> keys;
    keys.reserve(tokens.size());
    for (const Token& token : tokens) {
        const auto* units = reinterpret_cast<const std::uint8_t*>(text.data() + token.start);
        keys.push_back(hashBytes({units, token.length * sizeof(char16_t)}));
    }
    return keys;
}

void addRange(std::vector<TextRange>& ranges, std::uint32_t start, std::uint32_t length)
{
    if (length == 0)
        return;
    if (!ranges.empty() && ranges.back().start + ranges.back().length == start)
        ranges.back().length += length;
    else
        ranges.push_back({start, length});
}

} // namespace

std::vector<DiffHunk> diffSequences(std::span<const std::uint64_t> left, std::span<const std::uint64_t> right,
                                    const std::function<bool()>& cancelled)
{
    // The common start and end are no work.
    std::size_t prefix = 0;
    while (prefix < left.size() && prefix < right.size() && left[prefix] == right[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < left.size() - prefix && suffix < right.size() - prefix &&
           left[left.size() - 1 - suffix] == right[right.size() - 1 - suffix])
        ++suffix;
    const auto x = left.subspan(prefix, left.size() - prefix - suffix);
    const auto y = right.subspan(prefix, right.size() - prefix - suffix);

    std::vector<char> leftChanged(left.size(), 0);
    std::vector<char> rightChanged(right.size(), 0);
    if (!x.empty() || !y.empty()) {
        // Lines without an equal on the other side are changed whatever else happens; leaving
        // them out shortens the search a lot for files that differ widely, and changes nothing
        // in the longest common subsequence.
        const std::vector<std::uint64_t> inLeft = sortedKeys(x);
        const std::vector<std::uint64_t> inRight = sortedKeys(y);
        std::vector<std::uint64_t> xKept;
        std::vector<std::uint64_t> yKept;
        std::vector<std::size_t> xIndex;
        std::vector<std::size_t> yIndex;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (std::binary_search(inRight.begin(), inRight.end(), x[i])) {
                xKept.push_back(x[i]);
                xIndex.push_back(prefix + i);
            } else {
                leftChanged[prefix + i] = 1;
            }
        }
        for (std::size_t j = 0; j < y.size(); ++j) {
            if (std::binary_search(inLeft.begin(), inLeft.end(), y[j])) {
                yKept.push_back(y[j]);
                yIndex.push_back(prefix + j);
            } else {
                rightChanged[prefix + j] = 1;
            }
        }
        std::vector<char> xKeptChanged(xKept.size(), 0);
        std::vector<char> yKeptChanged(yKept.size(), 0);
        Myers myers(xKept, yKept, xKeptChanged, yKeptChanged, cancelled);
        if (!myers.run())
            return {};
        for (std::size_t i = 0; i < xKept.size(); ++i)
            leftChanged[xIndex[i]] = xKeptChanged[i];
        for (std::size_t j = 0; j < yKept.size(); ++j)
            rightChanged[yIndex[j]] = yKeptChanged[j];
    }
    if (cancelled && cancelled())
        return {};

    std::vector<DiffHunk> hunks = hunksFrom(leftChanged, rightChanged);
    slideDown(hunks, left, right);
    return hunks;
}

InlineDiff diffInline(std::u16string_view left, std::u16string_view right)
{
    InlineDiff result;
    if (left == right)
        return result;
    const auto whole = [&] {
        result.left.clear();
        result.right.clear();
        addRange(result.left, 0, static_cast<std::uint32_t>(left.size()));
        addRange(result.right, 0, static_cast<std::uint32_t>(right.size()));
        return result;
    };
    if (left.size() > kMaxInlineLength || right.size() > kMaxInlineLength)
        return whole();

    const std::vector<Token> leftTokens = tokenize(left);
    const std::vector<Token> rightTokens = tokenize(right);
    const std::vector<std::uint64_t> leftKeys = tokenKeys(left, leftTokens);
    const std::vector<std::uint64_t> rightKeys = tokenKeys(right, rightTokens);
    std::uint64_t changed = 0;
    for (const DiffHunk& hunk : diffSequences(leftKeys, rightKeys)) {
        for (std::uint64_t t = hunk.leftStart; t < hunk.leftStart + hunk.leftCount; ++t) {
            addRange(result.left, leftTokens[t].start, leftTokens[t].length);
            changed += leftTokens[t].length;
        }
        for (std::uint64_t t = hunk.rightStart; t < hunk.rightStart + hunk.rightCount; ++t) {
            addRange(result.right, rightTokens[t].start, rightTokens[t].length);
            changed += rightTokens[t].length;
        }
    }
    // Too little in common for the fragments to mean anything.
    const auto total = static_cast<double>(left.size() + right.size());
    if (total > 0 && (total - static_cast<double>(changed)) / total < kMinInlineSimilarity)
        return whole();
    return result;
}

std::uint64_t hashBytes(std::span<const std::uint8_t> bytes, std::uint64_t seed)
{
    std::uint64_t hash = seed;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace aiedit::core
