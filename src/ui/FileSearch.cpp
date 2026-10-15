#include "ui/FileSearch.h"

#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/Collation.h"
#include "ui/Paths.h"
#include "ui/TextDecoder.h"

#include <QCollator>
#include <QDirIterator>
#include <QRegularExpression>

#include <algorithm>
#include <memory>

namespace aiedit::ui {

namespace {

constexpr qsizetype kShownCharacters = 300; // of a line, in the results

// The files to search, in a stable order: by folder, then name, numbers by value.
QStringList filesUnder(const FileSearchRequest& request, const std::function<bool()>& cancelled)
{
    QStringList files;
    // Hidden files and folders are left out (no QDir::Hidden), symbolic links to folders are
    // not followed, so a link cannot make the walk go round in circles.
    QDirIterator it(request.folder, request.nameFilters, QDir::Files | QDir::Readable,
                    request.subfolders ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext() && !cancelled())
        files << it.next();
    const QCollator collator = naturalCollator();
    std::sort(files.begin(), files.end(), [&collator](const QString& a, const QString& b) {
        const qsizetype slashA = std::max<qsizetype>(0, a.lastIndexOf(QLatin1Char('/')));
        const qsizetype slashB = std::max<qsizetype>(0, b.lastIndexOf(QLatin1Char('/')));
        if (const int folder = collator.compare(a.first(slashA), b.first(slashB)); folder != 0)
            return folder < 0;
        return collator.compare(a.sliced(slashA), b.sliced(slashB)) < 0;
    });
    return files;
}

} // namespace

FilterRule queryRule(const QString& query, bool regex, bool caseSensitive)
{
    FilterCondition condition;
    condition.pattern = regex ? query : QRegularExpression::escape(query);
    condition.caseSensitive = caseSensitive;
    FilterRule rule;
    rule.conditions = {condition};
    return rule;
}

FileSearchSummary searchFiles(const FileSearchRequest& request, const FileSearchHooks& hooks)
{
    FileSearchSummary summary;
    const auto cancelled = [&hooks] { return hooks.cancelled && hooks.cancelled(); };
    if (!request.rule.check().isEmpty())
        return summary;

    for (const QString& path : filesUnder(request, cancelled)) {
        if (cancelled() || summary.limited)
            break;
        std::error_code ec;
        const std::shared_ptr<const core::FileSource> source = core::FileSource::open(toPath(path), ec);
        const auto data = source ? source->bytes() : std::span<const std::uint8_t>();
        const core::FileTypeInfo type = core::detectFileType(data);
        if (!source || type.kind == core::ContentKind::Binary) {
            ++summary.filesSkipped;
            continue;
        }
        // Read as it would be when opened: the detected codec, the file's own line breaks.
        const core::TextCodec& codec = core::defaultCodec(type, data, request.legacyCodec);
        const core::LineEndings endings =
            core::countLineEndings(data.first(std::min(data.size(), core::kSniffSize)), codec.scheme);
        const auto index = std::make_shared<core::LineIndex>(data, codec.scheme, endings.lineBreak(),
                                                             core::bomLength(data, codec.scheme));
        index->build();

        FileMatches file;
        file.path = path;
        file.codec = QString::fromLatin1(codec.name.data());
        FilterHooks filter;
        filter.cancelled = [&] { return cancelled() || summary.limited; };
        filter.found = [&](std::vector<FilterMatch>&& batch) {
            const std::size_t room = request.maxMatches - summary.matches - file.matches.size();
            if (batch.size() >= room) {
                batch.resize(room);
                summary.limited = true;
            }
            file.matches.insert(file.matches.end(), batch.begin(), batch.end());
        };
        filterLines(request.rule, {source, index, codec}, 0, filter);

        // The start of each line, for the results; the file is not kept open for them.
        const TextDecoder decoder(codec);
        for (const FilterMatch& match : file.matches) {
            const core::Row row =
                core::LineIndex::readRow(data, match.start, match.line, false, codec.scheme, endings.lineBreak());
            const std::size_t length = static_cast<std::size_t>(row.contentEnd - row.start);
            QString text = decoder.decodePlain(data.subspan(row.start, length));
            const bool goesOn = row.end == row.contentEnd && row.end < data.size();
            if (text.size() > kShownCharacters || goesOn) {
                text.truncate(kShownCharacters);
                text += QStringLiteral("…");
            }
            file.texts << text;
        }
        summary.matches += file.matches.size();
        ++summary.filesSearched;
        if (hooks.progress)
            hooks.progress(summary.filesSearched);
        if (!file.matches.empty() && hooks.found)
            hooks.found(std::move(file));
    }
    return summary;
}

} // namespace aiedit::ui
