#include "ui/HexView.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "ui/Desktop.h"
#include "ui/SyntaxHighlighting.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <cmath>

namespace aiedit::ui {

namespace {

// The hex pane leaves an extra blank column after every group of this many bytes.
constexpr int kGroupSize = 8;
constexpr int kAutoScrollIntervalMs = 40;
constexpr int kWheelStep = 120; // QWheelEvent::angleDelta() units per notch
constexpr int kCArrayBytesPerLine = 16;

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

} // namespace

HexView::HexView(QWidget* parent) : QAbstractScrollArea(parent), m_rows(verticalScrollBar())
{
    for (int b = 0; b < 256; ++b) {
        m_hexText[b] = QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
        m_charText[b] = (b >= 0x20 && b < 0x7F) ? QString(QChar(b)) : QStringLiteral(".");
    }

    setFocusPolicy(Qt::StrongFocus);
    viewport()->setCursor(Qt::IBeamCursor);
    setFont(fixedFont());
    updateMetrics();
    updateColors();
    updateScrollBars();
}

HexView::~HexView() = default;

std::uint64_t HexView::size() const
{
    return m_source ? m_source->size() : 0;
}

void HexView::setSource(std::shared_ptr<const core::FileSource> source)
{
    m_source = std::move(source);
    m_rows.setTopRow(0);
    updateMetrics(); // the offset column width depends on the file size
    updateScrollBars();
    m_cursor = m_anchor = 1; // force the signals below
    setCursorAndAnchor(0, 0);
}

void HexView::replaceSource(std::shared_ptr<const core::FileSource> source)
{
    m_source = std::move(source);
    updateMetrics();
    updateScrollBars(); // clamps the top row
    const std::uint64_t last = size() == 0 ? 0 : size() - 1;
    setCursorAndAnchor(std::min(m_cursor, last), std::min(m_anchor, last));
}

void HexView::setBytesPerRow(int bytesPerRow)
{
    bytesPerRow = std::clamp(bytesPerRow, kMinBytesPerRow, kMaxBytesPerRow);
    if (bytesPerRow == m_bytesPerRow)
        return;
    m_bytesPerRow = bytesPerRow;
    updateMetrics();
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
}

std::uint64_t HexView::selectionStart() const
{
    return std::min(m_cursor, m_anchor);
}

std::uint64_t HexView::selectionLength() const
{
    return size() == 0 ? 0 : std::max(m_cursor, m_anchor) - selectionStart() + 1;
}

void HexView::setActivePane(Pane pane)
{
    if (pane == m_activePane)
        return;
    m_activePane = pane;
    viewport()->update();
}

void HexView::setCursorOffset(std::uint64_t offset, bool extendSelection)
{
    moveCursor(offset, extendSelection);
}

void HexView::goTo(std::uint64_t offset)
{
    const std::uint64_t row = std::min(offset, size() == 0 ? 0 : size() - 1) / m_bytesPerRow;
    const auto visible = static_cast<std::uint64_t>(visibleRows());
    if (row < m_rows.topRow() || row >= m_rows.topRow() + visible)
        setTopRow(row > visible / 2 ? row - visible / 2 : 0);
    moveCursor(offset, false);
}

void HexView::select(std::uint64_t start, std::uint64_t length)
{
    if (size() == 0)
        return;
    const std::uint64_t last = size() - 1;
    start = std::min(start, last);
    const std::uint64_t end = (length == 0 || length - 1 > last - start) ? (length == 0 ? start : last)
                                                                        : start + length - 1;
    setCursorAndAnchor(end, start);
    ensureCursorVisible();
}

void HexView::selectAll()
{
    if (size() > 0)
        setCursorAndAnchor(size() - 1, 0);
}

int HexView::visibleRows() const
{
    return std::max(1, (viewport()->height() - m_headerHeight) / m_lineHeight);
}

QSize HexView::sizeHint() const
{
    const int width = static_cast<int>(std::ceil(contentWidth())) + verticalScrollBar()->sizeHint().width() +
                      2 * frameWidth();
    return {width, QAbstractScrollArea::sizeHint().height()};
}

std::uint64_t HexView::rowCount() const
{
    const std::uint64_t n = size();
    return n == 0 ? 0 : (n - 1) / static_cast<std::uint64_t>(m_bytesPerRow) + 1;
}

std::uint64_t HexView::maxTopRow() const
{
    const std::uint64_t rows = rowCount();
    const auto visible = static_cast<std::uint64_t>(visibleRows());
    return rows > visible ? rows - visible : 0;
}

void HexView::updateScrollBars()
{
    m_rows.setRange(maxTopRow(), visibleRows());

    QScrollBar* horizontal = horizontalScrollBar();
    const int width = viewport()->width();
    horizontal->setRange(0, std::max(0, static_cast<int>(std::ceil(contentWidth())) - width));
    horizontal->setPageStep(width);
    horizontal->setSingleStep(std::max(1, static_cast<int>(m_charWidth)));
}

void HexView::setTopRow(std::uint64_t row)
{
    if (m_rows.setTopRow(row))
        viewport()->update();
}

void HexView::scrollContentsBy(int /*dx*/, int dy)
{
    if (dy != 0)
        m_rows.syncFromBar();
    viewport()->update();
}

void HexView::ensureCursorVisible()
{
    const auto bytesPerRow = static_cast<std::uint64_t>(m_bytesPerRow);
    const std::uint64_t row = m_cursor / bytesPerRow;
    const auto visible = static_cast<std::uint64_t>(visibleRows());
    if (row < m_rows.topRow())
        setTopRow(row);
    else if (row >= m_rows.topRow() + visible)
        setTopRow(row - visible + 1);

    const int column = static_cast<int>(m_cursor % bytesPerRow);
    const qreal left = m_activePane == Pane::Hex ? hexCellX(column) : textCellX(column);
    const qreal right = left + (m_activePane == Pane::Hex ? 2 : 1) * m_charWidth;
    QScrollBar* horizontal = horizontalScrollBar();
    if (left < horizontal->value())
        horizontal->setValue(static_cast<int>(left));
    else if (right > horizontal->value() + viewport()->width())
        horizontal->setValue(static_cast<int>(std::ceil(right)) - viewport()->width());
}

void HexView::moveCursor(std::uint64_t offset, bool extendSelection)
{
    offset = std::min(offset, size() == 0 ? 0 : size() - 1);
    setCursorAndAnchor(offset, extendSelection ? m_anchor : offset);
    ensureCursorVisible();
}

void HexView::setCursorAndAnchor(std::uint64_t cursor, std::uint64_t anchor)
{
    const std::uint64_t oldCursor = m_cursor;
    const std::uint64_t oldStart = selectionStart();
    const std::uint64_t oldLength = selectionLength();
    m_cursor = cursor;
    m_anchor = anchor;
    if (m_cursor != oldCursor)
        emit cursorMoved(m_cursor);
    if (selectionStart() != oldStart || selectionLength() != oldLength)
        emit selectionChanged(selectionStart(), selectionLength());
    viewport()->update();
}

// Layout ---------------------------------------------------------------------------------------

void HexView::updateMetrics()
{
    const QFontMetricsF metrics(font());
    m_charWidth = metrics.horizontalAdvance(QLatin1Char('0'));
    m_lineHeight = static_cast<int>(std::ceil(metrics.lineSpacing())) + 2;
    m_ascent = static_cast<int>(std::round(metrics.ascent())) + 1;
    m_headerHeight = m_lineHeight + 4;

    int digits = 1;
    for (std::uint64_t v = size() > 0 ? size() - 1 : 0; v >= 16; v /= 16)
        ++digits;
    m_offsetDigits = std::max(8, digits + digits % 2);

    // Left margin, offset column, gap.
    m_hexStart = m_charWidth * (1 + m_offsetDigits + 2);
    m_textStart = hexAreaEnd() + 3 * m_charWidth;
}

qreal HexView::hexCellX(int column) const
{
    return m_hexStart + m_charWidth * (column * 3 + column / kGroupSize);
}

qreal HexView::hexAreaEnd() const
{
    return hexCellX(m_bytesPerRow - 1) + 2 * m_charWidth;
}

qreal HexView::textCellX(int column) const
{
    return m_textStart + m_charWidth * column;
}

qreal HexView::contentWidth() const
{
    return textCellX(m_bytesPerRow) + m_charWidth;
}

QString HexView::formatOffset(std::uint64_t offset) const
{
    return QStringLiteral("%1").arg(offset, m_offsetDigits, 16, QLatin1Char('0')).toUpper();
}

void HexView::updateColors()
{
    const EditorColors colors = editorColors(palette());
    const bool dark = colors.dark;
    m_colors.base = colors.background;
    m_colors.text = colors.text;
    m_colors.dim = colors.lineNumber;
    m_colors.whitespace = dark ? QColor(0x81, 0xC7, 0x84) : QColor(0x2E, 0x7D, 0x32);
    m_colors.control = dark ? QColor(0xF4, 0x8F, 0xB1) : QColor(0xAD, 0x14, 0x57);
    m_colors.highByte = dark ? QColor(0xFF, 0xB7, 0x4D) : QColor(0xA8, 0x55, 0x00);
    m_colors.selection = colors.selection;
    m_colors.selectionText = colors.selectionText;
    m_colors.inactiveSelection = mix(m_colors.base, m_colors.selection, 0.35f);
    m_colors.currentRow = colors.currentLine;
    m_colors.separator = mix(m_colors.text, m_colors.base, 0.8f);
    m_colors.marks.clear();
    for (const QColor hue : {QColor(0x42, 0x85, 0xF4), QColor(0x34, 0xA8, 0x53), QColor(0xF9, 0xAB, 0x00),
                             QColor(0xA1, 0x42, 0xF4), QColor(0x00, 0xAC, 0xC1), QColor(0xE9, 0x1E, 0x63)})
        m_colors.marks << mix(m_colors.base, hue, dark ? 0.35f : 0.22f);
}

const QColor& HexView::byteColor(std::uint8_t byte) const
{
    if (byte == 0)
        return m_colors.dim;
    if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
        return m_colors.whitespace;
    if (byte < 0x20 || byte == 0x7F)
        return m_colors.control;
    if (byte >= 0x80)
        return m_colors.highByte;
    return m_colors.text;
}

// Painting ---------------------------------------------------------------------------------------

void HexView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), m_colors.base);
    painter.setFont(font());

    // Paint in content coordinates; the horizontal scroll bar shifts everything.
    const int scrollX = horizontalScrollBar()->value();
    const int width = viewport()->width();
    const int height = viewport()->height();
    painter.translate(-scrollX, 0);

    const int bytesPerRow = m_bytesPerRow;
    const qreal separatorHexX = m_hexStart - 1.5 * m_charWidth;
    const qreal separatorTextX = hexAreaEnd() + 1.5 * m_charWidth;

    // Header: column numbers, then the grid lines.
    painter.setPen(m_colors.dim);
    for (int c = 0; c < bytesPerRow; ++c) {
        painter.drawText(QPointF(hexCellX(c), m_ascent + 2), m_hexText[c]);
        painter.drawText(QPointF(textCellX(c), m_ascent + 2), QString(QLatin1Char("0123456789ABCDEF"[c % 16])));
    }
    painter.setPen(m_colors.separator);
    painter.drawLine(QPointF(scrollX, m_headerHeight - 0.5), QPointF(scrollX + width, m_headerHeight - 0.5));
    painter.drawLine(QPointF(separatorHexX, 0), QPointF(separatorHexX, height));
    painter.drawLine(QPointF(separatorTextX, 0), QPointF(separatorTextX, height));

    const std::uint64_t total = size();
    if (total == 0)
        return;

    painter.setClipRect(QRectF(scrollX, m_headerHeight, width, height - m_headerHeight));

    const auto bytes = m_source->bytes();
    const auto rows = rowCount();
    const auto bpr = static_cast<std::uint64_t>(bytesPerRow);
    const std::uint64_t cursorRow = m_cursor / bpr;
    const std::uint64_t selStart = selectionStart();
    const std::uint64_t selEnd = selStart + selectionLength(); // exclusive
    const bool hasRange = selectionLength() > 1;
    const bool focused = hasFocus();
    const bool hexActive = m_activePane == Pane::Hex;
    const int rowsToPaint = (height - m_headerHeight) / m_lineHeight + 1;

    auto paintCursor = [&](const QRectF& cell, bool activePane) {
        if (activePane && focused) {
            painter.fillRect(cell, m_colors.selection);
        } else {
            painter.setPen(activePane ? m_colors.selection : m_colors.dim);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    };

    for (int r = 0; r < rowsToPaint; ++r) {
        const std::uint64_t row = m_rows.topRow() + static_cast<std::uint64_t>(r);
        if (row >= rows)
            break;
        const int top = m_headerHeight + r * m_lineHeight;
        const int baseline = top + m_ascent;
        const std::uint64_t rowStart = row * bpr;
        const int count = static_cast<int>(std::min(bpr, total - rowStart));

        if (row == cursorRow)
            painter.fillRect(QRectF(scrollX, top, width, m_lineHeight), m_colors.currentRow);

        // Field marks under the selection, so that selecting stays visible.
        for (qsizetype m = 0; m < m_marks.size(); ++m) {
            const FieldMark& mark = m_marks[m];
            const std::uint64_t first = std::max(mark.offset, rowStart);
            const std::uint64_t end = std::min(mark.offset + mark.length, rowStart + static_cast<std::uint64_t>(count));
            if (first >= end)
                continue;
            const int c0 = static_cast<int>(first - rowStart);
            const int c1 = static_cast<int>(end - rowStart - 1);
            const QColor& tint = m_colors.marks[m % m_colors.marks.size()];
            const qreal hexLeft = hexCellX(c0) - m_charWidth / 2;
            painter.fillRect(QRectF(hexLeft, top, hexCellX(c1) + 2.5 * m_charWidth - hexLeft, m_lineHeight), tint);
            painter.fillRect(QRectF(textCellX(c0), top, (c1 - c0 + 1) * m_charWidth, m_lineHeight), tint);
        }

        if (hasRange) {
            const std::uint64_t first = std::max(selStart, rowStart);
            const std::uint64_t end = std::min(selEnd, rowStart + static_cast<std::uint64_t>(count));
            if (first < end) {
                const int c0 = static_cast<int>(first - rowStart);
                const int c1 = static_cast<int>(end - rowStart - 1);
                const qreal hexLeft = hexCellX(c0) - m_charWidth / 2;
                painter.fillRect(QRectF(hexLeft, top, hexCellX(c1) + 2.5 * m_charWidth - hexLeft, m_lineHeight),
                                 hexActive ? m_colors.selection : m_colors.inactiveSelection);
                painter.fillRect(QRectF(textCellX(c0), top, (c1 - c0 + 1) * m_charWidth, m_lineHeight),
                                 hexActive ? m_colors.inactiveSelection : m_colors.selection);
            }
        }

        painter.setPen(row == cursorRow ? m_colors.text : m_colors.dim);
        painter.drawText(QPointF(m_charWidth, baseline), formatOffset(rowStart));

        for (int c = 0; c < count; ++c) {
            const std::uint64_t offset = rowStart + static_cast<std::uint64_t>(c);
            const std::uint8_t byte = bytes[offset];
            const bool selected = hasRange && offset >= selStart && offset < selEnd;
            const bool isCursor = offset == m_cursor;
            if (isCursor) {
                paintCursor(QRectF(hexCellX(c) - 1, top, 2 * m_charWidth + 2, m_lineHeight), hexActive);
                paintCursor(QRectF(textCellX(c), top, m_charWidth, m_lineHeight), !hexActive);
            }

            const bool hexInverted = hexActive && (selected || (isCursor && focused));
            painter.setPen(hexInverted ? m_colors.selectionText : byteColor(byte));
            painter.drawText(QPointF(hexCellX(c), baseline), m_hexText[byte]);

            const bool textInverted = !hexActive && (selected || (isCursor && focused));
            painter.setPen(textInverted ? m_colors.selectionText : byteColor(byte));
            painter.drawText(QPointF(textCellX(c), baseline), m_charText[byte]);
        }
    }
}

