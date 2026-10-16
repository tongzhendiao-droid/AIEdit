#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace aiedit::core {

// Lines [leftStart, leftStart + leftCount) on the left became [rightStart, rightStart +
// rightCount) on the right; one count may be 0 (lines only removed, or only added). The lines
// between two hunks are the same on both sides.
struct DiffHunk
{
    std::uint64_t leftStart = 0;
    std::uint64_t leftCount = 0;
    std::uint64_t rightStart = 0;
    std::uint64_t rightCount = 0;

    bool operator==(const DiffHunk&) const = default;
};

// The differences between two sequences whose elements stand for lines: a hash of each line's
// text, made the same for lines that should count as equal (ignoring case, say).
//
// Myers' O(ND) algorithm, in linear space, after setting aside the common start and end and the
// lines that have no equal on the other side. Where the inputs differ a lot, the search for the
// shortest script is cut short, as GNU diff does, so that the time stays bounded; the result is
// then correct but not always minimal. A block that was only added or removed is moved down as
// far as it can go, which is where people expect it (after a closing brace rather than before).
//
// Returns no hunks if `cancelled` says so; the caller tells that from "no differences".
std::vector<DiffHunk> diffSequences(std::span<const std::uint64_t> left, std::span<const std::uint64_t> right,
                                    const std::function<bool()>& cancelled = {});

// A range of UTF-16 code units in a line.
struct TextRange
{
    std::uint32_t start = 0;
    std::uint32_t length = 0;

    bool operator==(const TextRange&) const = default;
};

// What differs inside two versions of a line: the ranges only on the left, and those only on the
// right.
struct InlineDiff
{
    std::vector<TextRange> left;
    std::vector<TextRange> right;
};

// Compares two versions of a line word by word: letters and digits run together, while CJK
// characters, other signs and spaces count one by one. When little is left in common, or a line
// is very long, the whole of both lines is marked instead of a scatter of fragments.
InlineDiff diffInline(std::u16string_view left, std::u16string_view right);

// FNV-1a: a fast 64-bit hash for line keys.
std::uint64_t hashBytes(std::span<const std::uint8_t> bytes, std::uint64_t seed = 14695981039346656037ull);

} // namespace aiedit::core
