#include "ui/Compare.h"

#include "core/FileSource.h"
#include "ui/Paths.h"
#include "ui/TextDecoder.h"

#include <QFileInfo>
#include <QLocale>
#include <QStringDecoder>

#include <algorithm>
#include <cstring>

namespace aiedit::ui {

namespace {

constexpr std::uint64_t kIndexStep = 8ull * 1024 * 1024; // indexed between checks for cancelling
constexpr std::size_t kRowBatch = 4096;
constexpr std::uint64_t kSeed = 14695981039346656037ull;

std::uint64_t hashText(const QString& text, std::uint64_t seed)
{
    return core::hashBytes({reinterpret_cast<const std::uint8_t*>(text.constData()),
                            static_cast<std::size_t>(text.size()) * sizeof(QChar)},
                           seed);
}

} // namespace

std::optional<CompareSide> fileSide(const QString& path, std::string_view legacyCodec, QString* error)
{
    std::error_code ec;
    std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec);
    if (!source) {
        if (error)
            *error = QString::fromLocal8Bit(ec.message());
        return std::nullopt;
    }
    const QFileInfo info(path);
    CompareSide side;
    side.label = info.fileName();
    side.path = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    side.type = core::detectFileType(source->bytes());
    side.codec = &core::defaultCodec(side.type, source->bytes(), legacyCodec);
    side.source = std::move(source);
    side.reload = [file = side.path]() -> std::shared_ptr<const core::FileSource> {
        std::error_code e;
        return core::FileSource::open(toPath(file), e);
    };
    return side;
}

CompareSide bytesSide(const QString& label, std::shared_ptr<const core::FileSource> source,
                      const core::TextCodec& codec)
{
    CompareSide side;
    side.label = label;
    side.type = core::detectFileType(source->bytes());
    side.codec = &codec;
    side.source = std::move(source);
    return side;
}

bool needsDecoding(const CompareSide& left, const CompareSide& right, const CompareOptions& options)
{
    return options.ignoreCase || options.ignoreWhitespace || left.codec->name != right.codec->name;
}

std::optional<LineKeys> lineKeys(const CompareSide& side, const CompareOptions& options, bool decode,
                                 const std::function<bool()>& cancelled)
{
    const auto stopped = [&cancelled] { return cancelled && cancelled(); };
    const std::span<const std::uint8_t> bytes = side.source->bytes();
    const core::TextCodec& codec = *side.codec;
    const core::LineEndings endings =
        core::countLineEndings(bytes.first(std::min<std::size_t>(bytes.size(), core::kSniffSize)), codec.scheme);
    auto index = std::make_shared<core::LineIndex>(bytes, codec.scheme, endings.lineBreak(),
                                                   core::bomLength(bytes, codec.scheme));
    while (!index->buildSome(kIndexStep))
        if (stopped())
            return std::nullopt;

    // Qt's decoder is much the faster; without ICU (Windows) it lacks the CJK codecs, which the
    // text view's own decoder has.
    QStringDecoder fast(codec.name.data(), QStringConverter::Flag::Stateless);
    std::optional<TextDecoder> slow;
    if (decode && !fast.isValid())
        slow.emplace(codec);
    const auto text = [&](std::span<const std::uint8_t> part) {
        if (slow)
            return slow->decodePlain(part);
        return QString(fast.decode(QByteArrayView(reinterpret_cast<const char*>(part.data()),
                                                  static_cast<qsizetype>(part.size()))));
    };

    LineKeys result;
    result.index = index;
    result.keys.reserve(static_cast<std::size_t>(index->lineCount()));
    std::uint64_t hash = kSeed;
    bool inLine = false;
    const std::uint64_t rows = index->rowCount();
    for (std::uint64_t first = 0; first < rows; first += kRowBatch) {
        if (stopped())
            return std::nullopt;
        for (const core::Row& row : index->rows(first, kRowBatch)) {
            if (!row.continuation) {
                if (inLine)
                    result.keys.push_back(hash);
                hash = kSeed;
                inLine = true;
            }
            const auto content = bytes.subspan(static_cast<std::size_t>(row.start),
                                               static_cast<std::size_t>(row.contentEnd - row.start));
            if (decode) {
                QString line = text(content);
                if (options.ignoreCase)
                    line = line.toCaseFolded();
                if (options.ignoreWhitespace)
                    line.removeIf([](QChar c) { return c.isSpace(); });
                hash = hashText(line, hash);
            } else {
                hash = core::hashBytes(content, hash);
            }
            if (!options.ignoreLineEndings && row.end > row.contentEnd) {
                // The line break, after a mark that keeps it apart from the text.
                const auto terminator = bytes.subspan(static_cast<std::size_t>(row.contentEnd),
                                                      static_cast<std::size_t>(row.end - row.contentEnd));
                hash = core::hashBytes({reinterpret_cast<const std::uint8_t*>("\xFF"), 1}, hash);
                hash = decode ? hashText(text(terminator), hash) : core::hashBytes(terminator, hash);
            }
        }
    }
    if (inLine)
        result.keys.push_back(hash);
    return result;
}

