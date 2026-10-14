#include "ui/TextView.h"

#include "ui/TextDocument.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QTextBoundaryFinder>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cppedit::ui {

namespace {

constexpr std::size_t kMaxCachedLayouts = 1024;
constexpr qreal kUnboundedLineWidth = 1e6; // rows never wrap
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

} // namespace

TextView::TextView(QWidget* parent) : QAbstractScrollArea(parent), m_rows(verticalScrollBar())
{
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    updateMetrics();
    updateColors();
    updateScrollBars();
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
        connect(m_document, &QObject::destroyed, this, [this] {
            m_document = nullptr;
            resetForDocument();
        });
    }
    m_rows.setTopRow(0);
    m_cursor = m_anchor = 0;
    resetForDocument();
}

void TextView::resetForDocument()
{
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
    viewport()->update();
}

QList<QTextLayout::FormatRange> TextView::rowFormats(std::uint64_t rowIndex) const
{
    if (rowIndex >= rowCount())
        return {};
    return layoutFor(m_document->index().row(rowIndex)).layout.formats();
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
    const auto half = static_cast<std::uint64_t>(visibleRows() / 2);
    setTopRow(*rowIndex > half ? *rowIndex - half : 0);
    moveCursor(m_document->index().row(*rowIndex).start, false);
    return true;
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

void TextView::scrollContentsBy(int /*dx*/, int dy)
{
    if (dy != 0)
        m_rows.syncFromBar();
    viewport()->update();
}

void TextView::updateScrollBars()
{
    m_rows.setRange(maxTopRow(), visibleRows());
    updateHorizontalRange();
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
}

void TextView::updateColors()
{
    const QPalette& pal = palette();
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    m_colors.base = pal.color(QPalette::Base);
    m_colors.text = pal.color(QPalette::Text);
    m_colors.dim = mix(m_colors.text, m_colors.base, 0.55f);
    m_colors.gutter = mix(m_colors.base, m_colors.text, dark ? 0.06f : 0.035f);
    m_colors.selection = pal.color(QPalette::Active, QPalette::Highlight);
    m_colors.selectionText = pal.color(QPalette::Active, QPalette::HighlightedText);
    m_colors.inactiveSelection = mix(m_colors.base, m_colors.selection, 0.35f);
    m_colors.inactiveSelectionText = m_colors.text;
    m_colors.currentLine = mix(m_colors.base, m_colors.selection, dark ? 0.14f : 0.08f);
}

int TextView::gutterWidth() const
{
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
    return gutterWidth() + m_charWidth / 2;
}

const TextView::RowLayout& TextView::layoutFor(const core::Row& row) const
{
    if (const auto it = m_layouts.find(row.start); it != m_layouts.end() && it->second->row == row)
        return *it->second;
    if (m_layouts.size() >= kMaxCachedLayouts)
        m_layouts.clear();

    auto entry = std::make_unique<RowLayout>();
    entry->row = row;
    const auto bytes = m_document->bytes().subspan(row.start, static_cast<std::size_t>(row.contentEnd - row.start));
    entry->decoded = m_document->decoder().decode(bytes);
    if (m_highlighter) {
        const std::uint64_t rowIndex = m_document->index().rowIndexAt(row.start);
        SyntaxState state = stateBefore(rowIndex);
        entry->layout.setFormats(m_highlighter->highlightLine(entry->decoded.text, state));
        if (m_stateAfterRow.size() >= kMaxCachedStates)
            m_stateAfterRow.clear();
        m_stateAfterRow[rowIndex] = state;
    }

    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(m_tabWidth * m_charWidth);
    entry->layout.setText(entry->decoded.text);
    entry->layout.setFont(font());
    entry->layout.setTextOption(option);
    entry->layout.setCacheEnabled(true);
    entry->layout.beginLayout();
    QTextLine line = entry->layout.createLine();
    line.setLineWidth(kUnboundedLineWidth);
    line.setPosition(QPointF(0, 0));
    entry->layout.endLayout();
    m_maxRowWidth = std::max(m_maxRowWidth, line.naturalTextWidth());

    const RowLayout& result = *entry;
    m_layouts[row.start] = std::move(entry);
    return result;
}

void TextView::clearLayouts()
{
    m_layouts.clear();
}

// Navigation ---------------------------------------------------------------------------------

std::uint64_t TextView::cursorRowIndex() const
{
    return rowCount() == 0 ? 0 : m_document->index().rowIndexAt(m_cursor);
}

bool TextView::isLastRow(const core::Row& row) const
{
    return row.end >= documentEnd();
}

std::uint64_t TextView::lastCaretOffset(const core::Row& row) const
{
    const bool split = row.end == row.contentEnd && !isLastRow(row);
    if (!split)
        return row.contentEnd;
    // The split point itself belongs to the next row, so stop before the last character.
    const RowLayout& layout = layoutFor(row);
    const int last = layout.layout.previousCursorPosition(static_cast<int>(layout.decoded.text.size()));
    return row.start + layout.decoded.byteAt(last);
}

std::uint64_t TextView::offsetAtX(const core::Row& row, qreal x) const
{
    const RowLayout& layout = layoutFor(row);
    const int position = layout.layout.lineAt(0).xToCursor(std::max<qreal>(0, x), QTextLine::CursorBetweenCharacters);
    return std::min(row.start + layout.decoded.byteAt(position), lastCaretOffset(row));
}

qreal TextView::xOfOffset(const core::Row& row, std::uint64_t offset) const
{
    const RowLayout& layout = layoutFor(row);
    offset = std::clamp(offset, row.start, row.contentEnd);
    const auto position = layout.decoded.positionAt(static_cast<std::uint32_t>(offset - row.start));
    return layout.layout.lineAt(0).cursorToX(static_cast<int>(position));
}

std::uint64_t TextView::stepOffset(std::uint64_t offset, bool forward, QTextLayout::CursorMode mode) const
{
    const core::LineIndex& index = m_document->index();
    const std::uint64_t rowIndex = index.rowIndexAt(offset);
    const core::Row row = index.row(rowIndex);
    const RowLayout& layout = layoutFor(row);
    const int size = static_cast<int>(layout.decoded.text.size());
    const int position = static_cast<int>(
        layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(offset, row.contentEnd) - row.start)));

    if (forward) {
        if (position < size)
            return row.start + layout.decoded.byteAt(layout.layout.nextCursorPosition(position, mode));
        return isLastRow(row) ? std::max(offset, row.contentEnd) : row.end; // over the line terminator
    }
    if (position > 0)
        return row.start + layout.decoded.byteAt(layout.layout.previousCursorPosition(position, mode));
    return rowIndex == 0 ? offset : lastCaretOffset(index.row(rowIndex - 1));
}

