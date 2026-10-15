#pragma once

#include "ui/LogFilter.h"

#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>
#include <QRegularExpression>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace aiedit::ui {

class TextDocument;

// The lines a filter let through, each with its line number, coloured by log level and with
// what the rule matched marked. Painted directly rather than through an item model, so that
// millions of results cost no more than a few: only the rows on screen are ever decoded.
class FilterResultsView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit FilterResultsView(QWidget* parent = nullptr);
    ~FilterResultsView() override;

    void setDocument(TextDocument* document);
    // Marks what these conditions match (the inclusions) on every line shown.
    void setHighlights(const FilterRule& rule);

    const std::vector<FilterMatch>& matches() const { return m_matches; }
    int count() const { return static_cast<int>(m_matches.size()); }
    void append(const std::vector<FilterMatch>& batch);
    // Drops the results on `line` and after it.
    void truncate(std::uint64_t line);
    // Drops everything. The line that was current is selected again if a later run finds it.
    void clear();
    // The document's text changed: decode the lines again.
    void refresh();

    int currentResult() const { return m_current; }
    void setCurrentResult(int result, bool extendSelection = false);
    // First and last selected result; empty without results.
    std::optional<std::pair<int, int>> selection() const;
    // What a result shows: the start of its line, control characters made visible.
    QString displayText(int result) const;
    // A result's whole line, as copying gives it.
    QString lineText(int result) const;
    // The selected lines, each ended with a line break.
    QString selectedText() const;

public slots:
    void copy();
    void selectAll();

signals:
    // The current result changed by keyboard or mouse: show its line.
    void currentLineChanged(std::uint64_t line);
    // Enter or a double click: go to the line and work there.
    void activated(std::uint64_t line);
    void statusMessage(const QString& message);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Entry
    {
        QString text;
        LogLevelSpan level;
        qreal width = 0;
    };

    const Entry* entry(int result) const;
    void updateMetrics();
    void updateColors();
    void updateScrollBars();
    void ensureVisible(int result);
    void moveCurrent(int result, bool extendSelection);
    int resultAt(int y) const;
    int visibleRows() const;
    int gutterWidth() const;

    TextDocument* m_document = nullptr;
    std::vector<FilterMatch> m_matches;
    std::vector<QRegularExpression> m_highlights;
    mutable QCache<int, Entry> m_cache;
    mutable qreal m_widest = 0; // of the lines painted so far, for the horizontal scroll range
    int m_current = -1;
    int m_anchor = -1;
    std::optional<std::uint64_t> m_wantedLine; // current before a clear(), to find again

    int m_lineHeight = 0;
    qreal m_charWidth = 0;
    struct Colors
    {
        QColor text;
        QColor dim;
        QColor gutter;
        QColor selection;
        QColor selectionText;
        QColor inactiveSelection;
        QColor match;
        QColor error;
        QColor warning;
    } m_colors;
};

} // namespace aiedit::ui
