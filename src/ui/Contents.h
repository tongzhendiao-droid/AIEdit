#pragma once

#include "ui/LogFilter.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

namespace aiedit::ui {

// A heading of a book or another long text.
struct Heading
{
    std::uint64_t line = 0;  // 0-based
    std::uint64_t start = 0; // where the line starts, in bytes
    QString title;
    int level = 2; // 1 for volumes and parts, 2 for chapters and the like
};

// The rule that finds headings in most books: short lines such as 第三回, 第十二章 风起,
// 五帝本纪第一, 杂阿含经卷第一, 卷二, 序, 后记, 正文 日者列传, Chapter 7 or a Markdown heading.
// An AI-written rule can take its place for books it does not fit.
FilterRule defaultHeadingRule();

// The headings of a text under `context` that pass `rule`, in order. Only lines of at most 40
// characters can be headings. A heading that is just a number (第一回) takes its title from
// the short lines after it; of numbered headings that come twice (a list of contents at the
// start, then the chapters), the later ones stay.
std::vector<Heading> findHeadings(const FilterRule& rule, const SearchContext& context,
                                  const std::function<bool()>& cancelled = {});

// Where the text of a heading ends: at the next heading, or at the end of the text.
std::uint64_t headingEnd(const std::vector<Heading>& headings, std::size_t index, std::uint64_t textEnd);

// The heading a byte offset is under; -1 before the first one.
int headingAt(const std::vector<Heading>& headings, std::uint64_t offset);

} // namespace aiedit::ui
