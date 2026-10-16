#include "ui/TextView.h"

#include "core/TextCodec.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDocument.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QTextBoundaryFinder>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aiedit::ui {

namespace {

// Reading mode: the width of the text, in CJK characters, and the space a line takes.
constexpr qreal kReadingColumn = 40;
constexpr qreal kReadingLineSpacing = 1.7;
constexpr std::size_t kMaxCachedLayouts = 1024;
constexpr std::uint64_t kMaxCachedLayoutBytes = 4ull * 1024 * 1024;
constexpr qreal kUnboundedLineWidth = 1e6; // rows never wrap
// With word wrap on, a block holds at most this many rows (64 KiB). Every kMaxBlockRows-th row
// starts a new block, so the block holding a row is found from at most that many rows back.
constexpr std::uint64_t kMaxBlockRows = 16;
constexpr int kAutoScrollIntervalMs = 40;
constexpr int kWheelStep = 120; // QWheelEvent::angleDelta() units per notch
constexpr int kCaretWidth = 2;
// The column of a caret deep inside a split overlong line needs every row before it decoded;
// past this many rows the status bar does without.
constexpr std::uint64_t kMaxColumnScanRows = 64;
// Highlighting needs the state left by every line before. Files up to this size are highlighted
// exactly from the top, remembering the state every kStateCheckpointRows rows; bigger ones start
// kHighlightLookbackRows above the row, which is right except inside very long comments.
constexpr std::uint64_t kExactHighlightBytes = 4ull * 1024 * 1024;
constexpr std::uint64_t kStateCheckpointRows = 64;
constexpr std::uint64_t kHighlightLookbackRows = 256;
constexpr std::size_t kMaxCachedStates = 8192;

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

std::uint64_t countCharacters(const QString& text, qsizetype end)
{
    return static_cast<std::uint64_t>(
        std::count_if(text.begin(), text.begin() + end, [](QChar c) { return !c.isLowSurrogate(); }));
}

// Whether a key press types text (as QPlainTextEdit decides it).
bool isTypedText(const QKeyEvent* event)
{
    const QString text = event->text();
    if (text.isEmpty())
        return false;
    const QChar c = text.at(0);
    // Formatting characters such as ZWNJ and ZWJ are typed with Ctrl+Shift on some layouts.
    if (c.category() == QChar::Other_Format)
        return true;
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers == Qt::ControlModifier || modifiers == (Qt::ControlModifier | Qt::ShiftModifier))
        return false;
    if (c.isPrint() || c.category() == QChar::Other_PrivateUse)
        return true;
    return c.isHighSurrogate() && text.size() > 1 && text.at(1).isLowSurrogate();
}

// The line on which a cursor position shows; the last line for a position past every line.
QTextLine lineAtPosition(const QTextLayout& layout, int position)
{
    const QTextLine line = layout.lineForTextPosition(position);
    return line.isValid() ? line : layout.lineAt(layout.lineCount() - 1);
}

} // namespace

TextView::TextView(QWidget* parent) : QAbstractScrollArea(parent), m_rows(verticalScrollBar())
{
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    updateMetrics();
    updateColors();
    updateScrollBars();
    connect(verticalScrollBar(), &QAbstractSlider::actionTriggered, this, &TextView::scrollBarAction);
}

TextView::~TextView() = default;

void TextView::setDocument(TextDocument* document)
{
    if (document == m_document)
        return;
    if (m_document)
        disconnect(m_document, nullptr, this, nullptr);
    m_document = document;
    if (m_document) {
        connect(m_document, &TextDocument::indexGrew, this, &TextView::indexGrew);
        connect(m_document, &TextDocument::indexFinished, this, &TextView::indexGrew);
        connect(m_document, &TextDocument::codecChanged, this, &TextView::resetForDocument);
        connect(m_document, &TextDocument::sourceChanged, this, &TextView::resetForDocument);
        connect(m_document, &TextDocument::edited, this,
                [this](std::uint64_t offset, std::uint64_t removed, std::uint64_t inserted) {
                    // Bookmarks after the change move with the text; one whose line start went
                    // goes to the line that took it in.
                    for (std::uint64_t& bookmark : m_bookmarks) {
                        if (bookmark > offset)
                            bookmark = bookmark < offset + removed ? offset : bookmark - removed + inserted;
                    }
                    tidyBookmarks();
                    documentEdited(offset, removed);
                });
        connect(m_document, &TextDocument::indexFinished, this, &TextView::tidyBookmarks);
        connect(m_document, &QObject::destroyed, this, [this] {
            m_document = nullptr;
            resetForDocument();
        });
    }
    m_rows.setTopRow(0);
    m_topOffset = 0;
    m_cursor = m_anchor = 0;
    m_bookmarks.clear();
    resetForDocument();
}

void TextView::resetForDocument()
{
    tidyBookmarks(); // the file may have changed under them
    resetHighlighting();
    m_maxRowWidth = 0;
    m_desiredX.reset();
    updateScrollBars();
    const std::uint64_t cursor = m_document ? m_document->snapToCharStart(m_cursor) : 0;
    const std::uint64_t anchor = m_document ? m_document->snapToCharStart(m_anchor) : 0;
    m_cursor = m_anchor = ~std::uint64_t{0}; // force the change signals
    setCursorAndAnchor(cursor, anchor);
}

void TextView::indexGrew()
{
    updateScrollBars();
    viewport()->update();
}

void TextView::setSyntax(const QString& name)
{
    if (name == m_syntax)
        return;
    m_syntax = name;
    resetHighlighting();
    updateScrollBars(); // bold or italic text can wrap differently
    viewport()->update();
}

QList<QTextLayout::FormatRange> TextView::rowFormats(std::uint64_t rowIndex) const
{
    if (rowIndex >= rowCount())
        return {};
    return layoutFor(blockAt(rowIndex)).layout.formats();
}

void TextView::resetHighlighting()
{
    m_highlighter = std::make_unique<SyntaxHighlighter>(m_syntax, palette());
    if (!m_highlighter->isActive())
        m_highlighter.reset();
    m_stateCheckpoints.assign(1, SyntaxState());
    m_stateAfterRow.clear();
    clearLayouts();
}

SyntaxState TextView::stateBefore(std::uint64_t rowIndex) const
{
    if (rowIndex == 0)
        return {};
    if (const auto it = m_stateAfterRow.find(rowIndex - 1); it != m_stateAfterRow.end())
        return it->second;

    const core::LineIndex& index = m_document->index();
    const auto highlightRows = [&](std::uint64_t first, std::uint64_t count, SyntaxState& state) {
        for (const core::Row& row : index.rows(first, static_cast<std::size_t>(count))) {
            const auto bytes = m_document->bytes().subspan(row.start, static_cast<std::size_t>(row.contentEnd - row.start));
            m_highlighter->highlightLine(m_document->decoder().decode(bytes).text, state);
        }
    };

    std::uint64_t from = 0;
    SyntaxState state;
    if (m_document->bytes().size() <= kExactHighlightBytes) {
        const std::uint64_t checkpoint = rowIndex / kStateCheckpointRows;
        while (m_stateCheckpoints.size() <= checkpoint) {
            SyntaxState next = m_stateCheckpoints.back();
            highlightRows((m_stateCheckpoints.size() - 1) * kStateCheckpointRows, kStateCheckpointRows, next);
            m_stateCheckpoints.push_back(next);
        }
        from = checkpoint * kStateCheckpointRows;
        state = m_stateCheckpoints[static_cast<std::size_t>(checkpoint)];
    } else {
        from = rowIndex > kHighlightLookbackRows ? rowIndex - kHighlightLookbackRows : 0;
    }
    highlightRows(from, rowIndex - from, state);
    return state;
}

void TextView::setTabWidth(int spaces)
{
    spaces = std::clamp(spaces, 1, 16);
    if (spaces == m_tabWidth)
        return;
    m_tabWidth = spaces;
    clearLayouts();
    updateScrollBars();
    viewport()->update();
}

void TextView::setWordWrap(bool on)
{
    m_wrapWanted = on;
    applyWordWrap(on || m_readingMode);
}

void TextView::setReadingMode(bool on)
{
    if (on == m_readingMode)
        return;
    const std::uint64_t top = rowCount() == 0 ? 0 : lineStart(topLine());
    m_readingMode = on;
    updateMetrics(); // the line height
    applyWordWrap(m_wrapWanted || on);
    m_topOffset = top;
    m_wrapWidth = wrapWidth();
    clearLayouts();
    updateScrollBars();
    if (!m_wordWrap && rowCount() > 0)
        setTopRow(m_document->index().rowIndexAt(top));
    viewport()->update();
}

