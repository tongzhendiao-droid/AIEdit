#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aiedit::core {

struct PdfText
{
    enum class Status
    {
        Ok,
        Truncated,  // the text reached the limit; what came before is there
        Encrypted,  // not read: the text is encrypted
        NoText,     // read, but no page shows text (scanned pages, or not a PDF after all)
    };
    Status status = Status::Ok;
    std::string utf8; // the pages' text, a blank line between pages
    int pages = 0;
    // Glyphs whose fonts give no way back to characters (no ToUnicode map, no known encoding).
    std::size_t unmappedGlyphs = 0;
};

// The text the pages of a PDF show, in the order their content draws it, with lines and spaces
// inferred from where the glyphs are placed. Tolerant of what writers get wrong: objects are
// found by scanning the file rather than by trusting its cross-reference table. Not read:
// encrypted files, text drawn as images, fonts whose glyphs carry no characters.
PdfText extractPdfText(std::span<const std::uint8_t> data, std::size_t maxText = 64u * 1024 * 1024);

} // namespace aiedit::core
