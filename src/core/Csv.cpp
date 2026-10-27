#include "core/Csv.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace aiedit::core {

char detectCsvSeparator(std::span<const std::uint8_t> data)
{
    // Count each candidate in the first lines, outside quotes; the one whose count is the same
    // on most lines, and not zero, wins. Ties go to the earlier candidate.
    static constexpr std::array<char, 4> candidates = {'\t', ',', ';', '|'};
    constexpr int kLines = 20;
    std::array<std::array<int, kLines>, candidates.size()> counts{};
    int lines = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < data.size() && i < 64 * 1024 && lines < kLines; ++i) {
        const char c = static_cast<char>(data[i]);
        if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && c == '\n') {
            ++lines;
        } else if (!quoted) {
            for (std::size_t k = 0; k < candidates.size(); ++k) {
                if (c == candidates[k])
                    ++counts[k][lines];
            }
        }
    }
    if (lines < kLines && !data.empty())
        ++lines; // the last line, without its line break
    char best = ',';
    double bestScore = 0;
    for (std::size_t k = 0; k < candidates.size(); ++k) {
        // How many lines share the most common count.
        std::array<int, kLines> sorted = counts[k];
        std::sort(sorted.begin(), sorted.begin() + lines);
        int run = 0;
        int bestRun = 0;
        int mode = 0;
        for (int i = 0; i < lines; ++i) {
            run = i > 0 && sorted[i] == sorted[i - 1] ? run + 1 : 1;
            if (run > bestRun) {
                bestRun = run;
                mode = sorted[i];
            }
        }
        if (mode == 0)
            continue;
        const double score = bestRun + std::log2(1.0 + mode) / 10; // more columns break ties
        if (score > bestScore) {
            bestScore = score;
            best = candidates[k];
        }
    }
    return best;
}

CsvTable readCsv(std::span<const std::uint8_t> data, char separator, std::size_t maxRows, std::size_t maxCells,
                 const std::atomic<bool>* cancelled)
{
    CsvTable table;
    const std::uint8_t sep = static_cast<std::uint8_t>(separator);
    std::size_t i = 0;
    // A UTF-8 byte order mark is not part of the first cell.
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        i = 3;
    const std::size_t n = data.size();
    while (i < n) {
        if (table.rowCount() >= maxRows || table.cells.size() >= maxCells ||
            (cancelled && (table.rowCount() & 0xFFF) == 0 && *cancelled)) {
            table.truncated = true;
            break;
        }
        table.rowOffsets.push_back(i);
        table.rowFirstCell.push_back(static_cast<std::uint32_t>(table.cells.size()));
        for (;;) {
            CsvTable::Cell cell;
            if (i < n && data[i] == '"') {
                cell.quoted = true;
                cell.start = ++i;
                while (i < n) {
                    if (data[i] == '"') {
                        if (i + 1 < n && data[i + 1] == '"') {
                            i += 2;
                            continue;
                        }
                        break;
                    }
                    ++i;
                }
                cell.length = static_cast<std::uint32_t>(std::min<std::size_t>(i - cell.start, UINT32_MAX));
                if (i < n)
                    ++i; // the closing quote
                // Whatever follows it before the separator is kept out, as spreadsheets do.
                while (i < n && data[i] != sep && data[i] != '\n' && data[i] != '\r')
                    ++i;
            } else {
                cell.start = i;
                while (i < n && data[i] != sep && data[i] != '\n' && data[i] != '\r')
                    ++i;
                cell.length = static_cast<std::uint32_t>(std::min<std::size_t>(i - cell.start, UINT32_MAX));
            }
            table.cells.push_back(cell);
            if (i < n && data[i] == sep) {
                ++i;
                continue;
            }
            // The end of the row: \n, \r\n or \r.
            if (i < n && data[i] == '\r')
                ++i;
            if (i < n && data[i] == '\n')
                ++i;
            break;
        }
        const std::size_t width = table.cells.size() - table.rowFirstCell.back();
        table.columns = std::max(table.columns, width);
    }
    table.rowFirstCell.push_back(static_cast<std::uint32_t>(table.cells.size()));
    return table;
}

std::string csvCellBytes(std::span<const std::uint8_t> data, const CsvTable::Cell& cell)
{
    if (cell.start >= data.size())
        return {};
    const std::size_t length = std::min<std::size_t>(cell.length, data.size() - cell.start);
    std::string s(reinterpret_cast<const char*>(data.data() + cell.start), length);
    if (cell.quoted) {
        std::size_t out = 0;
        for (std::size_t k = 0; k < s.size(); ++k) {
            s[out++] = s[k];
            if (s[k] == '"' && k + 1 < s.size() && s[k + 1] == '"')
                ++k;
        }
        s.resize(out);
    }
    return s;
}

} // namespace aiedit::core
