#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace cppedit::core {

enum class ContentKind
{
    Empty,
    Text,
    Binary,
};

enum class Encoding
{
    Unknown, // binary content, or empty
    Ascii,
    Utf8,
    Utf16LE,
    Utf16BE,
    Utf32LE,
    Utf32BE,
    Legacy, // text, but not UTF-8: GBK, Big5, Latin-1, ... (to be narrowed down by a charset detector)
};

struct FileTypeInfo
{
    ContentKind kind = ContentKind::Empty;
    Encoding encoding = Encoding::Unknown;
    bool hasBom = false;
    std::string_view format; // from a magic signature, e.g. "ELF" or "PNG"; empty if unknown
};

// How many leading bytes detectFileType() looks at.
inline constexpr std::size_t kSniffSize = 64 * 1024;

// Classifies content as empty, text or binary, guesses the text encoding and names well-known
// binary formats by their magic bytes. Pass the whole file; only the first kSniffSize bytes are read.
FileTypeInfo detectFileType(std::span<const std::uint8_t> data);

// Strict UTF-8 validation (no overlong forms, surrogates or code points above U+10FFFF).
// With allowTruncatedTail, a multi-byte sequence cut off by the end of the buffer is accepted.
bool isValidUtf8(std::span<const std::uint8_t> data, bool allowTruncatedTail = false);

} // namespace cppedit::core