// Events -----------------------------------------------------------------------------------------

bool HexView::event(QEvent* event)
{
    // Tab switches between the hex and text panes instead of moving focus, as in other hex editors.
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            setActivePane(m_activePane == Pane::Hex ? Pane::Text : Pane::Hex);
            ensureCursorVisible();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

bool HexView::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const FieldMark* mark = size() > 0 && help->pos().y() >= m_headerHeight ? markAt(hitTest(help->pos()).offset) : nullptr;
        if (!mark) {
            QToolTip::hideText();
            return true;
        }
        QString text = QStringLiteral("<b>%1</b><br>0x%2–0x%3 (%4)")
                           .arg(mark->name.toHtmlEscaped(), formatOffset(mark->offset),
                                formatOffset(mark->offset + mark->length - 1),
                                tr("%n byte(s)", nullptr, static_cast<int>(std::min<std::uint64_t>(mark->length, INT_MAX))));
        if (!mark->detail.isEmpty())
            text += QStringLiteral("<br>") + mark->detail.toHtmlEscaped();
        QToolTip::showText(help->globalPos(), text, viewport());
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void HexView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void HexView::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics();
        updateScrollBars();
        viewport()->update();
    } else if (event->type() == QEvent::PaletteChange) {
        updateColors();
        viewport()->update();
    }
}

