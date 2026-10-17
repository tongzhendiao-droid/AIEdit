#include "ui/FolderSummary.h"

#include "core/TextCodec.h"
#include "ui/Codec.h"
#include "ui/Collation.h"
#include "ui/FileTypeSniffer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QSet>

#include <algorithm>
#include <deque>
#include <optional>
#include <vector>

namespace aiedit::ui {

namespace {

constexpr std::size_t kLargest = 10;
constexpr qsizetype kMaxKinds = 15;
constexpr qsizetype kMaxExtensions = 15;
constexpr qsizetype kMaxMismatches = 20;
constexpr qsizetype kMaxSubfolders = 12; // listed under each folder of the first level
constexpr qsizetype kMaxPreviewLine = 200;
constexpr qint64 kPreviewBytes = 4096;

struct Totals
{
    qint64 files = 0;
    qint64 folders = 0;
    qint64 bytes = 0;
};

struct Count
{
    qint64 files = 0;
    qint64 bytes = 0;
};

QString shownSize(qint64 bytes)
{
    return QLocale::c().formattedDataSize(bytes, 1);
}

QString files(qint64 count)
{
    return count == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(count);
}

QString folders(qint64 count)
{
    return count == 1 ? QStringLiteral("1 folder") : QStringLiteral("%1 folders").arg(count);
}

QString fromView(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString kindOf(const SniffedType& type)
{
    if (type.unreadable)
        return QStringLiteral("unreadable");
    switch (type.type.kind) {
    case core::ContentKind::Empty:
        return QStringLiteral("empty");
    case core::ContentKind::Binary:
        return type.type.format.empty() ? QStringLiteral("binary (unknown format)") : fromView(type.type.format);
    case core::ContentKind::Text:
        return type.type.encoding == core::Encoding::Ascii ? QStringLiteral("text, ASCII")
                                                            : QStringLiteral("text, %1").arg(type.codec);
    }
    return {};
}

// The home folder as ~: the model needs no user name.
QString shownPath(const QString& path)
{
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

// How much a text file is likely to say about the folder, lower first; -1 for not at all.
int previewRank(const QString& name, int depth)
{
    const QString lower = name.toLower();
    const bool readme = lower.startsWith(QLatin1String("readme"));
    if (depth == 0) {
        if (readme)
            return 0;
        for (const char* suffix : {".md", ".txt", ".rst"})
            if (lower.endsWith(QLatin1String(suffix)))
                return 1;
        for (const char* suffix : {".yaml", ".yml", ".toml", ".json", ".ini", ".cfg", ".conf", ".xml", ".csv"})
            if (lower.endsWith(QLatin1String(suffix)))
                return 2;
        return 3;
    }
    if (depth == 1)
        return readme ? 4 : 5;
    return -1;
}

// The first lines of a text file, each cut short if long.
QStringList firstLines(const QString& path, const QString& codec, int count)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = file.read(kPreviewBytes);
    const QByteArray name = codec.toLatin1();
    const core::TextCodec* known =
        core::findCodec(std::string_view(name.constData(), static_cast<std::size_t>(name.size())));
    const Codec decoder(known ? *known : core::utf8Codec());
    QString text = decoder.isValid() ? decoder.decode(head) : QString::fromUtf8(head);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    QStringList lines = text.split(QLatin1Char('\n'));
    if (head.size() == kPreviewBytes && lines.size() > 1)
        lines.removeLast(); // cut off by the end of what was read
    QStringList shown;
    for (QString line : std::as_const(lines)) {
        if (shown.size() >= count)
            break;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.size() > kMaxPreviewLine)
            line = line.left(kMaxPreviewLine) + QStringLiteral(" …");
        shown << line;
    }
    return shown;
}

template <typename Map>
std::vector<std::pair<QString, Count>> byFiles(const Map& counts)
{
    std::vector<std::pair<QString, Count>> sorted;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        sorted.emplace_back(it.key(), it.value());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second.files != b.second.files ? a.second.files > b.second.files : a.first < b.first;
    });
    return sorted;
}

} // namespace

