#pragma once

#include "core/LineIndex.h"
#include "core/TextCodec.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

template <typename T>
class QPromise;

namespace cppedit::core {
class FileSource;
}

namespace cppedit::ui {

enum class SearchMode
{
    Text,
    Regex,
    Hex,
};

struct SearchRequest
{
    SearchMode mode = SearchMode::Text;
    QString query;
    bool caseSensitive = false;
    bool forward = true;
    // Forward: the first offset a match may start at. Backward: matches must start before it.
    std::uint64_t from = 0;
    bool wrap = true;
};

// Everything a search needs, owned so that it can run on another thread while the document
// changes its codec or closes.
struct SearchContext
{
    std::shared_ptr<const core::FileSource> source;
    std::shared_ptr<const core::LineIndex> index;
    core::TextCodec codec;
};

struct SearchOutcome
{
    enum class Status
    {
        Found,
        NotFound,
        InvalidQuery,
        Cancelled,
    };

    Status status = Status::NotFound;
    std::uint64_t start = 0; // the match is [start, end)
    std::uint64_t end = 0;
    bool wrapped = false;
    QString error; // why the query is invalid
};

struct SearchHooks
{
    std::function<bool()> cancelled;
    std::function<void(int permille)> progress;
};

// Finds the next (or previous) match.
//
// Exact text and hex patterns are searched as bytes: the text is first encoded in the
// document's codec, and matches that do not start on a character boundary are skipped.
// Case-insensitive text and regular expressions decode the text in blocks of rows and use
// QRegularExpression, so they work in every encoding; matches cannot span blocks of ~1 MiB.
SearchOutcome search(const SearchRequest& request, const SearchContext& context, const SearchHooks& hooks = {});

// QtConcurrent::run entry point: progress in permille, cancellation through the future.
void runSearch(QPromise<SearchOutcome>& promise, const SearchRequest& request, const SearchContext& context);

} // namespace cppedit::ui
