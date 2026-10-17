#include "ui/CompareView.h"

#include "core/FileSource.h"
#include "ui/Desktop.h"
#include "ui/SyntaxHighlighting.h"
#include "ui/TextDecoder.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>

#include <algorithm>
#include <climits>
#include <cmath>

namespace aiedit::ui {

namespace {

constexpr int kCachedLines = 4000;
constexpr int kCachedInline = 1000;
constexpr int kDividerWidth = 6;
constexpr int kRulerWidth = 12;
constexpr int kTabWidth = 4;
constexpr int kWheelRows = 3;

QColor mix(const QColor& a, const QColor& b, float t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

int digits(std::uint64_t n)
{
    int count = 1;
    for (; n >= 10; n /= 10)
        ++count;
    return count;
}

quint64 textKey(bool left, std::uint64_t line)
{
    return (line << 1) | (left ? 0u : 1u);
}

} // namespace

CompareView::CompareView(QWidget* parent)
    : QAbstractScrollArea(parent), m_scroll(verticalScrollBar()), m_texts(kCachedLines), m_inline(kCachedInline)
{
    setFocusPolicy(Qt::StrongFocus);
    setFont(fixedFont());
    viewport()->setCursor(Qt::IBeamCursor);
    updateMetrics();
    updateColors();
    updateScrollBars();
}

CompareView::~CompareView() = default;

void CompareView::setComparison(const CompareSide& left, const CompareSide& right, const TextComparison& comparison,
                                bool onlyDifferences)
{
    const auto setSide = [](Side& side, const CompareSide& info, std::shared_ptr<const core::LineIndex> index) {
        side.info = info;
        side.index = std::move(index);
        side.decoder = std::make_unique<TextDecoder>(*info.codec);
    };
    setSide(m_left, left, comparison.left);
    setSide(m_right, right, comparison.right);
    m_rejected.clear(); // of the hunks before
    m_texts.clear();
    m_inline.clear();
    m_widest = 0;
    m_hunks = comparison.hunks;
    m_rows.build(m_hunks, comparison.left->lineCount(), comparison.right->lineCount(), onlyDifferences);
    m_current = 0;
    m_scroll.setTopRow(0);
    horizontalScrollBar()->setValue(0);
    updateGutters();
    rowsChanged();
    // Start at the first change.
    if (m_rows.hunkCount() > 0)
        setCurrentRow(m_rows.firstRowOf(0));
    emit currentChanged();
}

void CompareView::clear()
{
    m_left = Side();
    m_right = Side();
    m_hunks.clear();
    m_rows.build({}, 0, 0, false);
    m_texts.clear();
    m_inline.clear();
    m_current = 0;
    rowsChanged();
}

void CompareView::setOnlyDifferences(bool on)
{
    if (!hasComparison())
        return;
    // Stay on the line shown at the current row.
    const CompareRows::Row row = m_rows.at(m_current);
    const bool left = row.left.has_value();
    const std::uint64_t line = left ? *row.left : row.right.value_or(0);
    m_rows.build(m_hunks, m_left.index->lineCount(), m_right.index->lineCount(), on);
    rowsChanged();
    setCurrentRow(m_rows.rowOfLine(left, line));
    ensureVisible(m_current, true);
}

int CompareView::currentHunk() const
{
    return m_rows.at(m_current).hunk;
}

bool CompareView::goToNextDifference()
{
    const int current = currentHunk();
    int next = current >= 0 ? current + 1 : m_rows.hunkAtOrAfter(m_current);
    if (next < 0 || next >= m_rows.hunkCount())
        return false;
    setCurrentRow(m_rows.firstRowOf(next));
    ensureVisible(m_current, true);
    return true;
}

bool CompareView::goToPreviousDifference()
{
    const int current = currentHunk();
    const int previous = current >= 0 ? current - 1 : m_rows.hunkBefore(m_current);
    if (previous < 0)
        return false;
    setCurrentRow(m_rows.firstRowOf(previous));
    ensureVisible(m_current, true);
    return true;
}

void CompareView::setCurrentRow(std::uint64_t row)
{
    if (m_rows.count() == 0)
        return;
    m_current = std::min(row, m_rows.count() - 1);
    viewport()->update();
    emit currentChanged();
}

void CompareView::setLeftActive(bool left)
{
    if (left == m_leftActive)
        return;
    m_leftActive = left;
    viewport()->update();
    emit currentChanged();
}

QString CompareView::rowText(std::uint64_t row, bool left) const
{
    if (!hasComparison() || row >= m_rows.count())
        return {};
    const CompareRows::Row shown = m_rows.at(row);
    const std::optional<std::uint64_t> line = left ? shown.left : shown.right;
    if (shown.kind == CompareRows::Kind::Fold || !line)
        return {};
    // Whole, with real control characters, as copying wants.
    const Side& side = left ? m_left : m_right;
    const std::optional<std::uint64_t> first = side.index->rowIndexOfLine(*line);
    if (!first)
        return {};
    const auto bytes = side.info.source->bytes();
    std::uint64_t end = side.index->row(*first).contentEnd;
    for (std::uint64_t r = *first + 1; r < side.index->rowCount(); ++r) {
        const core::Row next = side.index->row(r);
        if (!next.continuation)
            break;
        end = next.contentEnd;
    }
    const std::uint64_t start = side.index->row(*first).start;
    return side.decoder->decodePlain(bytes.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
}

void CompareView::copy()
{
    const QString text = rowText(m_current, m_leftActive);
    if (!text.isEmpty() || m_rows.at(m_current).kind != CompareRows::Kind::Fold)
        QGuiApplication::clipboard()->setText(text);
}

QString CompareView::lineText(bool left, std::uint64_t line, bool* cut) const
{
    const quint64 key = textKey(left, line);
    if (QString* cached = m_texts.object(key)) {
        if (cut)
            *cut = cached->endsWith(QChar(0x2026));
        return *cached;
    }
    const Side& side = left ? m_left : m_right;
    bool longLine = false;
    QString text = shownLine(side.info.source->bytes(), *side.index, *side.decoder, line, &longLine);
    if (longLine)
        text += QChar(0x2026);
    if (cut)
        *cut = longLine;
    m_texts.insert(key, new QString(text));
    return text;
}

const core::InlineDiff& CompareView::inlineDiff(std::uint64_t leftLine, std::uint64_t rightLine) const
{
    if (core::InlineDiff* cached = m_inline.object(leftLine))
        return *cached;
    const QString left = lineText(true, leftLine);
    const QString right = lineText(false, rightLine);
    auto* diff = new core::InlineDiff(core::diffInline({reinterpret_cast<const char16_t*>(left.utf16()),
                                                         static_cast<std::size_t>(left.size())},
                                                        {reinterpret_cast<const char16_t*>(right.utf16()),
                                                         static_cast<std::size_t>(right.size())}));
    m_inline.insert(leftLine, diff);
    return *diff;
}

// Geometry -----------------------------------------------------------------------------------------

int CompareView::visibleRows() const
{
    return std::max(1, viewport()->height() / std::max(1, m_lineHeight));
}

int CompareView::rulerWidth() const
{
    return kRulerWidth;
}

int CompareView::paneWidth() const
{
    return std::max(0, (viewport()->width() - rulerWidth() - kDividerWidth) / 2);
}

int CompareView::rightPaneLeft() const
{
    return paneWidth() + kDividerWidth;
}

std::optional<std::uint64_t> CompareView::rowAt(int y) const
{
    if (y < 0)
        return std::nullopt;
    const std::uint64_t row = m_scroll.topRow() + static_cast<std::uint64_t>(y / std::max(1, m_lineHeight));
    if (row >= m_rows.count())
        return std::nullopt;
    return row;
}

bool CompareView::isOnLeft(int x) const
{
    return x < paneWidth() + kDividerWidth / 2;
}

// Painting -----------------------------------------------------------------------------------------

void CompareView::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), m_colors.base);
    const QRect area = viewport()->rect();
    const int pane = paneWidth();
    const int rightLeft = rightPaneLeft();
    const QRect ruler(area.width() - rulerWidth(), 0, rulerWidth(), area.height());
    painter.fillRect(QRect(pane, 0, kDividerWidth, area.height()), m_colors.gutter);