TextComparison compareText(const CompareSide& left, const CompareSide& right, const CompareOptions& options,
                           const std::function<bool()>& cancelled)
{
    TextComparison comparison;
    const auto leftBytes = left.source->bytes();
    const auto rightBytes = right.source->bytes();
    comparison.identicalBytes = leftBytes.size() == rightBytes.size() &&
                                (leftBytes.empty() || std::memcmp(leftBytes.data(), rightBytes.data(), leftBytes.size()) == 0);
    const bool decode = needsDecoding(left, right, options);
    std::optional<LineKeys> leftKeys = lineKeys(left, options, decode, cancelled);
    std::optional<LineKeys> rightKeys = leftKeys ? lineKeys(right, options, decode, cancelled) : std::nullopt;
    if (!leftKeys || !rightKeys) {
        comparison.cancelled = true;
        return comparison;
    }
    comparison.left = leftKeys->index;
    comparison.right = rightKeys->index;
    if (!comparison.identicalBytes)
        comparison.hunks = core::diffSequences(leftKeys->keys, rightKeys->keys, cancelled);
    comparison.cancelled = cancelled && cancelled();
    return comparison;
}

QString shownLine(std::span<const std::uint8_t> bytes, const core::LineIndex& index, const TextDecoder& decoder,
                  std::uint64_t line, bool* cut)
{
    if (cut)
        *cut = false;
    const std::optional<std::uint64_t> rowIndex = index.rowIndexOfLine(line);
    if (!rowIndex)
        return {};
    const core::Row row = index.row(*rowIndex);
    if (cut && *rowIndex + 1 < index.rowCount())
        *cut = index.row(*rowIndex + 1).continuation;
    return decoder
        .decode(bytes.subspan(static_cast<std::size_t>(row.start), static_cast<std::size_t>(row.contentEnd - row.start)))
        .text;
}

// CompareRows --------------------------------------------------------------------------------------

void CompareRows::build(const std::vector<core::DiffHunk>& hunks, std::uint64_t leftLines, std::uint64_t rightLines,
                        bool onlyDifferences)
{
    m_hunks = hunks;
    m_leftLines = leftLines;
    m_rightLines = rightLines;
    m_onlyDifferences = onlyDifferences;
    m_unfolded.clear();
    rebuild();
}

bool CompareRows::unfold(std::uint64_t row)
{
    if (row >= m_rows)
        return false;
    auto segment = std::upper_bound(m_segments.begin(), m_segments.end(), row,
                                    [](std::uint64_t r, const Segment& s) { return r < s.firstRow; });
    --segment;
    if (segment->kind != Kind::Fold)
        return false;
    m_unfolded.insert(segment->region);
    rebuild();
    return true;
}

void CompareRows::rebuild()
{
    m_segments.clear();
    m_hunkRows.clear();
    m_rows = 0;
    m_left = 0;
    m_right = 0;
    for (std::size_t h = 0; h < m_hunks.size(); ++h) {
        const core::DiffHunk& hunk = m_hunks[h];
        addSame(hunk.leftStart - m_left, h == 0, false);
        m_hunkRows.push_back(m_rows);
        add({Kind::Changed, 0, std::max(hunk.leftCount, hunk.rightCount), hunk.leftStart, hunk.rightStart,
             hunk.leftCount, hunk.rightCount, static_cast<int>(h), 0});
        m_left = hunk.leftStart + hunk.leftCount;
        m_right = hunk.rightStart + hunk.rightCount;
    }
    addSame(m_leftLines - std::min(m_left, m_leftLines), m_hunks.empty(), true);
}

