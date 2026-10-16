#include "ui/QuickOpen.h"

#include "ui/FolderModels.h"
#include "ui/FuzzyMatch.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QPainter>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace aiedit::ui {

namespace {

constexpr int kMaxShown = 200;
constexpr int kVisibleRows = 12;
constexpr int kMaxFolderFiles = 50'000;
constexpr int kPrimaryBonus = 40; // matched within the name, rather than across the path
constexpr int kRankBonus = 3;
constexpr qint64 kRelistAfterMs = 30'000; // a folder listed longer ago is listed again

enum Role
{
    DetailRole = Qt::UserRole + 1,
    PositionsRole,       // in the text
    DetailPositionsRole, // in the detail
    ShortcutRole,
    CheckedRole, // set for commands: a check for those that are on
};

// The pieces of a query each have to match, best within the text, else (unless `textOnly`)
// across detail and text. With `positions`, where they matched in the candidate's combined
// text, ascending.
template <typename Candidate>
std::optional<int> score(const QStringList& pieces, const Candidate& candidate, bool textOnly,
                         std::vector<int>* positions)
{
    const QStringView combined(candidate.combined);
    const QStringView primary = combined.sliced(candidate.primaryStart);
    int total = 0;
    std::vector<int> found;
    std::vector<int>* where = positions ? &found : nullptr;
    for (const QString& piece : pieces) {
        std::optional<int> matched;
        if (!piece.contains(u'/')) {
            matched = fuzzyMatch(piece, primary, where);
            if (matched) {
                *matched += kPrimaryBonus;
                for (int& position : found)
                    position += candidate.primaryStart;
            }
        }
        if (!matched && !textOnly)
            matched = fuzzyMatch(piece, combined, where);
        if (!matched)
            return std::nullopt;
        total += *matched;
        if (positions)
            positions->insert(positions->end(), found.begin(), found.end());
    }
    if (positions) {
        std::sort(positions->begin(), positions->end());
        positions->erase(std::unique(positions->begin(), positions->end()), positions->end());
    }
    return total + candidate.rank * kRankBonus;
}

// Draws text from the left of `rect`, the characters at `positions` (ascending) in bold and in
// `matchedColor`; returns where it ended. Cut short with an ellipsis where the rectangle does.
int drawMatched(QPainter* painter, const QRect& rect, const QString& text, const QList<int>& positions,
                const QFont& font, const QColor& color, const QColor& matchedColor)
{
    QFont bold = font;
    bold.setBold(true);
    int x = rect.left();
    qsizetype next = 0; // in positions
    for (qsizetype i = 0; i < text.size();) {
        const bool matched = next < positions.size() && positions[next] == i;
        qsizetype end = i;
        if (matched) {
            while (next < positions.size() && positions[next] == end) {
                ++end;
                ++next;
            }
        } else {
            end = next < positions.size() ? std::min<qsizetype>(positions[next], text.size()) : text.size();
        }
        const QString run = text.mid(i, end - i);
        const QFontMetrics metrics(matched ? bold : font);
        painter->setFont(matched ? bold : font);
        painter->setPen(matched ? matchedColor : color);
        const int width = metrics.horizontalAdvance(run);
        if (x + width > rect.right()) {
            painter->drawText(QRect(x, rect.top(), rect.right() - x, rect.height()), Qt::AlignLeft | Qt::AlignVCenter,
                              metrics.elidedText(run, Qt::ElideRight, rect.right() - x));
            return rect.right();
        }
        painter->drawText(QRect(x, rect.top(), width, rect.height()), Qt::AlignLeft | Qt::AlignVCenter, run);
        x += width;
        i = end;
    }
    return x;
}

QList<int> toList(const std::vector<int>& positions, int from, int to, int shift)
{
    QList<int> list;
    for (const int position : positions)
        if (position >= from && position < to)
            list << position - shift;
    return list;
}

} // namespace

