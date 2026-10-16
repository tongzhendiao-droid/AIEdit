#include "ui/HexCompareView.h"

#include "core/FileSource.h"
#include "ui/SyntaxHighlighting.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>
#include <cmath>

namespace aiedit::ui {

namespace {

constexpr int kDividerWidth = 6;
constexpr int kRulerWidth = 12;
constexpr int kWheelRows = 3;
constexpr int kPadding = 1; // characters around columns

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

char hexDigit(int value)
{
    return "0123456789ABCDEF"[value & 0xF];
}

} // namespace

HexCompareView::HexCompareView(QWidget* parent) : QAbstractScrollArea(parent), m_scroll(verticalScrollBar())
{
    setFocusPolicy(Qt::StrongFocus);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    horizontalScrollBar()->setRange(0, 0);
    updateMetrics();
    updateColors();
}

HexCompareView::~HexCompareView() = default;

void HexCompareView::setComparison(const CompareSide& left, const CompareSide& right,
                                   const core::ByteComparison& comparison)
{
    m_left = left.source;
    m_right = right.source;
    m_comparison = comparison;
    std::uint64_t largest = std::max<std::uint64_t>(1, size());
    m_offsetDigits = 0;
    for (; largest > 0; largest >>= 4)
        ++m_offsetDigits;
    m_offsetDigits = std::max(8, m_offsetDigits);
    m_scroll.setTopRow(0);
    updateLayout();
    if (!m_comparison.ranges.empty())
        setCurrentRange(0);
    else
        setCursorOffset(0);
}

void HexCompareView::clear()
{
    m_left.reset();
    m_right.reset();
    m_comparison = {};
    m_range = -1;
    m_cursor = 0;
    updateLayout();
}

void HexCompareView::setCurrentRange(int range)
{
    if (range < 0 || range >= static_cast<int>(m_comparison.ranges.size()))
        return;
    m_range = range;
    m_cursor = m_comparison.ranges[static_cast<std::size_t>(range)].start;
    ensureVisible(m_cursor, true);
    viewport()->update();
    emit currentChanged();
}

bool HexCompareView::goToNextDifference()
{
    const auto& ranges = m_comparison.ranges;
    // The first range starting after the cursor.
    const auto next = std::upper_bound(ranges.begin(), ranges.end(), m_cursor,
                                       [](std::uint64_t offset, const core::ByteRange& r) { return offset < r.start; });
    if (next == ranges.end())
        return false;
    setCurrentRange(static_cast<int>(next - ranges.begin()));
    return true;
}

bool HexCompareView::goToPreviousDifference()
{
    const auto& ranges = m_comparison.ranges;
    const auto at = std::lower_bound(ranges.begin(), ranges.end(), m_cursor,
                                     [](const core::ByteRange& r, std::uint64_t offset) { return r.start < offset; });
    if (at == ranges.begin())
        return false;
    // From inside a range, its own start; from its start, the one before.
    setCurrentRange(static_cast<int>(at - ranges.begin()) - 1);
    return true;
}

void HexCompareView::setCursorOffset(std::uint64_t offset)
{
    if (!hasComparison())
        return;
    m_cursor = std::min(offset, size() == 0 ? 0 : size() - 1);
    m_range = rangeAt(m_cursor);
    viewport()->update();
    emit currentChanged();
}

int HexCompareView::rangeAt(std::uint64_t offset) const
{
    const auto& ranges = m_comparison.ranges;
    const auto after = std::upper_bound(ranges.begin(), ranges.end(), offset,
                                        [](std::uint64_t o, const core::ByteRange& r) { return o < r.start; });
    if (after == ranges.begin())
        return -1;
    const auto range = after - 1;
    return offset < range->end() ? static_cast<int>(range - ranges.begin()) : -1;
}

bool HexCompareView::differs(std::uint64_t offset) const
{
    const auto left = m_left->bytes();
    const auto right = m_right->bytes();
    if (offset >= left.size() || offset >= right.size())
        return true;
    return left[static_cast<std::size_t>(offset)] != right[static_cast<std::size_t>(offset)];
}

// Geometry -----------------------------------------------------------------------------------------

std::uint64_t HexCompareView::size() const
{
    if (!hasComparison())
        return 0;
    return std::max(m_comparison.leftSize, m_comparison.rightSize);
}

std::uint64_t HexCompareView::rowCount() const
{
    return (size() + static_cast<std::uint64_t>(m_bytesPerRow) - 1) / static_cast<std::uint64_t>(m_bytesPerRow);
}

int HexCompareView::visibleRows() const
{
    return std::max(1, viewport()->height() / std::max(1, m_lineHeight));
}

int HexCompareView::rulerWidth() const
{
    return kRulerWidth;
}

qreal HexCompareView::offsetWidth() const
{
    return (m_offsetDigits + 2 * kPadding) * m_charWidth;
}

qreal HexCompareView::sideWidth() const
{
    // "XX " for each byte, a space between halves, then the characters.
    return (m_bytesPerRow * 3 + 1 + 2 * kPadding + m_bytesPerRow + kPadding) * m_charWidth;
}

HexCompareView::Columns HexCompareView::columns(bool left) const
{
    const qreal start = offsetWidth() + (left ? 0 : sideWidth() + kDividerWidth);
    const qreal hex = start + kPadding * m_charWidth;
    return {hex, hex + (m_bytesPerRow * 3 + 1 + kPadding) * m_charWidth};
}

std::optional<std::pair<std::uint64_t, bool>> HexCompareView::byteAt(const QPoint& pos) const
{
    if (!hasComparison() || pos.y() < 0)
        return std::nullopt;
    const std::uint64_t row = m_scroll.topRow() + static_cast<std::uint64_t>(pos.y() / m_lineHeight);
    if (row >= rowCount())
        return std::nullopt;
    for (const bool left : {true, false}) {
        const Columns c = columns(left);
        const qreal x = pos.x();
        int column = -1;
        if (x >= c.hex && x < c.hex + m_bytesPerRow * 3 * m_charWidth + m_charWidth) {
            const qreal into = x - c.hex;
            const int half = m_bytesPerRow / 2;
            column = static_cast<int>(into / (3 * m_charWidth));
            if (column >= half) // past the space between the halves
                column = static_cast<int>((into - m_charWidth) / (3 * m_charWidth));
        } else if (x >= c.chars && x < c.chars + m_bytesPerRow * m_charWidth) {
            column = static_cast<int>((x - c.chars) / m_charWidth);
        }
        if (column >= 0 && column < m_bytesPerRow) {
            const std::uint64_t offset = row * static_cast<std::uint64_t>(m_bytesPerRow) + static_cast<std::uint64_t>(column);
            if (offset < size())
                return std::pair{offset, left};
        }
    }
    return std::nullopt;
}

// Painting -----------------------------------------------------------------------------------------

void HexCompareView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), m_colors.base);
    const QRect area = viewport()->rect();
    painter.fillRect(QRectF(0, 0, offsetWidth(), area.height()), m_colors.gutter);
    const qreal divider = offsetWidth() + sideWidth();
    painter.fillRect(QRectF(divider, 0, kDividerWidth, area.height()), m_colors.gutter);
    const QRect ruler(area.width() - rulerWidth(), 0, rulerWidth(), area.height());
    painter.fillRect(ruler, m_colors.gutter);
    if (!hasComparison())
        return;

    const std::uint64_t rows = rowCount();
    for (int y = 0; y < area.height(); y += m_lineHeight) {
        const std::uint64_t row = m_scroll.topRow() + static_cast<std::uint64_t>(y / m_lineHeight);
        if (row >= rows)
            break;
        const std::uint64_t rowOffset = row * static_cast<std::uint64_t>(m_bytesPerRow);
        QString offset(m_offsetDigits, QLatin1Char('0'));
        for (int i = 0; i < m_offsetDigits; ++i)
            offset[m_offsetDigits - 1 - i] = QLatin1Char(hexDigit(static_cast<int>(rowOffset >> (4 * i))));
        painter.setPen(m_colors.dim);
        painter.drawText(QRectF(kPadding * m_charWidth, y, m_offsetDigits * m_charWidth, m_lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, offset);
        paintSide(painter, true, rowOffset, y);
        paintSide(painter, false, rowOffset, y);
    }

    // The strip: where the differences are, and where the view is.
    const double total = static_cast<double>(std::max<std::uint64_t>(1, size()));
    int lastY = -1;
    for (const core::ByteRange& range : m_comparison.ranges) {
        const int y = ruler.top() + static_cast<int>(static_cast<double>(range.start) / total * ruler.height());
        if (y == lastY)
            continue;
        painter.fillRect(QRect(ruler.left() + 2, y, ruler.width() - 4, 3), mix(m_colors.removed, m_colors.added, 0.5f));
        lastY = y;
    }
    const double scale = static_cast<double>(ruler.height()) / static_cast<double>(std::max<std::uint64_t>(1, rows));
    QColor veil = m_colors.text;
    veil.setAlpha(40);
    painter.fillRect(QRect(ruler.left(), static_cast<int>(static_cast<double>(m_scroll.topRow()) * scale), ruler.width(),
                           std::max(4, static_cast<int>(visibleRows() * scale))),
                     veil);
}

