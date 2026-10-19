#include "ui/FilterResultsView.h"

#include "core/LineIndex.h"
#include "ui/Desktop.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDecoder.h"
#include "ui/TextDocument.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace aiedit::ui {

namespace {

constexpr int kCachedLines = 2000;
constexpr int kMaxCopiedLines = 200'000;
constexpr std::size_t kMaxRowsPerLine = 256; // 1 MiB: what a copied line is cut to
constexpr int kMaxMarksPerLine = 64;
constexpr int kTabWidth = 4;

bool lineBefore(const FilterMatch& match, std::uint64_t line)
{
    return match.line < line;
}

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

} // namespace

FilterResultsView::FilterResultsView(QWidget* parent) : QAbstractScrollArea(parent), m_cache(kCachedLines)
{
    setFocusPolicy(Qt::StrongFocus);
    setFont(fixedFont());
    updateMetrics();
    updateColors();
    updateScrollBars();
}

FilterResultsView::~FilterResultsView() = default;

void FilterResultsView::setDocument(TextDocument* document)
{
    if (m_document)
        disconnect(m_document, nullptr, this, nullptr);
    m_document = document;
    if (m_document)
        connect(m_document, &QObject::destroyed, this, [this] { m_document = nullptr; });
    refresh();
}

void FilterResultsView::setHighlights(const FilterRule& rule)
{
    m_highlights.clear();
    for (const FilterCondition& condition : rule.conditions) {
        if (condition.exclude)
            continue; // shown lines never match these
        QRegularExpression regex = condition.regex();
        if (regex.isValid())
            m_highlights.push_back(std::move(regex));
    }
    viewport()->update();
}

void FilterResultsView::append(const std::vector<FilterMatch>& batch)
{
    if (batch.empty())
        return;
    QScrollBar* vertical = verticalScrollBar();
    // Scrolled to the end of a list longer than the view: stay there as a growing log adds more.
    const bool atEnd = vertical->maximum() > 0 && vertical->value() == vertical->maximum();
    const std::size_t first = m_matches.size();
    m_matches.insert(m_matches.end(), batch.begin(), batch.end());

    if (m_wantedLine) {
        const auto found = std::lower_bound(m_matches.begin() + static_cast<std::ptrdiff_t>(first), m_matches.end(),
                                            *m_wantedLine, lineBefore);
        if (found != m_matches.end()) {
            m_current = m_anchor = static_cast<int>(found - m_matches.begin());
            m_wantedLine.reset();
            updateScrollBars();
            ensureVisible(m_current);
            viewport()->update();
            return;
        }
    }
    updateScrollBars();
    if (atEnd)
        vertical->setValue(vertical->maximum());
    viewport()->update();
}

void FilterResultsView::truncate(std::uint64_t line)
{
    const auto from = std::lower_bound(m_matches.begin(), m_matches.end(), line, lineBefore);
    if (from == m_matches.end())
        return;
    m_matches.erase(from, m_matches.end());
    if (m_current >= count())
        m_current = m_anchor = count() - 1;
    m_anchor = std::min(m_anchor, count() - 1);
    m_cache.clear();
    updateScrollBars();
    viewport()->update();
}

void FilterResultsView::clear()
{
    if (m_current >= 0 && !m_wantedLine)
        m_wantedLine = m_matches[static_cast<std::size_t>(m_current)].line;
    m_matches.clear();
    m_current = m_anchor = -1;
    m_cache.clear();
    m_widest = 0;
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    updateScrollBars();
    viewport()->update();
}

void FilterResultsView::refresh()
{
    m_cache.clear();
    m_widest = 0;
    updateScrollBars();
    viewport()->update();
}

void FilterResultsView::setCurrentResult(int result, bool extendSelection)
{
    if (count() == 0)
        return;
    m_current = std::clamp(result, 0, count() - 1);
    if (!extendSelection || m_anchor < 0)
        m_anchor = m_current;
    m_wantedLine.reset();
    ensureVisible(m_current);
    viewport()->update();
}

std::optional<std::pair<int, int>> FilterResultsView::selection() const
{
    if (m_current < 0)
        return std::nullopt;
    return std::pair(std::min(m_anchor, m_current), std::max(m_anchor, m_current));
}

QString FilterResultsView::displayText(int result) const
{
    const Entry* e = entry(result);
    return e ? e->text : QString();
}