void TextView::moveVertically(std::int64_t rows, bool extendSelection)
{
    const core::LineIndex& index = m_document->index();
    const std::uint64_t total = index.rowCount();
    const std::uint64_t current = cursorRowIndex();
    const core::Row row = index.row(current);
    if (!m_desiredX)
        m_desiredX = xOfOffset(row, m_cursor);

    std::uint64_t target = current;
    if (rows < 0) {
        const auto up = static_cast<std::uint64_t>(-rows);
        if (current == 0) {
            moveCursor(row.start, extendSelection, true);
            return;
        }
        target = current > up ? current - up : 0;
    } else {
        if (current + 1 >= total) {
            moveCursor(lastCaretOffset(row), extendSelection, true);
            return;
        }
        target = std::min(total - 1, current + static_cast<std::uint64_t>(rows));
    }
    moveCursor(offsetAtX(index.row(target), *m_desiredX), extendSelection, true);
}

void TextView::moveCursor(std::uint64_t offset, bool extendSelection, bool keepDesiredX)
{
    if (!keepDesiredX)
        m_desiredX.reset();
    setCursorAndAnchor(offset, extendSelection ? m_anchor : offset);
    ensureCursorVisible();
}

void TextView::setCursorAndAnchor(std::uint64_t cursor, std::uint64_t anchor)
{
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
    const std::uint64_t row = cursorRowIndex();
    const auto visible = static_cast<std::uint64_t>(visibleRows());
    if (row < topRow())
        setTopRow(row);
    else if (row >= topRow() + visible)
        setTopRow(row - visible + 1);

    const qreal caretX = textLeft() + xOfOffset(m_document->index().row(row), m_cursor);
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
    const core::Row row = index.row(rowIndex);

    std::uint64_t first = rowIndex;
    if (row.continuation) {
        const auto lineStart = index.rowIndexOfLine(row.line);
        if (!lineStart || rowIndex - *lineStart > kMaxColumnScanRows)
            return std::nullopt;
        first = *lineStart;
    }
    std::uint64_t column = 0;
    for (const core::Row& before : index.rows(first, static_cast<std::size_t>(rowIndex - first))) {
        const RowLayout& layout = layoutFor(before);
        column += countCharacters(layout.decoded.text, layout.decoded.text.size());
    }
    const RowLayout& layout = layoutFor(row);
    const auto position =
        layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(m_cursor, row.contentEnd) - row.start));
    return column + countCharacters(layout.decoded.text, position);
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