void HexCompareView::paintSide(QPainter& painter, bool left, std::uint64_t rowOffset, int y)
{
    const auto bytes = (left ? m_left : m_right)->bytes();
    const Columns c = columns(left);
    const QColor marked = left ? m_colors.removed : m_colors.added;
    const core::ByteRange* current =
        m_range >= 0 ? &m_comparison.ranges[static_cast<std::size_t>(m_range)] : nullptr;
    for (int i = 0; i < m_bytesPerRow; ++i) {
        const std::uint64_t offset = rowOffset + static_cast<std::uint64_t>(i);
        if (offset >= bytes.size())
            break;
        const std::uint8_t byte = bytes[static_cast<std::size_t>(offset)];
        const qreal hexX = c.hex + (i * 3 + (i >= m_bytesPerRow / 2 ? 1 : 0)) * m_charWidth;
        const qreal charX = c.chars + i * m_charWidth;
        const QRectF hexCell(hexX, y, 2 * m_charWidth, m_lineHeight);
        const QRectF charCell(charX, y, m_charWidth, m_lineHeight);
        if (differs(offset)) {
            painter.fillRect(hexCell.adjusted(-1, 0, 1, 0), marked);
            painter.fillRect(charCell, marked);
        }
        if (offset == m_cursor) {
            painter.setPen(left == m_leftActive ? m_colors.current : mix(m_colors.base, m_colors.current, 0.45f));
            painter.drawRect(hexCell.adjusted(-1, 0, 0, -1));
            painter.drawRect(charCell.adjusted(0, 0, -1, -1));
        } else if (current && offset >= current->start && offset < current->end()) {
            // The difference worked on: underlined.
            painter.fillRect(QRectF(hexX - 1, y + m_lineHeight - 2, 2 * m_charWidth + 2, 2), m_colors.current);
        }
        const char digits[] = {hexDigit(byte >> 4), hexDigit(byte), '\0'};
        painter.setPen(m_colors.text);
        painter.drawText(hexCell, Qt::AlignCenter, QString::fromLatin1(digits));
        const QChar shown = byte >= 0x20 && byte < 0x7F ? QChar(byte) : QChar(u'.');
        painter.setPen(byte >= 0x20 && byte < 0x7F ? m_colors.text : m_colors.dim);
        painter.drawText(charCell, Qt::AlignCenter, QString(shown));
    }
}

