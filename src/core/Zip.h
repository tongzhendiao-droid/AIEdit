#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aiedit::core {

// Reads the files in a ZIP archive held in memory, as docx, xlsx and epub are. Only what those
// need: stored and deflated entries, ZIP64 sizes; no encryption, no archives split in parts.
class ZipArchive
{
public:
    struct Entry
    {
        std::string name; // as stored: '/' between folders, UTF-8 or the archive's own code page
        std::uint16_t method = 0;
        std::uint16_t flags = 0;
        std::uint32_t crc = 0;
        std::uint64_t compressedSize = 0;
        std::uint64_t size = 0;
        std::uint64_t localHeaderOffset = 0;
    };

    // Nothing if `data` has no readable central directory. The archive refers to `data`, which
    // must outlive it.
    static std::optional<ZipArchive> open(std::span<const std::uint8_t> data);

    const std::vector<Entry>& entries() const { return m_entries; }
    // The entry with this name; names compare exactly, then ignoring ASCII case.
    const Entry* find(std::string_view name) const;
    // The entry's contents. Nothing if it is encrypted, compressed in a way not read here,
    // broken, or larger than `maxSize`.
    std::optional<std::vector<std::uint8_t>> read(const Entry& entry, std::size_t maxSize) const;
    std::optional<std::vector<std::uint8_t>> read(std::string_view name, std::size_t maxSize) const;

private:
    explicit ZipArchive(std::span<const std::uint8_t> data) : m_data(data) {}

    std::span<const std::uint8_t> m_data;
    std::vector<Entry> m_entries;
};

} // namespace aiedit::core