// Painting ---------------------------------------------------------------------------------------

void TextView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), m_colors.base);
    const int width = viewport()->width();
    const int height = viewport()->height();
    const int gutter = gutterWidth();
    painter.fillRect(QRect(0, 0, gutter, height), m_colors.gutter);
    if (rowCount() == 0)
        return;

    const core::LineIndex& index = m_document->index();
    const auto rows = index.rows(topRow(), static_cast<std::size_t>(height / m_lineHeight + 1));
    const std::uint64_t cursorRow = cursorRowIndex();
    const bool focused = hasFocus();
    const std::uint64_t selStart = selectionStart();
    const std::uint64_t selEnd = selectionEnd();
    const QColor& selectionBackground = focused ? m_colors.selection : m_colors.inactiveSelection;
    const QColor& selectionForeground = focused ? m_colors.selectionText : m_colors.inactiveSelectionText;
    const qreal scrollX = horizontalScrollBar()->value();
    const QRect textArea(gutter, 0, width - gutter, height);
    painter.setFont(font());

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const core::Row& row = rows[i];
        const bool isCursorRow = topRow() + i == cursorRow;
        const int top = static_cast<int>(i) * m_lineHeight;

        if (isCursorRow && !hasSelection())
            painter.fillRect(QRect(gutter, top, width - gutter, m_lineHeight), m_colors.currentLine);
        if (!row.continuation) {
            painter.setPen(isCursorRow ? m_colors.text : m_colors.dim);
            painter.drawText(QRectF(0, top, gutter - m_charWidth, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(row.line + 1));
        }

        const RowLayout& layout = layoutFor(row);
        const QTextLine line = layout.layout.lineAt(0);
        const QPointF origin(textLeft() - scrollX, top + m_ascent - line.ascent());

        painter.save();
        painter.setClipRect(textArea);
        QList<QTextLayout::FormatRange> selections;
        if (selStart < selEnd && selStart <= row.contentEnd && selEnd > row.start) {
            const auto from = layout.decoded.positionAt(static_cast<std::uint32_t>(std::max(selStart, row.start) - row.start));
            const auto to = layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(selEnd, row.contentEnd) - row.start));
            if (to > from) {
                QTextLayout::FormatRange range;
                range.start = static_cast<int>(from);
                range.length = static_cast<int>(to - from);
                range.format.setBackground(selectionBackground);
                range.format.setForeground(selectionForeground);
                selections.append(range);
            }
            // A selected line terminator shows as a narrow block after the text.
            if (selEnd > row.contentEnd && row.end > row.contentEnd)
                painter.fillRect(QRectF(origin.x() + line.naturalTextWidth(), top, m_charWidth / 2, m_lineHeight),
                                 selectionBackground);
        }
        painter.setPen(m_colors.text);
        layout.layout.draw(&painter, origin, selections);
        if (isCursorRow && focused) {
            const auto position =
                layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(m_cursor, row.contentEnd) - row.start));
            layout.layout.drawCursor(&painter, origin, static_cast<int>(position), kCaretWidth);
        }
        painter.restore();
    }

    // Rows laid out for the first time may be wider than the scroll range allows for.
    if (m_maxRowWidth > m_maxRowWidthInRange)
        QMetaObject::invokeMethod(this, &TextView::updateHorizontalRange, Qt::QueuedConnection);
}

// Events -----------------------------------------------------------------------------------------

void TextView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
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
    if (rowCount() == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    const bool extend = event->modifiers() & Qt::ShiftModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const auto mode = ctrl ? QTextLayout::SkipWords : QTextLayout::SkipCharacters;
    const core::LineIndex& index = m_document->index();

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
    case Qt::Key_Up:
        moveVertically(-1, extend);
        break;
    case Qt::Key_Down:
        moveVertically(1, extend);
        break;
    case Qt::Key_PageUp: {
        const int page = visibleRows();
        setTopRow(topRow() > static_cast<std::uint64_t>(page) ? topRow() - static_cast<std::uint64_t>(page) : 0);
        moveVertically(-page, extend);
        break;
    }
    case Qt::Key_PageDown: {
        const int page = visibleRows();
        setTopRow(topRow() + static_cast<std::uint64_t>(page));
        moveVertically(page, extend);
        break;
    }
    case Qt::Key_Home:
        moveCursor(ctrl ? m_document->textStart() : index.row(cursorRowIndex()).start, extend);
        break;
    case Qt::Key_End:
        moveCursor(ctrl ? documentEnd() : lastCaretOffset(index.row(cursorRowIndex())), extend);
        break;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
}