void HexView::keyPressEvent(QKeyEvent* event)
{
    if (event == QKeySequence::Copy) {
        copy();
        return;
    }
    if (event == QKeySequence::SelectAll) {
        selectAll();
        return;
    }
    if (size() == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    const bool extend = event->modifiers() & Qt::ShiftModifier;
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const auto bpr = static_cast<std::uint64_t>(m_bytesPerRow);
    const std::uint64_t last = size() - 1;
    const auto pageRows = static_cast<std::uint64_t>(visibleRows());
    const std::uint64_t page = bpr * pageRows;
    const std::uint64_t rowStart = m_cursor - m_cursor % bpr;

    std::uint64_t target = m_cursor;
    switch (event->key()) {
    case Qt::Key_Left:
        target = m_cursor > 0 ? m_cursor - 1 : 0;
        break;
    case Qt::Key_Right:
        target = std::min(last, m_cursor + 1);
        break;
    case Qt::Key_Up:
        target = m_cursor >= bpr ? m_cursor - bpr : m_cursor;
        break;
    case Qt::Key_Down:
        // From the second-to-last row, a short last row is reached at its final byte.
        if (last - m_cursor >= bpr)
            target = m_cursor + bpr;
        else if (m_cursor / bpr < last / bpr)
            target = last;
        break;
    case Qt::Key_PageUp:
        target = m_cursor >= page ? m_cursor - page : m_cursor % bpr;
        setTopRow(m_rows.topRow() > pageRows ? m_rows.topRow() - pageRows : 0);
        break;
    case Qt::Key_PageDown:
        target = last - m_cursor >= page ? m_cursor + page : last;
        setTopRow(m_rows.topRow() + pageRows);
        break;
    case Qt::Key_Home:
        target = ctrl ? 0 : rowStart;
        break;
    case Qt::Key_End:
        target = ctrl ? last : std::min(last, rowStart + bpr - 1);
        break;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    moveCursor(target, extend);
}

HexView::HitResult HexView::hitTest(const QPoint& pos, std::optional<Pane> pane) const
{
    const qreal x = pos.x() + horizontalScrollBar()->value();
    const Pane hitPane = pane ? *pane : (x < (hexAreaEnd() + m_textStart) / 2 ? Pane::Hex : Pane::Text);

    int column = 0;
    if (hitPane == Pane::Hex) {
        // A hex cell owns half a character of padding on each side.
        for (int c = 1; c < m_bytesPerRow && x >= hexCellX(c) - m_charWidth / 2; ++c)
            column = c;
    } else {
        column = std::clamp(static_cast<int>(std::floor((x - m_textStart) / m_charWidth)), 0, m_bytesPerRow - 1);
    }

    const auto rowDelta = static_cast<std::int64_t>(std::floor((pos.y() - m_headerHeight) / qreal(m_lineHeight)));
    std::uint64_t row = 0;
    if (rowDelta < 0) {
        const auto up = static_cast<std::uint64_t>(-rowDelta);
        row = m_rows.topRow() > up ? m_rows.topRow() - up : 0;
    } else {
        row = m_rows.topRow() + static_cast<std::uint64_t>(rowDelta);
    }

    const std::uint64_t rows = rowCount();
    if (rows == 0)
        return {hitPane, 0};
    row = std::min(row, rows - 1);
    const std::uint64_t offset = row * static_cast<std::uint64_t>(m_bytesPerRow) + static_cast<std::uint64_t>(column);
    return {hitPane, std::min(offset, size() - 1)};
}

void HexView::mousePressEvent(QMouseEvent* event)
{
    if (size() == 0) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    const HitResult hit = hitTest(event->position().toPoint());

    if (event->button() == Qt::RightButton) {
        // Keep the selection when right-clicking inside it, so the context menu can act on it.
        const bool inside = hit.offset >= selectionStart() && hit.offset < selectionStart() + selectionLength();
        if (!inside) {
            setActivePane(hit.pane);
            moveCursor(hit.offset, false);
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }

    setActivePane(hit.pane);
    moveCursor(hit.offset, event->modifiers() & Qt::ShiftModifier);
    m_dragging = true;
    m_lastDragPos = event->position().toPoint();
}

void HexView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        QAbstractScrollArea::mouseMoveEvent(event);
        return;
    }
    m_lastDragPos = event->position().toPoint();
    dragTo(m_lastDragPos);

    const bool outside = m_lastDragPos.y() < m_headerHeight || m_lastDragPos.y() >= viewport()->height();
    if (outside && !m_autoScrollTimer.isActive())
        m_autoScrollTimer.start(kAutoScrollIntervalMs, this);
    else if (!outside)
        m_autoScrollTimer.stop();
}

void HexView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_autoScrollTimer.stop();
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void HexView::timerEvent(QTimerEvent* event)
{
    if (event->timerId() != m_autoScrollTimer.timerId()) {
        QAbstractScrollArea::timerEvent(event);
        return;
    }
    // Scroll faster the further the pointer is outside the view.
    const int y = m_lastDragPos.y();
    if (y < m_headerHeight) {
        const auto rows = static_cast<std::uint64_t>(1 + (m_headerHeight - y) / m_lineHeight);
        setTopRow(m_rows.topRow() > rows ? m_rows.topRow() - rows : 0);
    } else if (y >= viewport()->height()) {
        setTopRow(m_rows.topRow() + static_cast<std::uint64_t>(1 + (y - viewport()->height()) / m_lineHeight));
    }
    dragTo(m_lastDragPos);
}

void HexView::dragTo(const QPoint& pos)
{
    setCursorAndAnchor(hitTest(pos, m_activePane).offset, m_anchor);
}

void HexView::wheelEvent(QWheelEvent* event)
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

    // Accumulate, so high-resolution touchpads scroll smoothly instead of not at all.
    m_wheelRemainder += delta;
    const int steps = m_wheelRemainder / kWheelStep;
    m_wheelRemainder -= steps * kWheelStep;
    if (steps != 0) {
        const auto rows = static_cast<std::uint64_t>(std::abs(steps) * QApplication::wheelScrollLines());
        if (steps > 0)
            setTopRow(m_rows.topRow() > rows ? m_rows.topRow() - rows : 0);
        else
            setTopRow(m_rows.topRow() + rows);
    }
    event->accept();
}

