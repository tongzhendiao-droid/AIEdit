#pragma once

#include "core/BinaryLayout.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aiedit::core {

struct Instruction
{
    std::uint64_t offset = 0;  // in the file
    std::uint64_t address = 0; // where it runs
    std::vector<std::uint8_t> bytes;
    std::string text; // "mov eax, 1"
};

// Whether this build can disassemble code of an architecture (it needs Capstone).
bool canDisassemble(Architecture architecture);

// The instructions around a file offset in an executable section: `before` of them, the one
// holding the offset, and `after` more. Code of varying length (x86) is decoded from well
// before, so that it lines up as the processor reads it; bytes that are no instruction show as
// data. Nothing if the offset is in no executable section, or the architecture is unknown.
std::vector<Instruction> disassembleAround(std::span<const std::uint8_t> data, const BinaryLayout& layout,
                                           std::uint64_t offset, int before = 5, int after = 8);

} // namespace aiedit::core
