#include "ui/GitIgnore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace aiedit::ui {

namespace {

// Case counts unless the file system ignores it, as git sees it there.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr Qt::CaseSensitivity kCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kCase = Qt::CaseSensitive;
#endif

bool isWildcard(QChar c)
{
    return c == u'*' || c == u'?' || c == u'[' || c == u'\\';
}

// A pattern as a regular expression: * and ? within a name, ** across folders ("**/" any
// folders or none, "/**" at the end everything inside), [...] as in the shell.
QString toRegex(QStringView glob)
{
    QString regex;
    for (qsizetype i = 0; i < glob.size(); ++i) {
        const QChar c = glob[i];
        if (c == u'*' && i + 1 < glob.size() && glob[i + 1] == u'*' && (i == 0 || glob[i - 1] == u'/') &&
            (i + 2 == glob.size() || glob[i + 2] == u'/')) {
            if (i + 2 < glob.size()) {
                regex += QStringLiteral("(?:.*/)?");
                i += 2; // and the slash
            } else {
                regex += QStringLiteral(".*");
                i += 1;
            }
        } else if (c == u'*') {
            regex += QStringLiteral("[^/]*");
            while (i + 1 < glob.size() && glob[i + 1] == u'*')
                ++i;
        } else if (c == u'?') {
            regex += QStringLiteral("[^/]");
        } else if (c == u'[') {
            // A ] first in the class is one of its characters.
            qsizetype close = i + 1;
            if (close < glob.size() && (glob[close] == u'!' || glob[close] == u'^'))
                ++close;
            if (close < glob.size() && glob[close] == u']')
                ++close;
            close = glob.indexOf(u']', close);
            if (close < 0) {
                regex += QStringLiteral("\\[");
                continue;
            }
            QString inside = glob.sliced(i + 1, close - i - 1).toString();
            if (inside.startsWith(u'!'))
                inside[0] = u'^';
            inside.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            regex += QLatin1Char('[') + inside + QLatin1Char(']');
            i = close;
        } else if (c == u'\\' && i + 1 < glob.size()) {
            regex += QRegularExpression::escape(glob.sliced(++i, 1));
        } else {
            regex += QRegularExpression::escape(glob.sliced(i, 1));
        }
    }
    return regex;
}

} // namespace

GitIgnore::GitIgnore(const QString& folder)
{
    // The top of the repository: where .git is, here or further up.
    QDir dir(QFileInfo(folder).absoluteFilePath());
    do {
        if (QFileInfo::exists(dir.filePath(QStringLiteral(".git")))) {
            m_top = dir.absolutePath();
            break;
        }
    } while (dir.cdUp());
    if (m_top.isEmpty())
        return;
    const QString relative = QDir(m_top).relativeFilePath(QFileInfo(folder).absoluteFilePath());
    if (relative != QStringLiteral("."))
        m_prefix = relative + QLatin1Char('/');
    // A folder that git ignores was opened to see what is in it.
    for (qsizetype slash = m_prefix.indexOf(u'/'); slash >= 0; slash = m_prefix.indexOf(u'/', slash + 1)) {
        if (ignoredInRepository(m_prefix.left(slash), true)) {
            m_top.clear();
            return;
        }
    }
}

const std::vector<GitIgnore::Pattern>& GitIgnore::patternsOf(const QString& folder)
{
    if (const auto known = m_patterns.constFind(folder); known != m_patterns.cend())
        return *known;
    std::vector<Pattern>& patterns = m_patterns[folder];
    QFile file(QDir(m_top).filePath(folder.isEmpty() ? QStringLiteral(".gitignore")
                                                    : folder + QStringLiteral("/.gitignore")));
    if (!file.open(QIODevice::ReadOnly))
        return patterns;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(u'\r'))
            line.chop(1);
        // Trailing spaces go, unless a backslash keeps one.
        while (line.endsWith(u' ') && !line.endsWith(QStringLiteral("\\ ")))
            line.chop(1);
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        Pattern pattern;
        if (line.startsWith(u'!')) {
            pattern.negated = true;
            line.remove(0, 1);
        } else if (line.startsWith(QStringLiteral("\\!")) || line.startsWith(QStringLiteral("\\#"))) {
            line.remove(0, 1);
        }
        if (line.endsWith(u'/')) {
            pattern.folders = true;
            line.chop(1);
        }
        // With a slash in it, a path from this folder; without, a name at any depth.
        pattern.name = !line.contains(u'/');
        if (line.startsWith(u'/'))
            line.remove(0, 1);
        if (line.isEmpty())
            continue;
        const bool plain = std::none_of(line.cbegin(), line.cend(), isWildcard);
        if (plain) {
            pattern.literal = line;
        } else if (line.startsWith(u'*') && std::none_of(line.cbegin() + 1, line.cend(), isWildcard)) {
            pattern.literal = line.mid(1); // *.o and the like, by far the most common
            pattern.suffix = true;
        } else {
            pattern.regex = QRegularExpression(QLatin1Char('^') + toRegex(line) + QLatin1Char('$'),
                                               kCase == Qt::CaseInsensitive ? QRegularExpression::CaseInsensitiveOption
                                                                            : QRegularExpression::NoPatternOption);
        }
        patterns.push_back(std::move(pattern));
    }
    return patterns;
}

bool GitIgnore::ignoredInRepository(const QString& path, bool isDir)
{
    // The .gitignore of each folder from the top down to the one holding the path; deeper ones
    // have the last word, and so do later lines.
    bool ignored = false;
    QString folder;
    for (qsizetype slash = -1;;) {
        const QStringView below = QStringView(path).sliced(folder.isEmpty() ? 0 : folder.size() + 1);
        const QStringView name = below.sliced(below.lastIndexOf(u'/') + 1);
        for (const Pattern& pattern : patternsOf(folder)) {
            if (pattern.folders && !isDir)
                continue;
            const QStringView subject = pattern.name ? name : below;
            bool matched = false;
            if (!pattern.literal.isEmpty() || pattern.suffix)
                matched = pattern.suffix ? subject.endsWith(pattern.literal, kCase)
                                         : subject.compare(pattern.literal, kCase) == 0;
            else
                matched = pattern.regex.matchView(subject).hasMatch();
            if (matched)
                ignored = !pattern.negated;
        }
        slash = path.indexOf(u'/', slash + 1);
        if (slash < 0)
            break;
        folder = path.left(slash);
    }
    return ignored;
}

bool GitIgnore::isIgnored(const QString& relativePath, bool isDir)
{
    return isActive() && ignoredInRepository(m_prefix + relativePath, isDir);
}

} // namespace aiedit::ui
