#pragma once

#include "core/ByteDiff.h"
#include "ui/Compare.h"
#include "ui/RowScrollBar.h"

#include <QAbstractScrollArea>
#include <QColor>

#include <cstdint>
#include <memory>

namespace aiedit::ui {

// Two binary files side by side in hex, the bytes that differ marked: at the same offsets, or,
// where bytes were inserted or removed, lined up again after them, the side without those bytes
// left blank and each side with its own offsets (M11). The cursor and the ranges are of
// positions in that layout (offsets, when not lined up). Moving from difference to difference
// follows the ranges of a core::ByteComparison; a strip on the right shows where they are. Only
// the rows on screen are drawn, so size does not matter.
class HexCompareView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit HexCompareView(QWidget* parent = nullptr);
    ~HexCompareView() override;

    void setComparison(const CompareSide& left, const CompareSide& right, const core::ByteComparison& comparison);
    void clear();
    bool hasComparison() const { return m_left != nullptr; }
    const core::ByteComparison& comparison() const { return m_comparison; }

    // The difference (range) at the cursor, or -1.
    int currentRange() const { return m_range; }
    void setCurrentRange(int range);
    bool goToNextDifference();
    bool goToPreviousDifference();
    // The position the cursor is on, and the side worked on (clicked last).
    std::uint64_t cursorOffset() const { return m_cursor; }
    void setCursorOffset(std::uint64_t offset);
    // The offset in a side's file at a position: its byte there, or where its bytes resume.
    std::uint64_t sideOffset(bool left, std::uint64_t position) const;
    bool isLinedUp() const { return !m_comparison.segments.empty(); }
    bool isLeftActive() const { return m_leftActive; }
    int bytesPerRow() const { return m_bytesPerRow; }

signals:
    void currentChanged();
    // Enter or a double click: open that side's file at the byte (an offset in that file).
    void openRequested(bool left, std::uint64_t offset);
    void zoomRequested(int steps);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Colors
    {
        QColor base;
        QColor text;
        QColor dim;
        QColor gutter;
        QColor removed;
        QColor added;
        QColor current;
    };
    // Where a side's columns start: its hex and its characters.
    struct Columns
    {
        qreal hex;
        qreal chars;
    };

    std::uint64_t size() const; // of the longer side
    std::uint64_t rowCount() const;
    int visibleRows() const;
    int rulerWidth() const;
    qreal offsetWidth() const;
    Columns columns(bool left) const;
    qreal sideWidth() const;
    // The byte under a point, and on which side; nullopt between columns.
    std::optional<std::pair<std::uint64_t, bool>> byteAt(const QPoint& pos) const;
    int rangeAt(std::uint64_t offset) const;
    bool differs(std::uint64_t offset) const;
    void paintSide(QPainter& painter, bool left, std::uint64_t rowOffset, int y);
    void updateMetrics();
    void updateColors();
    void updateLayout();
    void ensureVisible(std::uint64_t offset, bool center = false);
    void moveCursor(std::uint64_t offset);

    std::shared_ptr<const core::FileSource> m_left;
    std::shared_ptr<const core::FileSource> m_right;
    core::ByteComparison m_comparison;
    RowScrollBar m_scroll;
    std::uint64_t m_cursor = 0;
    int m_range = -1;
    bool m_leftActive = true;
    int m_bytesPerRow = 16;
    int m_offsetDigits = 8;
    qreal m_charWidth = 8;
    int m_lineHeight = 16;
    int m_wheelRemainder = 0;
    Colors m_colors;
};

} // namespace aiedit::ui
