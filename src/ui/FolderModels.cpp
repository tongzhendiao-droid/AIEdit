#include "ui/FolderModels.h"

#include "ui/Collation.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileIconProvider>
#include <QLocale>
#include <QStyle>

#include <algorithm>

namespace aiedit::ui {

namespace {

// Files found are handed over at least this often, or in batches this large.
constexpr qint64 kBatchIntervalMs = 100;
constexpr std::size_t kBatchSize = 500;

QVariant header(int section)
{
    switch (section) {
    case NameColumn:
        return FolderModel::tr("Name");
    case SizeColumn:
        return FolderModel::tr("Size");
    case TypeColumn:
        return FolderModel::tr("Type");
    case ModifiedColumn:
        return FolderModel::tr("Date Modified");
    default:
        return {};
    }
}

QIcon warningIcon()
{
    return QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
}

// The type column of either model: from the content, with a warning where the name disagrees.
QVariant typeData(FileTypeSniffer* sniffer, const QFileInfo& info, int role)
{
    if (info.isDir())
        return role == Qt::DisplayRole ? QVariant(FolderModel::tr("Folder")) : QVariant();
    if (!info.isFile())
        return role == Qt::DisplayRole ? QVariant(FolderModel::tr("Not a regular file")) : QVariant();
    const std::optional<SniffedType> type = sniffer->type(info);
    if (!type)
        return {};
    switch (role) {
    case Qt::DisplayRole:
        return type->label();
    case Qt::DecorationRole:
        return type->mismatch.empty() ? QVariant() : QVariant(warningIcon());
    case Qt::ToolTipRole: {
        const QString note = type->mismatchNote(info.fileName());
        return note.isEmpty() ? QVariant() : QVariant(note);
    }
    default:
        return {};
    }
}

// The panel shows names only: the rest is in the tooltip, the path in full, the size, time and
// type, and what is wrong with a name that does not match the content.
QString nameTip(FileTypeSniffer* sniffer, const QFileInfo& info)
{
    QStringList lines{QDir::toNativeSeparators(info.absoluteFilePath())};
    if (info.isFile()) {
        const QLocale locale;
        lines << QStringLiteral("%1 · %2").arg(locale.formattedDataSize(info.size()),
                                                locale.toString(info.lastModified(), QLocale::ShortFormat));
        if (const std::optional<SniffedType> type = sniffer->type(info)) {
            lines << FolderModel::tr("Type: %1").arg(type->label());
            if (const QString note = type->mismatchNote(info.fileName()); !note.isEmpty())
                lines << note;
        }
    }
    return lines.join(QLatin1Char('\n'));
}

// A file whose name says one thing and whose content another gets a warning for its icon.
bool misnamed(FileTypeSniffer* sniffer, const QFileInfo& info)
{
    if (!info.isFile())
        return false;
    const std::optional<SniffedType> type = sniffer->type(info);
    return type && !type->mismatch.empty();
}

} // namespace

FolderModel::FolderModel(FileTypeSniffer* sniffer, QObject* parent) : QFileSystemModel(parent), m_sniffer(sniffer)
{
    connect(sniffer, &FileTypeSniffer::found, this, [this](const QString& path) {
        const QModelIndex name = index(path, NameColumn);
        if (name.isValid())
            emit dataChanged(name, name.siblingAtColumn(TypeColumn),
                             {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole});
    });
}

QVariant FolderModel::data(const QModelIndex& index, int role) const
{
    if (index.column() == TypeColumn &&
        (role == Qt::DisplayRole || role == Qt::DecorationRole || role == Qt::ToolTipRole))
        return typeData(m_sniffer, fileInfo(index), role);
    if (index.column() == NameColumn && role == Qt::ToolTipRole)
        return nameTip(m_sniffer, fileInfo(index));
    if (index.column() == NameColumn && role == Qt::DecorationRole && misnamed(m_sniffer, fileInfo(index)))
        return warningIcon();
    return QFileSystemModel::data(index, role);
}

QVariant FolderModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        return header(section);
    return QFileSystemModel::headerData(section, orientation, role);
}

NameFilter::NameFilter(const QString& text)
{
    static const QRegularExpression separators(QStringLiteral("[;,\\s]+"));
    for (const QString& word : text.split(separators, Qt::SkipEmptyParts)) {
        Pattern pattern;
        const QString normalized = QDir::fromNativeSeparators(word);
        pattern.path = normalized.contains(QLatin1Char('/'));
        if (normalized.contains(QLatin1Char('*')) || normalized.contains(QLatin1Char('?')) ||
            normalized.contains(QLatin1Char('[')))
            pattern.wildcard = QRegularExpression::fromWildcard(normalized, Qt::CaseInsensitive);
        else
            pattern.part = normalized;
        m_patterns.push_back(std::move(pattern));
    }
}

bool NameFilter::matches(const QString& relativePath, const QString& fileName) const
{
    if (m_patterns.empty())
        return true;
    return std::any_of(m_patterns.begin(), m_patterns.end(), [&](const Pattern& pattern) {
        const QString& subject = pattern.path ? relativePath : fileName;
        return pattern.part.isEmpty() ? pattern.wildcard.match(subject).hasMatch()
                                      : subject.contains(pattern.part, Qt::CaseInsensitive);
    });
}

