#pragma once

#include <QList>
#include <QString>
#include <QTextLayout>

#include <memory>

class QPalette;

namespace aiedit::ui {

// Syntax highlighting through KSyntaxHighlighting (Kate's engine) when AIEdit is built with it;
// otherwise everything here reports "no highlighting". Callers need no #ifdefs.

struct SyntaxInfo
{
    QString name;              // stable identifier, e.g. "C++"
    QString translatedName;    // for display
    QString translatedSection; // menu group, e.g. "Sources"
};

bool isSyntaxHighlightingAvailable();
// Every selectable syntax, sorted by section and name.
QList<SyntaxInfo> availableSyntaxes();
// The syntax for a file name (extension or pattern), or an empty string.
QString syntaxForFileName(const QString& fileName);
QString translatedSyntaxName(const QString& name);

// The highlighting state between two lines (open comments, strings, …). Cheap to copy.
class SyntaxState
{
public:
    SyntaxState();
    ~SyntaxState();
    SyntaxState(const SyntaxState&);
    SyntaxState& operator=(const SyntaxState&);

private:
    friend class SyntaxHighlighter;
    struct Impl;
    std::shared_ptr<const Impl> m_impl; // null: the state before the first line
};

// Highlights lines with one syntax and a colour theme chosen to suit a palette.
class SyntaxHighlighter
{
public:
    // An unknown name (or a build without the library) gives an inactive highlighter.
    SyntaxHighlighter(const QString& syntaxName, const QPalette& palette);
    ~SyntaxHighlighter();
    SyntaxHighlighter(const SyntaxHighlighter&) = delete;
    SyntaxHighlighter& operator=(const SyntaxHighlighter&) = delete;

    bool isActive() const;
    QString syntaxName() const;
    // Formats for one line, given the state after the line before; `state` becomes the state
    // after this line.
    QList<QTextLayout::FormatRange> highlightLine(const QString& text, SyntaxState& state);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace aiedit::ui
