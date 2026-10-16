#pragma once

#include "core/EditBuffer.h"
#include "core/LineIndex.h"
#include "ui/RowScrollBar.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDecoder.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QColor>
#include <QPointer>
#include <QTextLayout>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class QAction;
class QMenu;

namespace aiedit::ui {

class TextDocument;

// Read-only text view for files of any size.
//
// Only the visible rows are decoded and laid out, with QTextLayout so that CJK, emoji, tabs
// and combining characters render properly. The model is byte offsets: the caret and the
// selection [selectionStart, selectionEnd) are positions in the file, which keeps this view in
// step with the hex view and with search results without any conversion layer.
//
// With word wrap on, a long line takes as many lines on screen as the width needs. Scrolling,
// the caret and the mouse then work in lines on screen; the vertical scroll bar still counts
// rows, which is exact enough for its position and needs no layout of the whole file.
//
// Unless read-only, it edits the document: typing (input methods included), deleting, Enter,
// Tab, cut, paste, undo and redo. Text goes into the file in the file's own codec.
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
    // Puts the caret at offset, and its line in the middle of the screen unless it is on screen
    // already.
    void goToOffset(std::uint64_t offset);

    // The first visible row; with word wrap on, the row in which the first line on screen starts.
    std::uint64_t topRow() const { return m_rows.topRow(); }
    // Where the first line on screen starts, and scrolling so that the line holding an offset is
    // the first one (to come back to a reading position).
    std::uint64_t firstVisibleOffset() const;
    void scrollToOffset(std::uint64_t offset);
    // Lines that fit on screen.
    int visibleRows() const;
    // Read-only by default. `reason` is what the status bar says when typing is refused.
    bool isReadOnly() const { return m_readOnly; }
    void setReadOnly(bool readOnly, const QString& reason = {});
    // Off by default: long lines then scroll sideways.
    // The user's choice; reading mode wraps lines whatever it is.
    bool wordWrap() const { return m_wrapWanted; }
    void setWordWrap(bool on);
    // For reading a book: no line numbers, a column of about 40 characters in the middle,
    // more space between lines, lines wrapped.
    bool isReadingMode() const { return m_readingMode; }
    void setReadingMode(bool on);
    int tabWidth() const { return m_tabWidth; }
    void setTabWidth(int spaces);
    // A KSyntaxHighlighting definition name such as "C++"; empty for plain text.
    QString syntax() const { return m_syntax; }
    void setSyntax(const QString& name);
    // Extra actions for the right-click menu (the AI actions), shown after Copy.
    void setContextActions(const QList<QAction*>& actions);
    // The right-click menu as it would open now; contextMenuEvent() shows it.
    std::unique_ptr<QMenu> createContextMenu();

    // The highlighting formats of a row, as the view draws them (for tests).
    QList<QTextLayout::FormatRange> rowFormats(std::uint64_t rowIndex) const;

    // 0-based line of the caret; its 0-based column in characters when that is cheap to get.
    std::uint64_t cursorLine() const;
    std::optional<std::uint64_t> cursorColumn() const;
    // Where the caret is drawn, in viewport coordinates; null while its line is scrolled out of
    // view.
    QRect cursorRect() const;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

    // The selection as text; empty if nothing is selected or it exceeds kMaxCopyBytes.
    QString selectedText() const;
    // Selects the word at the caret, as double-clicking it does; false if there is none.
    bool selectWordAtCursor();
    // Selects the caret's line, line break included; with whole lines selected, the next one
    // too (Ctrl+L, as in VS Code). A triple click selects a line as well.
    void selectLines();