void TextView::applyWordWrap(bool on)
{
    if (on == m_wordWrap)
        return;
    // Keep the same text at the top, and the caret in view if it was.
    const bool caretShown = !cursorRect().isNull();
    const std::uint64_t top = rowCount() == 0 ? 0 : lineStart(topLine());
    m_wordWrap = on;
    m_caretAtLineEnd = false;
    m_topOffset = top;
    m_wrapWidth = wrapWidth();
    m_maxRowWidth = 0;
    m_desiredX.reset();
    clearLayouts();
    setHorizontalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    updateScrollBars();
    if (!on && rowCount() > 0)
        setTopRow(m_document->index().rowIndexAt(top));
    if (caretShown)
        ensureCursorVisible();
    viewport()->update();
}

void TextView::setReadOnly(bool readOnly, const QString& reason)
{
    m_readOnly = readOnly;
    m_readOnlyReason = reason;
    m_preedit.clear();
    setAttribute(Qt::WA_InputMethodEnabled, !readOnly);
    viewport()->update();
}

std::uint64_t TextView::selectionStart() const
{
    return std::min(m_cursor, m_anchor);
}

std::uint64_t TextView::selectionEnd() const
{
    return std::max(m_cursor, m_anchor);
}

void TextView::setCursorOffset(std::uint64_t offset, bool extendSelection)
{
    if (m_document)
        moveCursor(m_document->snapToCharStart(offset), extendSelection);
}

void TextView::select(std::uint64_t start, std::uint64_t end)
{
    if (!m_document)
        return;
    setCursorAndAnchor(m_document->snapToCharStart(end), m_document->snapToCharStart(start));
    ensureCursorVisible();
}

void TextView::selectAll()
{
    if (m_document)
        setCursorAndAnchor(documentEnd(), m_document->textStart());
}

bool TextView::goToLine(std::uint64_t line)
{
    if (!m_document)
        return false;
    const auto rowIndex = m_document->index().rowIndexOfLine(line);
    if (!rowIndex)
        return false;
    const std::uint64_t start = m_document->index().row(*rowIndex).start;
    scrollToLine(moveLines(visualLineAt(start), -(visibleRows() / 2)));
    moveCursor(start, false);
    return true;
}

void TextView::goToOffset(std::uint64_t offset)
{
    if (!m_document || rowCount() == 0)
        return;
    const std::uint64_t target = m_document->snapToCharStart(offset);
    const VisualLine line = visualLineAt(target);
    if (!linesFromTop(line, visibleRows() - 1))
        scrollToLine(moveLines(line, -(visibleRows() / 2)));
    moveCursor(target, false);
}

int TextView::visibleRows() const
{
    return std::max(1, viewport()->height() / m_lineHeight);
}

std::uint64_t TextView::rowCount() const
{
    return m_document ? m_document->index().rowCount() : 0;
}

std::uint64_t TextView::maxTopRow() const
{
    const std::uint64_t rows = rowCount();
    const auto visible = static_cast<std::uint64_t>(visibleRows());
    return rows > visible ? rows - visible : 0;
}

std::uint64_t TextView::documentEnd() const
{
    return m_document ? m_document->bytes().size() : 0;
}

void TextView::setTopRow(std::uint64_t row)
{
    if (m_rows.setTopRow(row))
        viewport()->update();
}

void TextView::scrollToLine(const VisualLine& line)
{
    if (!m_wordWrap) {
        setTopRow(line.block.firstRow);
        return;
    }
    const std::uint64_t offset = std::min(lineStart(line), m_maxTopOffset);
    if (offset == m_topOffset)
        return;
    m_topOffset = offset;
    m_rows.setTopRow(m_document->index().rowIndexAt(offset));
    viewport()->update();
}

std::uint64_t TextView::firstVisibleOffset() const
{
    return rowCount() == 0 ? 0 : lineStart(topLine());
}

void TextView::scrollToOffset(std::uint64_t offset)
{
    if (rowCount() > 0)
        scrollToLine(visualLineAt(std::min(offset, documentEnd())));
}

void TextView::scrollLines(std::int64_t delta)
{
    if (rowCount() == 0 || delta == 0)
        return;
    if (!m_wordWrap) {
        const std::uint64_t top = topRow();
        const auto steps = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
        setTopRow(delta < 0 ? (top > steps ? top - steps : 0) : top + steps);
        return;
    }
    scrollToLine(moveLines(topLine(), delta));
}

void TextView::scrollBarAction(int action)
{
    // The bar counts rows; with word wrap, its arrows and page steps should move by lines.
    if (!m_wordWrap)
        return;
    std::int64_t lines = 0;
    switch (action) {
    case QAbstractSlider::SliderSingleStepAdd:
        lines = 1;
        break;
    case QAbstractSlider::SliderSingleStepSub:
        lines = -1;
        break;
    case QAbstractSlider::SliderPageStepAdd:
        lines = visibleRows();
        break;
    case QAbstractSlider::SliderPageStepSub:
        lines = -visibleRows();
        break;
    default:
        return;
    }
    scrollLines(lines);
    verticalScrollBar()->setSliderPosition(verticalScrollBar()->value()); // cancels the bar's own step
}

void TextView::scrollContentsBy(int /*dx*/, int dy)
{
    if (dy != 0 && m_rows.syncFromBar() && m_wordWrap && rowCount() > 0) {
        // The bar was dragged: show the line in which the chosen row starts, and at the bottom,
        // the end of the file.
        const std::uint64_t row = m_rows.topRow();
        m_topOffset = row >= m_rows.maxTopRow()
                          ? m_maxTopOffset
                          : std::min(lineStart(visualLineAt(m_document->index().row(row).start)), m_maxTopOffset);
    }
    viewport()->update();
}

void TextView::updateScrollBars()
{
    if (!m_wordWrap) {
        m_rows.setRange(maxTopRow(), visibleRows());
        updateHorizontalRange();
        return;
    }
    if (const qreal width = wrapWidth(); width != m_wrapWidth) {
        m_wrapWidth = width;
        clearLayouts();
    }
    horizontalScrollBar()->setRange(0, 0);
    if (rowCount() == 0) {
        m_topOffset = m_maxTopOffset = 0;
        m_rows.setRange(0, visibleRows());
        return;
    }
    const core::LineIndex& index = m_document->index();
    m_maxTopOffset = computeMaxTopOffset();
    m_topOffset = std::min(lineStart(visualLineAt(m_topOffset)), m_maxTopOffset);
    m_rows.setRange(index.rowIndexAt(m_maxTopOffset), visibleRows());
    m_rows.setTopRow(index.rowIndexAt(m_topOffset));
}

std::uint64_t TextView::computeMaxTopOffset() const
{
    const Block last = blockAt(rowCount() - 1);
    // While indexing goes on the end keeps moving, and laying it out each time would be wasted:
    // until then the last block may scroll up to the top.
    if (!m_document->index().isComplete())
        return last.span.start;
    // Only where lines wrap matters, not their colours: highlighting the end would need the
    // highlighting state of every line before it, after each edit.
    const QScopedValueRollback measuring(m_measuring, true);
    return lineStart(moveLines({last, lineCount(last) - 1}, -(visibleRows() - 1)));
}

void TextView::updateHorizontalRange()
{
    m_maxRowWidthInRange = m_maxRowWidth;
    QScrollBar* horizontal = horizontalScrollBar();
    const int width = viewport()->width();
    const int content = static_cast<int>(std::ceil(textLeft() + m_maxRowWidth + 2 * m_charWidth));
    horizontal->setRange(0, std::max(0, content - width));
    horizontal->setPageStep(width);
    horizontal->setSingleStep(std::max(1, static_cast<int>(m_charWidth)));
}

// Layout ---------------------------------------------------------------------------------------

void TextView::updateMetrics()
{
    // Measure with a layout so that fallback fonts (CJK, emoji) are included in the line height.
    const QFontMetricsF metrics(font());
    QTextLayout probe(QStringLiteral("Ag中😀"), font());
    probe.beginLayout();
    const QTextLine line = probe.createLine();
    probe.endLayout();

    m_charWidth = metrics.horizontalAdvance(QLatin1Char('0'));
    m_lineHeight = static_cast<int>(std::ceil(std::max(metrics.lineSpacing(), line.height()))) + 2;
    m_ascent = static_cast<int>(std::round(std::max(metrics.ascent(), line.ascent()))) + 1;
    m_columnWidth = kReadingColumn * metrics.horizontalAdvance(QChar(0x4E2D)); // 中
    if (m_readingMode) {
        // Room between the lines of a book, the text in the middle of each line.
        const int extra = static_cast<int>(std::round(m_lineHeight * (kReadingLineSpacing - 1)));
        m_lineHeight += extra;
        m_ascent += extra / 2;
    }
}

void TextView::updateColors()
{
    const EditorColors colors = editorColors(palette());
    m_colors.base = colors.background;
    m_colors.text = colors.text;
    m_colors.dim = colors.lineNumber;
    m_colors.gutter = colors.gutter;
    m_colors.selection = colors.selection;
    m_colors.selectionText = colors.selectionText;
    m_colors.inactiveSelection = mix(m_colors.base, m_colors.selection, 0.35f);
    m_colors.inactiveSelectionText = m_colors.text;
    m_colors.currentLine = colors.currentLine;
    m_colors.bookmark = mix(m_colors.gutter, m_colors.selection, 0.55f);
}