void CompareRows::addSame(std::uint64_t count, bool atStart, bool atEnd)
{
    if (count == 0)
        return;
    const std::uint64_t region = m_left;
    // Around a change, a few same lines stay; at the start and the end of the files, none need to.
    const std::uint64_t head = atStart ? 0 : kContext;
    const std::uint64_t tail = atEnd ? 0 : kContext;
    if (!m_onlyDifferences || m_unfolded.contains(region) || count <= head + tail + 1) {
        add({Kind::Same, 0, count, m_left, m_right, count, count, -1, 0});
        m_left += count;
        m_right += count;
        return;
    }
    if (head > 0)
        add({Kind::Same, 0, head, m_left, m_right, head, head, -1, 0});
    const std::uint64_t folded = count - head - tail;
    add({Kind::Fold, 0, 1, m_left + head, m_right + head, folded, folded, -1, region});
    if (tail > 0)
        add({Kind::Same, 0, tail, m_left + head + folded, m_right + head + folded, tail, tail, -1, 0});
    m_left += count;
    m_right += count;
}

void CompareRows::add(Segment segment)
{
    segment.firstRow = m_rows;
    m_rows += segment.rows;
    m_segments.push_back(segment);
}

CompareRows::Row CompareRows::at(std::uint64_t row) const
{
    Row result;
    if (row >= m_rows)
        return result;
    const auto segment = std::upper_bound(m_segments.begin(), m_segments.end(), row,
                                          [](std::uint64_t r, const Segment& s) { return r < s.firstRow; }) -
                         1;
    const std::uint64_t k = row - segment->firstRow;
    switch (segment->kind) {
    case Kind::Same:
        result.left = segment->left + k;
        result.right = segment->right + k;
        break;
    case Kind::Fold:
        result.kind = Kind::Fold;
        result.left = segment->left;
        result.right = segment->right;
        result.folded = segment->leftCount;
        break;
    default:
        result.hunk = segment->hunk;
        if (k < segment->leftCount)
            result.left = segment->left + k;
        if (k < segment->rightCount)
            result.right = segment->right + k;
        result.kind = result.left && result.right ? Kind::Changed : result.left ? Kind::Removed : Kind::Added;
        break;
    }
    return result;
}

int CompareRows::hunkAtOrAfter(std::uint64_t row) const
{
    // Hunks lie in order and do not overlap: the one starting last at or before the row may hold
    // it; otherwise the next one is after it.
    const auto after = std::upper_bound(m_hunkRows.begin(), m_hunkRows.end(), row);
    if (after != m_hunkRows.begin()) {
        const auto h = static_cast<std::size_t>(after - m_hunkRows.begin()) - 1;
        if (row < m_hunkRows[h] + std::max(m_hunks[h].leftCount, m_hunks[h].rightCount))
            return static_cast<int>(h);
    }
    return after == m_hunkRows.end() ? -1 : static_cast<int>(after - m_hunkRows.begin());
}

int CompareRows::hunkBefore(std::uint64_t row) const
{
    const auto before = std::lower_bound(m_hunkRows.begin(), m_hunkRows.end(), row);
    return static_cast<int>(before - m_hunkRows.begin()) - 1;
}

std::uint64_t CompareRows::rowOfLine(bool left, std::uint64_t line) const
{
    if (m_segments.empty())
        return 0;
    // Segments go down both sides in order; the last one starting at or before the line has it.
    const auto start = [left](const Segment& s) { return left ? s.left : s.right; };
    auto segment = std::upper_bound(m_segments.begin(), m_segments.end(), line,
                                    [&start](std::uint64_t l, const Segment& s) { return l < start(s); });
    if (segment != m_segments.begin())
        --segment;
    const std::uint64_t count = left ? segment->leftCount : segment->rightCount;
    if (segment->kind == Kind::Fold)
        return segment->firstRow;
    const std::uint64_t k = std::min(line - std::min(line, start(*segment)), count == 0 ? 0 : count - 1);
    return segment->firstRow + std::min(k, segment->rows - 1);
}

// Unified diff -------------------------------------------------------------------------------------