QString FilterResultsView::lineText(int result) const
{
    if (!m_document || result < 0 || result >= count())
        return {};
    const FilterMatch& match = m_matches[static_cast<std::size_t>(result)];
    const core::LineIndex& index = m_document->index();
    const auto bytes = m_document->bytes();
    if (match.start >= bytes.size())
        return {};
    // A long line goes on in further rows, up to a limit.
    const auto readRow = [&](std::uint64_t start, bool continuation) {
        return core::LineIndex::readRow(bytes, start, match.line, continuation, index.scheme(),
                                        index.lineBreak());
    };
    core::Row row = readRow(match.start, false);
    std::uint64_t end = row.contentEnd;
    for (std::size_t rows = 1; rows < kMaxRowsPerLine; ++rows) {
        if (row.end != row.contentEnd || row.end >= bytes.size())
            break; // the line ended
        row = readRow(row.end, true);
        end = row.contentEnd;
    }
    const std::size_t length = static_cast<std::size_t>(end - match.start);
    return m_document->decoder().decodePlain(bytes.subspan(match.start, length));
}

QString FilterResultsView::selectedText() const
{
    const std::optional<std::pair<int, int>> range = selection();
    if (!range)
        return {};
    QString text;
    const int last = std::min(range->second, range->first + kMaxCopiedLines - 1);
    for (int result = range->first; result <= last; ++result) {
        text += lineText(result);
        text += u'\n';
    }
    return text;
}

void FilterResultsView::copy()
{
    const std::optional<std::pair<int, int>> range = selection();
    if (!range)
        return;
    QApplication::clipboard()->setText(selectedText());
    const int lines = range->second - range->first + 1;
    if (lines > kMaxCopiedLines)
        emit statusMessage(tr("Copied the first %L1 of %L2 selected lines").arg(kMaxCopiedLines).arg(lines));
    else
        emit statusMessage(tr("Copied %n line(s)", "", lines));
}

void FilterResultsView::selectAll()
{
    if (count() == 0)
        return;
    m_anchor = 0;
    m_current = count() - 1;
    m_wantedLine.reset();
    viewport()->update();
}

const FilterResultsView::Entry* FilterResultsView::entry(int result) const
{
    if (Entry* cached = m_cache.object(result))
        return cached;
    if (!m_document || result < 0 || result >= count())
        return nullptr;
    const FilterMatch& match = m_matches[static_cast<std::size_t>(result)];
    const core::LineIndex& index = m_document->index();
    const auto bytes = m_document->bytes();
    if (match.start >= bytes.size())
        return nullptr;
    const core::Row row =
        core::LineIndex::readRow(bytes, match.start, match.line, false, index.scheme(), index.lineBreak());
    auto* e = new Entry;
    e->text = m_document->decoder()
                  .decode(bytes.subspan(row.start, static_cast<std::size_t>(row.contentEnd - row.start)))
                  .text;
    if (row.end == row.contentEnd && row.end < bytes.size())
        e->text += QStringLiteral(" …"); // only the start of a very long line is shown
    e->level = findLogLevel(e->text);
    e->width = QFontMetricsF(font()).horizontalAdvance(e->text);
    m_cache.insert(result, e);
    return e;
}

// Painting ------------------------------------------------------------------------------------------

