#pragma once

#include "core/TextCodec.h"

#include <QHash>
#include <QString>
#include <QStringDecoder>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace aiedit::ui {

// Decoded text that remembers where every UTF-16 code unit came from.
struct DecodedText
{
    QString text;
    // byteOffsets[i]: offset, relative to the decoded bytes, of the character that code unit i
    // belongs to. One extra trailing entry holds the number of bytes decoded, so a cursor
    // position p (0..text.size()) maps to byteOffsets[p].
    std::vector<std::uint32_t> byteOffsets;

    std::uint32_t byteAt(qsizetype position) const { return byteOffsets[static_cast<std::size_t>(position)]; }
    // The first cursor position at or after a relative byte offset.
    qsizetype positionAt(std::uint32_t byteOffset) const;
};

// Decodes text in one codec character by character, so that positions in the decoded text can
// be traced back to bytes (needed for the cursor, selections and Text/Hex synchronisation).
//
// Malformed bytes become U+FFFD, one per byte. Control characters are shown as Unicode
// "control pictures" (␀, ␛, ␍ …) so they stay visible; tabs are kept for tab stops. The
// Unicode line and paragraph separators show as ␤ and ¶, so that a row stays one line.
class TextDecoder
{
public:
    explicit TextDecoder(const core::TextCodec& codec);

    const core::TextCodec& codec() const { return m_codec; }

    // With controlPictures off, control characters (including line breaks) are kept as they are,
    // as searching needs; the position map is the same either way.
    DecodedText decode(std::span<const std::uint8_t> bytes, bool controlPictures = true) const;
    // Plain decoding for copying text out: real control characters, no position map.
    QString decodePlain(std::span<const std::uint8_t> bytes) const;

private:
    char32_t decodeChar(std::span<const std::uint8_t> bytes, std::size_t length) const;
    char32_t decodeLegacy(std::span<const std::uint8_t> bytes, std::size_t length) const;

    core::TextCodec m_codec;
    std::array<char16_t, 256> m_singleByte{}; // lookup table for single-byte codecs
    mutable QStringDecoder m_decoder;
    mutable QHash<quint32, char32_t> m_cache; // legacy multi-byte characters, keyed by their bytes
};

} // namespace aiedit::ui
