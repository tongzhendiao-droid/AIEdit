#pragma once

#include "core/Diff.h"
#include "ui/Compare.h"
#include "ui/RowScrollBar.h"

#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>

#include <cstdint>
#include <memory>
#include <optional>

namespace aiedit::ui {

class TextDecoder;

// Two versions side by side, row by row (see CompareRows): same lines across from each other,
// removed and added ones against a gap, and in changed lines the words that changed. Painted
// directly, decoding only the rows on screen, so that files of any size can be compared. A strip
// on the right shows where in the files the changes are; clicking it goes there.
class CompareView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit CompareView(QWidget* parent = nullptr);
    ~CompareView() override;

    // Shows a comparison. The sides hold the bytes the comparison's indexes were made of.
    void setComparison(const CompareSide& left, const CompareSide& right, const TextComparison& comparison,
                       bool onlyDifferences);
    void clear();
    bool hasComparison() const { return m_left.index != nullptr; }
    // Folds runs of same lines to the few around each change, or shows everything.
    void setOnlyDifferences(bool on);
    const CompareRows& rows() const { return m_rows; }

    std::uint64_t currentRow() const { return m_current; }
    void setCurrentRow(std::uint64_t row);
    // The side worked on: the one clicked last, which copying and opening go to.
    bool isLeftActive() const { return m_leftActive; }
    void setLeftActive(bool left);
    // The change at the current row, or -1.
    int currentHunk() const;
    // Moves to the next or previous change; false if there is none.
    bool goToNextDifference();
    bool goToPreviousDifference();
    // The line a row shows on one side, as copying gives it; empty on a gap or a fold.
    QString rowText(std::uint64_t row, bool left) const;

public slots:
    void copy();

signals:
    // The current row or side changed.
    void currentChanged();
    // Enter or a double click on a line: open its file there.
    void openRequested(bool left, std::uint64_t line);
    void zoomRequested(int steps);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Side
    {
        CompareSide info;
        std::shared_ptr<const core::LineIndex> index;
        std::unique_ptr<TextDecoder> decoder;
        int gutter = 0; // the width of the line numbers
    };

    struct Colors
    {
        QColor base;
        QColor text;
        QColor dim;
        QColor gutter;
        QColor gap;
        QColor gapLines;
        QColor removed;
        QColor removedWords;
        QColor added;
        QColor addedWords;
        QColor fold;
        QColor current;
        QColor selection;
    };

    // A line's text, cached; `cut` says the line goes on past what is shown.
    QString lineText(bool left, std::uint64_t line, bool* cut = nullptr) const;
    const core::InlineDiff& inlineDiff(std::uint64_t leftLine, std::uint64_t rightLine) const;
    void paintSide(QPainter& painter, bool left, const CompareRows::Row& row, const QRect& rect, bool current);
    void paintRuler(QPainter& painter, const QRect& rect) const;
    void rowsChanged();
    void updateMetrics();
    void updateColors();
    void updateGutters();
    void updateScrollBars();
    int visibleRows() const;
    int rulerWidth() const;
    int paneWidth() const;
    int rightPaneLeft() const;
    std::optional<std::uint64_t> rowAt(int y) const;
    bool isOnLeft(int x) const;
    void moveCurrent(std::uint64_t row);
    void ensureVisible(std::uint64_t row, bool center = false);
    void activate();

    Side m_left;
    Side m_right;
    std::vector<core::DiffHunk> m_hunks;
    CompareRows m_rows;
    RowScrollBar m_scroll;
    std::uint64_t m_current = 0;
    bool m_leftActive = true;
    mutable QCache<quint64, QString> m_texts;
    mutable QCache<quint64, core::InlineDiff> m_inline; // by left line
    // For the strip: where each change starts, as a share of all rows, and how it changes.
    struct Mark
    {
        double at;
        CompareRows::Kind kind;
    };
    std::vector<Mark> m_marks;
    qreal m_charWidth = 8;
    int m_lineHeight = 16;
    qreal m_widest = 0;
    int m_wheelRemainder = 0;
    Colors m_colors;
};

} // namespace aiedit::ui
