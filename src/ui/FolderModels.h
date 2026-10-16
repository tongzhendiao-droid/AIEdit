#pragma once

#include "ui/FileTypeSniffer.h"

#include <QAbstractTableModel>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QIcon>
#include <QRegularExpression>

#include <functional>
#include <vector>

namespace aiedit::ui {

// The columns of both folder models, in QFileSystemModel's order. The panel shows the names
// only; the rest is in their tooltips.
enum FolderColumn
{
    NameColumn,
    SizeColumn,
    TypeColumn,
    ModifiedColumn,
    FolderColumnCount,
};

// The folder tree: QFileSystemModel, with the type of each file told from its content (see
// FileTypeSniffer) rather than from its name. A file whose name says otherwise gets a warning
// for its icon; the tooltip of a name says what the file is.
class FolderModel : public QFileSystemModel
{
    Q_OBJECT

public:
    explicit FolderModel(FileTypeSniffer* sniffer, QObject* parent = nullptr);

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    FileTypeSniffer* m_sniffer;
};

// What the folder panel's filter lets through: names that match a wildcard pattern (*.log)
// or contain a text (error); with a slash, the path within the folder (logs/*.gz). Several,
// separated by spaces, commas or semicolons, let through what any of them does. Case does not
// matter.
class NameFilter
{
public:
    explicit NameFilter(const QString& text = {});

    bool isEmpty() const { return m_patterns.empty(); }
    bool matches(const QString& relativePath, const QString& fileName) const;

private:
    struct Pattern
    {
        QRegularExpression wildcard; // when the pattern has * ? or [
        QString part;                // otherwise
        bool path = false;
    };
    std::vector<Pattern> m_patterns;
};

struct ListedFile
{
    QFileInfo info;
    QString relativePath; // within the folder listed, with forward slashes
};

struct FileListHooks
{
    std::function<bool()> cancelled;
    // Files that passed, and how many files and folders have been looked at so far.
    std::function<void(std::vector<ListedFile>&& files, int visited)> found;
};

struct FileListSummary
{
    int visited = 0;
    int matched = 0;
    bool limited = false; // stopped at maxMatches or maxVisited
};

inline constexpr int kMaxListedFiles = 10'000;
inline constexpr int kMaxVisitedEntries = 500'000;

// The files under `folder` that pass the filter, one folder after another in natural order:
// the files of a folder, then its subfolders. Hidden files and folders are left out unless
// `hidden`, links to folders are not followed, and pipes and devices are not listed.
FileListSummary listFiles(const QString& folder, const NameFilter& filter, bool hidden, const FileListHooks& hooks = {},
                          int maxMatches = kMaxListedFiles, int maxVisited = kMaxVisitedEntries);

// The files the filter found, for the folder panel's list: the path within the folder (with the
// size, type and time of change, as in FolderModel).
class FileListModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit FileListModel(FileTypeSniffer* sniffer, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void clear();
    void append(std::vector<ListedFile>&& files);
    QString filePath(const QModelIndex& index) const;
    QModelIndex indexOf(const QString& path) const;

private:
    std::vector<ListedFile> m_files;
    QHash<QString, int> m_rows; // by absolute path, for the type column
    FileTypeSniffer* m_sniffer;
    QIcon m_fileIcon;
};

} // namespace aiedit::ui
