#include "ui/FuzzyMatch.h"

#include <algorithm>
#include <limits>

namespace aiedit::ui {

namespace {

constexpr int kMatch = 1;
constexpr int kSameCase = 1;
constexpr int kAdjacent = 5;     // right after the character matched before
constexpr int kWordStart = 6;
constexpr int kSegmentStart = 8; // the start of the text, or of a name in a path
constexpr int kGap = 1;          // for each character skipped between two matches
constexpr int kMaxLead = 3;      // what skipping characters before the first match costs at most
constexpr int kMaxText = 1024;   // longer texts are matched on their end, where a path's name is
constexpr int kNone = std::numeric_limits<int>::min() / 4;

// Case folded, quickly for ASCII.
inline char16_t fold(QChar c)
{
    const char16_t u = c.unicode();
    if (u < 128)
        return u >= u'A' && u <= u'Z' ? static_cast<char16_t>(u + 32) : u;
    return c.toLower().unicode();
}

int startBonus(QStringView text, int at)
{
    if (at == 0)
        return kSegmentStart;
    const QChar before = text[at - 1];
    const QChar here = text[at];
    if (before == u'/' || before == u'\\')
        return kSegmentStart;
    if (before == u'_' || before == u'-' || before == u'.' || before == u' ')
        return kWordStart;
    if (before.isLower() && here.isUpper())
        return kWordStart;
    if (!before.isLetterOrNumber() && here.isLetterOrNumber())
        return kWordStart;
    return 0;
}

} // namespace

std::optional<int> fuzzyMatch(QStringView query, QStringView text, std::vector<int>* positions)
{
    const int skipped = text.size() > kMaxText ? static_cast<int>(text.size() - kMaxText) : 0;
    text = text.sliced(skipped);
    const int n = static_cast<int>(query.size());
    const int m = static_cast<int>(text.size());
    if (positions)
        positions->clear();
    if (n == 0)
        return 0;
    if (n > m)
        return std::nullopt;
    // All there, in order? Most texts are not, and this is quick.
    {
        int i = 0;
        for (int j = 0; j < m && i < n; ++j)
            if (fold(text[j]) == fold(query[i]))
                ++i;
        if (i < n)
            return std::nullopt;
    }

    // The best score with query[i] matched at text[j], one row of i at a time; with positions,
    // where query[i - 1] matched for each, to go back along. The rows are kept from one call to
    // the next, which comes for every file of a folder as the user types.
    thread_local std::vector<int> previous;
    thread_local std::vector<int> current;
    previous.assign(static_cast<std::size_t>(m), kNone);
    current.assign(static_cast<std::size_t>(m), kNone);
    std::vector<int> from;
    if (positions)
        from.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(m), -1);
    for (int i = 0; i < n; ++i) {
        const char16_t wanted = fold(query[i]);
        // The best previous[k] + k * kGap for k < j - 1: a match further back, less the gap.
        int bestBefore = kNone;
        int bestBeforeAt = -1;
        for (int j = 0; j < m; ++j) {
            if (i > 0 && j >= 2 && previous[j - 2] != kNone && previous[j - 2] + (j - 2) * kGap > bestBefore) {
                bestBefore = previous[j - 2] + (j - 2) * kGap;
                bestBeforeAt = j - 2;
            }
            current[j] = kNone;
            if (fold(text[j]) != wanted)
                continue;
            const int score = kMatch + startBonus(text, j) + (text[j] == query[i] ? kSameCase : 0);
            if (i == 0) {
                current[j] = score - std::min(j, kMaxLead);
                continue;
            }
            int best = kNone;
            int at = -1;
            if (j > 0 && previous[j - 1] != kNone) {
                best = previous[j - 1] + kAdjacent;
                at = j - 1;
            }
            if (bestBefore != kNone && bestBefore - (j - 1) * kGap > best) {
                best = bestBefore - (j - 1) * kGap;
                at = bestBeforeAt;
            }
            if (best == kNone)
                continue;
            current[j] = best + score;
            if (positions)
                from[static_cast<std::size_t>(i) * static_cast<std::size_t>(m) + static_cast<std::size_t>(j)] = at;
        }
        std::swap(previous, current);
    }

    const auto last = std::max_element(previous.begin(), previous.begin() + m);
    if (*last == kNone)
        return std::nullopt;
    if (positions) {
        positions->resize(static_cast<std::size_t>(n));
        int at = static_cast<int>(last - previous.begin());
        for (int i = n - 1; i >= 0; --i) {
            (*positions)[static_cast<std::size_t>(i)] = skipped + at;
            at = from[static_cast<std::size_t>(i) * static_cast<std::size_t>(m) + static_cast<std::size_t>(at)];
        }
    }
    return *last;
}

} // namespace aiedit::ui
