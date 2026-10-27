#include "core/Zip.h"

#include "core/Inflate.h"

#include <algorithm>

namespace aiedit::core {

namespace {

constexpr std::uint32_t kEndOfDirectory = 0x06054b50;
constexpr std::uint32_t kZip64EndOfDirectory = 0x06064b50;
constexpr std::uint32_t kZip64Locator = 0x07064b50;
constexpr std::uint32_t kDirectoryEntry = 0x02014b50;
constexpr std::uint32_t kLocalHeader = 0x04034b50;

std::uint16_t u16(std::span<const std::uint8_t> d, std::size_t at)
{
    return static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t at)
{
    return static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) |
           (static_cast<std::uint32_t>(d[at + 2]) << 16) | (static_cast<std::uint32_t>(d[at + 3]) << 24);
}

std::uint64_t u64(std::span<const std::uint8_t> d, std::size_t at)
{
    return u32(d, at) | (static_cast<std::uint64_t>(u32(d, at + 4)) << 32);
}

bool equalsIgnoringCase(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

} // namespace

std::optional<ZipArchive> ZipArchive::open(std::span<const std::uint8_t> data)
{
    // The end of the central directory: 22 bytes, then a comment of up to 64 KiB.
    if (data.size() < 22)
        return std::nullopt;
    std::size_t end = data.size() - 22;
    const std::size_t lowest = end > 0xFFFF ? end - 0xFFFF : 0;
    for (;;) {
        if (u32(data, end) == kEndOfDirectory)
            break;
        if (end == lowest)
            return std::nullopt;
        --end;
    }

    std::uint64_t count = u16(data, end + 10);
    std::uint64_t directorySize = u32(data, end + 12);
    std::uint64_t directoryOffset = u32(data, end + 16);
    if ((count == 0xFFFF || directoryOffset == 0xFFFFFFFF) && end >= 20 && u32(data, end - 20) == kZip64Locator) {
        const std::uint64_t record = u64(data, end - 20 + 8);
        if (record + 56 <= data.size() && u32(data, static_cast<std::size_t>(record)) == kZip64EndOfDirectory) {
            count = u64(data, static_cast<std::size_t>(record) + 32);
            directorySize = u64(data, static_cast<std::size_t>(record) + 40);
            directoryOffset = u64(data, static_cast<std::size_t>(record) + 48);
        }
    }
    if (directoryOffset > data.size() || directorySize > data.size() - directoryOffset)
        return std::nullopt;

    ZipArchive archive(data);
    std::size_t at = static_cast<std::size_t>(directoryOffset);
    const std::size_t directoryEnd = static_cast<std::size_t>(directoryOffset + directorySize);
    for (std::uint64_t i = 0; i < count; ++i) {
        if (at + 46 > directoryEnd || u32(data, at) != kDirectoryEntry)
            break; // what was read so far still opens
        Entry entry;
        entry.flags = u16(data, at + 8);
        entry.method = u16(data, at + 10);
        entry.crc = u32(data, at + 16);
        entry.compressedSize = u32(data, at + 20);
        entry.size = u32(data, at + 24);
        const std::size_t nameLength = u16(data, at + 28);
        const std::size_t extraLength = u16(data, at + 30);
        const std::size_t commentLength = u16(data, at + 32);
        entry.localHeaderOffset = u32(data, at + 42);
        const std::size_t next = at + 46 + nameLength + extraLength + commentLength;
        if (next > directoryEnd)
            break;
        entry.name.assign(reinterpret_cast<const char*>(data.data() + at + 46), nameLength);

        // ZIP64: the sizes and offset that did not fit, in this order, in extra field 1.
        std::size_t extra = at + 46 + nameLength;
        const std::size_t extraEnd = extra + extraLength;
        while (extra + 4 <= extraEnd) {
            const std::uint16_t id = u16(data, extra);
            const std::size_t length = u16(data, extra + 2);
            std::size_t field = extra + 4;
            const std::size_t fieldEnd = std::min(field + length, extraEnd);
            if (id == 1) {
                for (std::uint64_t* value : {&entry.size, &entry.compressedSize, &entry.localHeaderOffset}) {
                    if (*value != 0xFFFFFFFF || field + 8 > fieldEnd)
                        continue;
                    *value = u64(data, field);
                    field += 8;
                }
            }
            extra = fieldEnd;
        }
        archive.m_entries.push_back(std::move(entry));
        at = next;
    }
    return archive;
}

const ZipArchive::Entry* ZipArchive::find(std::string_view name) const
{
    for (const Entry& entry : m_entries) {
        if (entry.name == name)
            return &entry;
    }
    for (const Entry& entry : m_entries) {
        if (equalsIgnoringCase(entry.name, name))
            return &entry;
    }
    return nullptr;
}

std::optional<std::vector<std::uint8_t>> ZipArchive::read(const Entry& entry, std::size_t maxSize) const
{
    if ((entry.flags & 1) != 0 || entry.size > maxSize)
        return std::nullopt; // encrypted, or too large
    const std::uint64_t local = entry.localHeaderOffset;
    if (local + 30 > m_data.size() || u32(m_data, static_cast<std::size_t>(local)) != kLocalHeader)
        return std::nullopt;
    const std::uint64_t start = local + 30 + u16(m_data, static_cast<std::size_t>(local) + 26) +
                                u16(m_data, static_cast<std::size_t>(local) + 28);
    if (start > m_data.size() || entry.compressedSize > m_data.size() - start)
        return std::nullopt;
    const auto stored = m_data.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(entry.compressedSize));

    std::vector<std::uint8_t> bytes;
    if (entry.method == 0) {
        bytes.assign(stored.begin(), stored.end());
    } else if (entry.method == 8) {
        bytes.reserve(static_cast<std::size_t>(entry.size));
        if (!inflate(stored, maxSize, bytes))
            return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (bytes.size() != entry.size || crc32(bytes) != entry.crc)
        return std::nullopt;
    return bytes;
}

std::optional<std::vector<std::uint8_t>> ZipArchive::read(std::string_view name, std::size_t maxSize) const
{
    const Entry* entry = find(name);
    return entry ? read(*entry, maxSize) : std::nullopt;
}

} // namespace aiedit::core
