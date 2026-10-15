#pragma once

#include "ui/SearchJob.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace aiedit::ui {

struct ReplaceRequest
{
    SearchMode mode = SearchMode::Text; // Text or Regex; bytes in hex are not replaced
    QString query;
    bool caseSensitive = false;
    QString replacement;
    // What \n in a regex replacement inserts: the line terminator the file uses.
    QString lineTerminator = QStringLiteral("\n");
};

// What Replace All does, as one change: the bytes [start, end) become `bytes`. Everything
// between the matches is copied as it was, byte for byte, so nothing else gets re-encoded.
struct ReplaceAllResult
{
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::vector<std::uint8_t> bytes;
    std::size_t count = 0;
    QString error; // the query is invalid, or the codec cannot write a replacement
};

// The text that replaces the match at [start, end). In text mode that is the replacement as
// typed. In regex mode $1 or \1 stand for a group ($0 for the whole match), \n for a line
// break and \t for a tab. Nothing, with the reason in `error`, if [start, end) is not a match.
std::optional<QString> replacementFor(const ReplaceRequest& request, const SearchContext& context,
                                      std::uint64_t start, std::uint64_t end, QString* error = nullptr);

// Every match in the document replaced. Like searching, regular expressions run over blocks of
// about 1 MiB of decoded rows, which matches cannot span. In regex mode, empty matches count
// too: ^ inserts at the start of every line.
ReplaceAllResult replaceAll(const ReplaceRequest& request, const SearchContext& context);

} // namespace aiedit::ui
