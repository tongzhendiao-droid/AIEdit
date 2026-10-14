#pragma once

#include <cstdint>

class QScrollBar;

namespace cppedit::ui {

// Drives a QScrollBar from a 64-bit "top row". QScrollBar counts in int, so larger ranges are
// mapped proportionally onto kScaledMaximum steps; the exact row is kept here, not in the bar.
class RowScrollBar
{
public:
    static constexpr int kScaledMaximum = 1 << 30;

    explicit RowScrollBar(QScrollBar* bar) : m_bar(bar) {}

    std::uint64_t topRow() const { return m_topRow; }
    std::uint64_t maxTopRow() const { return m_maxTopRow; }
    bool isScaled() const { return m_scaled; }

    // Sets the scrollable range and clamps the top row into it.
    void setRange(std::uint64_t maxTopRow, int pageRows);
    // Returns whether the top row changed (after clamping).
    bool setTopRow(std::uint64_t row);
    // Call from scrollContentsBy(): adopts the bar's position when the user moved it.
    // Returns whether the top row changed.
    bool syncFromBar();

private:
    int valueForRow(std::uint64_t row) const;
    std::uint64_t rowForValue(int value) const;
    void moveBar();

    QScrollBar* m_bar;
    std::uint64_t m_topRow = 0;
    std::uint64_t m_maxTopRow = 0;
    bool m_scaled = false;
    bool m_movingBar = false; // the bar is being moved by us, not the user
};

} // namespace cppedit::ui
