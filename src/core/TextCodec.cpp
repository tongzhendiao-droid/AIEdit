#include "core/TextCodec.h"

#include <algorithm>
#include <array>
#include <initializer_list>

#ifdef CPPEDIT_HAVE_UCHARDET
#include <uchardet.h>
#endif

namespace cppedit::core {

namespace {

constexpr TextCodec kCodecs[] = {
    {"UTF-8", CharScheme::Utf8},
    {"UTF-16LE", CharScheme::Utf16LE},
    {"UTF-16BE", CharScheme::Utf16BE},
    {"UTF-32LE", CharScheme::Utf32LE},
    {"UTF-32BE", CharScheme::Utf32BE},
    {"GB18030", CharScheme::Gb18030},
    {"Big5", CharScheme::Big5},
    {"Shift_JIS", CharScheme::ShiftJis},
    {"EUC-JP", CharScheme::EucJp},
    {"EUC-KR", CharScheme::EucKr},
    {"windows-1252", CharScheme::SingleByte},
    {"ISO-8859-1", CharScheme::SingleByte},
    {"ISO-8859-15", CharScheme::SingleByte},
    {"windows-1250", CharScheme::SingleByte},
    {"ISO-8859-2", CharScheme::SingleByte},
    {"windows-1251", CharScheme::SingleByte},
    {"KOI8-R", CharScheme::SingleByte},
    {"windows-1253", CharScheme::SingleByte},
    {"windows-1254", CharScheme::SingleByte},
    {"windows-1256", CharScheme::SingleByte},
};

// How many non-ASCII bytes it takes before a single-byte charset guess means anything.
constexpr std::ptrdiff_t kMinHighBytesForSingleByteGuess = 16;

// Multi-byte codecs tried, in order, for text that is not UTF-8.
constexpr std::string_view kLegacyCandidates[] = {"GB18030", "Big5", "Shift_JIS", "EUC-KR", "EUC-JP"};

bool inRange(std::uint8_t b, std::uint8_t low, std::uint8_t high)
{
    return b >= low && b <= high;
}

char asciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return asciiLower(x) == asciiLower(y); });
}

bool isSurrogateHigh(std::uint32_t unit)
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool isSurrogateLow(std::uint32_t unit)
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

// True if every non-ASCII byte of the sample belongs to a well-formed character of the scheme
// and there is at least one multi-byte character.
bool fitsMultiByteScheme(std::span<const std::uint8_t> sample, CharScheme scheme, bool truncated)
{
    std::size_t multiByte = 0;
    std::size_t i = 0;
    while (i < sample.size()) {
        const std::uint8_t b = sample[i];
        if (b < 0x80) {
            ++i;
            continue;
        }
        const std::size_t length = charLength(sample.subspan(i), scheme);
        if (length > 1) {
            ++multiByte;
            i += length;
            continue;
        }
        if (scheme == CharScheme::ShiftJis && inRange(b, 0xA1, 0xDF)) { // half-width katakana
            ++i;
            continue;
        }
        if (truncated && sample.size() - i < 4) // a character cut off by the end of the sample
            break;
        return false;
    }
    return multiByte > 0;
}

// uchardet's statistical guess, if it has one and it is a codec we offer. It needs a few
// hundred bytes of text to say anything.
const TextCodec* detectCharset(std::span<const std::uint8_t> sample)
{
#ifdef CPPEDIT_HAVE_UCHARDET
    const uchardet_t detector = uchardet_new();
    uchardet_handle_data(detector, reinterpret_cast<const char*>(sample.data()), sample.size());
    uchardet_data_end(detector);
    const TextCodec* codec = findCodec(uchardet_get_charset(detector));
    uchardet_delete(detector);
    return codec;
#else
    (void)sample;
    return nullptr;
#endif
}

} // namespace

std::span<const TextCodec> knownCodecs()
{
    return kCodecs;
}