// A row: the text with what matched in bold, its detail dimmed after it, a shortcut on the right.
class QuickOpen::Delegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), option.fontMetrics.height() + 8));
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget); // the background

        painter->save();
        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = opt.state & QStyle::State_Enabled ? QPalette::Normal : QPalette::Disabled;
        const QColor color = opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        const QColor matched = selected ? color : opt.palette.color(group, QPalette::Link);
        QColor dim = color;
        dim.setAlphaF(0.6f);
        const int padding = opt.fontMetrics.horizontalAdvance(QLatin1Char('x'));
        QRect rect = opt.rect.adjusted(padding, 0, -padding, 0);

        if (const QVariant checked = index.data(CheckedRole); checked.isValid()) {
            const int column = opt.fontMetrics.height();
            if (checked.toBool()) {
                painter->setPen(color);
                painter->drawText(QRect(rect.left(), rect.top(), column, rect.height()), Qt::AlignCenter,
                                  QStringLiteral("✓"));
            }
            rect.setLeft(rect.left() + column);
        }
        if (const QString shortcut = index.data(ShortcutRole).toString(); !shortcut.isEmpty()) {
            painter->setFont(opt.font);
            painter->setPen(dim);
            painter->drawText(rect, Qt::AlignRight | Qt::AlignVCenter, shortcut);
            rect.setRight(rect.right() - opt.fontMetrics.horizontalAdvance(shortcut) - 2 * padding);
        }
        const int end = drawMatched(painter, rect, text, index.data(PositionsRole).value<QList<int>>(), opt.font,
                                    color, matched);
        if (const QString detail = index.data(DetailRole).toString(); !detail.isEmpty() && end < rect.right()) {
            QFont small = opt.font;
            small.setPointSizeF(small.pointSizeF() * 0.9);
            drawMatched(painter, QRect(end + 2 * padding, rect.top(), rect.right() - end - 2 * padding, rect.height()),
                        detail, index.data(DetailPositionsRole).value<QList<int>>(), small, dim, matched);
        }
        painter->restore();
    }
};

QuickOpen::QuickOpen(QWidget* parent) : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);

    m_input = new QLineEdit;
    m_input->setClearButtonEnabled(true);
    m_input->setPlaceholderText(tr("A file's name; > for commands, : for a line"));
    m_input->installEventFilter(this);
    connect(m_input, &QLineEdit::textChanged, this, &QuickOpen::refilter);

    m_list = new QListWidget;
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus); // typing goes on in the line
    m_list->setUniformItemSizes(true);
    m_list->setItemDelegate(new Delegate(m_list));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { accept(m_list->row(item)); });

    m_note = new QLabel;
    m_note->setForegroundRole(QPalette::PlaceholderText);
    m_note->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    layout->addWidget(m_input);
    layout->addWidget(m_list);
    layout->addWidget(m_note);

    connect(&m_listWatcher, &QFutureWatcher<FolderFiles>::finished, this, &QuickOpen::listed);
    // Clicking elsewhere puts it away, as it does in other editors.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (isVisible() && now && now != this && !isAncestorOf(now))
            dismiss(false);
    });
    if (parent)
        parent->installEventFilter(this);
    hide();
}

QuickOpen::~QuickOpen()
{
    m_listWatcher.cancel();
    m_listWatcher.waitForFinished();
}

void QuickOpen::setFolder(const QString& folder)
{
    if (folder == m_folder)
        return;
    m_folder = folder;
    m_folderFiles = {};
    m_listedAt.invalidate();
    m_shown.clear();
    m_matched.clear();
    m_matchedQuery.clear();
    if (m_listWatcher.isRunning())
        m_listWatcher.cancel(); // what it lists is dropped when it finishes
    if (isVisible()) {
        load(m_commands);
        refilter();
    }
}

void QuickOpen::open(const QString& text)
{
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    load(text.startsWith(u'>'));
    show();
    raise();
    {
        const QSignalBlocker blocker(m_input);
        m_input->setText(text);
    }
    refilter();
    m_input->setFocus();
}

QuickOpen::Candidate QuickOpen::fileCandidate(const QString& path, int rank, bool* inFolder) const
{
    const QFileInfo info(path);
    const QString name = info.fileName();
    QString folder = info.absolutePath();
    *inFolder = !m_folder.isEmpty() && (folder == m_folder || folder.startsWith(m_folder + QLatin1Char('/')));
    if (*inFolder) {
        folder = folder.mid(m_folder.size() + 1);
    } else if (const QString home = QDir::homePath(); folder == home || folder.startsWith(home + QLatin1Char('/'))) {
        folder = QLatin1Char('~') + folder.mid(home.size());
    }
    Candidate candidate;
    candidate.combined = folder.isEmpty() ? name : folder + QLatin1Char('/') + name;
    candidate.primaryStart = static_cast<int>(candidate.combined.size() - name.size());
    candidate.rank = rank;
    return candidate;
}