QString unifiedDiff(const CompareSide& left, const CompareSide& right, const TextComparison& comparison,
                    qsizetype maxChars, bool* cut, int context, qsizetype maxLine)
{
    if (cut)
        *cut = false;
    QString out = QStringLiteral("--- %1\n+++ %2\n").arg(left.label, right.label);
    if (!comparison.left || !comparison.right)
        return out;
    const TextDecoder leftDecoder(*left.codec);
    const TextDecoder rightDecoder(*right.codec);
    const auto leftBytes = left.source->bytes();
    const auto rightBytes = right.source->bytes();
    const auto line = [&](bool onLeft, std::uint64_t n) {
        bool longLine = false;
        QString text = onLeft ? shownLine(leftBytes, *comparison.left, leftDecoder, n, &longLine)
                              : shownLine(rightBytes, *comparison.right, rightDecoder, n, &longLine);
        if (longLine || text.size() > maxLine)
            text = text.left(maxLine) + QStringLiteral(" …");
        return text;
    };
    const auto& hunks = comparison.hunks;
    const auto span = static_cast<std::uint64_t>(context);
    std::size_t h = 0;
    while (h < hunks.size()) {
        // Hunks closer than twice the context go into one block.
        std::size_t last = h;
        while (last + 1 < hunks.size() &&
               hunks[last + 1].leftStart - (hunks[last].leftStart + hunks[last].leftCount) <= 2 * span)
            ++last;
        const std::uint64_t leftFrom = hunks[h].leftStart - std::min(hunks[h].leftStart, span);
        const std::uint64_t rightFrom = hunks[h].rightStart - (hunks[h].leftStart - leftFrom);
        const std::uint64_t leftTo = std::min(hunks[last].leftStart + hunks[last].leftCount + span,
                                              comparison.left->lineCount());
        const std::uint64_t rightTo = hunks[last].rightStart + hunks[last].rightCount +
                                      (leftTo - (hunks[last].leftStart + hunks[last].leftCount));
        out += QStringLiteral("@@ -%1,%2 +%3,%4 @@\n")
                   .arg(leftFrom + 1)
                   .arg(leftTo - leftFrom)
                   .arg(rightFrom + 1)
                   .arg(rightTo - rightFrom);
        std::uint64_t l = leftFrom;
        for (std::size_t k = h; k <= last; ++k) {
            const core::DiffHunk& hunk = hunks[k];
            for (; l < hunk.leftStart; ++l)
                out += QLatin1Char(' ') + line(true, l) + QLatin1Char('\n');
            for (std::uint64_t i = 0; i < hunk.leftCount; ++i)
                out += QLatin1Char('-') + line(true, hunk.leftStart + i) + QLatin1Char('\n');
            for (std::uint64_t i = 0; i < hunk.rightCount; ++i)
                out += QLatin1Char('+') + line(false, hunk.rightStart + i) + QLatin1Char('\n');
            l = hunk.leftStart + hunk.leftCount;
        }
        for (; l < leftTo; ++l)
            out += QLatin1Char(' ') + line(true, l) + QLatin1Char('\n');
        if (out.size() > maxChars) {
            out.truncate(maxChars);
            if (cut)
                *cut = true;
            break;
        }
        h = last + 1;
    }
    return out;
}

// Binary reports ----------------------------------------------------------------------------------

QString hexBytes(std::span<const std::uint8_t> bytes, std::uint64_t start, std::uint64_t count)
{
    QString out;
    for (std::uint64_t i = start; i < bytes.size() && i < start + count; ++i) {
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += QStringLiteral("%1").arg(bytes[static_cast<std::size_t>(i)], 2, 16, QLatin1Char('0')).toUpper();
    }
    return out;
}

QString describeOffset(const std::optional<core::BinaryLayout>& layout, std::uint64_t offset)
{
    if (!layout)
        return {};
    const core::Section* section = layout->sectionAt(offset);
    if (!section)
        return {};
    const std::uint64_t address = section->address + (offset - section->fileOffset);
    QString where = QStringLiteral("in %1, address 0x%2")
                        .arg(QString::fromStdString(section->name).isEmpty() ? QStringLiteral("a section")
                                                                              : QString::fromStdString(section->name))
                        .arg(address, 0, 16);
    if (layout->format.starts_with("PE"))
        where += QStringLiteral(" (RVA 0x%1)").arg(address - layout->imageBase, 0, 16);
    if (section->executable)
        where += QStringLiteral(", code");
    return where;
}

