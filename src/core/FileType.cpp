#include "core/FileType.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace aiedit::core {

namespace {

using namespace std::string_view_literals;

struct Magic
{
    std::string_view name;
    std::size_t offset;
    std::string_view bytes;
    std::size_t offset2 = 0;
    std::string_view bytes2 = {};
    // A short signature that also starts ordinary text; only trusted when the content is binary.
    bool weak = false;
};

// Literals with embedded NULs need the sv suffix to keep their full length, and a hex escape
// followed by a hex digit has to be split ("\x7F" "ELF") so the escape does not swallow it.
constexpr Magic kMagics[] = {
    {"ELF", 0, "\x7F" "ELF"sv},
    {"PNG", 0, "\x89PNG\r\n\x1A\n"sv},
    {"JPEG", 0, "\xFF\xD8\xFF"sv},
    {"GIF", 0, "GIF87a"sv},
    {"GIF", 0, "GIF89a"sv},
    {"WebP", 0, "RIFF"sv, 8, "WEBP"sv},
    {"WAV", 0, "RIFF"sv, 8, "WAVE"sv},
    {"AVI", 0, "RIFF"sv, 8, "AVI "sv},
    {"TIFF", 0, "II*\0"sv},
    {"TIFF", 0, "MM\0*"sv},
    {"PSD", 0, "8BPS"sv},
    {"PDF", 0, "%PDF-"sv},
    {"ZIP", 0, "PK\x03\x04"sv},
    {"ZIP", 0, "PK\x05\x06"sv},
    {"ZIP", 0, "PK\x07\x08"sv},
    {"gzip", 0, "\x1F\x8B"sv},
    {"xz", 0, "\xFD" "7zXZ\0"sv},
    {"Zstandard", 0, "\x28\xB5\x2F\xFD"sv},
    {"LZ4", 0, "\x04\x22\x4D\x18"sv},
    {"7z", 0, "7z\xBC\xAF\x27\x1C"sv},
    {"RAR", 0, "Rar!\x1A\x07"sv},
    {"tar", 257, "ustar"sv},
    {"ar archive", 0, "!<arch>\n"sv},
    {"CAB", 0, "MSCF"sv},
    {"RPM", 0, "\xED\xAB\xEE\xDB"sv},
    {"SQLite", 0, "SQLite format 3\0"sv},
    {"OLE2 (DOC/XLS/MSI)", 0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1"sv},
    {"Mach-O", 0, "\xFE\xED\xFA\xCE"sv},
    {"Mach-O", 0, "\xFE\xED\xFA\xCF"sv},
    {"Mach-O", 0, "\xCE\xFA\xED\xFE"sv},
    {"Mach-O", 0, "\xCF\xFA\xED\xFE"sv},
    {"WebAssembly", 0, "\0asm"sv},
    {"Android DEX", 0, "dex\n"sv},
    {"Ogg", 0, "OggS"sv},
    {"FLAC", 0, "fLaC"sv},
    {"Matroska/WebM", 0, "\x1A\x45\xDF\xA3"sv},
    {"ISO media (MP4/MOV/HEIF)", 4, "ftyp"sv},
    {"pcap", 0, "\xD4\xC3\xB2\xA1"sv},
    {"pcap", 0, "\xA1\xB2\xC3\xD4"sv},
    {"pcapng", 0, "\x0A\x0D\x0D\x0A"sv},
    {"SquashFS", 0, "hsqs"sv},
    {"U-Boot image", 0, "\x27\x05\x19\x56"sv},
    {"Device Tree Blob", 0, "\xD0\x0D\xFE\xED"sv},
    {"Android boot image", 0, "ANDROID!"sv},
    {"UBI image", 0, "UBI#"sv},
    {"WOFF", 0, "wOFF"sv},
    {"WOFF2", 0, "wOF2"sv},
    {"PE/MZ executable", 0, "MZ"sv, 0, {}, true},
    {"BMP", 0, "BM"sv, 0, {}, true},
    {"bzip2", 0, "BZh"sv, 0, {}, true},
    {"MP3 (ID3)", 0, "ID3"sv, 0, {}, true},
};

bool matchesAt(std::span<const std::uint8_t> data, std::size_t offset, std::string_view bytes)
{
    return offset + bytes.size() <= data.size() &&
           std::memcmp(data.data() + offset, bytes.data(), bytes.size()) == 0;
}

std::uint32_t readBigEndian32(std::span<const std::uint8_t> data, std::size_t offset)
{
    return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16) |
           (std::uint32_t{data[offset + 2]} << 8) | std::uint32_t{data[offset + 3]};
}

// Returns the best signature match. Weak matches go to weakFormat instead of the result.
std::string_view matchMagic(std::span<const std::uint8_t> data, std::string_view& weakFormat)
{
    // CA FE BA BE starts both Java classes and Mach-O universal binaries. The next word is the
    // class file version (45 and up) or the architecture count (small); `file` splits at 30.
    if (data.size() >= 8 && matchesAt(data, 0, "\xCA\xFE\xBA\xBE"sv))
        return readBigEndian32(data, 4) > 30 ? "Java class"sv : "Mach-O universal"sv;

    for (const Magic& magic : kMagics) {
        if (!matchesAt(data, magic.offset, magic.bytes))
            continue;
        if (!magic.bytes2.empty() && !matchesAt(data, magic.offset2, magic.bytes2))
            continue;
        if (!magic.weak)
            return magic.name;
        if (weakFormat.empty())
            weakFormat = magic.name;
    }
    return {};
}

// Control characters that do not occur in normal text. Tab, line breaks, form feed and
// ESC (used by ANSI colour codes in logs) are fine.
bool isBinaryControl(std::uint8_t b)
{
    switch (b) {
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case 0x1B:
        return false;
    default:
        return b < 0x20 || b == 0x7F;
    }
}

// Mostly-ASCII UTF-16 without a BOM has a zero in every other byte.
Encoding guessUtf16WithoutBom(std::span<const std::uint8_t> data)
{
    const std::size_t units = data.size() / 2;
    if (units < 2)
        return Encoding::Unknown;

    std::size_t zeroEven = 0;
    std::size_t zeroOdd = 0;
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        zeroEven += data[i] == 0;
        zeroOdd += data[i + 1] == 0;
    }