// Events -------------------------------------------------------------------------------------------

void HexCompareView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateLayout();
}

void HexCompareView::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics();
        updateLayout();
    } else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        updateColors();
    }
    viewport()->update();
}

void HexCompareView::keyPressEvent(QKeyEvent* event)
{
    if (!hasComparison() || size() == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const auto row = static_cast<std::uint64_t>(m_bytesPerRow);
    const std::uint64_t page = row * static_cast<std::uint64_t>(visibleRows());
    const std::uint64_t last = size() - 1;
    switch (event->key()) {
    case Qt::Key_Left:
        moveCursor(m_cursor == 0 ? 0 : m_cursor - 1);
        return;
    case Qt::Key_Right:
        moveCursor(std::min(last, m_cursor + 1));
        return;
    case Qt::Key_Up:
        moveCursor(m_cursor - std::min(m_cursor, row));
        return;
    case Qt::Key_Down:
        moveCursor(std::min(last, m_cursor + row));
        return;
    case Qt::Key_PageUp:
        moveCursor(m_cursor - std::min(m_cursor, page));
        return;
    case Qt::Key_PageDown:
        moveCursor(std::min(last, m_cursor + page));
        return;
    case Qt::Key_Home:
        moveCursor(0);
        return;
    case Qt::Key_End:
        moveCursor(last);
        return;
    case Qt::Key_Tab:
        m_leftActive = !m_leftActive;
        viewport()->update();
        emit currentChanged();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        emit openRequested(m_leftActive, m_cursor);
        return;
    default:
        QAbstractScrollArea::keyPressEvent(event);
    }
}

void HexCompareView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->position().toPoint();
    if (pos.x() >= viewport()->width() - rulerWidth()) {
        const double share = std::clamp(static_cast<double>(pos.y()) / std::max(1, viewport()->height()), 0.0, 1.0);
        ensureVisible(static_cast<std::uint64_t>(share * static_cast<double>(size())), true);
        return;
    }
    if (const auto byte = byteAt(pos)) {
        m_leftActive = byte->second;
        setCursorOffset(byte->first);
    }
}

