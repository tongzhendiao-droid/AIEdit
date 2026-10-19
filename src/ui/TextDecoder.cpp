#include "ui/TextDecoder.h"

#include <algorithm>

namespace aiedit::ui {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool isSurrogate(char32_t c)
{
    return c >= 0xD800 && c <= 0xDFFF;
}

// What to show for a code point: C0 controls and DEL as control pictures, C1 as U+FFFD. The
// Unicode line and paragraph separators, which text layout would break the row at, show as
// ␤ and ¶.
char32_t displayable(char32_t c)
{
    if (c == '\t')
        return c;
    if (c < 0x20)
        return 0x2400 + c;
    if (c == 0x7F)
        return 0x2421;
    if (c >= 0x80 && c <= 0x9F)
        return kReplacement;
    if (c == 0x2028)
        return 0x2424;
    if (c == 0x2029)
        return 0x00B6;
    return c;
}

char32_t decodeUtf8(std::span<const std::uint8_t> bytes, std::size_t length)
{
    const std::uint8_t lead = bytes[0];
    if (length == 1)
        return lead < 0x80 ? lead : kReplacement;
    char32_t c = length == 2 ? lead & 0x1Fu : length == 3 ? lead & 0x0Fu : lead & 0x07u;
    for (std::size_t k = 1; k < length; ++k)
        c = (c << 6) | (bytes[k] & 0x3Fu);
    const char32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
    return (c < minimum || c > 0x10FFFF || isSurrogate(c)) ? kReplacement : c;
}

} // namespace

qsizetype DecodedText::positionAt(std::uint32_t byteOffset) const
{
    const auto it = std::lower_bound(byteOffsets.begin(), byteOffsets.end(), byteOffset);
    return static_cast<qsizetype>(std::min<std::ptrdiff_t>(it - byteOffsets.begin(), text.size()));
}

TextDecoder::TextDecoder(const core::TextCodec& codec) : m_codec(codec), m_converter(codec)
{
    if (m_codec.scheme == core::CharScheme::SingleByte) {
        for (int b = 0; b < 256; ++b) {
            const char byte = static_cast<char>(b);
            bool malformed = false;
            const QString decoded = m_converter.decode(QByteArrayView(&byte, 1), &malformed);
            m_singleByte[static_cast<std::size_t>(b)] =
                decoded.size() == 1 && !malformed ? decoded.front().unicode() : u'\uFFFD';
        }
    }
}

char32_t TextDecoder::decodeLegacy(std::span<const std::uint8_t> bytes, std::size_t length) const
{
    if (length == 1 && bytes[0] < 0x80)
        return bytes[0];

    quint32 key = 0;
    for (std::size_t k = 0; k < length; ++k)
        key = key << 8 | bytes[k];
    if (const auto it = m_cache.constFind(key); it != m_cache.constEnd())
        return *it;

    // Malformed or unmapped, a character is U+FFFD, also where ICU puts U+001A instead.
    bool malformed = false;
    const QString decoded = m_converter.decode(
        QByteArrayView(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(length)), &malformed);
    const auto ucs4 = decoded.toUcs4();
    const char32_t c = ucs4.size() == 1 && !malformed ? static_cast<char32_t>(ucs4.front()) : kReplacement;
    m_cache.insert(key, c);
    return c;
}

char32_t TextDecoder::decodeChar(std::span<const std::uint8_t> bytes, std::size_t length) const
{
    using core::CharScheme;
    const CharScheme scheme = m_codec.scheme;
    switch (scheme) {
    case CharScheme::SingleByte:
        return m_singleByte[bytes[0]];
    case CharScheme::Utf8:
        return decodeUtf8(bytes, length);
    case CharScheme::Utf16LE:
    case CharScheme::Utf16BE: {
        if (length < 2)
            return kReplacement;
        const char32_t unit = core::codeUnitAt(bytes, 0, scheme);
        if (length == 4)
            return 0x10000 + ((unit - 0xD800) << 10) + (core::codeUnitAt(bytes, 2, scheme) - 0xDC00);
        return isSurrogate(unit) ? kReplacement : unit;
    }
    case CharScheme::Utf32LE:
    case CharScheme::Utf32BE: {
        if (length < 4)
            return kReplacement;
        const char32_t c = core::codeUnitAt(bytes, 0, scheme);
        return (c > 0x10FFFF || isSurrogate(c)) ? kReplacement : c;
    }
    default:
        return decodeLegacy(bytes, length);
    }
}

DecodedText TextDecoder::decode(std::span<const std::uint8_t> bytes, bool controlPictures) const
{
    DecodedText out;
    out.text.reserve(static_cast<qsizetype>(bytes.size()));
    out.byteOffsets.reserve(bytes.size() + 1);

    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto rest = bytes.subspan(i);
        const std::size_t length = std::max<std::size_t>(1, core::charLength(rest, m_codec.scheme));
        const char32_t decoded = decodeChar(rest, length);
        const char32_t c = controlPictures ? displayable(decoded) : decoded;
        const auto start = static_cast<std::uint32_t>(i);
        if (c > 0xFFFF) {
            out.text += QChar(QChar::highSurrogate(c));
            out.text += QChar(QChar::lowSurrogate(c));
            out.byteOffsets.push_back(start);
            out.byteOffsets.push_back(start);
        } else {
            out.text += QChar(static_cast<char16_t>(c));
            out.byteOffsets.push_back(start);
        }
        i += length;
    }
    out.byteOffsets.push_back(static_cast<std::uint32_t>(bytes.size()));
    return out;
}

QString TextDecoder::decodePlain(std::span<const std::uint8_t> bytes) const
{
    const auto view = QByteArrayView(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    if (m_codec.scheme == core::CharScheme::Utf8)
        return QString::fromUtf8(view);
    return m_converter.decode(view);
}

} // namespace aiedit::ui
