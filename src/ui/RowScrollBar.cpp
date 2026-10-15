#include "ui/RowScrollBar.h"

#include <QScopedValueRollback>
#include <QScrollBar>

#include <algorithm>
#include <cmath>

namespace aiedit::ui {

void RowScrollBar::setRange(std::uint64_t maxTopRow, int pageRows)
{
    const QScopedValueRollback guard(m_movingBar, true);
    m_maxTopRow = maxTopRow;
    m_topRow = std::min(m_topRow, maxTopRow);
    m_scaled = maxTopRow > static_cast<std::uint64_t>(kScaledMaximum);
    if (m_scaled) {
        const double pageFraction = static_cast<double>(pageRows) / static_cast<double>(maxTopRow);
        m_bar->setRange(0, kScaledMaximum);
        m_bar->setPageStep(std::max(1, static_cast<int>(pageFraction * kScaledMaximum)));
    } else {
        m_bar->setRange(0, static_cast<int>(maxTopRow));
        m_bar->setPageStep(std::max(1, pageRows));
    }
    m_bar->setSingleStep(1);
    m_bar->setValue(valueForRow(m_topRow));
}

bool RowScrollBar::setTopRow(std::uint64_t row)
{
    row = std::min(row, m_maxTopRow);
    if (row == m_topRow)
        return false;
    m_topRow = row;
    moveBar();
    return true;
}

bool RowScrollBar::syncFromBar()
{
    if (m_movingBar)
        return false;
    const std::uint64_t row = std::min(rowForValue(m_bar->value()), m_maxTopRow);
    if (row == m_topRow)
        return false;
    m_topRow = row;
    return true;
}

void RowScrollBar::moveBar()
{
    // In scaled mode the bar position is lossy, so it must not feed back into m_topRow.
    const QScopedValueRollback guard(m_movingBar, true);
    m_bar->setValue(valueForRow(m_topRow));
}

int RowScrollBar::valueForRow(std::uint64_t row) const
{
    if (!m_scaled)
        return static_cast<int>(row);
    return m_maxTopRow == 0 ? 0
                            : static_cast<int>(std::llround(static_cast<double>(row) /
                                                            static_cast<double>(m_maxTopRow) * kScaledMaximum));
}

std::uint64_t RowScrollBar::rowForValue(int value) const
{
    value = std::max(0, value);
    if (!m_scaled)
        return static_cast<std::uint64_t>(value);
    return static_cast<std::uint64_t>(
        std::llround(static_cast<double>(value) / kScaledMaximum * static_cast<double>(m_maxTopRow)));
}

} // namespace aiedit::ui
