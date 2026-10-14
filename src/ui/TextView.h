#pragma once

#include "core/LineIndex.h"
#include "ui/RowScrollBar.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDecoder.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QColor>
#include <QTextLayout>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace aiedit::ui {

class TextDocument;

// Read-only text view for files of any size.
//
// Only the visible rows are decoded and laid out, with QTextLayout so that CJK, emoji, tabs
// and combining characters render properly. The model is byte offsets: the caret and the
// selection [selectionStart, selectionEnd) are positions in the file, which keeps this view in
// step with the hex view and with search results without any conversion layer.
class TextView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    static constexpr std::uint64_t kMaxCopyBytes = 16ull * 1024 * 1024;

    explicit TextView(QWidget* parent = nullptr);
    ~TextView() override;

    // Not owned; may be nullptr.
    void setDocument(TextDocument* document);
    TextDocument* document() const { return m_document; }

    std::uint64_t cursorOffset() const { return m_cursor; }
    std::uint64_t selectionStart() const;
    std::uint64_t selectionEnd() const;
    bool hasSelection() const { return m_cursor != m_anchor; }

    // Moves the caret to the start of the character containing offset and scrolls to it.
    void setCursorOffset(std::uint64_t offset, bool extendSelection = false);
    // Selects [start, end) with the caret at end (both snapped to character starts).
    void select(std::uint64_t start, std::uint64_t end);
    // Puts the caret at the start of a 0-based line and centres it. False if the line has not
    // been indexed (or does not exist).
    bool goToLine(std::uint64_t line);

    std::uint64_t topRow() const { return m_rows.topRow(); }
    int visibleRows() const;
    int tabWidth() const { return m_tabWidth; }
    void setTabWidth(int spaces);
    // A KSyntaxHighlighting definition name such as "C++"; empty for plain text.
    QString syntax() const { return m_syntax; }
    void setSyntax(const QString& name);
    // The highlighting formats of a row, as the view draws them (for tests).
    QList<QTextLayout::FormatRange> rowFormats(std::uint64_t rowIndex) const;

    // 0-based line of the caret; its 0-based column in characters when that is cheap to get.
    std::uint64_t cursorLine() const;
    std::optional<std::uint64_t> cursorColumn() const;

    // The selection as text; empty if nothing is selected or it exceeds kMaxCopyBytes.
    QString selectedText() const;

public slots:
    void selectAll();
    void copy();

signals:
    void cursorMoved(std::uint64_t offset);
    void selectionChanged(std::uint64_t start, std::uint64_t end);
    // Ctrl+wheel; positive steps mean larger text. The owner applies zoom to all its views.
    void zoomRequested(int steps);
    void statusMessage(const QString& message);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void timerEvent(QTimerEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct RowLayout
    {
        core::Row row;
        DecodedText decoded;
        QTextLayout layout;
    };

    struct Colors
    {
        QColor base;
        QColor text;
        QColor dim;
        QColor gutter;
        QColor currentLine;
        QColor selection;
        QColor selectionText;
        QColor inactiveSelection;
        QColor inactiveSelectionText;
    };

    const RowLayout& layoutFor(const core::Row& row) const;
    void clearLayouts();
    void resetHighlighting();
    // The highlighting state at the start of a row: exact for files small enough to highlight
    // from the top (with checkpoints), approximate (from a fixed number of rows back) otherwise.
    SyntaxState stateBefore(std::uint64_t rowIndex) const;
    void resetForDocument();
    void indexGrew();
    void updateMetrics();
    void updateColors();
    void updateScrollBars();
    void updateHorizontalRange();
    std::uint64_t rowCount() const;
    std::uint64_t maxTopRow() const;
    void setTopRow(std::uint64_t row);
    void ensureCursorVisible();
    void setCursorAndAnchor(std::uint64_t cursor, std::uint64_t anchor);
    void moveCursor(std::uint64_t offset, bool extendSelection, bool keepDesiredX = false);
    void moveVertically(std::int64_t rows, bool extendSelection);

    // Navigation in byte offsets.
    std::uint64_t cursorRowIndex() const;
    std::uint64_t documentEnd() const;
    bool isLastRow(const core::Row& row) const;
    // The furthest caret position still shown on this row (a split row's end shows on the next).
    std::uint64_t lastCaretOffset(const core::Row& row) const;
    std::uint64_t offsetAtX(const core::Row& row, qreal x) const;
    qreal xOfOffset(const core::Row& row, std::uint64_t offset) const;
    std::uint64_t stepOffset(std::uint64_t offset, bool forward, QTextLayout::CursorMode mode) const;
    std::uint64_t offsetAt(const QPoint& pos) const;
    void dragTo(const QPoint& pos);
    void selectWordAt(std::uint64_t offset);

    int gutterWidth() const;
    qreal textLeft() const;

    TextDocument* m_document = nullptr;
    RowScrollBar m_rows;
    std::uint64_t m_cursor = 0;
    std::uint64_t m_anchor = 0;
    std::optional<qreal> m_desiredX; // kept while moving up and down
    int m_tabWidth = 4;

    QString m_syntax;
    std::unique_ptr<SyntaxHighlighter> m_highlighter;
    mutable std::vector<SyntaxState> m_stateCheckpoints; // state before row i * kStateCheckpointRows
    mutable std::unordered_map<std::uint64_t, SyntaxState> m_stateAfterRow;

    mutable std::unordered_map<std::uint64_t, std::unique_ptr<RowLayout>> m_layouts; // by row start
    mutable qreal m_maxRowWidth = 0;
    qreal m_maxRowWidthInRange = -1; // what the horizontal scroll bar was last sized for

    bool m_dragging = false;
    QPoint m_lastDragPos;
    QBasicTimer m_autoScrollTimer;
    int m_wheelRemainder = 0;
    int m_zoomRemainder = 0;

    qreal m_charWidth = 8;
    int m_lineHeight = 16;
    int m_ascent = 12;
    Colors m_colors;
};

} // namespace aiedit::ui