    if (hasComparison()) {
        const qreal widest = m_widest;
        const std::uint64_t top = m_scroll.topRow();
        for (int y = 0; y < area.height(); y += m_lineHeight) {
            const std::uint64_t index = top + static_cast<std::uint64_t>(y / m_lineHeight);
            if (index >= m_rows.count())
                break;
            const CompareRows::Row row = m_rows.at(index);
            const bool current = index == m_current;
            if (row.kind == CompareRows::Kind::Fold) {
                const QRect band(0, y, ruler.left(), m_lineHeight);
                painter.fillRect(band, m_colors.fold);
                painter.setPen(m_colors.dim);
                painter.drawText(band, Qt::AlignCenter,
                                 tr("⋯ %n same line(s): click to show them ⋯", "", static_cast<int>(std::min<std::uint64_t>(row.folded, INT_MAX))));
                if (current) {
                    painter.setPen(m_colors.current);
                    painter.drawRect(band.adjusted(0, 0, -1, -1));
                }
                continue;
            }
            paintSide(painter, true, row, QRect(0, y, pane, m_lineHeight), current);
            paintSide(painter, false, row, QRect(rightLeft, y, pane, m_lineHeight), current);
        }
        if (m_widest > widest) // a longer line came into view: it can be scrolled to now
            QMetaObject::invokeMethod(this, &CompareView::updateScrollBars, Qt::QueuedConnection);
    }
    paintRuler(painter, ruler);
}