void HexView::setContextActions(const QList<QAction*>& actions)
{
    m_contextActions.clear();
    for (QAction* action : actions)
        m_contextActions << action;
}

std::unique_ptr<QMenu> HexView::createContextMenu()
{
    const bool hasData = size() > 0;
    auto menu = std::make_unique<QMenu>(this);
    menu->addAction(tr("Copy as Hex"), this, &HexView::copyAsHex)->setEnabled(hasData);
    menu->addAction(tr("Copy as Text"), this, &HexView::copyAsText)->setEnabled(hasData);
    menu->addAction(tr("Copy as C Array"), this, &HexView::copyAsCArray)->setEnabled(hasData);
    menu->addAction(tr("Copy Offset"), this, &HexView::copyOffset)->setEnabled(hasData);
    menu->addSeparator();
    for (QAction* action : m_contextActions) {
        if (action)
            menu->addAction(action);
    }
    menu->addSeparator();
    menu->addAction(tr("Select All"), this, &HexView::selectAll)->setEnabled(hasData);
    if (!m_marks.isEmpty()) {
        menu->addSeparator();
        menu->addAction(tr("Clear Field Marks"), this, &HexView::clearFieldMarks);
    }
    return menu;
}

void HexView::contextMenuEvent(QContextMenuEvent* event)
{
    createContextMenu()->exec(event->globalPos());
}

