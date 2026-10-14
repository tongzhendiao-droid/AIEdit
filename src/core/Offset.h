#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cppedit::core {

// Parses a "go to offset" expression.
//   0x1F, 1Fh, 1f  -> hexadecimal (a 0x prefix, an h suffix or any of a-f)
//   31             -> decimal
//   +16, -0x10     -> relative to `current`; a result below zero is clamped to 0
// Surrounding whitespace is ignored. Returns nullopt for malformed input or overflow.
std::optional<std::uint64_t> parseOffset(std::string_view text, std::uint64_t current);

} // namespace cppedit::core
