#pragma once

#include "ui/SearchJob.h"

#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace aiedit::ui {

// One test a line goes through: a regular expression it must contain, or for an exclusion
// must not. It can also compare a number from one of its named groups with a value: the
// pattern took (?<ms>\d+)ms with field ms, op > and value 1000 passes lines that took longer
// than a second.
struct FilterCondition
{
    bool exclude = false;
    QString pattern;
    bool caseSensitive = false;
    QString field; // a named group of the pattern (or its number); empty for none
    QString op;    // >, >=, <, <=, == or !=
    double value = 0;

    // The pattern with the options the filter uses (any case unless caseSensitive).
    QRegularExpression regex() const;
    // Empty if it can run; otherwise what is wrong with it.
    QString check() const;

    bool operator==(const FilterCondition&) const = default;
};

// Which lines of a log to show: those that pass every inclusion (or any one of them, with
// anyInclusion) and no exclusion. The AI writes these from a sentence; they run here, on
// the whole file, so the log itself never leaves the machine.
struct FilterRule
{
    QList<FilterCondition> conditions;
    bool anyInclusion = false;
    QString description; // what it is for, in a few words

    // Empty if it can run; otherwise what is wrong with it.
    QString check() const;
    QJsonObject toJson() const;
    static std::optional<FilterRule> fromJson(const QJsonObject& object, QString* error = nullptr);

    bool operator==(const FilterRule&) const = default;
};

// A line that passed: its 0-based number and where it starts, to show it and to go to it.
struct FilterMatch
{
    std::uint64_t line = 0;
    std::uint64_t start = 0; // byte offset

    bool operator==(const FilterMatch&) const = default;
};

struct FilterHooks
{
    std::function<bool()> cancelled;
    std::function<void(int permille)> progress; // of the whole file
    std::function<void(std::vector<FilterMatch>&& batch)> found; // matches in order, a block at a time
};

// Runs a rule over the lines from `firstRow` on (the first row of a line), so that the first
// matches can show while the rest of the file is read. Lines still being indexed are waited
// for. Of a line longer than 1 MiB only the start is tested. The rule must pass check().
void filterLines(const FilterRule& rule, const SearchContext& context, std::uint64_t firstRow,
                 const FilterHooks& hooks);
// The same in one go (for tests).
std::vector<FilterMatch> filterLines(const FilterRule& rule, const SearchContext& context,
                                     std::uint64_t firstRow = 0);

// How serious a log line is, from the first level word in its first 200 characters: ERROR,
// warn, [Info], FATAL … (any case, as whole words).
enum class LogLevel
{
    None,
    Trace, // TRACE, VERBOSE
    Debug,
    Info, // INFO, NOTICE
    Warning,
    Error, // ERROR, ERR, SEVERE
    Fatal, // FATAL, CRITICAL, CRIT, EMERG, ALERT, PANIC
};

struct LogLevelSpan
{
    LogLevel level = LogLevel::None;
    qsizetype start = 0; // where the word is
    qsizetype length = 0;
};

LogLevelSpan findLogLevel(QStringView line);

} // namespace aiedit::ui