QString binaryReport(const CompareSide& left, const CompareSide& right, const core::ByteComparison& comparison,
                     const std::optional<core::BinaryLayout>& leftLayout,
                     const std::optional<core::BinaryLayout>& rightLayout, bool* cut, int maxRanges,
                     qsizetype maxChars)
{
    if (cut)
        *cut = false;
    const QLocale c = QLocale::c();
    const auto describe = [&c](const CompareSide& side, std::uint64_t size, const std::optional<core::BinaryLayout>& layout) {
        QString text = QStringLiteral("%1: %2 bytes").arg(side.label, c.toString(static_cast<qulonglong>(size)));
        if (layout)
            text += QStringLiteral(", %1 %2").arg(QString::fromStdString(layout->format),
                                                  QString::fromLatin1(core::architectureName(layout->architecture)));
        return text;
    };
    QStringList out;
    out << QStringLiteral("Two binary files compared byte by byte, at the same offsets.");
    out << QStringLiteral("Left %1.").arg(describe(left, comparison.leftSize, leftLayout));
    out << QStringLiteral("Right %1.").arg(describe(right, comparison.rightSize, rightLayout));
    out << QStringLiteral("%1 ranges differ, %2 bytes in all%3.")
               .arg(c.toString(static_cast<qulonglong>(comparison.ranges.size())),
                    c.toString(static_cast<qulonglong>(comparison.differentBytes)),
                    comparison.limited ? QStringLiteral(" (more ranges than were listed)") : QString());
    if (comparison.shiftAt)
        out << QStringLiteral("From offset 0x%1 on, the right file looks shifted by %2 bytes (%3), so comparing at the "
                              "same offsets means little after it.")
                   .arg(*comparison.shiftAt, 0, 16)
                   .arg(comparison.shift)
                   .arg(comparison.shift > 0 ? QStringLiteral("bytes inserted") : QStringLiteral("bytes removed"));
    if (!core::canDisassemble(leftLayout ? leftLayout->architecture : core::Architecture::Unknown))
        out << QStringLiteral("(No disassembly here: the files are no known executables, or Capstone is missing.)");

    const auto leftBytes = left.source->bytes();
    const auto rightBytes = right.source->bytes();
    const auto code = [](const std::vector<core::Instruction>& instructions, const core::ByteRange& range) {
        QStringList lines;
        for (const core::Instruction& instruction : instructions) {
            const bool changed = instruction.offset < range.end() && instruction.offset + instruction.bytes.size() > range.start;
            QString bytes;
            for (const std::uint8_t byte : instruction.bytes)
                bytes += QStringLiteral("%1 ").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
            lines << QStringLiteral("    0x%1  %2 %3%4")
                         .arg(instruction.address, 0, 16)
                         .arg(bytes.leftJustified(24))
                         .arg(QString::fromStdString(instruction.text), changed ? QStringLiteral("   <-- differs") : QString());
        }
        return lines;
    };
    const int shown = std::min(maxRanges, static_cast<int>(comparison.ranges.size()));
    for (int i = 0; i < shown; ++i) {
        const core::ByteRange& range = comparison.ranges[static_cast<std::size_t>(i)];
        const std::uint64_t count = std::min<std::uint64_t>(range.length, 32);
        out << QString();
        out << QStringLiteral("Difference %1: offset 0x%2-0x%3 (%4 bytes)")
                   .arg(i + 1)
                   .arg(range.start, 0, 16)
                   .arg(range.end() - 1, 0, 16)
                   .arg(c.toString(static_cast<qulonglong>(range.length)));
        if (const QString where = describeOffset(leftLayout, range.start); !where.isEmpty())
            out << QStringLiteral("  %1").arg(where);
        out << QStringLiteral("  left : %1").arg(range.start < leftBytes.size() ? hexBytes(leftBytes, range.start, count)
                                                                                : QStringLiteral("(past its end)"));
        out << QStringLiteral("  right: %1").arg(range.start < rightBytes.size() ? hexBytes(rightBytes, range.start, count)
                                                                                 : QStringLiteral("(past its end)"));
        if (leftLayout && range.start < leftBytes.size()) {
            const auto instructions = core::disassembleAround(leftBytes, *leftLayout, range.start, 3, 3);
            if (!instructions.empty())
                out << QStringLiteral("  left code:") << code(instructions, range);
        }
        if (rightLayout && range.start < rightBytes.size()) {
            const auto instructions = core::disassembleAround(rightBytes, *rightLayout, range.start, 3, 3);
            if (!instructions.empty())
                out << QStringLiteral("  right code:") << code(instructions, range);
        }
        if (out.join(QLatin1Char('\n')).size() > maxChars)
            break;
    }
    if (shown < static_cast<int>(comparison.ranges.size()))
        out << QString() << QStringLiteral("(%1 more differences not listed.)").arg(comparison.ranges.size() - static_cast<std::size_t>(shown));
    QString report = out.join(QLatin1Char('\n'));
    if (report.size() > maxChars) {
        report.truncate(maxChars);
        if (cut)
            *cut = true;
    }
    return report;
}

} // namespace aiedit::ui