    // Bookmarked lines, by where each starts, in order. They move with edits and show in the
    // line numbers.
    const std::vector<std::uint64_t>& bookmarks() const { return m_bookmarks; }
    void setBookmarks(std::vector<std::uint64_t> offsets);
    // Bookmarks the caret's line, or takes its bookmark away.
    void toggleBookmark();
    // The caret goes to the next (or previous) bookmarked line, round past the end; false if
    // there is none.
    bool goToBookmark(bool forward);

public slots:
    void selectAll();
    void copy();
    void cut();
    void paste();
    void undo();
    void redo();
    // Replaces the selection, or inserts at the caret, as typing does.
    void insertText(const QString& text);
    // Replaces the selection with text as a step of its own (Find and Replace). False if the
    // document cannot be edited or its codec cannot write the text.
    bool replaceSelectedText(const QString& text);

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
    void inputMethodEvent(QInputMethodEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    // What the view lays out as one QTextLayout: a row, or with word wrap on, the rows of one
    // line (a bounded number of them), so that a line split for length still wraps as one
    // paragraph instead of breaking where the index split it.
    struct Block
    {
        core::Row span; // from the first row's start to the last row's end
        std::uint64_t firstRow = 0;
        std::uint64_t rowCount = 1;
    };

    // A line on screen: one line of a block's layout.
    struct VisualLine
    {
        Block block;
        int line = 0;

        bool operator==(const VisualLine& other) const
        {
            return block.firstRow == other.block.firstRow && line == other.line;
        }
    };

    // A caret position. Where a line on screen goes on in the next one (wrapped, or split for
    // length), the offset between them is both the end of the first and the start of the
    // second; atLineEnd picks the end.
    struct Caret
    {
        std::uint64_t offset = 0;
        bool atLineEnd = false;
    };

    struct BlockLayout
    {
        core::Row span;
        DecodedText decoded;
        QTextLayout layout;
        bool highlighted = false; // laid out with the highlighting formats (if there are any)
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
        QColor bookmark;
    };

    const BlockLayout& layoutFor(const Block& block) const;
    QList<QTextLayout::FormatRange> highlight(const Block& block, const DecodedText& decoded) const;
    void clearLayouts();
    void resetHighlighting();
    // The highlighting state at the start of a row: exact for files small enough to highlight
    // from the top (with checkpoints), approximate (from a fixed number of rows back) otherwise.
    SyntaxState stateBefore(std::uint64_t rowIndex) const;
    void resetForDocument();
    void indexGrew();
    void updateMetrics();
    void updateColors();
    // Wraps lines or not, keeping the text at the top where it was.
    void applyWordWrap(bool on);
    void updateScrollBars();
    void updateHorizontalRange();
    std::uint64_t rowCount() const;
    std::uint64_t maxTopRow() const;
    void setTopRow(std::uint64_t row);
    void ensureCursorVisible();
    void setCursorAndAnchor(std::uint64_t cursor, std::uint64_t anchor);
    void moveCursor(std::uint64_t offset, bool extendSelection, bool keepDesiredX = false);
    void moveCursor(const Caret& caret, bool extendSelection, bool keepDesiredX = false);
    void moveVertically(std::int64_t lines, bool extendSelection);

    // Blocks and lines on screen. All of these need rowCount() > 0.
    Block blockAt(std::uint64_t rowIndex) const;
    // Up to `count` consecutive blocks, the first one starting at row `first`.
    std::vector<Block> blocksFrom(std::uint64_t first, std::size_t count) const;
    int lineCount(const Block& block) const;
    // The line on which a caret at offset shows (at the start of a line, not the end).
    VisualLine visualLineAt(std::uint64_t offset) const;
    // The line on which the caret shows.
    VisualLine caretLine() const;
    VisualLine topLine() const;
    // `from` moved by `delta` lines, stopping at the first and the last line.
    VisualLine moveLines(VisualLine from, std::int64_t delta) const;
    // How far below the top line `line` is, if it is at most `limit` lines.
    std::optional<int> linesFromTop(const VisualLine& line, int limit) const;
    std::uint64_t lineStart(const VisualLine& line) const;
    void scrollToLine(const VisualLine& line);
    void scrollLines(std::int64_t delta);
    void scrollBarAction(int action);
    qreal wrapWidth() const;
    std::uint64_t computeMaxTopOffset() const;

    // Navigation in byte offsets.
    std::uint64_t cursorRowIndex() const;
    std::uint64_t documentEnd() const;
    bool isLastBlock(const Block& block) const;
    // The last offset in a block before the next block starts (for a split block, before its
    // last character).
    std::uint64_t lastCaretOffset(const Block& block) const;
    Caret lineEndCaret(const VisualLine& line) const;
    Caret caretAtX(const VisualLine& line, qreal x) const;
    qreal xOnLine(const VisualLine& line, std::uint64_t offset) const;
    std::uint64_t stepOffset(std::uint64_t offset, bool forward, QTextLayout::CursorMode mode) const;
    Caret caretAt(const QPoint& pos) const;
    void dragTo(const QPoint& pos);
    void selectWordAt(std::uint64_t offset);

    // Editing.
    // False, with a word in the status bar, when the document cannot be edited.
    bool canEdit();
    // Where an edit at offset goes: past the file's last line terminator there is no line on
    // screen, and a caret there shows at the end of the last line, so the edit goes there too.
    std::uint64_t editOffset(std::uint64_t offset) const;
    void replaceSelection(const QString& text, core::EditBuffer::Merge merge);
    bool replaceRange(std::uint64_t start, std::uint64_t end, const QString& text, core::EditBuffer::Merge merge);
    void deleteBackward(bool word);
    void deleteForward(bool word);
    void insertLineBreak();
    // Tab and Shift+Tab on several lines: indents or unindents each.
    void indentLines(bool unindent);
    // What Tab inserts: a tab, or the spaces the file indents with.
    QString indentUnit() const;
    void documentEdited(std::uint64_t offset, std::uint64_t removed);

    int gutterWidth() const;
    qreal textLeft() const;
    // Where the line holding `offset` starts; offsets the index has not reached stay as they are.
    std::uint64_t lineStartAt(std::uint64_t offset) const;
    // Where the next line starts after the one holding `offset`, or the end of the text.
    std::uint64_t lineEndAt(std::uint64_t offset) const;
    // Past the spaces and tabs a line starts with.
    std::uint64_t indentationEnd(std::uint64_t lineStart) const;
    // Bookmarks on the starts of their lines, in order, once each.
    void tidyBookmarks();

    TextDocument* m_document = nullptr;
    RowScrollBar m_rows;
    QList<QPointer<QAction>> m_contextActions;
    std::uint64_t m_cursor = 0;
    std::uint64_t m_anchor = 0;
    bool m_caretAtLineEnd = false;
    bool m_readOnly = true;
    QString m_readOnlyReason;
    QString m_preedit; // what an input method is composing, shown at the caret
    std::optional<qreal> m_desiredX; // kept while moving up and down
    int m_tabWidth = 4;

    bool m_wordWrap = false;   // lines wrap now
    bool m_wrapWanted = false; // the user's choice
    bool m_readingMode = false;
    qreal m_wrapWidth = 0;           // the width wrapped layouts were made for
    std::uint64_t m_topOffset = 0;    // word wrap: where the first line on screen starts
    std::uint64_t m_maxTopOffset = 0; // word wrap: as far as m_topOffset can scroll

    QString m_syntax;
    std::unique_ptr<SyntaxHighlighter> m_highlighter;
    mutable std::vector<SyntaxState> m_stateCheckpoints; // state before row i * kStateCheckpointRows
    mutable std::unordered_map<std::uint64_t, SyntaxState> m_stateAfterRow;

    mutable std::unordered_map<std::uint64_t, std::unique_ptr<BlockLayout>> m_layouts; // by block start
    mutable std::uint64_t m_cachedLayoutBytes = 0;
    // Set while laying out only to measure (where lines wrap), which needs no highlighting.
    mutable bool m_measuring = false;
    mutable qreal m_maxRowWidth = 0;
    qreal m_maxRowWidthInRange = -1; // what the horizontal scroll bar was last sized for

    bool m_dragging = false;
    QPoint m_lastDragPos;
    QElapsedTimer m_lastDoubleClick; // a press soon after is a triple click
    QPoint m_lastDoubleClickPos;
    QBasicTimer m_autoScrollTimer;
    int m_wheelRemainder = 0;
    int m_zoomRemainder = 0;

    qreal m_charWidth = 8;
    int m_lineHeight = 16;
    int m_ascent = 12;
    qreal m_columnWidth = 0; // of the text in reading mode
    Colors m_colors;
    std::vector<std::uint64_t> m_bookmarks;
};

} // namespace aiedit::ui