int TextView::gutterWidth() const
{
    if (m_readingMode)
        return 0; // no line numbers in a book
    std::uint64_t lines = m_document ? std::max<std::uint64_t>(1, m_document->index().lineCount()) : 1;
    int digits = 1;
    while (lines >= 10) {
        lines /= 10;
        ++digits;
    }
    return static_cast<int>(std::ceil((std::max(3, digits) + 2) * m_charWidth));
}

qreal TextView::textLeft() const
{
    if (m_readingMode) // the column in the middle
        return std::max(2 * m_charWidth, (viewport()->width() - m_columnWidth) / 2);
    return gutterWidth() + m_charWidth / 2;
}

qreal TextView::wrapWidth() const
{
    if (m_readingMode)
        return std::max(8 * m_charWidth, std::min(m_columnWidth, viewport()->width() - 4 * m_charWidth));
    // Leaves room for the caret after the last character.
    return std::max(8 * m_charWidth, viewport()->width() - textLeft() - m_charWidth / 2);
}

const TextView::BlockLayout& TextView::layoutFor(const Block& block) const
{
    const core::Row& span = block.span;
    const bool highlighted = !m_highlighter || !m_measuring;
    if (const auto it = m_layouts.find(span.start); it != m_layouts.end()) {
        if (it->second->span == span && (it->second->highlighted || !highlighted))
            return *it->second;
        // The block grew (the index got further), or it is to be shown now: lay it out again.
        m_cachedLayoutBytes -= it->second->span.contentEnd - it->second->span.start;
        m_layouts.erase(it);
    }
    const std::uint64_t size = span.contentEnd - span.start;
    if (m_layouts.size() >= kMaxCachedLayouts || m_cachedLayoutBytes + size > kMaxCachedLayoutBytes) {
        m_layouts.clear();
        m_cachedLayoutBytes = 0;
    }

    auto entry = std::make_unique<BlockLayout>();
    entry->span = span;
    entry->highlighted = highlighted;
    entry->decoded = m_document->decoder().decode(m_document->bytes().subspan(span.start, static_cast<std::size_t>(size)));
    if (m_highlighter && highlighted)
        entry->layout.setFormats(highlight(block, entry->decoded));

    QTextOption option;
    option.setWrapMode(m_wordWrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setTabStopDistance(m_tabWidth * m_charWidth);
    entry->layout.setText(entry->decoded.text);
    entry->layout.setFont(font());
    entry->layout.setTextOption(option);
    entry->layout.setCacheEnabled(true);
    entry->layout.beginLayout();
    // Lines go on a fixed pitch with aligned baselines, whatever fallback fonts they need.
    for (int i = 0;; ++i) {
        QTextLine line = entry->layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(m_wordWrap ? m_wrapWidth : kUnboundedLineWidth);
        line.setPosition(QPointF(0, i * m_lineHeight + m_ascent - line.ascent()));
        if (!m_wordWrap) {
            m_maxRowWidth = std::max(m_maxRowWidth, line.naturalTextWidth());
            break; // a row is one line
        }
    }
    entry->layout.endLayout();

    const BlockLayout& result = *entry;
    m_cachedLayoutBytes += size;
    m_layouts[span.start] = std::move(entry);
    return result;
}

QList<QTextLayout::FormatRange> TextView::highlight(const Block& block, const DecodedText& decoded) const
{
    // Row by row, as lines, also when word wrap lays several out together.
    const std::vector<core::Row> rows =
        block.rowCount == 1 ? std::vector<core::Row>{block.span}
                            : m_document->index().rows(block.firstRow, static_cast<std::size_t>(block.rowCount));
    QList<QTextLayout::FormatRange> formats;
    SyntaxState state = stateBefore(block.firstRow);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const qsizetype from = decoded.positionAt(static_cast<std::uint32_t>(rows[i].start - block.span.start));
        const qsizetype to = decoded.positionAt(static_cast<std::uint32_t>(rows[i].contentEnd - block.span.start));
        for (QTextLayout::FormatRange range : m_highlighter->highlightLine(decoded.text.mid(from, to - from), state)) {
            range.start += static_cast<int>(from);
            formats.append(range);
        }
        if (m_stateAfterRow.size() >= kMaxCachedStates)
            m_stateAfterRow.clear();
        m_stateAfterRow[block.firstRow + i] = state;
    }
    return formats;
}

void TextView::clearLayouts()
{
    m_layouts.clear();
    m_cachedLayoutBytes = 0;
}

// Blocks and lines on screen -----------------------------------------------------------------

TextView::Block TextView::blockAt(std::uint64_t rowIndex) const
{
    if (!m_wordWrap)
        return {m_document->index().row(rowIndex), rowIndex, 1};
    // A block starts at least every kMaxBlockRows rows, and blocks are the same from any start.
    const std::uint64_t first = rowIndex - rowIndex % kMaxBlockRows;
    for (const Block& block : blocksFrom(first, static_cast<std::size_t>(kMaxBlockRows))) {
        if (rowIndex < block.firstRow + block.rowCount)
            return block;
    }
    return {m_document->index().row(rowIndex), rowIndex, 1}; // not indexed (yet)
}

std::vector<TextView::Block> TextView::blocksFrom(std::uint64_t first, std::size_t count) const
{
    const core::LineIndex& index = m_document->index();
    // Whether a row goes into the same block as the one before it.
    const auto joins = [this](const core::Row& row, std::uint64_t rowIndex) {
        return m_wordWrap && row.continuation && rowIndex % kMaxBlockRows != 0;
    };

    std::vector<Block> blocks;
    while (blocks.size() < count) {
        // Enough rows for the blocks still wanted, if each is one row, plus the longest block.
        const std::size_t wanted = count - blocks.size() + (m_wordWrap ? static_cast<std::size_t>(kMaxBlockRows) : 0);
        const std::vector<core::Row> rows = index.rows(first, wanted);
        if (rows.empty())
            break;
        const bool more = rows.size() == wanted; // rows may follow that were not read
        std::size_t i = 0;
        while (i < rows.size() && blocks.size() < count) {
            std::size_t j = i + 1;
            while (j < rows.size() && joins(rows[j], first + j))
                ++j;
            if (j == rows.size() && more && i > 0)
                break; // the block may go on past what was read: read again from its start
            Block block{rows[i], first + i, j - i};
            block.span.contentEnd = rows[j - 1].contentEnd;
            block.span.end = rows[j - 1].end;
            blocks.push_back(block);
            i = j;
        }
        first += i;
    }
    return blocks;
}

int TextView::lineCount(const Block& block) const
{
    return layoutFor(block).layout.lineCount();
}

TextView::VisualLine TextView::visualLineAt(std::uint64_t offset) const
{
    const Block block = blockAt(m_document->index().rowIndexAt(offset));
    const BlockLayout& layout = layoutFor(block);
    offset = std::clamp(offset, block.span.start, block.span.contentEnd);
    const auto position = static_cast<int>(layout.decoded.positionAt(static_cast<std::uint32_t>(offset - block.span.start)));
    return {block, lineAtPosition(layout.layout, position).lineNumber()};
}

TextView::VisualLine TextView::caretLine() const
{
    const VisualLine line = visualLineAt(m_cursor);
    if (!m_caretAtLineEnd || m_cursor != lineStart(line))
        return line;
    if (line.line > 0)
        return {line.block, line.line - 1};
    if (line.block.span.continuation && line.block.firstRow > 0) {
        const Block previous = blockAt(line.block.firstRow - 1);
        return {previous, lineCount(previous) - 1};
    }
    return line;
}

TextView::VisualLine TextView::topLine() const
{
    return m_wordWrap ? visualLineAt(m_topOffset) : VisualLine{blockAt(topRow()), 0};
}

TextView::VisualLine TextView::moveLines(VisualLine from, std::int64_t delta) const
{
    if (!m_wordWrap) {
        // One line per row.
        const std::uint64_t row = from.block.firstRow;
        const auto steps = static_cast<std::uint64_t>(delta < 0 ? -delta : delta);
        const std::uint64_t target = delta < 0 ? (row > steps ? row - steps : 0) : std::min(rowCount() - 1, row + steps);
        return target == row ? from : VisualLine{blockAt(target), 0};
    }
    while (delta > 0) {
        const std::int64_t last = lineCount(from.block) - 1;
        if (from.line + delta <= last) {
            from.line += static_cast<int>(delta);
            return from;
        }
        delta -= last - from.line + 1;
        const std::vector<Block> next = blocksFrom(from.block.firstRow + from.block.rowCount, 1);
        if (next.empty()) {
            from.line = static_cast<int>(last);
            return from;
        }
        from = {next.front(), 0};
    }
    while (delta < 0) {
        if (from.line + delta >= 0) {
            from.line += static_cast<int>(delta);
            return from;
        }
        delta += from.line + 1;
        if (from.block.firstRow == 0) {
            from.line = 0;
            return from;
        }
        const Block previous = blockAt(from.block.firstRow - 1);
        from = {previous, lineCount(previous) - 1};
    }
    return from;
}

