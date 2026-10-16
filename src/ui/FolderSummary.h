#pragma once

#include <QString>

#include <functional>
#include <string_view>

namespace aiedit::ui {

struct FolderSummaryLimits
{
    int maxEntries = 20'000; // files and folders looked at
    int maxSniffed = 300;    // files whose first bytes are read, the shallowest first
    int maxLayoutLines = 150;
    int maxPreviews = 8; // text files whose first lines are included
    int previewLines = 10;
    qsizetype maxCharacters = 24'000;
};

// What the AI gets when asked what a folder holds, instead of the files themselves: how many
// files there are and of what kinds (told from their content), their extensions, sizes and
// times, the largest ones, names that do not match their content, the layout of the first two
// levels and the first lines of a few text files such as a README. Built on this machine; in
// English, for the model, with the home folder written as ~. Hidden files are left out unless
// `hidden`.
QString summarizeFolder(const QString& folder, std::string_view legacyCodec, bool hidden,
                        const std::function<bool()>& cancelled = {}, const FolderSummaryLimits& limits = {});

} // namespace aiedit::ui