void CompareView::paintSide(QPainter& painter, bool left, const CompareRows::Row& row, const QRect& rect, bool current)
{
    const Side& side = left ? m_left : m_right;
    const std::optional<std::uint64_t> line = left ? row.left : row.right;
    const QRect gutter(rect.left(), rect.top(), side.gutter, rect.height());
    const QRect text(rect.left() + side.gutter, rect.top(), rect.width() - side.gutter, rect.height());

    QColor background = m_colors.base;
    QColor words;
    const bool rejected = isRejected(row.hunk);
    switch (rejected ? CompareRows::Kind::Same : row.kind) {
    case CompareRows::Kind::Changed:
        background = left ? m_colors.removed : m_colors.added;
        words = left ? m_colors.removedWords : m_colors.addedWords;
        break;
    case CompareRows::Kind::Removed:
        background = m_colors.removed;
        break;
    case CompareRows::Kind::Added:
        background = m_colors.added;
        break;
    default:
        break;
    }
    if (!line) {
        // A gap across from lines the other side has.
        painter.fillRect(rect, m_colors.gap);
        painter.fillRect(rect, QBrush(m_colors.gapLines, Qt::BDiagPattern));
    } else {
        painter.fillRect(text, background);
        painter.fillRect(gutter, m_colors.gutter);
        painter.setPen(current && left == m_leftActive ? m_colors.text : m_colors.dim);
        painter.drawText(QRectF(gutter.left(), gutter.top(), gutter.width() - m_charWidth, gutter.height()),
                         Qt::AlignRight | Qt::AlignVCenter, QString::number(*line + 1));
    }
    if (current) {
        // The current row, marked more strongly on the side worked on.
        painter.setPen(left == m_leftActive ? m_colors.current : mix(m_colors.base, m_colors.current, 0.45f));
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
    }
    if (!line)
        return;

    const qreal scrollX = horizontalScrollBar()->value();
    // Characters past the right edge are not laid out: lines can be thousands long.
    const auto shown = static_cast<qsizetype>((scrollX + text.width()) / std::max<qreal>(1, m_charWidth)) + 64;
    const QString full = lineText(left, *line);
    const QString visible = full.left(shown);
    QList<QTextLayout::FormatRange> formats;
    if (rejected) {
        // Left out: the left stays as it is, the right is struck through.
        if (!left) {
            QTextCharFormat struck;
            struck.setFontStrikeOut(true);
            struck.setForeground(m_colors.dim);
            formats.append({0, static_cast<int>(visible.size()), struck});
        }
    } else if (row.kind == CompareRows::Kind::Changed && row.left && row.right) {
        QTextCharFormat marked;
        marked.setBackground(words);
        const core::InlineDiff& diff = inlineDiff(*row.left, *row.right);
        for (const core::TextRange& range : left ? diff.left : diff.right) {
            if (range.start >= visible.size())
                break;
            formats.append({static_cast<int>(range.start), static_cast<int>(range.length), marked});
        }
    }
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(kTabWidth * m_charWidth);
    QTextLayout layout(visible, font());
    layout.setTextOption(option);
    layout.setFormats(formats);
    layout.beginLayout();
    QTextLine textLine = layout.createLine();
    layout.endLayout();
    if (!textLine.isValid())
        return;
    m_widest = std::max(m_widest, textLine.naturalTextWidth() +
                                      static_cast<qreal>(full.size() - visible.size()) * m_charWidth);
    painter.save();
    painter.setClipRect(text);
    painter.setPen(m_colors.text);
    layout.draw(&painter, QPointF(text.left() + m_charWidth / 2 - scrollX, rect.top() + (m_lineHeight - textLine.height()) / 2));
    painter.restore();
}