std::optional<int> TextView::linesFromTop(const VisualLine& line, int limit) const
{
    const VisualLine top = topLine();
    if (!m_wordWrap) {
        const std::uint64_t row = line.block.firstRow;
        const std::uint64_t topRow = top.block.firstRow;
        if (row < topRow || row - topRow > static_cast<std::uint64_t>(limit))
            return std::nullopt;
        return static_cast<int>(row - topRow);
    }
    // Lines from the top line to the start of `block`.
    int lines = -top.line;
    Block block = top.block;
    while (block.firstRow < line.block.firstRow) {
        lines += lineCount(block);
        if (lines > limit)
            return std::nullopt;
        const std::vector<Block> next = blocksFrom(block.firstRow + block.rowCount, 1);
        if (next.empty())
            return std::nullopt;
        block = next.front();
    }
    if (block.firstRow != line.block.firstRow) // above the top
        return std::nullopt;
    lines += line.line;
    if (lines < 0 || lines > limit)
        return std::nullopt;
    return lines;
}

std::uint64_t TextView::lineStart(const VisualLine& line) const
{
    const BlockLayout& layout = layoutFor(line.block);
    return line.block.span.start + layout.decoded.byteAt(layout.layout.lineAt(line.line).textStart());
}

// Navigation ---------------------------------------------------------------------------------

std::uint64_t TextView::cursorRowIndex() const
{
    return rowCount() == 0 ? 0 : m_document->index().rowIndexAt(m_cursor);
}

bool TextView::isLastBlock(const Block& block) const
{
    return block.span.end >= documentEnd();
}

std::uint64_t TextView::lastCaretOffset(const Block& block) const
{
    const core::Row& span = block.span;
    const bool split = span.end == span.contentEnd && !isLastBlock(block);
    if (!split)
        return span.contentEnd;
    // The split point itself belongs to the next block, so stop before the last character.
    const BlockLayout& layout = layoutFor(block);
    const int last = layout.layout.previousCursorPosition(static_cast<int>(layout.decoded.text.size()));
    return span.start + layout.decoded.byteAt(last);
}

TextView::Caret TextView::lineEndCaret(const VisualLine& line) const
{
    const BlockLayout& layout = layoutFor(line.block);
    const core::Row& span = line.block.span;
    if (line.line + 1 >= layout.layout.lineCount()) {
        // The block's last line ends before the line terminator, or where a split block goes on.
        const bool split = span.end == span.contentEnd && !isLastBlock(line.block);
        return split ? Caret{span.end, true} : Caret{span.contentEnd};
    }
    // A wrapped line ends where the next one starts, or before the spaces it broke after (which
    // show nothing).
    const QTextLine text = layout.layout.lineAt(line.line);
    int end = text.textStart() + text.textLength();
    if (!layout.decoded.text.at(end - 1).isSpace())
        return {span.start + layout.decoded.byteAt(end), true};
    while (end > text.textStart() && layout.decoded.text.at(end - 1).isSpace())
        --end;
    return {span.start + layout.decoded.byteAt(end)};
}

TextView::Caret TextView::caretAtX(const VisualLine& line, qreal x) const
{
    const BlockLayout& layout = layoutFor(line.block);
    const QTextLine text = layout.layout.lineAt(line.line);
    x = std::max<qreal>(0, x);
    int position = text.xToCursor(x, QTextLine::CursorBetweenCharacters);
    const Caret end = lineEndCaret(line);
    // Past the end of a wrapped line, xToCursor() stops before the last character (it takes that
    // for a space to break at), so pick the nearer side of it here.
    const auto endPosition =
        static_cast<int>(layout.decoded.positionAt(static_cast<std::uint32_t>(end.offset - line.block.span.start)));
    if (position < endPosition && layout.layout.nextCursorPosition(position) >= endPosition &&
        std::abs(x - text.cursorToX(endPosition)) < std::abs(x - text.cursorToX(position)))
        position = endPosition;
    const std::uint64_t offset = line.block.span.start + layout.decoded.byteAt(position);
    return offset >= end.offset ? end : Caret{offset};
}

qreal TextView::xOnLine(const VisualLine& line, std::uint64_t offset) const
{
    const BlockLayout& layout = layoutFor(line.block);
    offset = std::clamp(offset, line.block.span.start, line.block.span.contentEnd);
    const auto position = layout.decoded.positionAt(static_cast<std::uint32_t>(offset - line.block.span.start));
    return layout.layout.lineAt(line.line).cursorToX(static_cast<int>(position));
}

std::uint64_t TextView::stepOffset(std::uint64_t offset, bool forward, QTextLayout::CursorMode mode) const
{
    const Block block = blockAt(m_document->index().rowIndexAt(offset));
    const core::Row& span = block.span;
    const BlockLayout& layout = layoutFor(block);
    const int size = static_cast<int>(layout.decoded.text.size());
    const int position = static_cast<int>(
        layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(offset, span.contentEnd) - span.start)));

    if (forward) {
        if (position < size)
            return span.start + layout.decoded.byteAt(layout.layout.nextCursorPosition(position, mode));
        return isLastBlock(block) ? std::max(offset, span.contentEnd) : span.end; // over the line terminator
    }
    if (position > 0)
        return span.start + layout.decoded.byteAt(layout.layout.previousCursorPosition(position, mode));
    return block.firstRow == 0 ? offset : lastCaretOffset(blockAt(block.firstRow - 1));
}

void TextView::moveVertically(std::int64_t lines, bool extendSelection)
{
    const VisualLine current = caretLine();
    if (!m_desiredX)
        m_desiredX = xOnLine(current, m_cursor);
    const VisualLine target = moveLines(current, lines);
    if (target == current) {
        // Already on the first or the last line: go to its start or end.
        moveCursor(lines < 0 ? Caret{lineStart(current)} : lineEndCaret(current), extendSelection, true);
        return;
    }
    moveCursor(caretAtX(target, *m_desiredX), extendSelection, true);
}

void TextView::moveCursor(std::uint64_t offset, bool extendSelection, bool keepDesiredX)
{
    moveCursor(Caret{offset}, extendSelection, keepDesiredX);
}

void TextView::moveCursor(const Caret& caret, bool extendSelection, bool keepDesiredX)
{
    if (!keepDesiredX)
        m_desiredX.reset();
    if (m_document)
        m_document->breakUndoMerge(); // typing somewhere else is a new step
    setCursorAndAnchor(caret.offset, extendSelection ? m_anchor : caret.offset);
    m_caretAtLineEnd = caret.atLineEnd;
    ensureCursorVisible();
}

void TextView::setCursorAndAnchor(std::uint64_t cursor, std::uint64_t anchor)
{
    m_caretAtLineEnd = false;
    const std::uint64_t oldCursor = m_cursor;
    const std::uint64_t oldStart = selectionStart();
    const std::uint64_t oldEnd = selectionEnd();
    m_cursor = cursor;
    m_anchor = anchor;
    if (m_cursor != oldCursor)
        emit cursorMoved(m_cursor);
    if (selectionStart() != oldStart || selectionEnd() != oldEnd)
        emit selectionChanged(selectionStart(), selectionEnd());
    viewport()->update();
}

void TextView::ensureCursorVisible()
{
    if (rowCount() == 0)
        return;
    const VisualLine caret = caretLine();
    const int visible = visibleRows();
    if (!linesFromTop(caret, visible - 1)) {
        // Above the view it goes on the top line, below it on the bottom one.
        const bool above = m_wordWrap ? lineStart(caret) < lineStart(topLine()) : caret.block.firstRow < topRow();
        scrollToLine(above ? caret : moveLines(caret, -(visible - 1)));
    }
    if (hasFocus() && !m_readOnly)
        QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
    if (m_wordWrap)
        return; // nothing to scroll sideways

    const qreal caretX = textLeft() + xOnLine(caret, m_cursor);
    if (m_maxRowWidth > m_maxRowWidthInRange)
        updateHorizontalRange();
    QScrollBar* horizontal = horizontalScrollBar();
    const qreal scrollX = horizontal->value();
    if (caretX - scrollX < textLeft())
        horizontal->setValue(static_cast<int>(std::floor(caretX - textLeft())));
    else if (caretX - scrollX > viewport()->width() - 2 * m_charWidth)
        horizontal->setValue(static_cast<int>(std::ceil(caretX - viewport()->width() + 2 * m_charWidth)));
}

std::uint64_t TextView::cursorLine() const
{
    return rowCount() == 0 ? 0 : m_document->index().row(cursorRowIndex()).line;
}

