#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cppedit::core {

// A byte sequence to find; positions marked in `wildcard` match any byte.
struct BytePattern
{
    std::vector<std::uint8_t> bytes;
    std::vector<bool> wildcard; // empty, or one entry per byte

    bool hasWildcards() const;
    std::size_t size() const { return bytes.size(); }
};

// Parses hex bytes such as "4D 5A ?? 00", "4d5a9000" or "4D-5A". "??" (or "?") is a wildcard.
// Returns nullopt for malformed input or an empty or all-wildcard pattern.
std::optional<BytePattern> parseHexPattern(std::string_view text);

struct SearchControl
{
    const std::atomic<bool>* cancelled = nullptr;
    // Called now and then with the number of bytes scanned so far.
    std::function<void(std::uint64_t scanned)> progress;
};

// Finds the first match starting at or after `from` (forward), or the last match starting
// before `from` (backward). Searches in chunks so that it can be cancelled.
std::optional<std::uint64_t> findPattern(std::span<const std::uint8_t> data, const BytePattern& pattern,
                                         std::uint64_t from, bool forward, const SearchControl& control = {});

} // namespace cppedit::core