QString summarizeFolder(const QString& folder, std::string_view legacyCodec, bool hidden,
                        const std::function<bool()>& cancelled, const FolderSummaryLimits& limits)
{
    const auto stopped = [&cancelled] { return cancelled && cancelled(); };
    const QString rootPath = QDir(folder).absolutePath();
    const QCollator collator = naturalCollator();
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (hidden)
        filters |= QDir::Hidden;

    Totals all;
    bool limited = false;
    QDateTime oldest;
    QDateTime newest;
    QHash<QString, Count> extensions; // ".log", or "" for none
    QHash<QString, Count> kinds;      // from the content
    int sniffed = 0;
    struct Large
    {
        qint64 size;
        QString relative;
        QString path;
        QString kind;
    };
    std::vector<Large> largest;
    QStringList mismatches;
    QHash<QString, Totals> levels; // the folders of the first two levels, by relative path
    struct Entry
    {
        QString name;
        bool folder;
        qint64 size;
        QString kind;
    };
    std::vector<Entry> top;            // what the folder itself holds
    QHash<QString, QStringList> below; // the folders in each folder of the first level
    struct Candidate
    {
        int rank;
        QString relative;
        QString path;
        QString codec;
    };
    std::vector<Candidate> candidates;
    QSet<QString> previewed; // folders of the first level that have a candidate already

    struct Folder
    {
        QString path;
        QString relative;
        int depth;
    };
    std::deque<Folder> queue{{rootPath, QString(), 0}};
    int seen = 0;
    while (!queue.empty() && !limited) {
        if (stopped())
            return {};
        const Folder dir = queue.front();
        queue.pop_front();
        QFileInfoList entries = QDir(dir.path).entryInfoList(filters, QDir::NoSort);
        std::sort(entries.begin(), entries.end(), [&collator](const QFileInfo& a, const QFileInfo& b) {
            if (a.isDir() != b.isDir())
                return a.isDir();
            return collator.compare(a.fileName(), b.fileName()) < 0;
        });
        const QString first = dir.relative.section(QLatin1Char('/'), 0, 0);
        const QString second = dir.relative.section(QLatin1Char('/'), 0, 1);
        for (const QFileInfo& entry : std::as_const(entries)) {
            if (++seen > limits.maxEntries) {
                limited = true;
                break;
            }
            const QString relative =
                dir.relative.isEmpty() ? entry.fileName() : dir.relative + QLatin1Char('/') + entry.fileName();
            if (entry.isDir()) {
                ++all.folders;
                if (dir.depth >= 1)
                    ++levels[first].folders;
                if (dir.depth >= 2)
                    ++levels[second].folders;
                if (dir.depth == 0)
                    top.push_back({entry.fileName(), true, 0, {}});
                else if (dir.depth == 1)
                    below[dir.relative] << entry.fileName();
                if (!entry.isSymLink())
                    queue.push_back({entry.absoluteFilePath(), relative, dir.depth + 1});
                continue;
            }
            if (!entry.isFile())
                continue;

            const qint64 size = entry.size();
            ++all.files;
            all.bytes += size;
            const QDateTime modified = entry.lastModified();
            if (!oldest.isValid() || modified < oldest)
                oldest = modified;
            if (!newest.isValid() || modified > newest)
                newest = modified;
            const QString suffix = entry.suffix().toLower();
            Count& extension = extensions[suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix];
            ++extension.files;
            extension.bytes += size;
            for (const auto& [level, deep] : {std::pair{first, dir.depth >= 1}, std::pair{second, dir.depth >= 2}}) {
                if (!deep)
                    continue;
                Totals& totals = levels[level];
                ++totals.files;
                totals.bytes += size;
            }

            std::optional<SniffedType> type;
            if (sniffed < limits.maxSniffed) {
                type = FileTypeSniffer::sniff(entry.absoluteFilePath(), legacyCodec);
                ++sniffed;
                Count& kind = kinds[kindOf(*type)];
                ++kind.files;
                kind.bytes += size;
                if (!type->mismatch.empty() && mismatches.size() < kMaxMismatches) {
                    const QByteArray name = entry.fileName().toUtf8();
                    mismatches << QStringLiteral("- %1: the name says %2, but the content is %3")
                                      .arg(relative,
                                           fromView(core::formatForName(
                                               {name.constData(), static_cast<std::size_t>(name.size())})),
                                           fromView(type->mismatch));
                }
                if (type->type.kind == core::ContentKind::Text) {
                    const int rank = previewRank(entry.fileName(), dir.depth);
                    const bool another = dir.depth == 1 && rank == 5 && previewed.contains(dir.relative);
                    if (rank >= 0 && !another) {
                        if (dir.depth == 1 && rank == 5)
                            previewed.insert(dir.relative);
                        candidates.push_back({rank, relative, entry.absoluteFilePath(), type->codec});
                    }
                }
            }
            if (dir.depth == 0)
                top.push_back({entry.fileName(), false, size, type ? kindOf(*type) : QString()});
            if (largest.size() < kLargest || size > largest.back().size) {
                const Large large{size, relative, entry.absoluteFilePath(), type ? kindOf(*type) : QString()};
                largest.insert(std::upper_bound(largest.begin(), largest.end(), large,
                                                [](const Large& a, const Large& b) { return a.size > b.size; }),
                               large);
                if (largest.size() > kLargest)
                    largest.pop_back();
            }
        }
    }
    if (stopped())
        return {};

    QStringList out;
    out << QStringLiteral("Folder: %1").arg(shownPath(rootPath));
    out << QStringLiteral("Looked at: %1 and %2, %3 in all%4.")
               .arg(files(all.files), folders(all.folders), shownSize(all.bytes),
                    hidden ? QString() : QStringLiteral(" (hidden files and folders left out)"));
    if (limited)
        out << QStringLiteral("Only the first %1 entries were looked at; the folder holds more.")
                   .arg(limits.maxEntries);
    if (oldest.isValid())
        out << QStringLiteral("Files modified between %1 and %2.")
                   .arg(oldest.date().toString(Qt::ISODate), newest.date().toString(Qt::ISODate));

    if (!kinds.isEmpty()) {
        out << QString() << QStringLiteral("Kinds of files, told from the content of %1 of them:").arg(sniffed);
        const auto sorted = byFiles(kinds);
        for (std::size_t i = 0; i < sorted.size() && static_cast<qsizetype>(i) < kMaxKinds; ++i)
            out << QStringLiteral("- %1: %2, %3")
                       .arg(sorted[i].first, files(sorted[i].second.files), shownSize(sorted[i].second.bytes));
    }
    if (!extensions.isEmpty()) {
        out << QString() << QStringLiteral("File name extensions:");
        const auto sorted = byFiles(extensions);
        Count rest;
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (static_cast<qsizetype>(i) >= kMaxExtensions) {
                rest.files += sorted[i].second.files;
                rest.bytes += sorted[i].second.bytes;
                continue;
            }
            out << QStringLiteral("- %1: %2, %3")
                       .arg(sorted[i].first.isEmpty() ? QStringLiteral("no extension") : sorted[i].first,
                            files(sorted[i].second.files), shownSize(sorted[i].second.bytes));
        }
        if (rest.files > 0)
            out << QStringLiteral("- %1 other extensions: %2, %3")
                       .arg(QString::number(static_cast<qsizetype>(sorted.size()) - kMaxExtensions), files(rest.files),
                            shownSize(rest.bytes));
    }
    if (!largest.empty()) {
        out << QString() << QStringLiteral("Largest files:");
        for (Large& large : largest) {
            if (large.kind.isEmpty())
                large.kind = kindOf(FileTypeSniffer::sniff(large.path, legacyCodec));
            out << QStringLiteral("- %1: %2, %3").arg(large.relative, shownSize(large.size), large.kind);
        }
    }
    if (!mismatches.isEmpty())
        out << QString() << QStringLiteral("Names that do not match the content:") << mismatches;

    if (!top.empty()) {
        out << QString() << QStringLiteral("Layout of the first two levels:");
        int lines = 0;
        for (std::size_t i = 0; i < top.size(); ++i) {
            if (lines >= limits.maxLayoutLines) {
                out << QStringLiteral("- … and %1 more entries").arg(top.size() - i);
                break;
            }
            const Entry& entry = top[i];
            ++lines;
            if (!entry.folder) {
                const QString size = shownSize(entry.size);
                out << QStringLiteral("- %1 (%2)")
                           .arg(entry.name, entry.kind.isEmpty() ? size : size + QStringLiteral(", ") + entry.kind);
                continue;
            }
            const Totals totals = levels.value(entry.name);
            out << QStringLiteral("- %1/ (%2, %3, %4)")
                       .arg(entry.name, files(totals.files), folders(totals.folders), shownSize(totals.bytes));
            const QStringList inside = below.value(entry.name);
            for (qsizetype k = 0; k < inside.size(); ++k) {
                if (k >= kMaxSubfolders) {
                    out << QStringLiteral("  - … and %1 more folders").arg(inside.size() - k);
                    ++lines;
                    break;
                }
                const QString relative = entry.name + QLatin1Char('/') + inside[k];
                const Totals sub = levels.value(relative);
                out << QStringLiteral("  - %1/ (%2, %3)").arg(relative, files(sub.files), shownSize(sub.bytes));
                ++lines;
            }
        }
    }
    if (all.files == 0)
        out << QString() << QStringLiteral("There are no files in it.");

    std::stable_sort(candidates.begin(), candidates.end(), [&collator](const Candidate& a, const Candidate& b) {
        return a.rank != b.rank ? a.rank < b.rank : collator.compare(a.relative, b.relative) < 0;
    });
    if (candidates.size() > static_cast<std::size_t>(limits.maxPreviews))
        candidates.resize(static_cast<std::size_t>(limits.maxPreviews));
    bool heading = false;
    for (const Candidate& candidate : candidates) {
        if (stopped())
            return {};
        const QStringList lines = firstLines(candidate.path, candidate.codec, limits.previewLines);
        if (lines.isEmpty())
            continue;
        if (!heading) {
            out << QString() << QStringLiteral("The start of some text files:");
            heading = true;
        }
        out << QStringLiteral("--- %1 (first %2 lines)").arg(candidate.relative).arg(lines.size()) << lines;
    }

    QString summary = out.join(QLatin1Char('\n'));
    if (summary.size() > limits.maxCharacters) {
        summary.truncate(limits.maxCharacters);
        summary += QStringLiteral("\n[The summary is cut short here.]");
    }
    return summary;
}

} // namespace aiedit::ui