std::optional<std::uint64_t> TextView::cursorColumn() const
{
    if (rowCount() == 0)
        return 0;
    const core::LineIndex& index = m_document->index();
    const std::uint64_t rowIndex = cursorRowIndex();
    const Block block = blockAt(rowIndex);

    // The rows of the line before the caret's block, which are text up to the block's start.
    std::uint64_t column = 0;
    if (block.span.continuation) {
        const auto lineStart = index.rowIndexOfLine(block.span.line);
        if (!lineStart || rowIndex - *lineStart > kMaxColumnScanRows)
            return std::nullopt;
        const std::uint64_t start = index.row(*lineStart).start;
        const QString before = m_document->decoder()
                                   .decode(m_document->bytes().subspan(start, static_cast<std::size_t>(block.span.start - start)))
                                   .text;
        column = countCharacters(before, before.size());
    }
    const BlockLayout& layout = layoutFor(block);
    const auto position = layout.decoded.positionAt(
        static_cast<std::uint32_t>(std::min(m_cursor, block.span.contentEnd) - block.span.start));
    return column + countCharacters(layout.decoded.text, position);
}

QRect TextView::cursorRect() const
{
    if (!m_document)
        return {};
    if (rowCount() == 0) // empty: the caret stands at the start of the first line
        return QRect(static_cast<int>(std::floor(textLeft())), 0, kCaretWidth, m_lineHeight);
    const VisualLine caret = caretLine();
    const std::optional<int> line = linesFromTop(caret, visibleRows());
    if (!line)
        return {};
    const qreal scrollX = m_wordWrap ? 0 : horizontalScrollBar()->value();
    const qreal x = textLeft() - scrollX + xOnLine(caret, m_cursor);
    return QRect(static_cast<int>(std::floor(x)), *line * m_lineHeight, kCaretWidth, m_lineHeight);
}

QString TextView::selectedText() const
{
    if (!m_document || !hasSelection() || selectionEnd() - selectionStart() > kMaxCopyBytes)
        return {};
    const auto bytes = m_document->bytes().subspan(selectionStart(),
                                                   static_cast<std::size_t>(selectionEnd() - selectionStart()));
    return m_document->decoder().decodePlain(bytes);
}

void TextView::copy()
{
    if (!hasSelection())
        return;
    const std::uint64_t length = selectionEnd() - selectionStart();
    if (length > kMaxCopyBytes) {
        emit statusMessage(tr("The selection is too large to copy (limit: %1)")
                               .arg(QLocale().formattedDataSize(static_cast<qint64>(kMaxCopyBytes))));
        return;
    }
    QGuiApplication::clipboard()->setText(selectedText());
    emit statusMessage(tr("Copied %n byte(s)", nullptr, static_cast<int>(length)));
}

// Editing ----------------------------------------------------------------------------------------

bool TextView::canEdit()
{
    if (!m_document)
        return false;
    if (m_readOnly)
        emit statusMessage(m_readOnlyReason.isEmpty() ? tr("This document is read-only") : m_readOnlyReason);
    return !m_readOnly;
}

std::uint64_t TextView::editOffset(std::uint64_t offset) const
{
    const core::LineIndex& index = m_document->index();
    if (offset < documentEnd() || index.rowCount() == 0 || !index.isComplete())
        return offset;
    return std::min(offset, index.row(index.rowCount() - 1).contentEnd);
}

void TextView::replaceSelection(const QString& text, core::EditBuffer::Merge merge)
{
    const std::uint64_t start = hasSelection() ? selectionStart() : editOffset(m_cursor);
    replaceRange(start, hasSelection() ? selectionEnd() : start, text, merge);
}

bool TextView::replaceRange(std::uint64_t start, std::uint64_t end, const QString& text, core::EditBuffer::Merge merge)
{
    const std::optional<std::uint64_t> inserted = m_document->replace(start, end, text, merge);
    if (!inserted) {
        emit statusMessage(tr("These characters cannot be written in this file's encoding (%1)")
                               .arg(QString::fromLatin1(m_document->codec().name.data())));
        return false;
    }
    m_desiredX.reset();
    setCursorAndAnchor(start + *inserted, start + *inserted);
    ensureCursorVisible();
    return true;
}

bool TextView::replaceSelectedText(const QString& text)
{
    if (!canEdit())
        return false;
    m_document->breakUndoMerge();
    return replaceRange(selectionStart(), selectionEnd(), text, core::EditBuffer::Merge::No);
}

void TextView::insertText(const QString& text)
{
    if (canEdit())
        replaceSelection(text, core::EditBuffer::Merge::Typing);
}

void TextView::deleteBackward(bool word)
{
    if (hasSelection()) {
        replaceRange(selectionStart(), selectionEnd(), {}, core::EditBuffer::Merge::No);
        return;
    }
    if (rowCount() == 0)
        return;
    const std::uint64_t caret = editOffset(m_cursor);
    const std::uint64_t start = stepOffset(caret, false, word ? QTextLayout::SkipWords : QTextLayout::SkipCharacters);
    if (start < caret)
        replaceRange(start, caret, {}, core::EditBuffer::Merge::DeleteBackward);
}

void TextView::deleteForward(bool word)
{
    if (hasSelection()) {
        replaceRange(selectionStart(), selectionEnd(), {}, core::EditBuffer::Merge::No);
        return;
    }
    if (rowCount() == 0)
        return;
    const std::uint64_t caret = editOffset(m_cursor);
    const std::uint64_t end = stepOffset(caret, true, word ? QTextLayout::SkipWords : QTextLayout::SkipCharacters);
    if (end > caret)
        replaceRange(caret, end, {}, core::EditBuffer::Merge::DeleteForward);
}

void TextView::insertLineBreak()
{
    QString text = m_document->lineTerminator();
    // The new line starts indented like the one it splits, up to the caret.
    if (rowCount() > 0) {
        const core::LineIndex& index = m_document->index();
        const std::uint64_t caret = hasSelection() ? selectionStart() : editOffset(m_cursor);
        if (const auto first = index.rowIndexOfLine(index.row(index.rowIndexAt(caret)).line)) {
            const core::Row start = index.row(*first);
            const std::uint64_t end = std::min({caret, start.contentEnd, start.start + core::LineIndex::kMaxRowBytes});
            const QString before = m_document->decoder().decodePlain(
                m_document->bytes().subspan(start.start, static_cast<std::size_t>(end - start.start)));
            qsizetype indent = 0;
            while (indent < before.size() && (before.at(indent) == u' ' || before.at(indent) == u'\t'))
                ++indent;
            text += before.left(indent);
        }
    }
    // Past the last line break there is no line to type on: at the end of a file whose last line
    // has none, the new line gets one as well, and the caret goes before it.
    const core::LineIndex& index = m_document->index();
    const std::uint64_t end = hasSelection() ? selectionEnd() : editOffset(m_cursor);
    const bool openEnd = end == documentEnd() &&
                         (index.rowCount() == 0 || index.row(index.rowCount() - 1).end ==
                                                       index.row(index.rowCount() - 1).contentEnd);
    const std::optional<QByteArray> terminator = m_document->encode(m_document->lineTerminator());
    if (openEnd && terminator)
        text += m_document->lineTerminator();
    replaceSelection(text, core::EditBuffer::Merge::Typing);
    if (openEnd && terminator && m_cursor >= static_cast<std::uint64_t>(terminator->size()))
        setCursorOffset(m_cursor - static_cast<std::uint64_t>(terminator->size()));
}

QString TextView::indentUnit() const
{
    // Tabs, unless more lines start with spaces; then as many as the indents have in common
    // (2 for most YAML, 4 for much code), or the tab width.
    int tabs = 0;
    int spaced = 0;
    int width = 0;
    for (const core::Row& row : m_document->index().rows(0, 2000)) {
        if (row.continuation)
            continue;
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(row.contentEnd - row.start, 64));
        const QString start = m_document->decoder().decodePlain(m_document->bytes().subspan(row.start, length));
        int spaces = 0;
        while (spaces < start.size() && start.at(spaces) == u' ')
            ++spaces;
        if (start.startsWith(u'\t')) {
            ++tabs;
        } else if (spaces > 0 && spaces < start.size()) {
            ++spaced;
            width = std::gcd(width, spaces);
        }
    }
    if (spaced <= tabs)
        return QStringLiteral("\t");
    return QString(width >= 2 && width <= 8 ? width : m_tabWidth, u' ');
}

