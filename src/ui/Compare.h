#pragma once

#include "core/BinaryLayout.h"
#include "core/ByteDiff.h"
#include "core/Diff.h"
#include "core/Disassembler.h"
#include "core/FileType.h"
#include "core/LineIndex.h"
#include "core/TextCodec.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

class TextDecoder;

// One side of a comparison: the bytes of a file, of a document as it is being edited, or of the
// clipboard, and how to read them.
struct CompareSide
{
    QString label; // what the comparison calls it: a file name, "Clipboard", …
    QString path;  // the file, if there is one, to open it at a line
    std::shared_ptr<const core::FileSource> source;
    core::FileTypeInfo type;
    const core::TextCodec* codec = nullptr;
    // Fetches the bytes again, for Refresh: the file from disk, the document as it is now.
    // Nullptr when they cannot be had any more.
    std::function<std::shared_ptr<const core::FileSource>()> reload;
};

// A side for a file on disk, read in the codec it is detected to have; nullopt, with the reason
// in `error`, if it cannot be opened.
std::optional<CompareSide> fileSide(const QString& path, std::string_view legacyCodec, QString* error);
// A side for bytes in memory, such as a document's unsaved text or the clipboard.
CompareSide bytesSide(const QString& label, std::shared_ptr<const core::FileSource> source,
                      const core::TextCodec& codec);

struct CompareOptions
{
    bool ignoreWhitespace = false; // all of it: spaces added, removed or changed
    bool ignoreCase = false;
    bool ignoreLineEndings = true; // CRLF and LF, and a missing line break at the end

    bool operator==(const CompareOptions&) const = default;
};

// A side's lines, indexed, each with a key: a hash of the line after what the options ignore.
struct LineKeys
{
    std::shared_ptr<core::LineIndex> index;
    std::vector<std::uint64_t> keys;
};

// Indexes a side and makes its keys. With `decode`, lines are compared as text (decoded, so that
// a GBK file and its UTF-8 copy are equal); without it, as bytes, which is faster and enough when
// both sides are read alike and nothing is to be ignored. Nullopt if cancelled.
std::optional<LineKeys> lineKeys(const CompareSide& side, const CompareOptions& options, bool decode,
                                 const std::function<bool()>& cancelled = {});

// Whether two sides must be compared as decoded text (see lineKeys()).
bool needsDecoding(const CompareSide& left, const CompareSide& right, const CompareOptions& options);

struct TextComparison
{
    std::shared_ptr<core::LineIndex> left;
    std::shared_ptr<core::LineIndex> right;
    std::vector<core::DiffHunk> hunks;
    bool identicalBytes = false; // not just equal lines: the very same bytes
    bool cancelled = false;
};

// Compares two sides line by line, on the calling thread (a worker's).
TextComparison compareText(const CompareSide& left, const CompareSide& right, const CompareOptions& options,
                           const std::function<bool()>& cancelled = {});

// The left side with only the accepted changes made: the right's lines in place of each
// accepted hunk, the left's bytes everywhere else.
QByteArray acceptChanges(const TextComparison& comparison, std::span<const std::uint8_t> left,
                         std::span<const std::uint8_t> right, const std::vector<bool>& accepted);

// A line as shown: decoded, control characters made visible as the text view shows them. A line
// longer than an index row (4 KiB) is cut to its first row, and `cut` says so.
QString shownLine(std::span<const std::uint8_t> bytes, const core::LineIndex& index, const TextDecoder& decoder,
                  std::uint64_t line, bool* cut = nullptr);

// What a comparison shows, row by row: same lines side by side, changed ones paired, removed and
// added ones against a gap. With `onlyDifferences`, each run of same lines is folded to the few
// around the changes; a fold can be opened.
class CompareRows
{
public:
    enum class Kind
    {
        Same,
        Changed, // a line on each side, which differ
        Removed, // only on the left
        Added,   // only on the right
        Fold,    // same lines, not shown
    };

