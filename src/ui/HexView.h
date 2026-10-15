#pragma once

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QColor>
#include <QList>
#include <QPointer>
#include <QString>

#include "ui/FieldMark.h"
#include "ui/RowScrollBar.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

class QAction;
class QMenu;

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

// Read-only hex view with three columns: offset | hex bytes | characters.
//
// It paints only the visible rows, straight from the (memory-mapped) file, so scrolling and
// painting cost the same for any file size. Row positions are 64-bit; the scroll bar is
// scaled when a file has more rows than an int can count.
//
// Selection model: the cursor is always on a byte and the selection always includes it, so
// the selection is the inclusive range between the anchor and the cursor.
class HexView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    enum class Pane
    {
        Hex,
        Text,
    };

    // Copying more than this would freeze the UI building a huge string.
    static constexpr std::uint64_t kMaxCopyBytes = 16ull * 1024 * 1024;
    static constexpr int kMinBytesPerRow = 1;
    static constexpr int kMaxBytesPerRow = 64;

    explicit HexView(QWidget* parent = nullptr);
    ~HexView() override;

    void setSource(std::shared_ptr<const core::FileSource> source);
    // Like setSource(), but keeps the cursor, selection and scroll position where they still fit.
    void replaceSource(std::shared_ptr<const core::FileSource> source);
    const std::shared_ptr<const core::FileSource>& source() const { return m_source; }
    std::uint64_t size() const;

    int bytesPerRow() const { return m_bytesPerRow; }
    void setBytesPerRow(int bytesPerRow);

    std::uint64_t cursorOffset() const { return m_cursor; }
    std::uint64_t selectionStart() const;
    std::uint64_t selectionLength() const;
    Pane activePane() const { return m_activePane; }
    void setActivePane(Pane pane);

    // Moves the cursor (clamped to the file) and scrolls just enough to show it.
    // Without extendSelection the selection collapses to the cursor byte.
    void setCursorOffset(std::uint64_t offset, bool extendSelection = false);
    // Like setCursorOffset(), but centres the row when the target is off screen.
    void goTo(std::uint64_t offset);
    // Selects [start, start + length) and puts the cursor on its last byte.
    void select(std::uint64_t start, std::uint64_t length);

    std::uint64_t topRow() const { return m_rows.topRow(); }
    int visibleRows() const;
    // Wide enough for every column, so a split view can give the hex view exactly that.
    QSize sizeHint() const override;

    // Coloured ranges with names, e.g. fields the AI assistant worked out. Hovering shows them.
    void setFieldMarks(QList<FieldMark> marks);
    const QList<FieldMark>& fieldMarks() const { return m_marks; }
    void clearFieldMarks();

    // Extra actions for the right-click menu (the AI actions), shown after the copy actions.
    void setContextActions(const QList<QAction*>& actions);
    // The right-click menu as it would open now; contextMenuEvent() shows it.
    std::unique_ptr<QMenu> createContextMenu();

    // Text for the selection in different formats; empty if the selection exceeds kMaxCopyBytes.
    QString selectionAsHex() const;
    QString selectionAsText() const; // UTF-8 when valid, else printable ASCII with '.' for the rest
    QString selectionAsCArray() const;

public slots:
    void selectAll();
    void copy(); // in the format of the active pane
    void copyAsHex();
    void copyAsText();
    void copyAsCArray();
    void copyOffset();

signals:
    void cursorMoved(std::uint64_t offset);
    void selectionChanged(std::uint64_t start, std::uint64_t length);
    // Ctrl+wheel; positive steps mean larger text. The owner applies zoom to all its views.
    void zoomRequested(int steps);
    void statusMessage(const QString& message);

protected:
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void timerEvent(QTimerEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct HitResult
    {
        Pane pane;
        std::uint64_t offset;
    };

    struct Colors
    {
        QColor base;
        QColor text;
        QColor dim;
        QColor whitespace;
        QColor control;
        QColor highByte;
        QColor currentRow;
        QColor selection;
        QColor selectionText;
        QColor inactiveSelection;
        QColor separator;
        QList<QColor> marks; // field mark tints, used in turn
    };

    void updateMetrics();
    void updateColors();
    void updateScrollBars();
    std::uint64_t rowCount() const;
    std::uint64_t maxTopRow() const;
    void setTopRow(std::uint64_t row);
    void ensureCursorVisible();
    void moveCursor(std::uint64_t offset, bool extendSelection);
    void setCursorAndAnchor(std::uint64_t cursor, std::uint64_t anchor);
    // With `pane` set, the column is computed within that pane wherever the point is (dragging).
    HitResult hitTest(const QPoint& pos, std::optional<Pane> pane = std::nullopt) const;
    void dragTo(const QPoint& pos);
    void copyToClipboard(const QString& text, const QString& message);
    QString formatOffset(std::uint64_t offset) const;
    const FieldMark* markAt(std::uint64_t offset) const;

    qreal hexCellX(int column) const;
    qreal textCellX(int column) const;
    qreal hexAreaEnd() const;
    qreal contentWidth() const;
    const QColor& byteColor(std::uint8_t byte) const;

    std::shared_ptr<const core::FileSource> m_source;
    int m_bytesPerRow = 16;

    std::uint64_t m_cursor = 0;
    std::uint64_t m_anchor = 0;
    Pane m_activePane = Pane::Hex;

    RowScrollBar m_rows;
    int m_wheelRemainder = 0;
    int m_zoomRemainder = 0;

    bool m_dragging = false;
    QPoint m_lastDragPos;
    QBasicTimer m_autoScrollTimer;

    // Layout, in pixels.
    qreal m_charWidth = 8;
    int m_lineHeight = 16;
    int m_ascent = 12;
    int m_headerHeight = 20;
    int m_offsetDigits = 8;
    qreal m_hexStart = 0;
    qreal m_textStart = 0;

    Colors m_colors;
    QList<FieldMark> m_marks; // sorted by offset
    QList<QPointer<QAction>> m_contextActions;
    std::array<QString, 256> m_hexText;
    std::array<QString, 256> m_charText;
};

} // namespace aiedit::ui