void TextView::indentLines(bool unindent)
{
    if (rowCount() == 0)
        return;
    const core::LineIndex& index = m_document->index();
    const std::uint64_t from = hasSelection() ? selectionStart() : editOffset(m_cursor);
    const std::uint64_t to = hasSelection() ? selectionEnd() : from;
    const std::uint64_t first = index.rowIndexAt(from);
    std::uint64_t last = index.rowIndexAt(to);
    if (last > first && index.row(last).start == to)
        --last; // the selection ends at the start of that line
    const std::vector<core::Row> rows = index.rows(first, static_cast<std::size_t>(last - first + 1));

    // One change from the first line to the last, built from their bytes untouched plus the
    // indents added or removed, so nothing else gets re-encoded.
    const QString unit = indentUnit();
    const std::optional<QByteArray> encodedUnit = m_document->encode(unit);
    const auto bytes = m_document->bytes();
    std::vector<std::uint8_t> text;
    for (const core::Row& row : rows) {
        std::uint64_t keep = row.start;
        if (!row.continuation && row.contentEnd > row.start) {
            if (!unindent && encodedUnit) {
                text.insert(text.end(), encodedUnit->begin(), encodedUnit->end());
            } else if (unindent) {
                // At most one indent: a tab, or spaces up to the width of one.
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(row.contentEnd - row.start, 64));
                const DecodedText head = m_document->decoder().decode(bytes.subspan(row.start, length), false);
                const qsizetype width = unit == QStringLiteral("\t") ? m_tabWidth : unit.size();
                qsizetype remove = 0;
                if (head.text.startsWith(u'\t')) {
                    remove = 1;
                } else {
                    while (remove < head.text.size() && remove < width && head.text.at(remove) == u' ')
                        ++remove;
                }
                keep = row.start + head.byteAt(remove);
            }
        }
        text.insert(text.end(), bytes.begin() + static_cast<std::ptrdiff_t>(keep),
                    bytes.begin() + static_cast<std::ptrdiff_t>(row.end));
    }
    const std::uint64_t start = rows.front().start;
    const std::uint64_t end = rows.back().end;
    if (text.size() == end - start && std::equal(text.begin(), text.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start)))
        return; // nothing to unindent
    m_document->replaceBytes(start, end, text);
    // The lines stay selected, up to the end of the last one's text.
    const core::Row lastRow = rows.back();
    const std::uint64_t selectionEnd = start + text.size() - (lastRow.end - lastRow.contentEnd);
    m_desiredX.reset();
    setCursorAndAnchor(selectionEnd, start);
    ensureCursorVisible();
}

void TextView::cut()
{
    if (!hasSelection() || !canEdit())
        return;
    if (selectionEnd() - selectionStart() > kMaxCopyBytes) {
        copy(); // says why not
        return;
    }
    copy();
    replaceRange(selectionStart(), selectionEnd(), {}, core::EditBuffer::Merge::No);
}

void TextView::paste()
{
    if (!canEdit())
        return;
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    // Line breaks as the file has them.
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n');
    if (const QString terminator = m_document->lineTerminator(); terminator != QStringLiteral("\n"))
        text.replace(QStringLiteral("\n"), terminator);
    replaceSelection(text, core::EditBuffer::Merge::No);
}

void TextView::undo()
{
    if (m_readOnly || !m_document)
        return;
    if (const auto range = m_document->undo()) {
        m_desiredX.reset();
        setCursorAndAnchor(range->second, range->first); // what came back stays selected
        ensureCursorVisible();
    }
}

void TextView::redo()
{
    if (m_readOnly || !m_document)
        return;
    if (const auto range = m_document->redo()) {
        m_desiredX.reset();
        setCursorAndAnchor(range->second, range->second);
        ensureCursorVisible();
    }
}

void TextView::documentEdited(std::uint64_t offset, std::uint64_t /*removed*/)
{
    // Rows before the change are as they were, and so are their highlighting and layouts; the
    // rest moved, or changed.
    const std::uint64_t row = rowCount() == 0 ? 0 : m_document->index().rowIndexAt(offset);
    m_stateCheckpoints.resize(
        std::min(m_stateCheckpoints.size(), static_cast<std::size_t>(row / kStateCheckpointRows) + 1));
    std::erase_if(m_stateAfterRow, [row](const auto& entry) { return entry.first >= row; });
    std::erase_if(m_layouts, [this, offset](const auto& entry) {
        if (entry.second->span.end <= offset)
            return false;
        m_cachedLayoutBytes -= entry.second->span.contentEnd - entry.second->span.start;
        return true;
    });
    updateScrollBars();
    viewport()->update();
}

void TextView::inputMethodEvent(QInputMethodEvent* event)
{
    if (m_readOnly || !m_document) {
        event->ignore();
        return;
    }
    if (!event->commitString().isEmpty())
        replaceSelection(event->commitString(), core::EditBuffer::Merge::Typing);
    m_preedit = event->preeditString();
    viewport()->update();
    event->accept();
}

QVariant TextView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return !m_readOnly && m_document;
    case Qt::ImCursorRectangle:
        // In this widget's coordinates; the viewport sits inside its frame.
        return cursorRect().translated(viewport()->geometry().topLeft());
    case Qt::ImFont:
        return font();
    case Qt::ImHints:
        return static_cast<int>(Qt::ImhMultiLine);
    case Qt::ImCurrentSelection:
        return selectionEnd() - selectionStart() <= 4096 ? selectedText() : QString();
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

bool TextView::focusNextPrevChild(bool next)
{
    // Tab is for indenting when the text can be edited.
    return m_readOnly && QAbstractScrollArea::focusNextPrevChild(next);
}

// Painting ---------------------------------------------------------------------------------------

void TextView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), m_colors.base);
    const int width = viewport()->width();
    const int height = viewport()->height();
    const int gutter = gutterWidth();
    painter.fillRect(QRect(0, 0, gutter, height), m_colors.gutter);
    if (rowCount() == 0) {
        // An empty file still has its first line, where typing starts.
        if (!m_document)
            return;
        painter.setFont(font());
        painter.setPen(m_colors.text);
        painter.drawText(QRectF(0, 0, gutter - m_charWidth, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("1"));
        if (hasFocus())
            painter.fillRect(cursorRect(), m_colors.text);
        return;
    }

    const VisualLine top = topLine();
    const VisualLine caret = caretLine();
    const bool focused = hasFocus();
    const std::uint64_t selStart = selectionStart();
    const std::uint64_t selEnd = selectionEnd();
    const QColor& selectionBackground = focused ? m_colors.selection : m_colors.inactiveSelection;
    const QColor& selectionForeground = focused ? m_colors.selectionText : m_colors.inactiveSelectionText;
    const qreal scrollX = m_wordWrap ? 0 : horizontalScrollBar()->value();
    const QRect textArea(gutter, 0, width - gutter, height);
    painter.setFont(font());

    // Paints a block whose first line is at y; returns its height.
    const auto paintBlock = [&](const Block& block, int y) {
        const core::Row& span = block.span;
        const BlockLayout& layout = layoutFor(block);
        const int lines = layout.layout.lineCount();
        const QPointF origin(textLeft() - scrollX, y);
        const bool isCaretBlock = block.firstRow == caret.block.firstRow;

        if (isCaretBlock && !hasSelection() && !m_readingMode)
            painter.fillRect(QRect(gutter, y + caret.line * m_lineHeight, width - gutter, m_lineHeight),
                             m_colors.currentLine);
        const bool bookmarked =
            !span.continuation && std::binary_search(m_bookmarks.begin(), m_bookmarks.end(), span.start);
        if (bookmarked && gutter > 0) {
            // The line number on a coloured tab.
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_colors.bookmark);
            const qreal inset = m_charWidth / 4;
            painter.drawRoundedRect(QRectF(inset, y + 1, gutter - m_charWidth / 2 - inset, m_lineHeight - 2), inset,
                                    inset);
        }
        if (!span.continuation && gutter > 0) {
            painter.setPen(isCaretBlock || bookmarked ? m_colors.text : m_colors.dim);
            painter.drawText(QRectF(0, y, gutter - m_charWidth, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(span.line + 1));
        }

        painter.save();
        painter.setClipRect(textArea);
        QList<QTextLayout::FormatRange> selections;
        if (selStart < selEnd && selStart <= span.contentEnd && selEnd > span.start) {
            const auto from = layout.decoded.positionAt(static_cast<std::uint32_t>(std::max(selStart, span.start) - span.start));
            const auto to = layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(selEnd, span.contentEnd) - span.start));
            if (to > from) {
                QTextLayout::FormatRange range;
                range.start = static_cast<int>(from);
                range.length = static_cast<int>(to - from);
                range.format.setForeground(selectionForeground);
                selections.append(range);
                // The background is painted here rather than by the layout, which would leave
                // gaps between lines: it fills only each line's own height, not the line pitch.
                const int firstLine = lineAtPosition(layout.layout, range.start).lineNumber();
                const int lastLine = lineAtPosition(layout.layout, range.start + range.length).lineNumber();
                for (int i = firstLine; i <= lastLine; ++i) {
                    const QTextLine line = layout.layout.lineAt(i);
                    const int a = std::max(range.start, line.textStart());
                    const int b = std::min(range.start + range.length, line.textStart() + line.textLength());
                    if (a >= b)
                        continue;
                    const qreal x1 = line.cursorToX(a);
                    const qreal x2 = line.cursorToX(b);
                    painter.fillRect(QRectF(origin.x() + std::min(x1, x2), y + i * m_lineHeight, std::abs(x2 - x1),
                                            m_lineHeight),
                                     selectionBackground);
                }
            }
            // A selected line terminator shows as a narrow block after the text.
            if (selEnd > span.contentEnd && span.end > span.contentEnd) {
                const QTextLine last = layout.layout.lineAt(lines - 1);
                painter.fillRect(QRectF(origin.x() + last.naturalTextWidth(), y + (lines - 1) * m_lineHeight,
                                        m_charWidth / 2, m_lineHeight),
                                 selectionBackground);
            }
        }
        painter.setPen(m_colors.text);
        layout.layout.draw(&painter, origin, selections, textArea); // skips lines outside the view
        if (isCaretBlock && focused) {
            const QTextLine line = layout.layout.lineAt(caret.line);
            const auto position = layout.decoded.positionAt(
                static_cast<std::uint32_t>(std::clamp(m_cursor, span.start, span.contentEnd) - span.start));
            painter.fillRect(QRectF(origin.x() + line.cursorToX(static_cast<int>(position)), origin.y() + line.y(),
                                    kCaretWidth, line.height()),
                             m_colors.text);
        }
        painter.restore();
        return lines * m_lineHeight;
    };

    // Every block takes at least one line, so a screenful of blocks is always enough.
    int y = -top.line * m_lineHeight;
    std::uint64_t next = top.block.firstRow;
    std::size_t count = static_cast<std::size_t>(height / m_lineHeight + 1);
    while (y < height) {
        const std::vector<Block> blocks = blocksFrom(next, count);
        if (blocks.empty())
            break;
        for (const Block& block : blocks) {
            y += paintBlock(block, y);
            next = block.firstRow + block.rowCount;
            if (y >= height)
                break;
        }
        count = static_cast<std::size_t>(std::max(0, height - y) / m_lineHeight + 1);
    }

    // What an input method is composing shows at the caret, over the text after it, underlined.
    if (!m_preedit.isEmpty() && focused) {
        const QRect caretRect = cursorRect();
        if (!caretRect.isNull()) {
            const QFontMetricsF metrics(font());
            const QRectF area(caretRect.left(), caretRect.top(), metrics.horizontalAdvance(m_preedit) + kCaretWidth,
                              m_lineHeight);
            painter.fillRect(area, m_colors.base);
            painter.setPen(m_colors.text);
            painter.drawText(QPointF(area.left(), area.top() + m_ascent), m_preedit);
            painter.drawLine(QPointF(area.left(), area.bottom() - 1), QPointF(area.right() - kCaretWidth, area.bottom() - 1));
            painter.fillRect(QRectF(area.right() - kCaretWidth, area.top(), kCaretWidth, m_lineHeight), m_colors.text);
        }
    }

    // Rows laid out for the first time may be wider than the scroll range allows for.
    if (!m_wordWrap && m_maxRowWidth > m_maxRowWidthInRange)
        QMetaObject::invokeMethod(this, &TextView::updateHorizontalRange, Qt::QueuedConnection);
}

