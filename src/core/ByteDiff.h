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

// Two binary files compared byte by byte, at the same offsets.
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
    bool cancelled = false;
};

ByteComparison compareBytes(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
                            const std::function<bool()>& cancelled = {}, std::size_t maxRanges = 100'000);

} // namespace aiedit::core