void QuickOpen::load(bool commands)
{
    m_commands = commands;
    m_matched.clear();
    m_matchedQuery.clear();
    m_items.clear();
    m_candidates.clear();
    m_listedRelative.clear();
    m_shown.clear();
    const QList<Item> items = commands ? (m_commandSource ? m_commandSource() : QList<Item>())
                                       : (m_fileSource ? m_fileSource() : QList<Item>());
    QSet<QString> paths;
    for (const Item& item : items) {
        if (!commands && (item.path.isEmpty() || paths.contains(item.path)))
            continue;
        paths.insert(item.path);
        m_items.push_back(item);
    }
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items[i];
        Candidate candidate;
        if (commands) {
            candidate.combined = item.detail.isEmpty() ? item.text : item.detail + QLatin1Char(' ') + item.text;
            candidate.primaryStart = static_cast<int>(candidate.combined.size() - item.text.size());
            candidate.rank = item.rank;
        } else {
            bool inFolder = false;
            candidate = fileCandidate(item.path, item.rank, &inFolder);
            if (inFolder)
                m_listedRelative.insert(candidate.combined);
        }
        candidate.item = static_cast<int>(i);
        m_candidates.push_back(candidate);
    }
    // The folder's files, listed again when they may have changed.
    if (!commands && !m_folder.isEmpty() &&
        (m_folderFiles.folder != m_folder || !m_listedAt.isValid() || m_listedAt.elapsed() > kRelistAfterMs))
        listFolder();
}

void QuickOpen::listFolder()
{
    if (!m_listing.isEmpty()) {
        if (m_listing == m_folder)
            return;
        m_listWatcher.cancel(); // listed() drops it, and lists the folder now open
        return;
    }
    m_listing = m_folder;
    m_listWatcher.setFuture(QtConcurrent::run([folder = m_folder](QPromise<FolderFiles>& promise) {
        FolderFiles result;
        result.folder = folder;
        FileListHooks hooks;
        hooks.cancelled = [&promise] { return promise.isCanceled(); };
        hooks.found = [&result](std::vector<ListedFile>&& files, int) {
            for (ListedFile& file : files) {
                Candidate candidate;
                candidate.combined = std::move(file.relativePath);
                candidate.primaryStart = static_cast<int>(candidate.combined.lastIndexOf(QLatin1Char('/')) + 1);
                result.files.push_back(std::move(candidate));
            }
        };
        result.limited = listFiles(folder, NameFilter(), false, hooks, kMaxFolderFiles).limited;
        promise.addResult(std::move(result));
    }));
    if (isVisible())
        refilter(); // to say so
}

void QuickOpen::listed()
{
    const QString listing = std::exchange(m_listing, QString());
    if (m_listWatcher.future().isCanceled() || m_listWatcher.future().resultCount() == 0 || listing != m_folder) {
        if (isVisible() && !m_commands && !m_folder.isEmpty())
            listFolder(); // the folder changed meanwhile
        return;
    }
    FolderFiles files = m_listWatcher.future().takeResult();
    if (files.folder != m_folder)
        return;
    m_folderFiles = std::move(files);
    m_matched.clear(); // they pointed into the files before
    m_matchedQuery.clear();
    m_listedAt.start();
    if (isVisible() && !m_commands)
        refilter();
}