// Events -----------------------------------------------------------------------------------------

void TextView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars(); // with word wrap, also re-wraps for the new width
}

void TextView::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics();
        clearLayouts();
        m_maxRowWidth = 0;
        updateScrollBars();
        viewport()->update();
    } else if (event->type() == QEvent::PaletteChange) {
        updateColors();
        resetHighlighting(); // the colour theme follows the palette
        updateScrollBars();
        viewport()->update();
    }
}

void TextView::keyPressEvent(QKeyEvent* event)
{
    if (event == QKeySequence::Copy) {
        copy();
        return;
    }
    if (event == QKeySequence::SelectAll) {
        selectAll();
        return;
    }

    const bool extend = event->modifiers() & Qt::ShiftModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const auto mode = ctrl ? QTextLayout::SkipWords : QTextLayout::SkipCharacters;

    const int key = event->key();
    const bool editing = event == QKeySequence::Undo || event == QKeySequence::Redo || event == QKeySequence::Cut ||
                         event == QKeySequence::Paste || isTypedText(event) || key == Qt::Key_Tab ||
                         key == Qt::Key_Backtab || key == Qt::Key_Backspace || key == Qt::Key_Delete ||
                         key == Qt::Key_Return || key == Qt::Key_Enter;
    if (editing && m_document) {
        if (!canEdit())
            return;
        if (event == QKeySequence::Undo)
            undo();
        else if (event == QKeySequence::Redo)
            redo();
        else if (event == QKeySequence::Cut)
            cut();
        else if (event == QKeySequence::Paste)
            paste();
        else if (event->key() == Qt::Key_Backspace)
            deleteBackward(ctrl);
        else if (event->key() == Qt::Key_Delete)
            deleteForward(ctrl);
        else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            insertLineBreak();
        else if (event->key() == Qt::Key_Backtab)
            indentLines(true);
        else if (event->key() == Qt::Key_Tab && hasSelection() &&
                 m_document->index().row(m_document->index().rowIndexAt(selectionStart())).line !=
                     m_document->index().row(m_document->index().rowIndexAt(selectionEnd())).line)
            indentLines(false);
        else if (event->key() == Qt::Key_Tab)
            replaceSelection(indentUnit(), core::EditBuffer::Merge::Typing);
        else
            replaceSelection(event->text(), core::EditBuffer::Merge::Typing);
        return;
    }
    if (rowCount() == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
        if (!extend && hasSelection())
            moveCursor(selectionStart(), false);
        else
            moveCursor(stepOffset(m_cursor, false, mode), extend);
        break;
    case Qt::Key_Right:
        if (!extend && hasSelection())
            moveCursor(selectionEnd(), false);
        else
            moveCursor(stepOffset(m_cursor, true, mode), extend);
        break;
    case Qt::Key_Up: // with Ctrl, the text scrolls under the caret
        if (ctrl && !extend)
            scrollLines(-1);
        else
            moveVertically(-1, extend);
        break;
    case Qt::Key_Down:
        if (ctrl && !extend)
            scrollLines(1);
        else
            moveVertically(1, extend);
        break;
    case Qt::Key_PageUp:
        scrollLines(-visibleRows());
        moveVertically(-visibleRows(), extend);
        break;
    case Qt::Key_PageDown:
        scrollLines(visibleRows());
        moveVertically(visibleRows(), extend);
        break;
    case Qt::Key_Home: {
        if (ctrl) {
            moveCursor(m_document->textStart(), extend);
            break;
        }
        // On a line's first row: to where its text starts, then to the very start.
        const std::uint64_t start = lineStart(caretLine());
        std::uint64_t target = start;
        if (!m_readingMode && lineStartAt(start) == start && indentationEnd(start) != m_cursor)
            target = indentationEnd(start);
        moveCursor(target, extend);
        break;
    }
    case Qt::Key_End:
        moveCursor(ctrl ? Caret{documentEnd()} : lineEndCaret(caretLine()), extend);
        break;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
}

TextView::Caret TextView::caretAt(const QPoint& pos) const
{
    if (rowCount() == 0)
        return {m_document ? m_document->textStart() : 0};
    const auto lines = static_cast<std::int64_t>(std::floor(pos.y() / static_cast<qreal>(m_lineHeight)));
    const qreal scrollX = m_wordWrap ? 0 : horizontalScrollBar()->value();
    return caretAtX(moveLines(topLine(), lines), pos.x() + scrollX - textLeft());
}

void TextView::mousePressEvent(QMouseEvent* event)
{
    if (rowCount() == 0) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    const Caret caret = caretAt(pos);
    const std::uint64_t offset = caret.offset;

    if (event->button() == Qt::RightButton) {
        // Keep the selection when right-clicking inside it, so the context menu can act on it.
        if (!hasSelection() || offset < selectionStart() || offset > selectionEnd())
            moveCursor(caret, false);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }

    const bool extend = event->modifiers() & Qt::ShiftModifier;
    if (!extend && m_lastDoubleClick.isValid() && m_lastDoubleClick.elapsed() < QApplication::doubleClickInterval() &&
        (pos - m_lastDoubleClickPos).manhattanLength() <= QApplication::startDragDistance()) {
        // A third click: the whole line.
        m_lastDoubleClick.invalidate();
        setCursorOffset(offset);
        selectLines();
        return;
    }
    if (pos.x() < gutterWidth()) {
        // A click on a line number selects the whole line as shown, terminator included.
        const core::Row span = blockAt(m_document->index().rowIndexAt(offset)).span;
        if (extend) {
            moveCursor(span.end, true);
        } else {
            setCursorAndAnchor(span.end, span.start);
            ensureCursorVisible();
        }
    } else {
        moveCursor(caret, extend);
    }
    m_dragging = true;
    m_lastDragPos = pos;
}

void TextView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    m_lastDragPos = event->position().toPoint();
    dragTo(m_lastDragPos);

    const bool outside = m_lastDragPos.y() < 0 || m_lastDragPos.y() >= viewport()->height();
    if (outside && !m_autoScrollTimer.isActive())
        m_autoScrollTimer.start(kAutoScrollIntervalMs, this);
    else if (!outside)
        m_autoScrollTimer.stop();
}

