#include "ui/SearchJob.h"

#include "core/FileSource.h"
#include "core/Search.h"
#include "ui/TextDecoder.h"

#include <QCoreApplication>
#include <QPromise>
#include <QRegularExpression>
#include <QStringEncoder>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>

namespace aiedit::ui {

namespace {

// Regular expressions run over this much decoded text at a time.
constexpr std::uint64_t kRegexBlockBytes = 1024 * 1024;
constexpr std::size_t kMaxRowsPerBlock = 4096;
constexpr auto kIndexWait = std::chrono::milliseconds(20);

using Match = std::pair<std::uint64_t, std::uint64_t>; // [start, end)

bool isCancelled(const SearchHooks& hooks)
{
    return hooks.cancelled && hooks.cancelled();
}

void reportProgress(const SearchHooks& hooks, std::uint64_t done, std::uint64_t total)
{
    if (hooks.progress && total > 0)
        hooks.progress(static_cast<int>(std::min<std::uint64_t>(1000, done * 1000 / total)));
}

// Whether a byte match of encoded text may start at offset, i.e. on a character boundary.
bool startsOnCharacter(const SearchContext& context, std::uint64_t offset)
{
    const core::LineIndex& index = *context.index;
    if (offset < index.textStart())
        return false;
    switch (context.codec.scheme) {
    case core::CharScheme::SingleByte:
    case core::CharScheme::Utf8: // self-synchronising: valid UTF-8 cannot match mid-character
        return true;
    case core::CharScheme::Utf16LE:
    case core::CharScheme::Utf16BE:
    case core::CharScheme::Utf32LE:
    case core::CharScheme::Utf32BE:
        return (offset - index.textStart()) % core::codeUnitSize(context.codec.scheme) == 0;
    default:
        return core::snapToCharStart(index, offset) == offset;
    }
}

std::optional<std::uint64_t> findBytes(const SearchContext& context, const core::BytePattern& pattern,
                                       std::uint64_t from, bool forward, bool onCharacters, const SearchHooks& hooks)
{
    const auto data = context.source->bytes();
    const std::uint64_t span = forward ? data.size() - std::min<std::uint64_t>(from, data.size()) : from;
    std::atomic<bool> cancelled{false};
    core::SearchControl control;
    control.cancelled = &cancelled;
    control.progress = [&](std::uint64_t scanned) {
        cancelled = isCancelled(hooks);
        reportProgress(hooks, scanned, span);
    };

    for (;;) {
        if (isCancelled(hooks))
            return std::nullopt;
        const auto match = core::findPattern(data, pattern, from, forward, control);
        if (!match || !onCharacters || startsOnCharacter(context, *match))
            return match;
        from = forward ? *match + 1 : *match; // inside a character: keep looking
    }
}

class RegexFinder
{
public:
    RegexFinder(const SearchContext& context, const QRegularExpression& regex, const SearchHooks& hooks)
        : m_index(*context.index), m_data(context.source->bytes()), m_regex(regex),
          m_decoder(context.codec), m_hooks(hooks)
    {
    }

    std::optional<Match> forward(std::uint64_t from)
    {
        from = std::clamp<std::uint64_t>(from, m_index.textStart(), m_data.size());
        if (!waitUntilIndexed(from))
            return std::nullopt;
        std::uint64_t row = m_index.rowIndexAt(from);
        for (;;) {
            if (isCancelled(m_hooks) || !waitForRows(row + 1))
                return std::nullopt;
            const auto rows = m_index.rows(row, kMaxRowsPerBlock);
            std::size_t count = 0;
            std::uint64_t bytes = 0;
            while (count < rows.size() && (count == 0 || bytes < kRegexBlockBytes)) {
                bytes += rows[count].end - rows[count].start;
                ++count;
            }

            const std::uint64_t blockStart = rows.front().start;
            const DecodedText text = decode(blockStart, rows[count - 1].end);
            const qsizetype position = text.positionAt(static_cast<std::uint32_t>(std::max(from, blockStart) - blockStart));
            auto matches = m_regex.globalMatch(text.text, position);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                if (match.capturedLength() > 0) // an empty match would select nothing
                    return Match{blockStart + text.byteAt(match.capturedStart()),
                                 blockStart + text.byteAt(match.capturedEnd())};
            }
            reportProgress(m_hooks, rows[count - 1].end - from, m_data.size() - from);
            row += count;
        }
    }

