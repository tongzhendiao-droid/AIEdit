#pragma once

#include "core/FileType.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aiedit::core {

// How an encoding lays characters out in bytes: enough to find line breaks and character
// boundaries without decoding. Decoding itself is left to the UI layer (Qt/ICU).
enum class CharScheme
{
    SingleByte,
    Utf8,
    Utf16LE,
    Utf16BE,
    Utf32LE,
    Utf32BE,
    Gb18030,
    Big5,
    ShiftJis,
    EucJp,
    EucKr,
};

struct TextCodec
{
    std::string_view name; // IANA/ICU name, e.g. "GB18030"; also what the decoder is created from
    CharScheme scheme;
    // The Windows code page with these characters, for a Qt without ICU (its builds for Windows):
    // 54936 for GB18030, 950 for Big5 and so on; 0 for UTF-16 and UTF-32, which have none (Qt
    // converts them itself). EUC-KR has 949, which adds characters EUC-KR lacks; its byte
    // structure leaves them out.
    std::uint16_t codePage;
};

// Every codec AIEdit offers, Unicode first.
std::span<const TextCodec> knownCodecs();
// Case-insensitive lookup; nullptr if unknown.
const TextCodec* findCodec(std::string_view name);
const TextCodec& utf8Codec();

std::size_t codeUnitSize(CharScheme scheme);
// The code unit at `offset`, assembled with the scheme's byte order. offset + unit size <= size.
std::uint32_t codeUnitAt(std::span<const std::uint8_t> data, std::size_t offset, CharScheme scheme);
// Length in bytes of the character starting at data[0]: at least 1 (malformed bytes count
// as one character each) and never more than data.size().
std::size_t charLength(std::span<const std::uint8_t> data, CharScheme scheme);
// Size of the byte order mark at the start of data for this scheme, or 0.
std::size_t bomLength(std::span<const std::uint8_t> data, CharScheme scheme);

// Picks the codec to show a file with. For legacy (non-UTF-8) text, uchardet's guess is used
// when it is available and fits the content; otherwise the first CJK codec whose byte structure
// fits, trying `preferredLegacy` (from the user's locale) first, with windows-1252 as the
// fallback. Binary content gets ISO-8859-1, which maps every byte to one character.
const TextCodec& defaultCodec(const FileTypeInfo& type, std::span<const std::uint8_t> data,
                              std::string_view preferredLegacy = "GB18030");

} // namespace aiedit::core