void TextView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    m_autoScrollTimer.stop();
}

void TextView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rowCount() > 0 && event->position().x() >= gutterWidth()) {
        selectWordAt(caretAt(event->position().toPoint()).offset);
        m_lastDoubleClick.start();
        m_lastDoubleClickPos = event->position().toPoint();
    } else {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
    }
}

void TextView::selectLines()
{
    if (!m_document || rowCount() == 0)
        return;
    const std::uint64_t start = selectionStart();
    const std::uint64_t end = selectionEnd();
    const auto atLineStart = [this](std::uint64_t offset) { return lineStartAt(offset) == offset; };
    std::uint64_t to = 0;
    if (hasSelection() && atLineStart(start) && atLineStart(end))
        to = lineEndAt(end); // whole lines already: one more
    else // the lines it touches; one it ends at the start of is not taken in
        to = lineEndAt(hasSelection() && atLineStart(end) ? end - 1 : end);
    select(lineStartAt(start), to);
}

std::uint64_t TextView::lineEndAt(std::uint64_t offset) const
{
    const core::LineIndex& index = m_document->index();
    if (const auto next = index.rowIndexOfLine(index.row(index.rowIndexAt(offset)).line + 1))
        return index.row(*next).start;
    return documentEnd();
}

std::uint64_t TextView::indentationEnd(std::uint64_t lineStart) const
{
    const core::Row row = m_document->index().row(m_document->index().rowIndexAt(lineStart));
    const auto bytes = m_document->bytes();
    const core::CharScheme scheme = m_document->codec().scheme;
    const std::size_t unit = core::codeUnitSize(scheme);
    std::uint64_t at = lineStart;
    while (at + unit <= row.contentEnd) {
        const std::uint32_t c = core::codeUnitAt(bytes, static_cast<std::size_t>(at), scheme);
        if (c != ' ' && c != '\t')
            break;
        at += unit;
    }
    return at;
}

bool TextView::selectWordAtCursor()
{
    if (rowCount() == 0)
        return false;
    selectWordAt(m_cursor);
    return hasSelection();
}

std::uint64_t TextView::lineStartAt(std::uint64_t offset) const
{
    if (!m_document || rowCount() == 0)
        return 0;
    const core::LineIndex& index = m_document->index();
    if (!index.isComplete() && offset >= index.indexedBytes())
        return offset;
    core::Row row = index.row(index.rowIndexAt(offset));
    if (row.continuation) {
        if (const auto first = index.rowIndexOfLine(row.line))
            row = index.row(*first);
    }
    return row.start;
}

void TextView::tidyBookmarks()
{
    const std::uint64_t size = m_document ? m_document->bytes().size() : 0;
    std::erase_if(m_bookmarks, [size](std::uint64_t offset) { return offset > size; });
    for (std::uint64_t& bookmark : m_bookmarks)
        bookmark = lineStartAt(bookmark);
    std::sort(m_bookmarks.begin(), m_bookmarks.end());
    m_bookmarks.erase(std::unique(m_bookmarks.begin(), m_bookmarks.end()), m_bookmarks.end());
    viewport()->update();
}

void TextView::setBookmarks(std::vector<std::uint64_t> offsets)
{
    m_bookmarks = std::move(offsets);
    tidyBookmarks();
}

void TextView::toggleBookmark()
{
    if (!m_document)
        return;
    const std::uint64_t start = lineStartAt(m_cursor);
    const QString line = QLocale().toString(static_cast<qulonglong>(cursorLine() + 1));
    const auto at = std::lower_bound(m_bookmarks.begin(), m_bookmarks.end(), start);
    if (at != m_bookmarks.end() && *at == start) {
        m_bookmarks.erase(at);
        emit statusMessage(tr("Bookmark on line %1 removed").arg(line));
    } else {
        m_bookmarks.insert(at, start);
        emit statusMessage(tr("Line %1 bookmarked").arg(line));
    }
    viewport()->update();
}

bool TextView::goToBookmark(bool forward)
{
    if (!m_document || rowCount() == 0)
        return false;
    // Only lines the index has reached can be gone to.
    const core::LineIndex& index = m_document->index();
    std::vector<std::uint64_t> reachable = m_bookmarks;
    if (!index.isComplete())
        std::erase_if(reachable, [&index](std::uint64_t offset) { return offset >= index.indexedBytes(); });
    if (reachable.empty())
        return false;
    const std::uint64_t here = lineStartAt(m_cursor);
    std::uint64_t target = 0;
    if (forward) {
        const auto next = std::upper_bound(reachable.begin(), reachable.end(), here);
        target = next != reachable.end() ? *next : reachable.front();
    } else {
        const auto next = std::lower_bound(reachable.begin(), reachable.end(), here);
        target = next != reachable.begin() ? *std::prev(next) : reachable.back();
    }
    goToOffset(target);
    return true;
}

void TextView::selectWordAt(std::uint64_t offset)
{
    const Block block = blockAt(m_document->index().rowIndexAt(offset));
    const core::Row& span = block.span;
    const BlockLayout& layout = layoutFor(block);
    const QString& text = layout.decoded.text;
    if (text.isEmpty())
        return;
    const auto position = std::min<qsizetype>(
        layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(offset, span.contentEnd) - span.start)),
        text.size() - 1);

    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    finder.setPosition(position);
    qsizetype start = finder.isAtBoundary() ? position : finder.toPreviousBoundary();
    finder.setPosition(position);
    qsizetype end = finder.toNextBoundary();
    start = std::max<qsizetype>(0, start);
    end = end < 0 ? text.size() : end;

    setCursorAndAnchor(span.start + layout.decoded.byteAt(end), span.start + layout.decoded.byteAt(start));
    ensureCursorVisible();
}

void TextView::timerEvent(QTimerEvent* event)
{
    if (event->timerId() != m_autoScrollTimer.timerId()) {
        QAbstractScrollArea::timerEvent(event);
        return;
    }
    // Scroll faster the further the pointer is outside the view.
    const int y = m_lastDragPos.y();
    if (y < 0)
        scrollLines(-(1 + -y / m_lineHeight));
    else if (y >= viewport()->height())
        scrollLines(1 + (y - viewport()->height()) / m_lineHeight);
    dragTo(m_lastDragPos);
}

void TextView::dragTo(const QPoint& pos)
{
    const Caret caret = caretAt(pos);
    setCursorAndAnchor(caret.offset, m_anchor);
    m_caretAtLineEnd = caret.atLineEnd;
}

void TextView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (event->modifiers() & Qt::ControlModifier) {
        m_zoomRemainder += delta;
        const int steps = m_zoomRemainder / kWheelStep;
        m_zoomRemainder -= steps * kWheelStep;
        if (steps != 0)
            emit zoomRequested(steps);
        event->accept();
        return;
    }
    if (delta == 0) {
        QAbstractScrollArea::wheelEvent(event); // horizontal scrolling
        return;
    }
    m_wheelRemainder += delta;
    const int steps = m_wheelRemainder / kWheelStep;
    m_wheelRemainder -= steps * kWheelStep;
    if (steps != 0)
        scrollLines(-static_cast<std::int64_t>(steps) * QApplication::wheelScrollLines());
    event->accept();
}

void TextView::setContextActions(const QList<QAction*>& actions)
{
    m_contextActions.clear();
    for (QAction* action : actions)
        m_contextActions << action;
}

std::unique_ptr<QMenu> TextView::createContextMenu()
{
    auto menu = std::make_unique<QMenu>(this);
    if (!m_readOnly && m_document) {
        menu->addAction(tr("Undo"), this, &TextView::undo)->setEnabled(m_document->canUndo());
        menu->addAction(tr("Redo"), this, &TextView::redo)->setEnabled(m_document->canRedo());
        menu->addSeparator();
        menu->addAction(tr("Cut"), this, &TextView::cut)->setEnabled(hasSelection());
    }
    menu->addAction(tr("Copy"), this, &TextView::copy)->setEnabled(hasSelection());
    if (!m_readOnly && m_document)
        menu->addAction(tr("Paste"), this, &TextView::paste)
            ->setEnabled(!QGuiApplication::clipboard()->text().isEmpty());
    menu->addSeparator();
    for (QAction* action : m_contextActions) {
        if (action)
            menu->addAction(action);
    }
    menu->addSeparator();
    menu->addAction(tr("Select All"), this, &TextView::selectAll)->setEnabled(rowCount() > 0);
    return menu;
}

void TextView::contextMenuEvent(QContextMenuEvent* event)
{
    createContextMenu()->exec(event->globalPos());
}

void TextView::focusInEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void TextView::focusOutEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

} // namespace aiedit::ui