std::uint64_t TextView::offsetAt(const QPoint& pos) const
{
    if (rowCount() == 0)
        return m_document ? m_document->textStart() : 0;
    const auto rowDelta = static_cast<std::int64_t>(std::floor(pos.y() / static_cast<qreal>(m_lineHeight)));
    std::uint64_t rowIndex = 0;
    if (rowDelta < 0) {
        const auto up = static_cast<std::uint64_t>(-rowDelta);
        rowIndex = topRow() > up ? topRow() - up : 0;
    } else {
        rowIndex = topRow() + static_cast<std::uint64_t>(rowDelta);
    }
    rowIndex = std::min(rowIndex, rowCount() - 1);
    const qreal x = pos.x() + horizontalScrollBar()->value() - textLeft();
    return offsetAtX(m_document->index().row(rowIndex), x);
}

void TextView::mousePressEvent(QMouseEvent* event)
{
    if (rowCount() == 0) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->position().toPoint();
    const std::uint64_t offset = offsetAt(pos);

    if (event->button() == Qt::RightButton) {
        // Keep the selection when right-clicking inside it, so the context menu can act on it.
        if (!hasSelection() || offset < selectionStart() || offset > selectionEnd())
            moveCursor(offset, false);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }

    const bool extend = event->modifiers() & Qt::ShiftModifier;
    if (pos.x() < gutterWidth()) {
        // A click on a line number selects the whole row, terminator included.
        const core::Row row = m_document->index().row(m_document->index().rowIndexAt(offset));
        if (extend) {
            moveCursor(row.end, true);
        } else {
            setCursorAndAnchor(row.end, row.start);
            ensureCursorVisible();
        }
    } else {
        moveCursor(offset, extend);
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
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_autoScrollTimer.stop();
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void TextView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rowCount() > 0 && event->position().x() >= gutterWidth())
        selectWordAt(offsetAt(event->position().toPoint()));
    else
        QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void TextView::selectWordAt(std::uint64_t offset)
{
    const core::Row row = m_document->index().row(m_document->index().rowIndexAt(offset));
    const RowLayout& layout = layoutFor(row);
    const QString& text = layout.decoded.text;
    if (text.isEmpty())
        return;
    const auto position = std::min<qsizetype>(
        layout.decoded.positionAt(static_cast<std::uint32_t>(std::min(offset, row.contentEnd) - row.start)),
        text.size() - 1);

    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    finder.setPosition(position);
    qsizetype start = finder.isAtBoundary() ? position : finder.toPreviousBoundary();
    finder.setPosition(position);
    qsizetype end = finder.toNextBoundary();
    start = std::max<qsizetype>(0, start);
    end = end < 0 ? text.size() : end;

    setCursorAndAnchor(row.start + layout.decoded.byteAt(end), row.start + layout.decoded.byteAt(start));
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
    if (y < 0) {
        const auto rows = static_cast<std::uint64_t>(1 + -y / m_lineHeight);
        setTopRow(topRow() > rows ? topRow() - rows : 0);
    } else if (y >= viewport()->height()) {
        setTopRow(topRow() + static_cast<std::uint64_t>(1 + (y - viewport()->height()) / m_lineHeight));
    }
    dragTo(m_lastDragPos);
}

void TextView::dragTo(const QPoint& pos)
{
    setCursorAndAnchor(offsetAt(pos), m_anchor);
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
    if (steps != 0) {
        const auto rows = static_cast<std::uint64_t>(std::abs(steps) * QApplication::wheelScrollLines());
        setTopRow(steps > 0 ? (topRow() > rows ? topRow() - rows : 0) : topRow() + rows);
    }
    event->accept();
}

void TextView::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    menu.addAction(tr("Copy"), this, &TextView::copy)->setEnabled(hasSelection());
    menu.addSeparator();
    menu.addAction(tr("Select All"), this, &TextView::selectAll)->setEnabled(rowCount() > 0);
    menu.exec(event->globalPos());
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

} // namespace cppedit::ui