void HexView::focusInEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusInEvent(event);
    viewport()->update();
}

void HexView::focusOutEvent(QFocusEvent* event)
{
    QAbstractScrollArea::focusOutEvent(event);
    viewport()->update();
}

// Field marks ---------------------------------------------------------------------------------------

void HexView::setFieldMarks(QList<FieldMark> marks)
{
    std::stable_sort(marks.begin(), marks.end(),
                     [](const FieldMark& a, const FieldMark& b) { return a.offset < b.offset; });
    m_marks = std::move(marks);
    viewport()->update();
}

void HexView::clearFieldMarks()
{
    m_marks.clear();
    viewport()->update();
}

const FieldMark* HexView::markAt(std::uint64_t offset) const
{
    // Later (more deeply nested) marks win where marks overlap.
    const FieldMark* found = nullptr;
    for (const FieldMark& mark : m_marks) {
        if (mark.offset > offset)
            break;
        if (offset < mark.offset + mark.length)
            found = &mark;
    }
    return found;
}

// Copying ------------------------------------------------------------------------------------------

QString HexView::selectionAsHex() const
{
    const std::uint64_t length = selectionLength();
    if (length == 0 || length > kMaxCopyBytes)
        return {};
    const auto bytes = m_source->bytes().subspan(selectionStart(), static_cast<std::size_t>(length));
    QString out;
    out.reserve(static_cast<qsizetype>(length * 3));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0)
            out += QLatin1Char(' ');
        out += m_hexText[bytes[i]];
    }
    return out;
}

