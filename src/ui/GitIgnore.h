#pragma once

#include <QHash>
#include <QRegularExpression>
#include <QString>

#include <vector>

namespace aiedit::ui {

// What git leaves out of a working tree, by the .gitignore files from the top of the repository
// down to each file: names and paths with * ? [...] and **, ! to take one back in, a trailing /
// for folders only, the last that matches deciding. Not the user's global excludes. The files
// are read as the folders are visited, so one of these is for one thread.
//
// Outside a repository nothing is ignored; nor is anything when the folder itself is ignored
// (a build folder opened on purpose).
class GitIgnore
{
public:
    explicit GitIgnore(const QString& folder);

    bool isActive() const { return !m_top.isEmpty(); }
    // A file or folder in the folder, by its path in it with forward slashes.
    bool isIgnored(const QString& relativePath, bool isDir);

private:
    struct Pattern
    {
        QRegularExpression regex; // for those that are not a plain name or *.ext
        QString literal;          // a plain name, or with `suffix` the end of one
        bool suffix = false;
        bool name = false; // matches a name at any depth, not a path from its folder
        bool folders = false;
        bool negated = false;
    };

    // Of a folder, by its path from the top of the repository ("" for the top).
    const std::vector<Pattern>& patternsOf(const QString& folder);
    // A path from the top of the repository.
    bool ignoredInRepository(const QString& path, bool isDir);

    QString m_top;    // of the repository; empty when there is none
    QString m_prefix; // the folder's path in it, with a trailing slash ("" for the top)
    QHash<QString, std::vector<Pattern>> m_patterns;
};

} // namespace aiedit::ui
