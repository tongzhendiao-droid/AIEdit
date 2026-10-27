#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

// Writing Markdown: what the editor does for it, on plain text, apart from any widget.
namespace aiedit::ui::markdown {

// By the file's extension (.md, .markdown and the like).
bool isMarkdownFile(const QString& fileName);

// Columns the text takes in a monospaced font: two for CJK and other wide characters.
int displayWidth(QStringView text);

// What Enter does at the end of a list item, given the text of its line up to the caret: go on
// with the list (`insert`: the next item's marker, numbered on), or on an item with nothing in
// it end the list (`remove` that many characters of the line, the marker). Nothing outside a list.
struct ListBreak
{
    QString insert;
    int remove = 0;
};
std::optional<ListBreak> listBreak(QStringView lineBeforeCaret);
// Whether a line is a list item ("- ", "* ", "1. ", "- [ ] " …), however indented.
bool isListItem(QStringView line);

// A table's lines with their columns lined up and padded (as wide as the widest cell, at least
// three for the dashes), keeping each column's alignment (:--, :-:, --:); nothing if the lines
// are not a table (a header row, then a row of dashes).
std::optional<QStringList> formatTable(const QStringList& lines);

// Marks such as ** or ` put around [start, end) of a line, or taken away if they are there
// already, just outside it or at its ends. (One * and two share their character: *** is both.)
// The line as it becomes, and where the text is in it then.
struct MarkedText
{
    QString line;
    qsizetype start = 0;
    qsizetype end = 0;
};
MarkedText toggleMark(const QString& line, qsizetype start, qsizetype end, const QString& mark);

// Where the caret goes in a table row lined up by formatTable(): at the start of the cell it was
// in, at `column` of the row as it was.
qsizetype cellColumn(const QString& row, qsizetype column, const QString& formattedRow);

// The block-level marks a line can start with.
enum class LinePrefix
{
    Heading1,
    Heading2,
    Heading3,
    Bullet,   // "- "
    Numbered, // "1. ", "2. " …
    Task,     // "- [ ] "
    Quote,    // "> "
};
// Gives the lines the mark, replacing another heading or list mark, or takes it away if every
// line with text has it already. Blank lines stay as they are.
QStringList toggleLinePrefix(const QStringList& lines, LinePrefix prefix);

// Headings outside code blocks, in order: where each starts (its line, from 0) and its level.
struct Heading
{
    int line = 0;
    int level = 1;
    QString text;
};
QList<Heading> headings(QStringView text);

} // namespace aiedit::ui::markdown