QString HexView::selectionAsText() const
{
    const std::uint64_t length = selectionLength();
    if (length == 0 || length > kMaxCopyBytes)
        return {};
    const auto bytes = m_source->bytes().subspan(selectionStart(), static_cast<std::size_t>(length));
    if (core::isValidUtf8(bytes))
        return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    QString out;
    out.reserve(static_cast<qsizetype>(length));
    for (const std::uint8_t byte : bytes)
        out += m_charText[byte];
    return out;
}

QString HexView::selectionAsCArray() const
{
    const std::uint64_t length = selectionLength();
    if (length == 0 || length > kMaxCopyBytes)
        return {};
    const auto bytes = m_source->bytes().subspan(selectionStart(), static_cast<std::size_t>(length));
    QString out = QStringLiteral("const unsigned char data[%1] = {").arg(length);
    out.reserve(static_cast<qsizetype>(length * 6 + 64));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out += (i % kCArrayBytesPerLine == 0) ? QStringLiteral("\n    ") : QStringLiteral(" ");
        out += QStringLiteral("0x") + m_hexText[bytes[i]];
        if (i + 1 < bytes.size())
            out += QLatin1Char(',');
    }
    out += QStringLiteral("\n};\n");
    return out;
}

void HexView::copy()
{
    if (m_activePane == Pane::Hex)
        copyAsHex();
    else
        copyAsText();
}

void HexView::copyAsHex()
{
    copyToClipboard(selectionAsHex(), {});
}

void HexView::copyAsText()
{
    copyToClipboard(selectionAsText(), {});
}

void HexView::copyAsCArray()
{
    copyToClipboard(selectionAsCArray(), {});
}

void HexView::copyOffset()
{
    if (size() > 0)
        copyToClipboard(QStringLiteral("0x") + formatOffset(m_cursor), tr("Copied offset"));
}

void HexView::copyToClipboard(const QString& text, const QString& message)
{
    if (text.isEmpty()) {
        if (selectionLength() > kMaxCopyBytes)
            emit statusMessage(tr("The selection is too large to copy (limit: %1)")
                                   .arg(QLocale().formattedDataSize(static_cast<qint64>(kMaxCopyBytes))));
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    emit statusMessage(message.isEmpty()
                           ? tr("Copied %n byte(s)", nullptr, static_cast<int>(selectionLength()))
                           : message);
}

} // namespace aiedit::ui