void HexCompareView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (const auto byte = byteAt(event->position().toPoint())) {
        m_leftActive = byte->second;
        setCursorOffset(byte->first);
        emit openRequested(m_leftActive, m_cursor);
    }
}

void HexCompareView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        m_wheelRemainder += event->angleDelta().y();
        const int steps = m_wheelRemainder / 120;
        m_wheelRemainder -= steps * 120;
        if (steps != 0)
            emit zoomRequested(steps);
        event->accept();
        return;
    }
    const int notches = event->angleDelta().y() / 120;
    if (notches != 0) {
        const std::uint64_t top = m_scroll.topRow();
        const auto delta = static_cast<std::uint64_t>(std::abs(notches) * kWheelRows);
        if (m_scroll.setTopRow(notches > 0 ? top - std::min(top, delta) : top + delta))
            viewport()->update();
    }
    event->accept();
}

void HexCompareView::scrollContentsBy(int, int dy)
{
    if (dy != 0)
        m_scroll.syncFromBar();
    viewport()->update();
}

// Layout -------------------------------------------------------------------------------------------

void HexCompareView::moveCursor(std::uint64_t offset)
{
    setCursorOffset(offset);
    ensureVisible(m_cursor);
}

void HexCompareView::ensureVisible(std::uint64_t offset, bool center)
{
    const std::uint64_t row = offset / static_cast<std::uint64_t>(m_bytesPerRow);
    const std::uint64_t top = m_scroll.topRow();
    const auto rows = static_cast<std::uint64_t>(visibleRows());
    std::uint64_t target = top;
    if (center)
        target = row - std::min(row, rows / 3);
    else if (row < top)
        target = row;
    else if (row >= top + rows)
        target = row - rows + 1;
    if (m_scroll.setTopRow(target))
        viewport()->update();
}

void HexCompareView::updateLayout()
{
    // Sixteen bytes a row when they fit side by side; eight in a narrow window.
    const std::uint64_t cursorRowStart = m_cursor;
    m_bytesPerRow = 16;
    const auto needed = [this] { return offsetWidth() + 2 * sideWidth() + kDividerWidth + rulerWidth(); };
    if (needed() > viewport()->width())
        m_bytesPerRow = 8;
    const std::uint64_t rows = rowCount();
    const auto page = static_cast<std::uint64_t>(visibleRows());
    m_scroll.setRange(rows > page ? rows - page : 0, visibleRows());
    ensureVisible(cursorRowStart);
    viewport()->update();
}

void HexCompareView::updateMetrics()
{
    const QFontMetricsF metrics(font());
    m_charWidth = metrics.horizontalAdvance(QLatin1Char('0'));
    m_lineHeight = static_cast<int>(std::ceil(metrics.lineSpacing())) + 2;
}

void HexCompareView::updateColors()
{
    const EditorColors colors = editorColors(palette());
    const QColor base = colors.background;
    const bool dark = colors.dark;
    m_colors.base = base;
    m_colors.text = colors.text;
    m_colors.dim = colors.lineNumber;
    m_colors.gutter = colors.gutter;
    const QColor red = dark ? QColor(0xe5, 0x53, 0x4b) : QColor(0xd7, 0x3a, 0x49);
    const QColor green = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x28, 0xa7, 0x45);
    m_colors.removed = mix(base, red, dark ? 0.45f : 0.32f);
    m_colors.added = mix(base, green, dark ? 0.42f : 0.30f);
    m_colors.current = colors.selection;
}

} // namespace aiedit::ui