bool FilterResultsView::event(QEvent* event)
{
    // Copy and Select All here are this view's own, not the document's menu actions.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll)) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void FilterResultsView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    const QRect area = viewport()->rect();
    painter.fillRect(event->rect(), m_colors.base);
    const int gutter = gutterWidth();
    painter.fillRect(QRect(0, 0, gutter, area.height()), m_colors.gutter);

    const int top = verticalScrollBar()->value();
    const qreal scrollX = horizontalScrollBar()->value();
    const qreal textLeft = gutter + m_charWidth / 2;
    const std::optional<std::pair<int, int>> selected = selection();
    const bool focused = hasFocus();
    // Characters beyond the right edge are not laid out (lines can be thousands long).
    const qsizetype shownChars =
        static_cast<qsizetype>((scrollX + area.width()) / std::max<qreal>(1, m_charWidth)) + 64;
    const qreal widest = m_widest;

    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(kTabWidth * m_charWidth);
    QTextCharFormat bold;
    bold.setFontWeight(QFont::Bold);
    QTextCharFormat marked;
    marked.setBackground(m_colors.match);
    QTextCharFormat markedSelected; // on the selection colour, a light veil reads better than yellow
    markedSelected.setBackground(QColor(m_colors.selectionText.red(), m_colors.selectionText.green(),
                                        m_colors.selectionText.blue(), 60));

    for (int y = 0, result = top; y < area.height() && result < count(); y += m_lineHeight, ++result) {
        const bool isSelected = selected && result >= selected->first && result <= selected->second;
        if (isSelected)
            painter.fillRect(QRect(gutter, y, area.width() - gutter, m_lineHeight),
                             focused ? m_colors.selection : m_colors.inactiveSelection);
        painter.setPen(m_colors.dim);
        painter.drawText(QRectF(0, y, gutter - m_charWidth, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(m_matches[static_cast<std::size_t>(result)].line + 1));

        const Entry* e = entry(result);
        if (!e)
            continue;
        m_widest = std::max(m_widest, e->width);
        const QString text = e->text.left(shownChars);

        QList<QTextLayout::FormatRange> formats;
        QColor color = m_colors.text;
        switch (e->level.level) {
        case LogLevel::Fatal:
        case LogLevel::Error:
            color = m_colors.error;
            break;
        case LogLevel::Warning:
            color = m_colors.warning;
            break;
        case LogLevel::Trace:
        case LogLevel::Debug:
            color = m_colors.dim;
            break;
        default:
            break;
        }
        if (isSelected && focused)
            color = m_colors.selectionText;
        if (e->level.level != LogLevel::None && e->level.start < text.size())
            formats.append({static_cast<int>(e->level.start), static_cast<int>(e->level.length), bold});
        for (const QRegularExpression& regex : m_highlights) {
            QRegularExpressionMatchIterator it = regex.globalMatchView(text);
            for (int marks = 0; it.hasNext() && marks < kMaxMarksPerLine;) {
                const QRegularExpressionMatch match = it.next();
                if (match.capturedLength() > 0) {
                    const int start = static_cast<int>(match.capturedStart());
                    const int length = static_cast<int>(match.capturedLength());
                    formats.append({start, length, isSelected && focused ? markedSelected : marked});
                    ++marks;
                }
            }
        }

        QTextLayout layout(text, font());
        layout.setTextOption(option);
        layout.setFormats(formats);
        layout.beginLayout();
        QTextLine line = layout.createLine();
        layout.endLayout();
        if (!line.isValid())
            continue;
        painter.save();
        painter.setClipRect(QRect(gutter, y, area.width() - gutter, m_lineHeight));
        painter.setPen(color);
        layout.draw(&painter, QPointF(textLeft - scrollX, y + (m_lineHeight - line.height()) / 2));
        painter.restore();
    }
    if (m_widest > widest) // a longer line came into view: it can be scrolled to now
        QMetaObject::invokeMethod(this, &FilterResultsView::updateScrollBars, Qt::QueuedConnection);
}

void FilterResultsView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void FilterResultsView::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics();
        refresh();
    } else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        updateColors();
        viewport()->update();
    }
}

void FilterResultsView::scrollContentsBy(int, int)
{
    viewport()->update();
}

void FilterResultsView::updateMetrics()
{
    const QFontMetricsF metrics(font());
    // As the text view measures: with a layout, so that fallback fonts count.
    QTextLayout probe(QStringLiteral("Ag中😀"), font());
    probe.beginLayout();
    const QTextLine line = probe.createLine();
    probe.endLayout();
    m_charWidth = metrics.horizontalAdvance(QLatin1Char('0'));
    m_lineHeight = static_cast<int>(std::ceil(std::max(metrics.lineSpacing(), line.height()))) + 2;
}

void FilterResultsView::updateColors()
{
    const EditorColors colors = editorColors(palette());
    const QColor base = colors.background;
    const bool dark = colors.dark;
    m_colors.base = base;
    m_colors.text = colors.text;
    m_colors.dim = colors.lineNumber;
    m_colors.gutter = colors.gutter;
    m_colors.selection = colors.selection;
    m_colors.selectionText = colors.selectionText;
    m_colors.inactiveSelection = mix(base, m_colors.selection, 0.35f);
    m_colors.match = dark ? QColor(255, 214, 0, 80) : QColor(255, 200, 0, 110);
    m_colors.error = dark ? QColor(0xef, 0x6c, 0x6c) : QColor(0xc6, 0x28, 0x28);
    m_colors.warning = dark ? QColor(0xff, 0xb7, 0x4d) : QColor(0xa8, 0x5f, 0x00);
}

