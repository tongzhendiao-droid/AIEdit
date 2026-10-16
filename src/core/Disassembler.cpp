#include "core/Disassembler.h"

#include <algorithm>

#ifdef AIEDIT_HAVE_CAPSTONE
#include <capstone/capstone.h>
#endif

namespace aiedit::core {

namespace {

// x86 instructions take up to 15 bytes, and decoding from a wrong start falls into step within a
// few of them; this much lead is plenty.
constexpr std::uint64_t kVariableLead = 2048;
constexpr std::uint64_t kMaxInstructionBytes = 16;

} // namespace

bool canDisassemble(Architecture architecture)
{
#ifdef AIEDIT_HAVE_CAPSTONE
    return architecture != Architecture::Unknown;
#else
    (void)architecture;
    return false;
#endif
}

std::vector<Instruction> disassembleAround(std::span<const std::uint8_t> data, const BinaryLayout& layout,
                                           std::uint64_t offset, int before, int after)
{
    std::vector<Instruction> result;
#ifdef AIEDIT_HAVE_CAPSTONE
    const Section* section = layout.sectionAt(offset);
    if (!section || !section->executable || layout.bigEndian || offset >= data.size())
        return result;
    cs_arch arch = CS_ARCH_X86;
    int mode = CS_MODE_64;
    std::uint64_t unit = 1; // what instructions are aligned to
    switch (layout.architecture) {
    case Architecture::X86:
        mode = CS_MODE_32;
        break;
    case Architecture::X86_64:
        break;
    case Architecture::Arm:
        arch = CS_ARCH_ARM;
        // Windows on ARM runs Thumb-2; ELF code is taken as ARM.
        mode = layout.format.starts_with("PE") ? CS_MODE_THUMB : CS_MODE_ARM;
        unit = layout.format.starts_with("PE") ? 2 : 4;
        break;
    case Architecture::Arm64:
#if CS_API_MAJOR >= 6
        arch = CS_ARCH_AARCH64;
#else
        arch = CS_ARCH_ARM64;
#endif
        mode = CS_MODE_ARM;
        unit = 4;
        break;
    case Architecture::RiscV64:
        arch = CS_ARCH_RISCV;
        mode = CS_MODE_RISCV64 | CS_MODE_RISCVC;
        unit = 2;
        break;
    case Architecture::Unknown:
        return result;
    }
    csh handle = 0;
    if (cs_open(arch, static_cast<cs_mode>(mode), &handle) != CS_ERR_OK)
        return result;
    cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);

    const std::uint64_t sectionEnd =
        section->fileOffset + std::min<std::uint64_t>(section->fileSize, data.size() - section->fileOffset);
    const std::uint64_t lead = unit == 1 ? kVariableLead : unit * static_cast<std::uint64_t>(before + 1);
    std::uint64_t start = offset - std::min(offset - section->fileOffset, lead);
    start -= (start - section->fileOffset) % unit; // aligned as the section is
    const std::uint64_t end =
        std::min(sectionEnd, offset + kMaxInstructionBytes * static_cast<std::uint64_t>(after + 2));
    const std::uint64_t base = section->address + (start - section->fileOffset);

    cs_insn* decoded = nullptr;
    const std::size_t count = cs_disasm(handle, data.data() + start, static_cast<std::size_t>(end - start), base, 0, &decoded);
    std::size_t holding = count;
    for (std::size_t i = 0; i < count; ++i) {
        if (decoded[i].address - base + start + decoded[i].size > offset) {
            holding = i;
            break;
        }
    }
    if (holding < count) {
        const std::size_t first = holding - std::min<std::size_t>(holding, static_cast<std::size_t>(before));
        const std::size_t last = std::min(count, holding + static_cast<std::size_t>(after) + 1);
        for (std::size_t i = first; i < last; ++i) {
            const cs_insn& insn = decoded[i];
            Instruction instruction;
            instruction.address = insn.address;
            instruction.offset = insn.address - base + start;
            instruction.bytes.assign(insn.bytes, insn.bytes + insn.size);
            instruction.text = insn.mnemonic;
            if (insn.op_str[0] != '\0') {
                instruction.text += ' ';
                instruction.text += insn.op_str;
            }
            result.push_back(std::move(instruction));
        }
    }
    if (decoded)
        cs_free(decoded, count);
    cs_close(&handle);
#else
    (void)data;
    (void)layout;
    (void)offset;
    (void)before;
    (void)after;
#endif
    return result;
}

} // namespace aiedit::core
