#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace aiedit::core {

struct ByteRange
{
    std::uint64_t start = 0;
    std::uint64_t length = 0;

    std::uint64_t end() const { return start + length; }
    bool operator==(const ByteRange&) const = default;
};

// A stretch of two files lined up: the same bytes on both sides, or bytes that differ, as many
// or as few on each side (none: only inserted, or only removed). `position` is where it starts
// when shown side by side, each stretch taking as many places as its longer side.
struct ByteSegment
{
    std::uint64_t position = 0;
    std::uint64_t left = 0;
    std::uint64_t leftLength = 0;
    std::uint64_t right = 0;
    std::uint64_t rightLength = 0;
    bool same = false;

    std::uint64_t length() const { return leftLength > rightLength ? leftLength : rightLength; }
    bool operator==(const ByteSegment&) const = default;
};

// Two binary files compared byte by byte: at the same offsets, or, where bytes were inserted or
// removed, lined up again after them (M11).
struct ByteComparison
{
    std::uint64_t leftSize = 0;
    std::uint64_t rightSize = 0;
    // Where the bytes differ, in order: runs closer than a few bytes are one range. If one side is
    // longer, its extra bytes are the last range.
    std::vector<ByteRange> ranges;
    std::uint64_t differentBytes = 0; // extra bytes included
    bool limited = false;             // there were more ranges than were listed
    // From shiftAt on, the right side looks moved by `shift` bytes (positive: bytes were
    // inserted on the right), so comparing at the same offsets means little after it. Found
    // near the first difference, where most bytes after it differ.
    std::optional<std::uint64_t> shiftAt;
    std::int64_t shift = 0;
    // Lined up (when bytes were inserted or removed, and both files are small enough): the
    // stretches in order, and `ranges` are of positions, not offsets. Empty: compared at the
    // same offsets, where positions are offsets.
    std::vector<ByteSegment> segments;
    bool cancelled = false;
};

// How many places the two files take side by side.
std::uint64_t positionCount(const ByteComparison& comparison);
// The bytes shown at a position: each side's offset, if that side has a byte there.
struct BytesAt
{
    std::optional<std::uint64_t> left;
    std::optional<std::uint64_t> right;
    bool same = false;
};
BytesAt bytesAt(const ByteComparison& comparison, std::span<const std::uint8_t> left,
                std::span<const std::uint8_t> right, std::uint64_t position);
// The bytes of each side shown in a range of positions; a side with none there gets an empty
// range where its bytes would be.
struct SideRanges
{
    ByteRange left;
    ByteRange right;
};
SideRanges sideRanges(const ByteComparison& comparison, const ByteRange& positions);

// Lines up two byte strings: the bytes they share in order, and what differs between them.
// Byte by byte where little differs; for large files, first by chunks cut where the content
// says (so an insertion moves only the cuts near it), then byte by byte inside each changed
// chunk. Stretches of the same bytes shorter than a few bytes go into the change around them.
std::vector<ByteSegment> alignBytes(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
                                    const std::function<bool()>& cancelled = {});

ByteComparison compareBytes(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
                            const std::function<bool()>& cancelled = {}, std::size_t maxRanges = 100'000);

} // namespace aiedit::core