    struct Row
    {
        Kind kind = Kind::Same;
        std::optional<std::uint64_t> left;  // 0-based line; for a fold, the first one folded
        std::optional<std::uint64_t> right;
        int hunk = -1;            // the hunk the row shows, for changed, removed and added rows
        std::uint64_t folded = 0; // for a fold: how many lines it stands for
    };

    static constexpr int kContext = 3;

    void build(const std::vector<core::DiffHunk>& hunks, std::uint64_t leftLines, std::uint64_t rightLines,
               bool onlyDifferences);
    // Shows the lines that the fold at `row` stands for. False if it is no fold.
    bool unfold(std::uint64_t row);

    std::uint64_t count() const { return m_rows; }
    Row at(std::uint64_t row) const;
    int hunkCount() const { return static_cast<int>(m_hunkRows.size()); }
    std::uint64_t firstRowOf(int hunk) const { return m_hunkRows[static_cast<std::size_t>(hunk)]; }
    // The hunk at `row` or the first one after it, and the last one before it; -1 for none.
    int hunkAtOrAfter(std::uint64_t row) const;
    int hunkBefore(std::uint64_t row) const;
    // The row showing a line of one side, or the fold holding it.
    std::uint64_t rowOfLine(bool left, std::uint64_t line) const;

private:
    struct Segment
    {
        Kind kind; // Same, Fold, or Changed for a hunk (its rows say which)
        std::uint64_t firstRow;
        std::uint64_t rows;
        std::uint64_t left;
        std::uint64_t right;
        std::uint64_t leftCount;
        std::uint64_t rightCount;
        int hunk;
        std::uint64_t region; // for a fold: where its run of same lines starts on the left
    };

    void rebuild();
    void addSame(std::uint64_t count, bool atStart, bool atEnd);
    void add(Segment segment);

    std::vector<core::DiffHunk> m_hunks;
    std::uint64_t m_leftLines = 0;
    std::uint64_t m_rightLines = 0;
    bool m_onlyDifferences = false;
    std::set<std::uint64_t> m_unfolded; // runs of same lines shown in full, by their first left line
    std::vector<Segment> m_segments;
    std::vector<std::uint64_t> m_hunkRows;
    std::uint64_t m_rows = 0;
    std::uint64_t m_left = 0; // while building: the next line on each side
    std::uint64_t m_right = 0;
};

// The comparison as a unified diff (what `diff -u` prints), for the AI: at most `maxChars`
// characters, each line cut to `maxLine`, and `cut` set when something was left out.
QString unifiedDiff(const CompareSide& left, const CompareSide& right, const TextComparison& comparison,
                    qsizetype maxChars, bool* cut = nullptr, int context = 3, qsizetype maxLine = 300);

// Bytes as hex, "74 05 90"; at most `count` of them from `start`, as far as there are.
QString hexBytes(std::span<const std::uint8_t> bytes, std::uint64_t start, std::uint64_t count);

// Where an offset lies in an executable, in English: "in .text, address 0x140002630 (RVA 0x2630)";
// empty outside the sections, or for a file that is no executable.
QString describeOffset(const std::optional<core::BinaryLayout>& layout, std::uint64_t offset);

// Two binaries compared, for the AI, in English: sizes and formats, then for the first
// `maxRanges` differences their offsets, the bytes on each side, where they lie, and in code the
// instructions around them on each side as Capstone decodes them here, so that the AI explains
// rather than decodes. At most `maxChars` characters; `cut` set when something was left out.
QString binaryReport(const CompareSide& left, const CompareSide& right, const core::ByteComparison& comparison,
                     const std::optional<core::BinaryLayout>& leftLayout,
                     const std::optional<core::BinaryLayout>& rightLayout, bool* cut = nullptr, int maxRanges = 20,
                     qsizetype maxChars = 24'000);

} // namespace aiedit::ui
