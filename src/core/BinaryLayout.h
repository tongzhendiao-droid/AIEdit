#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aiedit::core {

enum class Architecture
{
    Unknown,
    X86,
    X86_64,
    Arm,   // 32-bit ARM (Thumb-2 for Windows on ARM)
    Arm64,
    RiscV64,
};

struct Section
{
    std::string name;
    std::uint64_t fileOffset = 0; // where its bytes are in the file
    std::uint64_t fileSize = 0;   // 0 for sections that take no room in the file (.bss)
    std::uint64_t address = 0;    // where it is loaded (for PE, the image base plus its RVA)
    bool executable = false;
};

// Where the parts of an executable or a library lie: PE (Windows .exe, .dll) or ELF (Linux
// programs, .so, .o), read from their headers.
struct BinaryLayout
{
    std::string format; // "PE32", "PE32+", "ELF32", "ELF64"
    Architecture architecture = Architecture::Unknown;
    bool bigEndian = false;
    std::uint64_t imageBase = 0; // PE: where the image is meant to load; an RVA is an address minus it
    std::vector<Section> sections;

    // The section holding a file offset; nullptr for the headers and anything outside them all.
    const Section* sectionAt(std::uint64_t offset) const;
};

// Reads the headers of a PE or ELF file; nullopt if it is neither, or they do not hold together.
std::optional<BinaryLayout> readBinaryLayout(std::span<const std::uint8_t> data);

// A name for an architecture, as people write it: "x86-64", "ARM64", …
const char* architectureName(Architecture architecture);

} // namespace aiedit::core