void QuickOpen::refilter()
{
    const QString text = m_input->text();
    if (const bool commands = text.startsWith(u'>'); commands != m_commands)
        load(commands);
    m_list->clear();
    m_shown.clear();
    m_line.reset();
    m_positionMode = !m_commands && text.startsWith(u':');
    if (m_positionMode) {
        showPosition(text.mid(1).trimmed());
        return;
    }

    QString query = (m_commands ? text.mid(1) : text).trimmed();
    if (!m_commands) {
        // "name:42" is a file and a line in it.
        static const QRegularExpression withLine(QStringLiteral("^(.*\\S)\\s*:(\\d+)$"));
        if (const QRegularExpressionMatch match = withLine.match(query); match.hasMatch()) {
            query = match.captured(1);
            if (const qulonglong line = match.captured(2).toULongLong(); line > 0)
                m_line = line - 1;
        }
    }
    const QStringList pieces = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // One word is a command's name; with more, one of them may name its menu ("view wrap").
    // Letters picked from the menu and the name together would match all sorts of commands.
    const bool textOnly = m_commands && pieces.size() == 1;
    const bool folderListed = !m_commands && m_folderFiles.folder == m_folder && !m_folder.isEmpty();

    struct Scored
    {
        int score;
        const Candidate* candidate;
    };
    std::vector<Scored> scored;
    if (pieces.isEmpty()) {
        m_matched.clear();
        m_matchedQuery.clear();
        // Nothing typed: what is open and recent, or the commands, in their order.
        for (const Candidate& candidate : m_candidates)
            scored.push_back({0, &candidate});
        if (folderListed && scored.empty()) {
            const std::size_t count = std::min(m_folderFiles.files.size(), static_cast<std::size_t>(kMaxShown));
            for (std::size_t i = 0; i < count; ++i)
                scored.push_back({0, &m_folderFiles.files[i]});
        }
    } else {
        // Typing on narrows down what the query before matched; anything else starts afresh.
        std::vector<const Candidate*> pool;
        if (!m_commands && !m_matchedQuery.isEmpty() && query.startsWith(m_matchedQuery)) {
            pool = std::move(m_matched);
        } else {
            pool.reserve(m_candidates.size() + (folderListed ? m_folderFiles.files.size() : 0));
            for (const Candidate& candidate : m_candidates)
                pool.push_back(&candidate);
            if (folderListed)
                for (const Candidate& candidate : m_folderFiles.files)
                    if (!m_listedRelative.contains(candidate.combined)) // offered already, as open or recent
                        pool.push_back(&candidate);
        }
        m_matched.clear();
        for (const Candidate* candidate : pool) {
            if (const auto points = score(pieces, *candidate, textOnly, nullptr)) {
                scored.push_back({*points, candidate});
                m_matched.push_back(candidate);
            }
        }
        m_matchedQuery = m_commands ? QString() : query;
        // Best first; among equals, shorter paths, then by name.
        const auto better = [](const Scored& a, const Scored& b) {
            if (a.score != b.score)
                return a.score > b.score;
            if (a.candidate->combined.size() != b.candidate->combined.size())
                return a.candidate->combined.size() < b.candidate->combined.size();
            return a.candidate->combined.compare(b.candidate->combined, Qt::CaseInsensitive) < 0;
        };
        const auto shown = scored.begin() + std::min<std::ptrdiff_t>(kMaxShown, std::ssize(scored));
        std::partial_sort(scored.begin(), shown, scored.end(), better);
        scored.erase(shown, scored.end());
    }

    const QChar separator = QDir::separator();
    for (const Scored& entry : scored) {
        const Candidate& candidate = *entry.candidate;
        std::vector<int> positions;
        if (!pieces.isEmpty())
            score(pieces, candidate, textOnly, &positions);
        const int detailEnd = std::max(0, candidate.primaryStart - 1); // before the separator
        auto* row = new QListWidgetItem(candidate.combined.mid(candidate.primaryStart));
        QString detail = candidate.combined.left(detailEnd);
        if (candidate.item >= 0 && m_commands) {
            const Item& item = m_items[static_cast<std::size_t>(candidate.item)];
            row->setData(ShortcutRole, item.shortcut);
            row->setData(CheckedRole, item.checkable && item.checked);
            if (!item.action || !item.action->isEnabled())
                row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
        } else {
            detail.replace(QLatin1Char('/'), separator);
            const QString path = candidate.item >= 0 ? m_items[static_cast<std::size_t>(candidate.item)].path
                                                     : m_folder + QLatin1Char('/') + candidate.combined;
            row->setToolTip(QDir::toNativeSeparators(path));
        }
        row->setData(DetailRole, detail);
        row->setData(PositionsRole, QVariant::fromValue(toList(positions, candidate.primaryStart,
                                                               static_cast<int>(candidate.combined.size()),
                                                               candidate.primaryStart)));
        row->setData(DetailPositionsRole, QVariant::fromValue(toList(positions, 0, detailEnd, 0)));
        m_list->addItem(row);
        m_shown.push_back(&candidate);
    }
    // With nothing typed, the file shown now comes first, and the one before it is selected.
    int current = 0;
    if (!m_commands && pieces.isEmpty() && m_shown.size() > 1 && m_shown.front()->item >= 0 &&
        m_items[static_cast<std::size_t>(m_shown.front()->item)].current)
        current = 1;
    if (m_list->count() > 0)
        m_list->setCurrentRow(current);

    QString note;
    if (m_list->count() == 0 && (m_commands || !isListing()))
        note = m_commands ? tr("No command matches") : tr("No file matches");
    else if (!m_commands && isListing())
        note = tr("Listing the files of %1…").arg(QDir(m_folder).dirName());
    else if (folderListed && m_folderFiles.limited)
        note = tr("Only the first %1 files of the folder are offered").arg(QLocale().toString(kMaxFolderFiles));
    m_note->setText(note);
    m_note->setVisible(!note.isEmpty());
    place();
}

