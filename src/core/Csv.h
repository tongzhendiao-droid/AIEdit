#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aiedit::core {

// Comma- or tab-separated values (RFC 4180: quoted fields may hold separators, quotes written
// twice, and line breaks), read straight from the bytes. That works for UTF-8 and for GBK,
// Big5 and Shift-JIS alike, whose multi-byte characters never contain the ASCII separators,
// quotes or line breaks. Cells are kept as byte ranges, decoded only when shown.
struct CsvTable
{
    struct Cell
    {
        std::uint64_t start = 0;   // of the field's text, inside its quotes if quoted
        std::uint32_t length = 0;  // to the closing quote, or the separator
        bool quoted = false;       // doubled quotes inside it still need undoing
    };
    std::vector<Cell> cells;
    std::vector<std::uint32_t> rowFirstCell; // each row's first cell, then one past the last
    std::vector<std::uint64_t> rowOffsets;   // where each row starts in the bytes
    std::size_t columns = 0;                 // of the widest row
    bool truncated = false;                  // rows were left out at a limit

    std::size_t rowCount() const { return rowOffsets.size(); }
    std::span<const Cell> row(std::size_t r) const
    {
        return std::span<const Cell>(cells).subspan(rowFirstCell[r], rowFirstCell[r + 1] - rowFirstCell[r]);
    }
};

// The separator a file most likely uses: tab, comma, semicolon or '|', by how evenly the first
// lines split on each.
char detectCsvSeparator(std::span<const std::uint8_t> data);

// Reads every row, up to `maxRows` rows and `maxCells` cells; `cancelled`, polled now and then,
// stops it early (as truncated).
CsvTable readCsv(std::span<const std::uint8_t> data, char separator, std::size_t maxRows = 2'000'000,
                 std::size_t maxCells = 40'000'000, const std::atomic<bool>* cancelled = nullptr);

// A cell's bytes, with the doubled quotes of a quoted field made single.
std::string csvCellBytes(std::span<const std::uint8_t> data, const CsvTable::Cell& cell);

} // namespace aiedit::core
