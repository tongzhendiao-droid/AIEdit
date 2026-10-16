#pragma once

#include "ui/SyntaxHighlighting.h"

#include <QByteArray>

#include <cstdint>
#include <optional>

namespace aiedit::ui {

class TextDocument;

enum class LineOperation
{
    SortAscending, // by the user's locale, numbers by value ("2" before "10")
    SortDescending,
    RemoveDuplicates, // keeps the first of equal lines
    RemoveEmpty,      // also lines of only spaces and tabs
    TrimTrailingWhitespace,
    MoveUp,
    MoveDown,
    Duplicate, // a copy of the lines goes below them, and the selection with it
    CopyUp,    // the same, the selection staying above
    Delete,
    Join,        // with the line after, or the selected ones together: a space for each break
    InsertBelow, // an empty line, indented as the one next to it, with the caret
    InsertAbove,
};

// What a line operation does: [start, end) becomes `bytes`, as one edit, after which
// [selectionStart, selectionEnd) is selected (an empty range is a caret).
struct LineEdit
{
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    QByteArray bytes;
    std::uint64_t selectionStart = 0;
    std::uint64_t selectionEnd = 0;
    std::uint64_t lines = 0; // the lines sorted, removed, trimmed, moved or copied
};

// The edit for the lines `first` to `last` (0-based, inclusive) of a document indexed whole,
// given the selection; nothing if the lines would stay as they are. Lines keep their own bytes,
// which are never decoded and encoded again, so bytes that the codec cannot read survive.
// Line breaks stay where they were: sorting moves the text of the lines, not their endings.
std::optional<LineEdit> lineOperation(LineOperation operation, const TextDocument& document,
                                      std::uint64_t first, std::uint64_t last, std::uint64_t selectionStart,
                                      std::uint64_t selectionEnd);

// Comments the lines out, with the line marker after the indentation they share (or the block
// markers around each), or takes the comments out if every line with text has one. Blank lines
// stay as they are. As bytes, too: only the markers are encoded.
std::optional<LineEdit> toggleComment(const TextDocument& document, const CommentMarkers& markers,
                                      std::uint64_t first, std::uint64_t last, std::uint64_t selectionStart,
                                      std::uint64_t selectionEnd);

} // namespace aiedit::ui