FileListSummary listFiles(const QString& folder, const NameFilter& filter, bool hidden, const FileListHooks& hooks,
                          int maxMatches, int maxVisited)
{
    FileListSummary summary;
    const QDir root(folder);
    const QCollator collator = naturalCollator();
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (hidden)
        filters |= QDir::Hidden;

    std::vector<ListedFile> batch;
    QElapsedTimer sinceHandover;
    sinceHandover.start();
    const auto handOver = [&] {
        if (!batch.empty() && hooks.found)
            hooks.found(std::move(batch), summary.visited);
        batch.clear();
        sinceHandover.restart();
    };

    // Depth first: the folders still to list, the next one last.
    std::vector<QString> pending{root.absolutePath()};
    while (!pending.empty() && !summary.limited) {
        if (hooks.cancelled && hooks.cancelled())
            break;
        const QString dir = pending.back();
        pending.pop_back();
        QFileInfoList entries = QDir(dir).entryInfoList(filters, QDir::NoSort);
        std::sort(entries.begin(), entries.end(), [&collator](const QFileInfo& a, const QFileInfo& b) {
            return collator.compare(a.fileName(), b.fileName()) < 0;
        });
        std::vector<QString> subfolders;
        for (const QFileInfo& entry : std::as_const(entries)) {
            if (++summary.visited > maxVisited) {
                summary.limited = true;
                break;
            }
            QString relative = root.relativeFilePath(entry.absoluteFilePath());
            if (hooks.skipped && hooks.skipped(relative, entry.isDir()))
                continue;
            if (entry.isDir()) {
                if (!entry.isSymLink())
                    subfolders.push_back(entry.absoluteFilePath());
                continue;
            }
            if (!filter.matches(relative, entry.fileName()))
                continue;
            batch.push_back({entry, std::move(relative)});
            if (++summary.matched >= maxMatches) {
                summary.limited = true;
                break;
            }
        }
        pending.insert(pending.end(), subfolders.rbegin(), subfolders.rend());
        if (batch.size() >= kBatchSize || sinceHandover.elapsed() >= kBatchIntervalMs)
            handOver();
    }
    handOver();
    summary.visited = std::min(summary.visited, maxVisited);
    return summary;
}

FileListModel::FileListModel(FileTypeSniffer* sniffer, QObject* parent)
    : QAbstractTableModel(parent), m_sniffer(sniffer), m_fileIcon(QFileIconProvider().icon(QFileIconProvider::File))
{
    connect(sniffer, &FileTypeSniffer::found, this, [this](const QString& path) {
        const auto row = m_rows.constFind(path);
        if (row == m_rows.cend())
            return;
        emit dataChanged(index(*row, NameColumn), index(*row, TypeColumn),
                         {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole});
    });
}

int FileListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_files.size());
}

int FileListModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : FolderColumnCount;
}

QVariant FileListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_files.size()))
        return {};
    const ListedFile& file = m_files[static_cast<std::size_t>(index.row())];
    switch (index.column()) {
    case NameColumn:
        if (role == Qt::DisplayRole)
            return QDir::toNativeSeparators(file.relativePath);
        if (role == Qt::ToolTipRole)
            return nameTip(m_sniffer, file.info);
        if (role == Qt::DecorationRole)
            return misnamed(m_sniffer, file.info) ? warningIcon() : m_fileIcon;
        return {};
    case SizeColumn:
        if (role == Qt::DisplayRole)
            return QLocale().formattedDataSize(file.info.size());
        if (role == Qt::TextAlignmentRole)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    case TypeColumn:
        return typeData(m_sniffer, file.info, role);
    case ModifiedColumn:
        if (role == Qt::DisplayRole)
            return QLocale().toString(file.info.lastModified(), QLocale::ShortFormat);
        return {};
    default:
        return {};
    }
}

QVariant FileListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
        return header(section);
    return QAbstractTableModel::headerData(section, orientation, role);
}

void FileListModel::clear()
{
    beginResetModel();
    m_files.clear();
    m_rows.clear();
    endResetModel();
}

void FileListModel::append(std::vector<ListedFile>&& files)
{
    if (files.empty())
        return;
    const int first = static_cast<int>(m_files.size());
    beginInsertRows({}, first, first + static_cast<int>(files.size()) - 1);
    for (ListedFile& file : files) {
        m_rows.insert(file.info.absoluteFilePath(), static_cast<int>(m_files.size()));
        m_files.push_back(std::move(file));
    }
    endInsertRows();
}

QString FileListModel::filePath(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_files.size()))
        return {};
    return m_files[static_cast<std::size_t>(index.row())].info.absoluteFilePath();
}

QModelIndex FileListModel::indexOf(const QString& path) const
{
    const auto row = m_rows.constFind(path);
    return row == m_rows.cend() ? QModelIndex() : index(*row, NameColumn);
}

} // namespace aiedit::ui
