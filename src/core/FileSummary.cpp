#include "core/FileSummary.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace aiedit::core {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";
constexpr std::size_t kDumpBytesPerLine = 16;

bool isPrintable(std::uint8_t b)
{
    return b >= 0x20 && b < 0x7F;
}

void appendHex(std::string& out, std::uint64_t value, int digits)
{
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        out += kHexDigits[(value >> shift) & 0xF];
}

} // namespace

std::string hexDump(std::span<const std::uint8_t> bytes, std::uint64_t baseOffset)
{
    std::string out;
    out.reserve(bytes.size() / kDumpBytesPerLine * 80 + 80);
    for (std::size_t line = 0; line < bytes.size(); line += kDumpBytesPerLine) {
        const auto row = bytes.subspan(line, std::min(kDumpBytesPerLine, bytes.size() - line));
        appendHex(out, baseOffset + line, 8);
        out += "  ";
        for (std::size_t i = 0; i < kDumpBytesPerLine; ++i) {
            if (i < row.size()) {
                appendHex(out, row[i], 2);
                out += ' ';
            } else {
                out += "   ";
            }
            if (i == 7)
                out += ' ';
        }
        out += " |";
        for (const std::uint8_t b : row)
            out += isPrintable(b) ? static_cast<char>(b) : '.';
        out += "|\n";
    }
    return out;
}

double entropy(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return 0;
    std::array<std::size_t, 256> counts{};
    for (const std::uint8_t b : bytes)
        ++counts[b];
    double result = 0;
    const auto total = static_cast<double>(bytes.size());
    for (const std::size_t count : counts) {
        if (count == 0)
            continue;
        const double p = static_cast<double>(count) / total;
        result -= p * std::log2(p);
    }
    return result;
}

std::vector<std::string> extractStrings(std::span<const std::uint8_t> bytes, std::size_t minLength,
                                        std::size_t maxCount)
{
    std::vector<std::string> result;
    std::string current;
    const auto flush = [&] {
        if (current.size() >= minLength && result.size() < maxCount)
            result.push_back(current);
        current.clear();
    };
    for (const std::uint8_t b : bytes) {
        if (isPrintable(b) || b == '\t')
            current += static_cast<char>(b);
        else
            flush();
        if (result.size() >= maxCount)
            return result;
    }
    flush();
    return result;
}

FileSummary summarizeFile(std::span<const std::uint8_t> data)
{
    FileSummary summary;
    summary.size = data.size();
    summary.type = detectFileType(data);
    const auto head = data.first(std::min(data.size(), kSummaryHeadBytes));
    summary.headEntropy = entropy(head);
    summary.headDump = hexDump(data.first(std::min(data.size(), kSummaryDumpBytes)));
    summary.strings = extractStrings(head);

    // Sample blocks spread over the whole file: a jump in entropy shows where compressed or
    // encrypted parts start.
    if (data.size() > kSummaryBlockBytes) {
        const std::uint64_t span = data.size() - kSummaryBlockBytes;
        const std::size_t blocks = std::min<std::uint64_t>(kSummaryBlocks, data.size() / kSummaryBlockBytes);
        for (std::size_t i = 0; i < blocks; ++i) {
            const std::uint64_t offset = blocks == 1 ? 0 : span * i / (blocks - 1);
            summary.blockEntropy.push_back(entropy(data.subspan(static_cast<std::size_t>(offset), kSummaryBlockBytes)));
        }
    }
    return summary;
}

} // namespace aiedit::core