void FilterResultsView::updateScrollBars()
{
    const int rows = visibleRows();
    QScrollBar* vertical = verticalScrollBar();
    vertical->setRange(0, std::max(0, count() - rows));
    vertical->setPageStep(rows);
    vertical->setSingleStep(1);
    const int width = std::max(1, viewport()->width() - gutterWidth());
    QScrollBar* horizontal = horizontalScrollBar();
    horizontal->setRange(0, std::max(0, static_cast<int>(std::ceil(m_widest + 2 * m_charWidth)) - width));
    horizontal->setPageStep(width);
    horizontal->setSingleStep(std::max(1, static_cast<int>(m_charWidth)));
}

int FilterResultsView::visibleRows() const
{
    return std::max(1, viewport()->height() / std::max(1, m_lineHeight));
}

int FilterResultsView::gutterWidth() const
{
    const std::uint64_t last = m_matches.empty() ? 0 : m_matches.back().line + 1;
    const int digits = std::max(4, static_cast<int>(QString::number(last).size()));
    return static_cast<int>(std::ceil((digits + 2) * m_charWidth));
}

void FilterResultsView::ensureVisible(int result)
{
    QScrollBar* vertical = verticalScrollBar();
    if (result < vertical->value())
        vertical->setValue(result);
    else if (result >= vertical->value() + visibleRows())
        vertical->setValue(result - visibleRows() + 1);
}

// Keyboard and mouse --------------------------------------------------------------------------------

void FilterResultsView::moveCurrent(int result, bool extendSelection)
{
    if (count() == 0)
        return;
    const int before = m_current;
    setCurrentResult(result, extendSelection);
    if (m_current != before || !extendSelection)
        emit currentLineChanged(m_matches[static_cast<std::size_t>(m_current)].line);
}

int FilterResultsView::resultAt(int y) const
{
    const int result = verticalScrollBar()->value() + y / std::max(1, m_lineHeight);
    return y >= 0 && result < count() ? result : -1;
}

void FilterResultsView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy)) {
        copy();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    const bool extend = event->modifiers() & Qt::ShiftModifier;
    int target = m_current;
    switch (event->key()) {
    case Qt::Key_Up:
        target = m_current - 1;
        break;
    case Qt::Key_Down:
        target = m_current + 1;
        break;
    case Qt::Key_PageUp:
        target = m_current - visibleRows();
        break;
    case Qt::Key_PageDown:
        target = m_current + visibleRows();
        break;
    case Qt::Key_Home:
        target = 0;
        break;
    case Qt::Key_End:
        target = count() - 1;
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_current >= 0)
            emit activated(m_matches[static_cast<std::size_t>(m_current)].line);
        return;
    default:
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    if (count() > 0)
        moveCurrent(std::max(0, target), extend);
}

void FilterResultsView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    const int result = resultAt(event->position().toPoint().y());
    if (event->button() != Qt::LeftButton || result < 0)
        return;
    moveCurrent(result, event->modifiers() & Qt::ShiftModifier);
}

void FilterResultsView::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton) || count() == 0)
        return;
    const int y = std::clamp(event->position().toPoint().y(), 0, viewport()->height() - 1);
    const int result = resultAt(y);
    if (result >= 0 && result != m_current) {
        setCurrentResult(result, true);
        emit currentLineChanged(m_matches[static_cast<std::size_t>(m_current)].line);
    }
}

void FilterResultsView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const int result = resultAt(event->position().toPoint().y());
    if (event->button() == Qt::LeftButton && result >= 0)
        emit activated(m_matches[static_cast<std::size_t>(result)].line);
}

void FilterResultsView::contextMenuEvent(QContextMenuEvent* event)
{
    const int result = resultAt(viewport()->mapFrom(this, event->pos()).y());
    const std::optional<std::pair<int, int>> range = selection();
    if (result >= 0 && (!range || result < range->first || result > range->second))
        moveCurrent(result, false);

    QMenu menu(this);
    QAction* copyAction = menu.addAction(tr("&Copy Lines"), this, &FilterResultsView::copy);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(selection().has_value());
    QAction* selectAllAction = menu.addAction(tr("Select &All"), this, &FilterResultsView::selectAll);
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    selectAllAction->setEnabled(count() > 0);
    menu.exec(event->globalPos());
}

} // namespace aiedit::ui