void QuickOpen::showPosition(const QString& position)
{
    m_position = position;
    bool number = false;
    position.toULongLong(&number);
    auto* row = new QListWidgetItem(position.isEmpty() ? tr("Type a line number, or an offset such as 0x1F")
                                    : number           ? tr("Go to line %1").arg(position)
                                                       : tr("Go to %1").arg(position));
    if (position.isEmpty())
        row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
    m_list->addItem(row);
    m_list->setCurrentRow(0);
    m_note->hide();
    place();
}

QStringList QuickOpen::shownTexts() const
{
    QStringList texts;
    for (int i = 0; i < m_list->count(); ++i)
        texts << m_list->item(i)->text();
    return texts;
}

int QuickOpen::currentRow() const
{
    return m_list->currentRow();
}

QString QuickOpen::note() const
{
    return m_note->isVisible() ? m_note->text() : QString();
}

void QuickOpen::accept(int row)
{
    if (m_positionMode) {
        if (m_position.isEmpty())
            return;
        const QString position = m_position;
        dismiss(true);
        emit positionChosen(position);
        return;
    }
    if (row < 0 || row >= std::ssize(m_shown))
        return;
    const Candidate& candidate = *m_shown[static_cast<std::size_t>(row)];
    if (m_commands) {
        // Back where it was first: the command works on what was there.
        const QPointer<QAction> action = m_items[static_cast<std::size_t>(candidate.item)].action;
        if (!action || !action->isEnabled())
            return;
        dismiss(true);
        action->trigger();
        return;
    }
    const QString path = candidate.item >= 0 ? m_items[static_cast<std::size_t>(candidate.item)].path
                                             : m_folder + QLatin1Char('/') + candidate.combined;
    const std::optional<std::uint64_t> line = m_line;
    dismiss(false);
    emit fileChosen(path, line);
}

void QuickOpen::dismiss(bool restoreFocus)
{
    if (!isVisible())
        return;
    hide();
    m_shown.clear();
    m_list->clear();
    if (restoreFocus && m_returnFocus)
        m_returnFocus->setFocus();
}

void QuickOpen::place()
{
    QWidget* parent = parentWidget();
    if (!parent)
        return;
    int top = 0;
    if (const auto* window = qobject_cast<QMainWindow*>(parent); window && window->centralWidget())
        top = window->centralWidget()->geometry().top();
    const int rows = std::min(m_list->count(), kVisibleRows);
    m_list->setVisible(rows > 0);
    if (rows > 0)
        m_list->setFixedHeight(rows * m_list->sizeHintForRow(0) + 2 * m_list->frameWidth());
    const int width = std::min(parent->width() - 16, std::max(360, std::min(720, parent->width() * 3 / 5)));
    const int height = layout()->sizeHint().height();
    setGeometry((parent->width() - width) / 2, top + 4, width, height);
}

bool QuickOpen::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible())
        place();
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const int count = m_list->count();
        const int row = m_list->currentRow();
        switch (key->key()) {
        case Qt::Key_Escape:
            dismiss(true);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            accept(row);
            return true;
        case Qt::Key_Down:
        case Qt::Key_Up:
            if (count > 0) // round past either end
                m_list->setCurrentRow((row + (key->key() == Qt::Key_Down ? 1 : count - 1)) % count);
            return true;
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            if (count > 0)
                m_list->setCurrentRow(
                    std::clamp(row + (key->key() == Qt::Key_PageDown ? kVisibleRows : -kVisibleRows), 0, count - 1));
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace aiedit::ui
