#include "core/BinaryLayout.h"

#include <algorithm>
#include <cstring>

namespace aiedit::core {

namespace {

constexpr std::uint32_t kPeExecute = 0x2000'0000; // IMAGE_SCN_MEM_EXECUTE
constexpr std::uint32_t kPeCode = 0x0000'0020;    // IMAGE_SCN_CNT_CODE
constexpr std::uint64_t kElfExecute = 0x4;        // SHF_EXECINSTR
constexpr std::uint32_t kElfNoBits = 8;           // SHT_NOBITS
constexpr std::uint32_t kElfLoad = 1;             // PT_LOAD
constexpr std::uint32_t kElfSegmentExecute = 1;   // PF_X
constexpr std::size_t kMaxSections = 4096;

// Reads integers at offsets, within bounds, in one byte order.
class Reader
{
public:
    Reader(std::span<const std::uint8_t> data, bool bigEndian) : m_data(data), m_bigEndian(bigEndian) {}

    bool has(std::uint64_t offset, std::uint64_t size) const
    {
        return offset <= m_data.size() && size <= m_data.size() - offset;
    }

    std::uint64_t read(std::uint64_t offset, int bytes) const
    {
        if (!has(offset, static_cast<std::uint64_t>(bytes)))
            return 0;
        std::uint64_t value = 0;
        for (int i = 0; i < bytes; ++i) {
            const std::uint8_t byte = m_data[static_cast<std::size_t>(offset) + static_cast<std::size_t>(i)];
            const int shift = 8 * (m_bigEndian ? bytes - 1 - i : i);
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return value;
    }
    std::uint16_t u16(std::uint64_t offset) const { return static_cast<std::uint16_t>(read(offset, 2)); }
    std::uint32_t u32(std::uint64_t offset) const { return static_cast<std::uint32_t>(read(offset, 4)); }
    std::uint64_t u64(std::uint64_t offset) const { return read(offset, 8); }

    // A string of up to `max` bytes, ending at a NUL.
    std::string text(std::uint64_t offset, std::size_t max) const
    {
        std::string out;
        for (std::size_t i = 0; i < max && has(offset + i, 1); ++i) {
            const char c = static_cast<char>(m_data[static_cast<std::size_t>(offset + i)]);
            if (c == '\0')
                break;
            out += c;
        }
        return out;
    }

private:
    std::span<const std::uint8_t> m_data;
    bool m_bigEndian;
};

std::optional<BinaryLayout> readPe(std::span<const std::uint8_t> data)
{
    const Reader le(data, false);
    if (!le.has(0, 0x40) || data[0] != 'M' || data[1] != 'Z')
        return std::nullopt;
    const std::uint32_t pe = le.u32(0x3C);
    if (!le.has(pe, 24) || std::memcmp(data.data() + pe, "PE\0\0", 4) != 0)
        return std::nullopt;
    const std::uint64_t coff = pe + 4;
    const std::uint16_t machine = le.u16(coff);
    const std::uint16_t sectionCount = le.u16(coff + 2);
    const std::uint16_t optionalSize = le.u16(coff + 16);
    const std::uint64_t optional = coff + 20;

    BinaryLayout layout;
    switch (le.u16(optional)) {
    case 0x10B:
        layout.format = "PE32";
        layout.imageBase = le.u32(optional + 28);
        break;
    case 0x20B:
        layout.format = "PE32+";
        layout.imageBase = le.u64(optional + 24);
        break;
    default:
        return std::nullopt;
    }
    switch (machine) {
    case 0x014C:
        layout.architecture = Architecture::X86;
        break;
    case 0x8664:
        layout.architecture = Architecture::X86_64;
        break;
    case 0x01C0:
    case 0x01C4:
        layout.architecture = Architecture::Arm;
        break;
    case 0xAA64:
        layout.architecture = Architecture::Arm64;
        break;
    default:
        break;
    }
    const std::uint64_t table = optional + optionalSize;
    for (std::uint16_t i = 0; i < sectionCount && i < kMaxSections; ++i) {
        const std::uint64_t header = table + i * 40ull;
        if (!le.has(header, 40))
            break;
        Section section;
        section.name = le.text(header, 8);
        const std::uint32_t virtualAddress = le.u32(header + 12);
        section.fileSize = le.u32(header + 16);
        section.fileOffset = le.u32(header + 20);
        section.address = layout.imageBase + virtualAddress;
        const std::uint32_t flags = le.u32(header + 36);
        section.executable = (flags & (kPeExecute | kPeCode)) != 0;
        layout.sections.push_back(std::move(section));
    }
    return layout;
}

std::optional<BinaryLayout> readElf(std::span<const std::uint8_t> data)
{
    if (data.size() < 52 || std::memcmp(data.data(), "\x7F" "ELF", 4) != 0)
        return std::nullopt;
    const bool is64 = data[4] == 2;
    if (data[4] != 1 && !is64)
        return std::nullopt;
    if (data[5] != 1 && data[5] != 2)
        return std::nullopt;
    BinaryLayout layout;
    layout.format = is64 ? "ELF64" : "ELF32";
    layout.bigEndian = data[5] == 2;
    const Reader r(data, layout.bigEndian);
    switch (r.u16(18)) {
    case 3:
        layout.architecture = Architecture::X86;
        break;
    case 62:
        layout.architecture = Architecture::X86_64;
        break;
    case 40:
        layout.architecture = Architecture::Arm;
        break;
    case 183:
        layout.architecture = Architecture::Arm64;
        break;
    case 243:
        layout.architecture = is64 ? Architecture::RiscV64 : Architecture::Unknown;
        break;
    default:
        break;
    }

    // Sections, named from the section name table.
    const std::uint64_t sectionTable = is64 ? r.u64(40) : r.u32(32);
    const std::uint16_t sectionSize = r.u16(is64 ? 58 : 46);
    const std::uint16_t sectionCount = r.u16(is64 ? 60 : 48);
    const std::uint16_t namesIndex = r.u16(is64 ? 62 : 50);
    if (sectionTable != 0 && sectionSize >= (is64 ? 64 : 40) && sectionCount > 0 &&
        r.has(sectionTable, static_cast<std::uint64_t>(sectionSize) * sectionCount)) {
        const auto header = [&](std::uint16_t i) { return sectionTable + static_cast<std::uint64_t>(i) * sectionSize; };
        const std::uint64_t names = namesIndex < sectionCount ? (is64 ? r.u64(header(namesIndex) + 24)
                                                                      : r.u32(header(namesIndex) + 16))
                                                              : 0;
        for (std::uint16_t i = 1; i < sectionCount && i < kMaxSections; ++i) { // 0 is always empty
            const std::uint64_t at = header(i);
            Section section;
            section.name = names != 0 ? r.text(names + r.u32(at), 64) : std::string();
            const std::uint32_t type = r.u32(at + 4);
            const std::uint64_t flags = is64 ? r.u64(at + 8) : r.u32(at + 8);
            section.address = is64 ? r.u64(at + 16) : r.u32(at + 12);
            section.fileOffset = is64 ? r.u64(at + 24) : r.u32(at + 16);
            section.fileSize = type == kElfNoBits ? 0 : (is64 ? r.u64(at + 32) : r.u32(at + 20));
            section.executable = (flags & kElfExecute) != 0;
            layout.sections.push_back(std::move(section));
        }
    }
    // Without section headers (stripped that far), the loaded segments say as much.
    if (layout.sections.empty()) {
        const std::uint64_t segmentTable = is64 ? r.u64(32) : r.u32(28);
        const std::uint16_t segmentSize = r.u16(is64 ? 54 : 42);
        const std::uint16_t segmentCount = r.u16(is64 ? 56 : 44);
        for (std::uint16_t i = 0; segmentTable != 0 && i < segmentCount && i < kMaxSections; ++i) {
            const std::uint64_t at = segmentTable + static_cast<std::uint64_t>(i) * segmentSize;
            if (!r.has(at, is64 ? 56 : 32) || r.u32(at) != kElfLoad)
                continue;
            Section segment;
            segment.name = "LOAD" + std::to_string(layout.sections.size());
            const std::uint32_t flags = is64 ? r.u32(at + 4) : r.u32(at + 24);
            segment.fileOffset = is64 ? r.u64(at + 8) : r.u32(at + 4);
            segment.address = is64 ? r.u64(at + 16) : r.u32(at + 8);
            segment.fileSize = is64 ? r.u64(at + 32) : r.u32(at + 16);
            segment.executable = (flags & kElfSegmentExecute) != 0;
            layout.sections.push_back(std::move(segment));
        }
    }
    return layout;
}

} // namespace

const Section* BinaryLayout::sectionAt(std::uint64_t offset) const
{
    for (const Section& section : sections)
        if (section.fileSize > 0 && offset >= section.fileOffset && offset - section.fileOffset < section.fileSize)
            return &section;
    return nullptr;
}

std::optional<BinaryLayout> readBinaryLayout(std::span<const std::uint8_t> data)
{
    if (std::optional<BinaryLayout> pe = readPe(data))
        return pe;
    return readElf(data);
}

const char* architectureName(Architecture architecture)
{
    switch (architecture) {
    case Architecture::X86:
        return "x86";
    case Architecture::X86_64:
        return "x86-64";
    case Architecture::Arm:
        return "ARM";
    case Architecture::Arm64:
        return "ARM64";
    case Architecture::RiscV64:
        return "RISC-V 64";
    case Architecture::Unknown:
        break;
    }
    return "unknown";
}

} // namespace aiedit::core
