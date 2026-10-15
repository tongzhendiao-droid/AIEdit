#pragma once

#include "core/FileType.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aiedit::core {

// A classic hex dump: "00000010  4D 5A 90 00 …  |MZ..|", 16 bytes per line.
std::string hexDump(std::span<const std::uint8_t> bytes, std::uint64_t baseOffset = 0);

// Shannon entropy in bits per byte, 0 to 8. Values near 8 mean compressed or encrypted data.
double entropy(std::span<const std::uint8_t> bytes);

// Runs of printable ASCII at least minLength long, as the `strings` tool finds them.
std::vector<std::string> extractStrings(std::span<const std::uint8_t> bytes, std::size_t minLength = 6,
                                        std::size_t maxCount = 64);

// What "what is this file?" sends to an AI instead of the file itself: a few facts computed
// on this machine.
struct FileSummary
{
    std::uint64_t size = 0;
    FileTypeInfo type;
    double headEntropy = 0;           // of the first kSummaryHeadBytes
    std::vector<double> blockEntropy; // of evenly spaced blocks across the whole file
    std::string headDump;             // hex dump of the first kSummaryDumpBytes
    std::vector<std::string> strings; // found in the first kSummaryHeadBytes
};

inline constexpr std::size_t kSummaryHeadBytes = 64 * 1024;
inline constexpr std::size_t kSummaryDumpBytes = 256;
inline constexpr std::size_t kSummaryBlocks = 16;
inline constexpr std::size_t kSummaryBlockBytes = 4096;

FileSummary summarizeFile(std::span<const std::uint8_t> data);

} // namespace aiedit::core
