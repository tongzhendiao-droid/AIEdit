#pragma once

#include "core/TextCodec.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace aiedit::core {

// Which code unit ends a line. Lf also covers CRLF: the CR is then left out of the row's text.
enum class LineBreak
{
    Lf,
    Cr,
};

struct LineEndings
{
    std::size_t lf = 0; // bare LF
    std::size_t crlf = 0;
    std::size_t cr = 0; // bare CR

    // CR-only files (classic Mac OS) break on CR; everything else on LF.
    LineBreak lineBreak() const { return lf == 0 && crlf == 0 && cr > 0 ? LineBreak::Cr : LineBreak::Lf; }
};

LineEndings countLineEndings(std::span<const std::uint8_t> sample, CharScheme scheme);

// One display row: a whole line, or a piece of a line longer than LineIndex::kMaxRowBytes.
struct Row
{
    std::uint64_t start = 0;      // first byte
    std::uint64_t contentEnd = 0; // end of the text, excluding the line terminator
    std::uint64_t end = 0;        // first byte of the next row
    std::uint64_t line = 0;       // 0-based logical line
    bool continuation = false;    // continues a line that was split for length

    bool operator==(const Row&) const = default;
};

// Splits text into rows: one per line, with overlong lines cut into several rows at
// character boundaries so that no row is expensive to lay out.
//
// Indexing scans the file once and keeps a checkpoint every kRowsPerCheckpoint rows; any row
// is then found by a checkpoint lookup plus a short forward scan, so memory stays small even
// for files with hundreds of millions of lines.
//
// Threading: build()/buildSome() run on one thread while the const accessors are used from
// others. Readers see the progress published so far.
class LineIndex
{
public:
    static constexpr std::size_t kMaxRowBytes = 4096;
    static constexpr std::uint64_t kRowsPerCheckpoint = 256;

    // Rows start at `textStart`, i.e. after a byte order mark.
    LineIndex(std::span<const std::uint8_t> data, CharScheme scheme, LineBreak lineBreak,
              std::uint64_t textStart = 0);

    // Indexes everything unless cancel() is called; returns true if indexing completed.
    bool build();
    // Indexes roughly maxBytes more; returns true once the whole text is indexed.
    bool buildSome(std::uint64_t maxBytes);
    void cancel() { m_cancelled = true; }

    bool isComplete() const;
    std::uint64_t indexedBytes() const; // end offset of the last published row
    std::uint64_t rowCount() const;
    std::uint64_t lineCount() const;

    // index < rowCount()
    Row row(std::uint64_t index) const;
    // Up to `count` consecutive rows starting at `first`, stopping at rowCount().
    std::vector<Row> rows(std::uint64_t first, std::size_t count) const;
    // The row containing offset; offsets past the indexed part give the last row.
    // Requires rowCount() > 0.
    std::uint64_t rowIndexAt(std::uint64_t offset) const;
    // The first row of a 0-based line, if that line has been indexed.
    std::optional<std::uint64_t> rowIndexOfLine(std::uint64_t line) const;

    std::span<const std::uint8_t> data() const { return m_data; }
    CharScheme scheme() const { return m_scheme; }
    LineBreak lineBreak() const { return m_lineBreak; }
    std::uint64_t textStart() const { return m_textStart; }

    // The row that starts at `start`. Splitting is deterministic, so scanning from any row
    // start reproduces exactly the rows the index saw.
    static Row readRow(std::span<const std::uint8_t> data, std::uint64_t start, std::uint64_t line,
                       bool continuation, CharScheme scheme, LineBreak lineBreak);

private:
    struct Checkpoint
    {
        std::uint64_t offset;
        std::uint64_t line;
        bool continuation;
    };

    struct Progress
    {
        std::uint64_t rows = 0;
        std::uint64_t lines = 0;
        std::uint64_t bytes = 0;
        bool complete = false;
    };

    Row readRowAt(const Checkpoint& at) const;
    static Row nextRow(std::span<const std::uint8_t> data, const Row& previous, CharScheme scheme,
                       LineBreak lineBreak);
    void publish(bool complete);

    std::span<const std::uint8_t> m_data;
    CharScheme m_scheme;
    LineBreak m_lineBreak;
    std::uint64_t m_textStart;

    // Builder state, only touched by the indexing thread.
    std::optional<Row> m_lastBuilt;
    std::uint64_t m_builtRows = 0;
    std::uint64_t m_lastPublishedBytes = 0;
    std::vector<Checkpoint> m_pending;

    mutable std::mutex m_mutex;
    std::vector<Checkpoint> m_checkpoints; // checkpoint i is the start of row i * kRowsPerCheckpoint
    Progress m_progress;
    std::atomic<bool> m_cancelled{false};
};

// The start of the character containing offset, clamped to [textStart, size]. Legacy CJK
// encodings need the index to find a known boundary; offsets past its indexed part are
// returned unchanged for them.
std::uint64_t snapToCharStart(const LineIndex& index, std::uint64_t offset);

} // namespace aiedit::core