    // The low bytes must look like text too, or an array of small integers would qualify.
    auto lowBytesLookLikeText = [&](std::size_t first) {
        std::size_t controls = 0;
        for (std::size_t i = first; i < data.size(); i += 2)
            controls += data[i] != 0 && isBinaryControl(data[i]);
        return controls * 20 <= units;
    };

    if (zeroOdd * 10 >= units * 7 && zeroEven * 10 <= units && lowBytesLookLikeText(0))
        return Encoding::Utf16LE;
    if (zeroEven * 10 >= units * 7 && zeroOdd * 10 <= units && lowBytesLookLikeText(1))
        return Encoding::Utf16BE;
    return Encoding::Unknown;
}

// What extensions stand for. Only extensions whose files have one signature in kMagics (or are
// always text) are here: ".dat", ".bin" or ".iso" promise nothing that could be checked.
constexpr std::string_view kText = "text";
constexpr std::pair<std::string_view, std::string_view> kExtensions[] = {
    {"png", "PNG"}, {"jpg", "JPEG"}, {"jpeg", "JPEG"}, {"jpe", "JPEG"}, {"jfif", "JPEG"}, {"gif", "GIF"},
    {"webp", "WebP"}, {"wav", "WAV"}, {"avi", "AVI"}, {"tif", "TIFF"}, {"tiff", "TIFF"}, {"psd", "PSD"},
    {"bmp", "BMP"}, {"dib", "BMP"}, {"pdf", "PDF"},
    // Office documents, e-books, Java and Android packages are ZIP archives inside.
    {"zip", "ZIP"}, {"jar", "ZIP"}, {"war", "ZIP"}, {"apk", "ZIP"}, {"aab", "ZIP"}, {"ipa", "ZIP"},
    {"xpi", "ZIP"}, {"whl", "ZIP"}, {"nupkg", "ZIP"}, {"vsix", "ZIP"}, {"kmz", "ZIP"}, {"docx", "ZIP"},
    {"xlsx", "ZIP"}, {"pptx", "ZIP"}, {"odt", "ZIP"}, {"ods", "ZIP"}, {"odp", "ZIP"}, {"epub", "ZIP"},
    {"gz", "gzip"}, {"tgz", "gzip"}, {"xz", "xz"}, {"txz", "xz"}, {"bz2", "bzip2"}, {"tbz2", "bzip2"},
    {"zst", "Zstandard"}, {"lz4", "LZ4"}, {"7z", "7z"}, {"rar", "RAR"}, {"tar", "tar"},
    {"a", "ar archive"}, {"deb", "ar archive"}, {"cab", "CAB"}, {"rpm", "RPM"},
    {"sqlite", "SQLite"}, {"sqlite3", "SQLite"},
    {"doc", "OLE2 (DOC/XLS/MSI)"}, {"xls", "OLE2 (DOC/XLS/MSI)"}, {"ppt", "OLE2 (DOC/XLS/MSI)"},
    {"msi", "OLE2 (DOC/XLS/MSI)"},
    {"exe", "PE/MZ executable"}, {"dll", "PE/MZ executable"}, {"sys", "PE/MZ executable"},
    {"efi", "PE/MZ executable"}, {"ocx", "PE/MZ executable"}, {"cpl", "PE/MZ executable"},
    {"scr", "PE/MZ executable"},
    {"so", "ELF"}, {"ko", "ELF"}, {"elf", "ELF"}, {"dylib", "Mach-O"}, {"class", "Java class"},
    {"dex", "Android DEX"}, {"wasm", "WebAssembly"},
    {"ogg", "Ogg"}, {"oga", "Ogg"}, {"ogv", "Ogg"}, {"opus", "Ogg"}, {"flac", "FLAC"}, {"mp3", "MP3 (ID3)"},
    {"mkv", "Matroska/WebM"}, {"mka", "Matroska/WebM"}, {"webm", "Matroska/WebM"},
    {"mp4", "ISO media (MP4/MOV/HEIF)"}, {"m4a", "ISO media (MP4/MOV/HEIF)"},
    {"m4v", "ISO media (MP4/MOV/HEIF)"}, {"mov", "ISO media (MP4/MOV/HEIF)"},
    {"heic", "ISO media (MP4/MOV/HEIF)"}, {"heif", "ISO media (MP4/MOV/HEIF)"},
    {"avif", "ISO media (MP4/MOV/HEIF)"}, {"3gp", "ISO media (MP4/MOV/HEIF)"},
    {"pcap", "pcap"}, {"pcapng", "pcapng"}, {"squashfs", "SquashFS"}, {"snap", "SquashFS"},
    {"woff", "WOFF"}, {"woff2", "WOFF2"},
    {"txt", kText}, {"log", kText}, {"md", kText}, {"markdown", kText}, {"rst", kText}, {"csv", kText},
    {"tsv", kText}, {"json", kText}, {"jsonl", kText}, {"ipynb", kText}, {"xml", kText}, {"html", kText},
    {"htm", kText}, {"svg", kText}, {"yaml", kText}, {"yml", kText}, {"toml", kText}, {"ini", kText},
    {"cfg", kText}, {"conf", kText}, {"properties", kText}, {"tex", kText}, {"bib", kText}, {"srt", kText},
    {"css", kText}, {"js", kText}, {"py", kText}, {"c", kText}, {"h", kText}, {"cc", kText}, {"cpp", kText},
    {"cxx", kText}, {"hpp", kText}, {"java", kText}, {"go", kText}, {"rs", kText}, {"rb", kText},
    {"php", kText}, {"sh", kText}, {"bash", kText}, {"bat", kText}, {"cmd", kText}, {"ps1", kText},
    {"sql", kText}, {"lua", kText}, {"cmake", kText}, {"patch", kText}, {"diff", kText},
};