    std::optional<Match> backward(std::uint64_t from)
    {
        from = std::clamp<std::uint64_t>(from, m_index.textStart(), m_data.size());
        if (from == m_index.textStart() || !waitUntilIndexed(from - 1))
            return std::nullopt;
        std::uint64_t lastRow = m_index.rowIndexAt(from - 1);
        for (;;) {
            if (isCancelled(m_hooks))
                return std::nullopt;
            const std::uint64_t firstRow = lastRow >= kMaxRowsPerBlock - 1 ? lastRow - (kMaxRowsPerBlock - 1) : 0;
            const auto rows = m_index.rows(firstRow, static_cast<std::size_t>(lastRow - firstRow + 1));
            std::size_t begin = rows.size();
            std::uint64_t bytes = 0;
            while (begin > 0 && (begin == rows.size() || bytes < kRegexBlockBytes)) {
                --begin;
                bytes += rows[begin].end - rows[begin].start;
            }

            const std::uint64_t blockStart = rows[begin].start;
            const DecodedText text = decode(blockStart, rows.back().end);
            std::optional<Match> latest;
            auto matches = m_regex.globalMatch(text.text);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                if (match.capturedLength() == 0)
                    continue;
                const std::uint64_t start = blockStart + text.byteAt(match.capturedStart());
                if (start >= from)
                    break;
                latest = Match{start, blockStart + text.byteAt(match.capturedEnd())};
            }
            if (latest)
                return latest;
            reportProgress(m_hooks, from - blockStart, from - m_index.textStart());
            if (firstRow + begin == 0)
                return std::nullopt;
            lastRow = firstRow + begin - 1;
        }
    }

private:
    DecodedText decode(std::uint64_t start, std::uint64_t end) const
    {
        return m_decoder.decode(m_data.subspan(start, static_cast<std::size_t>(end - start)), false);
    }

    // The indexer runs on its own thread and is usually far ahead; wait for it when it is not.
    bool waitForRows(std::uint64_t rows) const
    {
        while (m_index.rowCount() < rows) {
            if (m_index.isComplete() || isCancelled(m_hooks))
                return m_index.rowCount() >= rows;
            std::this_thread::sleep_for(kIndexWait);
        }
        return true;
    }

    bool waitUntilIndexed(std::uint64_t offset) const
    {
        while (!m_index.isComplete() && m_index.indexedBytes() <= offset) {
            if (isCancelled(m_hooks))
                return false;
            std::this_thread::sleep_for(kIndexWait);
        }
        return m_index.rowCount() > 0;
    }

    const core::LineIndex& m_index;
    std::span<const std::uint8_t> m_data;
    const QRegularExpression& m_regex;
    TextDecoder m_decoder; // our own: decoders are not shared between threads
    const SearchHooks& m_hooks;
};

SearchOutcome invalidQuery(const QString& error)
{
    SearchOutcome outcome;
    outcome.status = SearchOutcome::Status::InvalidQuery;
    outcome.error = error;
    return outcome;
}

} // namespace

SearchOutcome search(const SearchRequest& request, const SearchContext& context, const SearchHooks& hooks)
{
    if (request.query.isEmpty())
        return invalidQuery(QCoreApplication::translate("SearchJob", "Enter something to search for."));

    const bool useRegex =
        request.mode == SearchMode::Regex || (request.mode == SearchMode::Text && !request.caseSensitive);
    std::optional<core::BytePattern> pattern;
    QRegularExpression regex;
    if (request.mode == SearchMode::Hex) {
        pattern = core::parseHexPattern(request.query.toStdString());
        if (!pattern)
            return invalidQuery(QCoreApplication::translate(
                "SearchJob", "Not a valid hex pattern. Use pairs of hex digits, and ?? for any byte."));
    } else if (!useRegex) {
        QStringEncoder encoder(context.codec.name.data());
        const QByteArray encoded = encoder.encode(request.query);
        if (encoder.hasError() || encoded.isEmpty())
            return invalidQuery(QCoreApplication::translate("SearchJob", "The text cannot be written in %1.")
                                    .arg(QString::fromLatin1(context.codec.name.data())));
        pattern.emplace();
        pattern->bytes.assign(encoded.begin(), encoded.end());
    } else {
        regex.setPattern(request.mode == SearchMode::Regex ? request.query
                                                           : QRegularExpression::escape(request.query));
        QRegularExpression::PatternOptions options =
            QRegularExpression::MultilineOption | QRegularExpression::UseUnicodePropertiesOption;
        if (!request.caseSensitive)
            options |= QRegularExpression::CaseInsensitiveOption;
        regex.setPatternOptions(options);
        if (!regex.isValid())
            return invalidQuery(QCoreApplication::translate("SearchJob", "Invalid regular expression: %1")
                                    .arg(regex.errorString()));
    }

    RegexFinder regexFinder(context, regex, hooks);
    const auto find = [&](std::uint64_t from) -> std::optional<Match> {
        if (pattern) {
            const auto start =
                findBytes(context, *pattern, from, request.forward, request.mode == SearchMode::Text, hooks);
            return start ? std::optional<Match>(Match{*start, *start + pattern->size()}) : std::nullopt;
        }
        return request.forward ? regexFinder.forward(from) : regexFinder.backward(from);
    };

    SearchOutcome outcome;
    std::optional<Match> match = find(request.from);
    if (!match && request.wrap && !isCancelled(hooks)) {
        match = find(request.forward ? 0 : context.source->size());
        outcome.wrapped = match.has_value();
    }
    if (isCancelled(hooks))
        outcome.status = SearchOutcome::Status::Cancelled;
    else if (match) {
        outcome.status = SearchOutcome::Status::Found;
        outcome.start = match->first;
        outcome.end = match->second;
    }
    return outcome;
}

void runSearch(QPromise<SearchOutcome>& promise, const SearchRequest& request, const SearchContext& context)
{
    promise.setProgressRange(0, 1000);
    SearchHooks hooks;
    hooks.cancelled = [&promise] { return promise.isCanceled(); };
    hooks.progress = [&promise](int permille) { promise.setProgressValue(permille); };
    promise.addResult(search(request, context, hooks));
}

} // namespace aiedit::ui