const TextCodec* findCodec(std::string_view name)
{
    for (const TextCodec& codec : kCodecs) {
        if (equalsIgnoreCase(codec.name, name))
            return &codec;
    }
    return nullptr;
}

const TextCodec& utf8Codec()
{
    return kCodecs[0];
}

std::size_t codeUnitSize(CharScheme scheme)
{
    switch (scheme) {
    case CharScheme::Utf16LE:
    case CharScheme::Utf16BE:
        return 2;
    case CharScheme::Utf32LE:
    case CharScheme::Utf32BE:
        return 4;
    default:
        return 1;
    }
}

std::uint32_t codeUnitAt(std::span<const std::uint8_t> data, std::size_t offset, CharScheme scheme)
{
    const auto byte = [&](std::size_t i) { return std::uint32_t{data[offset + i]}; };
    switch (scheme) {
    case CharScheme::Utf16LE:
        return byte(0) | byte(1) << 8;
    case CharScheme::Utf16BE:
        return byte(0) << 8 | byte(1);
    case CharScheme::Utf32LE:
        return byte(0) | byte(1) << 8 | byte(2) << 16 | byte(3) << 24;
    case CharScheme::Utf32BE:
        return byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
    default:
        return byte(0);
    }
}

std::size_t charLength(std::span<const std::uint8_t> data, CharScheme scheme)
{
    if (data.empty())
        return 0;
    const std::uint8_t b = data[0];
    const std::size_t n = data.size();
    std::size_t length = 1;

    switch (scheme) {
    case CharScheme::SingleByte:
        break;
    case CharScheme::Utf8:
        if ((b & 0xE0) == 0xC0)
            length = 2;
        else if ((b & 0xF0) == 0xE0)
            length = 3;
        else if ((b & 0xF8) == 0xF0)
            length = 4;
        for (std::size_t k = 1; k < length; ++k) {
            if (k >= n || (data[k] & 0xC0) != 0x80) {
                length = 1;
                break;
            }
        }
        break;
    case CharScheme::Utf16LE:
    case CharScheme::Utf16BE:
        length = 2;
        if (n >= 4 && isSurrogateHigh(codeUnitAt(data, 0, scheme)) && isSurrogateLow(codeUnitAt(data, 2, scheme)))
            length = 4;
        break;
    case CharScheme::Utf32LE:
    case CharScheme::Utf32BE:
        length = 4;
        break;
    case CharScheme::Gb18030:
        if (inRange(b, 0x81, 0xFE) && n >= 2) {
            const std::uint8_t b2 = data[1];
            if (inRange(b2, 0x30, 0x39)) {
                if (n >= 4 && inRange(data[2], 0x81, 0xFE) && inRange(data[3], 0x30, 0x39))
                    length = 4;
            } else if (inRange(b2, 0x40, 0x7E) || inRange(b2, 0x80, 0xFE)) {
                length = 2;
            }
        }
        break;
    case CharScheme::Big5:
        if (inRange(b, 0x81, 0xFE) && n >= 2 && (inRange(data[1], 0x40, 0x7E) || inRange(data[1], 0xA1, 0xFE)))
            length = 2;
        break;
    case CharScheme::ShiftJis:
        if ((inRange(b, 0x81, 0x9F) || inRange(b, 0xE0, 0xFC)) && n >= 2 &&
            (inRange(data[1], 0x40, 0x7E) || inRange(data[1], 0x80, 0xFC)))
            length = 2;
        break;
    case CharScheme::EucJp:
        if (b == 0x8E) {
            if (n >= 2 && inRange(data[1], 0xA1, 0xDF))
                length = 2;
        } else if (b == 0x8F) {
            if (n >= 3 && inRange(data[1], 0xA1, 0xFE) && inRange(data[2], 0xA1, 0xFE))
                length = 3;
        } else if (inRange(b, 0xA1, 0xFE) && n >= 2 && inRange(data[1], 0xA1, 0xFE)) {
            length = 2;
        }
        break;
    case CharScheme::EucKr:
        if (inRange(b, 0xA1, 0xFE) && n >= 2 && inRange(data[1], 0xA1, 0xFE))
            length = 2;
        break;
    }
    return std::min(length, n);
}

