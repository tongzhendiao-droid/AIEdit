#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace aiedit::core {

// Parses a "go to offset" expression.
//   0x1F, 1Fh, 1f  -> hexadecimal (a 0x prefix, an h suffix or any of a-f)
//   31             -> decimal
//   +16, -0x10     -> relative to `current`; a result below zero is clamped to 0
// Surrounding whitespace is ignored. Returns nullopt for malformed input or overflow.
std::optional<std::uint64_t> parseOffset(std::string_view text, std::uint64_t current);

// Parses a "go to line" expression: a 1-based decimal line number, or +N / -N relative to
// `currentLine` (also 1-based). Results below line 1 are clamped to 1. Returns nullopt for
// malformed input or overflow.
std::optional<std::uint64_t> parseLine(std::string_view text, std::uint64_t currentLine);

} // namespace aiedit::core