void CompareView::setRejected(std::vector<bool> rejected)
{
    m_rejected = std::move(rejected);
    viewport()->update();
}

bool CompareView::isRejected(int hunk) const
{
    const auto at = static_cast<std::size_t>(hunk);
    return hunk >= 0 && at < m_rejected.size() && m_rejected[at];
}

void CompareView::paintRuler(QPainter& painter, const QRect& rect) const
{
    painter.fillRect(rect, m_colors.gutter);
    const std::uint64_t rows = m_rows.count();
    if (rows == 0 || rect.height() <= 0)
        return;
    const double scale = static_cast<double>(rect.height()) / static_cast<double>(rows);
    const QColor changed = mix(m_colors.removedWords, m_colors.addedWords, 0.5f);
    int lastY = -1;
    CompareRows::Kind lastKind = CompareRows::Kind::Same;
    for (const Mark& mark : m_marks) {
        const int y = rect.top() + static_cast<int>(mark.at * rect.height());
        if (y == lastY && mark.kind == lastKind)
            continue; // thousands of changes on one pixel row are drawn once
        const QColor color = mark.kind == CompareRows::Kind::Removed ? m_colors.removedWords
                             : mark.kind == CompareRows::Kind::Added ? m_colors.addedWords
                                                                     : changed;
        painter.fillRect(QRect(rect.left() + 2, y, rect.width() - 4, 3), color);
        lastY = y;
        lastKind = mark.kind;
    }
    // Where the view is.
    const int top = rect.top() + static_cast<int>(static_cast<double>(m_scroll.topRow()) * scale);
    const int height = std::max(4, static_cast<int>(static_cast<double>(visibleRows()) * scale));
    QColor veil = m_colors.text;
    veil.setAlpha(40);
    painter.fillRect(QRect(rect.left(), top, rect.width(), height), veil);
}

// Events -------------------------------------------------------------------------------------------

bool CompareView::event(QEvent* event)
{
    // Copy here, not in a window-wide action.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Copy)) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::event(event);
}

void CompareView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

void CompareView::changeEvent(QEvent* event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics();
        updateGutters();
        m_widest = 0;
        updateScrollBars();
    } else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        updateColors();
    }
    viewport()->update();
}

