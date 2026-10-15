#pragma once

#include "ui/LogFilter.h"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace aiedit::ui {

// Which files to search, and what their lines must pass. A plain query is a rule with one
// condition (see queryRule()); a filter rule from the filter panel works the same.
struct FileSearchRequest
{
    QString folder;
    QStringList nameFilters; // such as *.log; empty for every file
    bool subfolders = true;
    FilterRule rule;
    std::string_view legacyCodec = "windows-1252"; // tried first for text that is not UTF-8
    std::size_t maxMatches = 10'000;               // in all files together
};

// What one file holds that passed: its lines, each with the start of its text.
struct FileMatches
{
    QString path;
    QString codec;
    std::vector<FilterMatch> matches;
    QStringList texts; // the start of each line, as shown
};

struct FileSearchHooks
{
    std::function<bool()> cancelled;
    std::function<void(int filesSearched)> progress;
    std::function<void(FileMatches&& file)> found;
};

struct FileSearchSummary
{
    int filesSearched = 0;
    int filesSkipped = 0; // binary, unreadable
    std::size_t matches = 0;
    bool limited = false; // stopped at maxMatches
};

// A rule for a query: the text as it is (or a regular expression), anywhere in a line.
FilterRule queryRule(const QString& query, bool regex, bool caseSensitive);

// Searches the files under the folder, one after another, on the calling thread. Hidden files
// and folders and binary files are left out; each file is read in the encoding it is detected
// to have, as when it is opened.
FileSearchSummary searchFiles(const FileSearchRequest& request, const FileSearchHooks& hooks = {});

} // namespace aiedit::ui
