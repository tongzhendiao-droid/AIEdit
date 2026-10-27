#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aiedit::core {

// Decompresses raw DEFLATE data (RFC 1951), as ZIP entries hold it, appending to `out`. False
// if the data is broken or would grow past `maxSize`; `out` then keeps what came before.
bool inflate(std::span<const std::uint8_t> data, std::size_t maxSize, std::vector<std::uint8_t>& out);

// The same, wrapped in a zlib header (RFC 1950), as PDF's FlateDecode streams hold it. The
// checksum at the end is not required: PDF writers often cut it off.
bool zlibInflate(std::span<const std::uint8_t> data, std::size_t maxSize, std::vector<std::uint8_t>& out);

// CRC-32 as ZIP uses it.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

} // namespace aiedit::core