void CompareView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy)) {
        copy();
        return;
    }
    if (m_rows.count() == 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    const std::uint64_t last = m_rows.count() - 1;
    const auto page = static_cast<std::uint64_t>(visibleRows());
    switch (event->key()) {
    case Qt::Key_Up:
        moveCurrent(m_current == 0 ? 0 : m_current - 1);
        return;
    case Qt::Key_Down:
        moveCurrent(std::min(last, m_current + 1));
        return;
    case Qt::Key_PageUp:
        moveCurrent(m_current - std::min(m_current, page));
        return;
    case Qt::Key_PageDown:
        moveCurrent(std::min(last, m_current + page));
        return;
    case Qt::Key_Home:
        moveCurrent(0);
        return;
    case Qt::Key_End:
        moveCurrent(last);
        return;
    case Qt::Key_Left:
        setLeftActive(true);
        return;
    case Qt::Key_Right:
        setLeftActive(false);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activate();
        return;
    default:
        QAbstractScrollArea::keyPressEvent(event);
    }
}

void CompareView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->position().toPoint();
    if (pos.x() >= viewport()->width() - rulerWidth()) {
        // The strip: go to that part of the files.
        const std::uint64_t rows = m_rows.count();
        if (rows > 0) {
            const double share = std::clamp(static_cast<double>(pos.y()) / std::max(1, viewport()->height()), 0.0, 1.0);
            ensureVisible(std::min(rows - 1, static_cast<std::uint64_t>(share * static_cast<double>(rows))), true);
        }
        return;
    }
    const std::optional<std::uint64_t> row = rowAt(pos.y());
    if (!row)
        return;
    m_leftActive = isOnLeft(pos.x());
    if (m_rows.at(*row).kind == CompareRows::Kind::Fold) {
        m_current = *row;
        activate();
        return;
    }
    setCurrentRow(*row);
}

void CompareView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->position().toPoint();
    const std::optional<std::uint64_t> row = rowAt(pos.y());
    if (!row || pos.x() >= viewport()->width() - rulerWidth())
        return;
    m_leftActive = isOnLeft(pos.x());
    m_current = *row;
    activate();
}

void CompareView::wheelEvent(QWheelEvent* event)
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
    if (event->modifiers() & Qt::ShiftModifier || event->angleDelta().x() != 0) {
        QAbstractScrollArea::wheelEvent(event);
        return;
    }
    const int notches = event->angleDelta().y() / 120;
    if (notches != 0) {
        const std::int64_t delta = -static_cast<std::int64_t>(notches) * kWheelRows;
        const std::uint64_t top = m_scroll.topRow();
        const std::uint64_t target = delta < 0 ? top - std::min<std::uint64_t>(top, static_cast<std::uint64_t>(-delta))
                                               : top + static_cast<std::uint64_t>(delta);
        if (m_scroll.setTopRow(target))
            viewport()->update();
    }
    event->accept();
}

void CompareView::contextMenuEvent(QContextMenuEvent* event)
{
    const std::optional<std::uint64_t> row = rowAt(event->pos().y());
    if (!row)
        return;
    const bool left = isOnLeft(event->pos().x());
    setLeftActive(left);
    setCurrentRow(*row);
    const CompareRows::Row shown = m_rows.at(*row);
    QMenu menu(this);
    QAction* copyAction = menu.addAction(tr("&Copy Line"), this, &CompareView::copy);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(shown.kind != CompareRows::Kind::Fold && (left ? shown.left : shown.right));
    menu.addSeparator();
    for (const bool onLeft : {true, false}) {
        const Side& side = onLeft ? m_left : m_right;
        const std::optional<std::uint64_t> line = onLeft ? shown.left : shown.right;
        QAction* open = menu.addAction(tr("Open %1 at This Line").arg(side.info.label), this, [this, onLeft, line] {
            if (line)
                emit openRequested(onLeft, *line);
        });
        open->setEnabled(line.has_value() && !side.info.path.isEmpty() && shown.kind != CompareRows::Kind::Fold);
    }
    menu.exec(event->globalPos());
}