std::size_t bomLength(std::span<const std::uint8_t> data, CharScheme scheme)
{
    const auto startsWith = [&](std::initializer_list<std::uint8_t> bom) {
        return data.size() >= bom.size() && std::equal(bom.begin(), bom.end(), data.begin());
    };
    switch (scheme) {
    case CharScheme::Utf8:
        return startsWith({0xEF, 0xBB, 0xBF}) ? 3 : 0;
    case CharScheme::Utf16LE:
        return startsWith({0xFF, 0xFE}) ? 2 : 0;
    case CharScheme::Utf16BE:
        return startsWith({0xFE, 0xFF}) ? 2 : 0;
    case CharScheme::Utf32LE:
        return startsWith({0xFF, 0xFE, 0x00, 0x00}) ? 4 : 0;
    case CharScheme::Utf32BE:
        return startsWith({0x00, 0x00, 0xFE, 0xFF}) ? 4 : 0;
    case CharScheme::Gb18030:
        return startsWith({0x84, 0x31, 0x95, 0x33}) ? 4 : 0;
    default:
        return 0;
    }
}

const TextCodec& defaultCodec(const FileTypeInfo& type, std::span<const std::uint8_t> data,
                              std::string_view preferredLegacy)
{
    switch (type.encoding) {
    case Encoding::Ascii:
    case Encoding::Utf8:
        return utf8Codec();
    case Encoding::Utf16LE:
        return *findCodec("UTF-16LE");
    case Encoding::Utf16BE:
        return *findCodec("UTF-16BE");
    case Encoding::Utf32LE:
        return *findCodec("UTF-32LE");
    case Encoding::Utf32BE:
        return *findCodec("UTF-32BE");
    case Encoding::Legacy:
        break;
    case Encoding::Unknown:
        return type.kind == ContentKind::Binary ? *findCodec("ISO-8859-1") : utf8Codec();
    }

    const auto sample = data.first(std::min(data.size(), kSniffSize));
    const bool truncated = data.size() > sample.size();

    // A statistical guess beats byte structure (GBK and Big5 text are often valid in both),
    // as long as the content really has that structure. Unicode guesses are no use here:
    // detection already found that this is not UTF-8.
    if (const TextCodec* guess = detectCharset(sample)) {
        // Single-byte guesses from a handful of accented letters are coin flips (café -> ISO-8859-2).
        const auto highBytes = std::count_if(sample.begin(), sample.end(), [](std::uint8_t b) { return b >= 0x80; });
        if (guess->scheme == CharScheme::SingleByte && highBytes >= kMinHighBytesForSingleByteGuess)
            return *guess;
        if (guess->scheme != CharScheme::Utf8 && codeUnitSize(guess->scheme) == 1 &&
            fitsMultiByteScheme(sample, guess->scheme, truncated))
            return *guess;
    }

    const TextCodec* preferred = findCodec(preferredLegacy);

    std::array<const TextCodec*, std::size(kLegacyCandidates) + 1> candidates{};
    std::size_t count = 0;
    if (preferred && preferred->scheme != CharScheme::SingleByte)
        candidates[count++] = preferred;
    for (const std::string_view name : kLegacyCandidates) {
        const TextCodec* codec = findCodec(name);
        if (codec != preferred)
            candidates[count++] = codec;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (fitsMultiByteScheme(sample, candidates[i]->scheme, truncated))
            return *candidates[i];
    }
    if (preferred && preferred->scheme == CharScheme::SingleByte)
        return *preferred;
    return *findCodec("windows-1252");
}

} // namespace cppedit::core
