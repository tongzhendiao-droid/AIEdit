#pragma once

#include <QString>

#include <cstdint>
#include <span>

namespace aiedit::ui {

// Files that are read as the text they hold rather than as their bytes (M10): documents whose
// words are packed in ZIP, XML or PDF, and 易语言 sources. What comes out is only for reading:
// nothing is written back.
enum class ExtractedFormat
{
    None,
    Word,  // .docx
    Excel, // .xlsx
    Epub,
    Pdf,
    Epl, // 易语言 source (.e) or module (.ec)
};

// What the content can be read as, whatever the file is called; None for anything else.
ExtractedFormat extractableFormat(std::span<const std::uint8_t> bytes);
// "Word document", "易语言 source", ... for the status bar and menus.
QString formatName(ExtractedFormat format);

struct ExtractedText
{
    QString text;
    // Written as Markdown (headings, lists, tables), so it can be read drawn.
    bool markdown = false;
    // Why there is no text (an encrypted or broken file), or what was left out; empty if all
    // went well.
    QString note;
};

// The text of a file in that format. On failure the text is empty and the note says why.
ExtractedText extractText(ExtractedFormat format, std::span<const std::uint8_t> bytes);

// Parts of extractText(), for the tests.
ExtractedText extractWord(std::span<const std::uint8_t> bytes);
ExtractedText extractExcel(std::span<const std::uint8_t> bytes);
ExtractedText extractEpub(std::span<const std::uint8_t> bytes);
// A cell of a Markdown table: '|' escaped, line breaks as <br>.
QString markdownCell(QString text);

} // namespace aiedit::ui