FileTypeInfo text(Encoding encoding, bool hasBom = false)
{
    return {ContentKind::Text, encoding, hasBom, {}};
}

FileTypeInfo binary(std::string_view format)
{
    return {ContentKind::Binary, Encoding::Unknown, false, format};
}

} // namespace

bool isValidUtf8(std::span<const std::uint8_t> data, bool allowTruncatedTail)
{
    const std::size_t n = data.size();
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t lead = data[i];
        if (lead < 0x80) {
            ++i;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codePoint = lead & 0x1Fu;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codePoint = lead & 0x0Fu;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codePoint = lead & 0x07u;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (i + length > n) {
            if (!allowTruncatedTail)
                return false;
            for (std::size_t k = i + 1; k < n; ++k) {
                if ((data[k] & 0xC0) != 0x80)
                    return false;
            }
            return true;
        }

        for (std::size_t k = 1; k < length; ++k) {
            const std::uint8_t next = data[i + k];
            if ((next & 0xC0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

FileTypeInfo detectFileType(std::span<const std::uint8_t> data)
{
    if (data.empty())
        return {};

    const bool truncated = data.size() > kSniffSize;
    const auto sample = data.first(std::min(data.size(), kSniffSize));

    // UTF-32 LE must be tested before UTF-16 LE: its BOM starts with the same two bytes.
    if (matchesAt(sample, 0, "\xEF\xBB\xBF"sv))
        return text(Encoding::Utf8, true);
    if (matchesAt(sample, 0, "\xFF\xFE\0\0"sv))
        return text(Encoding::Utf32LE, true);
    if (matchesAt(sample, 0, "\0\0\xFE\xFF"sv))
        return text(Encoding::Utf32BE, true);
    if (matchesAt(sample, 0, "\xFF\xFE"sv))
        return text(Encoding::Utf16LE, true);
    if (matchesAt(sample, 0, "\xFE\xFF"sv))
        return text(Encoding::Utf16BE, true);

    std::string_view weakFormat;
    if (const std::string_view format = matchMagic(sample, weakFormat); !format.empty())
        return binary(format);

    if (std::memchr(sample.data(), 0, sample.size()) != nullptr) {
        if (const Encoding utf16 = guessUtf16WithoutBom(sample); utf16 != Encoding::Unknown)
            return text(utf16);
        return binary(weakFormat);
    }

    const auto controls = static_cast<std::size_t>(std::count_if(sample.begin(), sample.end(), isBinaryControl));
    const bool ascii = std::all_of(sample.begin(), sample.end(), [](std::uint8_t b) { return b < 0x80; });

    // Valid UTF-8 is strong evidence of text, so it may carry some control characters
    // (e.g. \x01-delimited data files). Without it, only near-clean content counts as text.
    if (ascii || isValidUtf8(sample, truncated)) {
        if (controls * 100 <= sample.size() * 30)
            return text(ascii ? Encoding::Ascii : Encoding::Utf8);
        return binary(weakFormat);
    }
    if (controls * 100 <= sample.size() * 2)
        return text(Encoding::Legacy);
    return binary(weakFormat);
}

std::string_view formatForName(std::string_view fileName)
{
    const std::size_t dot = fileName.rfind('.');
    // ".bashrc" is a name, not an extension.
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == fileName.size())
        return {};
    const std::string_view extension = fileName.substr(dot + 1);
    for (const auto& [known, format] : kExtensions) {
        if (known.size() == extension.size() &&
            std::equal(known.begin(), known.end(), extension.begin(), [](char a, char b) {
                return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b);
            }))
            return format;
    }
    return {};
}

std::string_view contentMismatch(std::string_view fileName, const FileTypeInfo& type)
{
    const std::string_view expected = formatForName(fileName);
    if (expected.empty() || type.kind == ContentKind::Empty)
        return {};
    if (expected == kText)
        return type.kind == ContentKind::Binary ? type.format : std::string_view();
    if (type.kind == ContentKind::Text)
        return kText;
    // "Mach-O universal" is a Mach-O too.
    if (type.format.empty() || type.format.starts_with(expected))
        return {};
    return type.format;
}

} // namespace aiedit::core