void CompareView::scrollContentsBy(int, int dy)
{
    if (dy != 0)
        m_scroll.syncFromBar();
    viewport()->update();
}

// Moving -------------------------------------------------------------------------------------------

void CompareView::moveCurrent(std::uint64_t row)
{
    setCurrentRow(row);
    ensureVisible(m_current);
}

void CompareView::ensureVisible(std::uint64_t row, bool center)
{
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

void CompareView::activate()
{
    const CompareRows::Row row = m_rows.at(m_current);
    if (row.kind == CompareRows::Kind::Fold) {
        const std::uint64_t at = m_current;
        if (m_rows.unfold(at)) {
            rowsChanged();
            setCurrentRow(at);
        }
        return;
    }
    const std::optional<std::uint64_t> line = m_leftActive ? row.left : row.right;
    if (line)
        emit openRequested(m_leftActive, *line);
}

// Metrics and colours ------------------------------------------------------------------------------

void CompareView::rowsChanged()
{
    m_marks.clear();
    const auto rows = static_cast<double>(std::max<std::uint64_t>(1, m_rows.count()));
    for (int h = 0; h < m_rows.hunkCount(); ++h) {
        const std::uint64_t first = m_rows.firstRowOf(h);
        m_marks.push_back({static_cast<double>(first) / rows, m_rows.at(first).kind});
    }
    updateScrollBars();
    viewport()->update();
}

void CompareView::updateMetrics()
{
    const QFontMetricsF metrics(font());
    QTextLayout probe(QStringLiteral("Ag中😀"), font());
    probe.beginLayout();
    const QTextLine line = probe.createLine();
    probe.endLayout();
    m_charWidth = metrics.horizontalAdvance(QLatin1Char('0'));
    m_lineHeight = static_cast<int>(std::ceil(std::max(metrics.lineSpacing(), line.height()))) + 2;
}

void CompareView::updateGutters()
{
    for (Side* side : {&m_left, &m_right}) {
        const std::uint64_t lines = side->index ? side->index->lineCount() : 0;
        side->gutter = static_cast<int>(std::ceil((digits(lines) + 2) * m_charWidth));
    }
}

void CompareView::updateColors()
{
    const EditorColors colors = editorColors(palette());
    const QColor base = colors.background;
    const bool dark = colors.dark;
    m_colors.base = base;
    m_colors.text = colors.text;
    m_colors.dim = colors.lineNumber;
    m_colors.gutter = colors.gutter;
    m_colors.selection = colors.selection;
    const QColor red = dark ? QColor(0xe5, 0x53, 0x4b) : QColor(0xd7, 0x3a, 0x49);
    const QColor green = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x28, 0xa7, 0x45);
    m_colors.removed = mix(base, red, dark ? 0.22f : 0.14f);
    m_colors.removedWords = mix(base, red, dark ? 0.50f : 0.38f);
    m_colors.added = mix(base, green, dark ? 0.20f : 0.13f);
    m_colors.addedWords = mix(base, green, dark ? 0.48f : 0.36f);
    m_colors.gap = mix(base, colors.text, 0.04f);
    m_colors.gapLines = mix(base, colors.text, 0.12f);
    m_colors.fold = mix(base, colors.text, 0.07f);
    m_colors.current = colors.selection;
}

void CompareView::updateScrollBars()
{
    const auto page = visibleRows();
    const std::uint64_t rows = m_rows.count();
    m_scroll.setRange(rows > static_cast<std::uint64_t>(page) ? rows - static_cast<std::uint64_t>(page) : 0, page);
    const int textWidth = std::max(0, paneWidth() - std::max(m_left.gutter, m_right.gutter));
    QScrollBar* horizontal = horizontalScrollBar();
    horizontal->setRange(0, std::max(0, static_cast<int>(std::ceil(m_widest + m_charWidth)) - textWidth));
    horizontal->setPageStep(textWidth);
    horizontal->setSingleStep(static_cast<int>(std::ceil(m_charWidth)) * 2);
}

} // namespace aiedit::ui
