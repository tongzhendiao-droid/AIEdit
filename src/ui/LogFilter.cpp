#include "ui/LogFilter.h"

#include "core/FileSource.h"
#include "ui/Codec.h"
#include "ui/TextDecoder.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

namespace aiedit::ui {

namespace {

constexpr std::uint64_t kBlockBytes = 1024 * 1024; // decoded at a time
constexpr std::size_t kMaxRowsPerBlock = 4096;
constexpr std::uint64_t kMaxLineBytes = 1024 * 1024; // of a longer line, only its start is tested
constexpr auto kIndexWait = std::chrono::milliseconds(20);

const QStringList& operators()
{
    static const QStringList list{QStringLiteral(">"),  QStringLiteral(">="), QStringLiteral("<"),
                                  QStringLiteral("<="), QStringLiteral("=="), QStringLiteral("!=")};
    return list;
}

struct Compiled
{
    QRegularExpression regex;
    bool exclude = false;
    int group = -1; // the captured number is compared with `value`, if not -1
    QString op;
    double value = 0;
};

std::optional<Compiled> compileCondition(const FilterCondition& condition, QString* problem)
{
    const auto fail = [problem](const QString& message) {
        if (problem)
            *problem = message;
        return std::nullopt;
    };
    if (condition.pattern.isEmpty())
        return fail(QCoreApplication::translate("LogFilter", "the regular expression is empty"));
    Compiled c;
    c.regex = condition.regex();
    if (!c.regex.isValid())
        return fail(QCoreApplication::translate("LogFilter", "invalid regular expression: %1")
                        .arg(c.regex.errorString()));
    if (!condition.field.isEmpty()) {
        bool numbered = false;
        c.group = condition.field.toInt(&numbered);
        if (!numbered)
            c.group = static_cast<int>(c.regex.namedCaptureGroups().indexOf(condition.field));
        if (c.group < 0 || c.group > c.regex.captureCount())
            return fail(QCoreApplication::translate("LogFilter", "the expression has no group named %1")
                            .arg(condition.field));
        if (!operators().contains(condition.op))
            return fail(QCoreApplication::translate("LogFilter", "unknown comparison %1").arg(condition.op));
        c.op = condition.op;
        c.value = condition.value;
    }
    c.exclude = condition.exclude;
    c.regex.optimize();
    return c;
}

std::optional<std::vector<Compiled>> compile(const FilterRule& rule, QString* error)
{
    if (rule.conditions.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("LogFilter", "The rule has no conditions yet");
        return std::nullopt;
    }
    std::vector<Compiled> compiled;
    for (qsizetype i = 0; i < rule.conditions.size(); ++i) {
        QString problem;
        std::optional<Compiled> c = compileCondition(rule.conditions[i], &problem);
        if (!c) {
            if (error)
                *error = QCoreApplication::translate("LogFilter", "Condition %1: %2").arg(i + 1).arg(problem);
            return std::nullopt;
        }
        compiled.push_back(std::move(*c));
    }
    return compiled;
}

bool compare(double a, const QString& op, double b)
{
    if (op == u">")
        return a > b;
    if (op == u">=")
        return a >= b;
    if (op == u"<")
        return a < b;
    if (op == u"<=")
        return a <= b;
    if (op == u"==")
        return a == b;
    return a != b;
}

bool passes(const std::vector<Compiled>& conditions, bool anyInclusion, QStringView line,
            QRegularExpression::MatchOptions options)
{
    bool included = false;
    bool inclusions = false;
    for (const Compiled& c : conditions) {
        const QRegularExpressionMatch match =
            c.regex.matchView(line, 0, QRegularExpression::NormalMatch, options);
        bool ok = match.hasMatch();
        if (ok && c.group >= 0) {
            bool number = false;
            const double value = match.capturedView(c.group).toDouble(&number);
            ok = number && compare(value, c.op, c.value);
        }
        if (c.exclude) {
            if (ok)
                return false;
            continue;
        }
        inclusions = true;
        if (ok)
            included = true;
        else if (!anyInclusion)
            return false;
    }
    return !inclusions || included;
}

void scan(const FilterRule& rule, const SearchContext& context, std::uint64_t firstRow,
          const std::function<void(std::vector<FilterMatch>&&)>& emitBatch,
          const std::function<bool()>& cancelled, const std::function<void(int)>& progress)
{
    const std::optional<std::vector<Compiled>> conditions = compile(rule, nullptr);
    if (!conditions)
        return;
    const core::LineIndex& index = *context.index;
    const auto data = context.source->bytes();
    const TextDecoder decoder(context.codec);
    // Blocks of short lines are decoded in one go (by Qt, or the system's code pages), many
    // times faster than by TextDecoder, which also notes where every character came from. The
    // lines are found again at their breaks: every supported encoding decodes a line break byte
    // to a line break. A block starts on a line start, so no state carries over from the one
    // before.
    const Codec blockDecoder(context.codec, QStringConverter::Flag::ConvertInitialBom);
    const char16_t lineBreak = index.lineBreak() == core::LineBreak::Lf ? u'\n' : u'\r';
    // What Qt's UTF-8 and legacy decoders produce is valid UTF-16, and so is what the code pages
    // do, so PCRE2 need not check it. Qt's UTF-16 and UTF-32 decoders may pass lone surrogates
    // through.
    const core::CharScheme scheme = context.codec.scheme;
    const bool wide = scheme == core::CharScheme::Utf16LE || scheme == core::CharScheme::Utf16BE ||
                      scheme == core::CharScheme::Utf32LE || scheme == core::CharScheme::Utf32BE;
    const QRegularExpression::MatchOptions fastOptions =
        wide ? QRegularExpression::NoMatchOption : QRegularExpression::DontCheckSubjectStringMatchOption;

    for (std::uint64_t row = firstRow;;) {
        if (cancelled())
            return;
        const std::vector<core::Row> rows = index.rows(row, kMaxRowsPerBlock);
        if (rows.empty()) {
            if (index.isComplete())
                break;
            std::this_thread::sleep_for(kIndexWait); // the indexer is still on its way
            continue;
        }
        if (rows.front().continuation) {
            // The rest of a line too long to test whole: its start has been tested.
            std::size_t skip = 1;
            while (skip < rows.size() && rows[skip].continuation)
                ++skip;
            row += skip;
            continue;
        }

        // Whole lines, about a block of them, decoded together. Of a very long line only the
        // start is tested, and the block ends with it.
        const auto lineIsWhole = [&](std::size_t end) {
            if (end < rows.size())
                return true; // a new line follows
            const bool complete = index.isComplete(); // before looking, so a late row is not missed
            const std::vector<core::Row> next = index.rows(row + end, 1);
            return next.empty() ? complete : !next.front().continuation;
        };
        std::vector<std::pair<std::size_t, std::size_t>> lines; // first row, end of the rows tested
        std::size_t consumed = 0;
        std::uint64_t blockBytes = 0;
        for (std::size_t i = 0; i < rows.size() && blockBytes < kBlockBytes;) {
            std::size_t j = i + 1;
            while (j < rows.size() && rows[j].continuation)
                ++j;
            std::size_t k = i;
            std::uint64_t lineBytes = 0;
            for (; k < j && lineBytes < kMaxLineBytes; ++k)
                lineBytes += rows[k].end - rows[k].start;
            if (lineBytes < kMaxLineBytes && !lineIsWhole(j))
                break; // the rest of it is not indexed yet: tested in a later round
            lines.emplace_back(i, k);
            blockBytes += lineBytes;
            consumed = j;
            i = j;
            if (k < j)
                break;
        }
        if (lines.empty()) {
            std::this_thread::sleep_for(kIndexWait);
            continue;
        }

        const std::uint64_t blockStart = rows.front().start;
        const std::uint64_t blockEnd = rows[lines.back().second - 1].end;
        std::vector<FilterMatch> matches;
        bool tested = false;
        const bool oneRowLines = std::all_of(lines.begin(), lines.end(),
                                             [](const auto& line) { return line.second == line.first + 1; });
        if (oneRowLines && blockDecoder.isValid()) {
            const QString text = blockDecoder.decode(
                QByteArrayView(data.data() + blockStart, static_cast<qsizetype>(blockEnd - blockStart)));
            QStringView rest(text);
            tested = true;
            for (const auto& [first, end] : lines) {
                const core::Row& r = rows[first];
                const qsizetype at = rest.indexOf(QChar(lineBreak));
                if ((at >= 0) != (r.end != r.contentEnd)) {
                    tested = false; // the breaks do not line up with the rows: decode the slow way
                    break;
                }
                QStringView line = at >= 0 ? rest.first(at) : rest;
                if (at >= 0 && lineBreak == u'\n' && line.endsWith(u'\r'))
                    line.chop(1); // CRLF
                if (passes(*conditions, rule.anyInclusion, line, fastOptions))
                    matches.push_back({r.line, r.start});
                rest = at >= 0 ? rest.sliced(at + 1) : QStringView();
            }
            tested = tested && rest.isEmpty();
        }
        if (!tested) {
            matches.clear();
            const std::size_t length = static_cast<std::size_t>(blockEnd - blockStart);
            const DecodedText text = decoder.decode(data.subspan(blockStart, length), false);
            const auto position = [&](std::uint64_t offset) {
                return text.positionAt(static_cast<std::uint32_t>(offset - blockStart));
            };
            for (const auto& [first, end] : lines) {
                const qsizetype from = position(rows[first].start);
                const qsizetype to = position(rows[end - 1].contentEnd);
                if (passes(*conditions, rule.anyInclusion, QStringView(text.text).sliced(from, to - from),
                           QRegularExpression::NoMatchOption))
                    matches.push_back({rows[first].line, rows[first].start});
            }
        }
        if (!matches.empty())
            emitBatch(std::move(matches));
        const std::uint64_t done = rows[consumed - 1].end;
        const std::uint64_t size = std::max<std::size_t>(1, data.size());
        progress(static_cast<int>(std::min<std::uint64_t>(1000, done * 1000 / size)));
        row += consumed;
    }
}

} // namespace

QRegularExpression FilterCondition::regex() const
{
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (!caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(pattern, options);
}

QString FilterCondition::check() const
{
    QString problem;
    compileCondition(*this, &problem);
    return problem;
}

QString FilterRule::check() const
{
    QString error;
    compile(*this, &error);
    return error;
}

QJsonObject FilterRule::toJson() const
{
    QJsonArray list;
    for (const FilterCondition& condition : conditions) {
        const QString type = condition.exclude ? QStringLiteral("exclude") : QStringLiteral("include");
        QJsonObject object{{QStringLiteral("type"), type}, {QStringLiteral("regex"), condition.pattern}};
        if (condition.caseSensitive)
            object.insert(QStringLiteral("case_sensitive"), true);
        if (!condition.field.isEmpty()) {
            object.insert(QStringLiteral("field"), condition.field);
            object.insert(QStringLiteral("op"), condition.op);
            object.insert(QStringLiteral("value"), condition.value);
        }
        list.append(object);
    }
    return {{QStringLiteral("description"), description},
            {QStringLiteral("match"), anyInclusion ? QStringLiteral("any") : QStringLiteral("all")},
            {QStringLiteral("conditions"), list}};
}

std::optional<FilterRule> FilterRule::fromJson(const QJsonObject& object, QString* error)
{
    FilterRule rule;
    rule.description = object.value(QStringLiteral("description")).toString();
    rule.anyInclusion = object.value(QStringLiteral("match")).toString() == u"any";
    for (const QJsonValue& value : object.value(QStringLiteral("conditions")).toArray()) {
        const QJsonObject item = value.toObject();
        FilterCondition condition;
        condition.exclude = item.value(QStringLiteral("type")).toString() == u"exclude";
        condition.pattern = item.value(QStringLiteral("regex")).toString();
        condition.caseSensitive = item.value(QStringLiteral("case_sensitive")).toBool();
        condition.field = item.value(QStringLiteral("field")).toString();
        condition.op = item.value(QStringLiteral("op")).toString();
        const QJsonValue number = item.value(QStringLiteral("value"));
        condition.value = number.isString() ? number.toString().toDouble() : number.toDouble();
        rule.conditions.append(condition);
    }
    if (const QString problem = rule.check(); !problem.isEmpty()) {
        if (error)
            *error = problem;
        return std::nullopt;
    }
    return rule;
}

void filterLines(const FilterRule& rule, const SearchContext& context, std::uint64_t firstRow,
                 const FilterHooks& hooks)
{
    scan(
        rule, context, firstRow,
        [&hooks](std::vector<FilterMatch>&& batch) {
            if (hooks.found)
                hooks.found(std::move(batch));
        },
        [&hooks] { return hooks.cancelled && hooks.cancelled(); },
        [&hooks](int permille) {
            if (hooks.progress)
                hooks.progress(permille);
        });
}

std::vector<FilterMatch> filterLines(const FilterRule& rule, const SearchContext& context,
                                     std::uint64_t firstRow)
{
    std::vector<FilterMatch> all;
    FilterHooks hooks;
    hooks.found = [&all](std::vector<FilterMatch>&& batch) {
        all.insert(all.end(), batch.begin(), batch.end());
    };
    filterLines(rule, context, firstRow, hooks);
    return all;
}

LogLevelSpan findLogLevel(QStringView line)
{
    static const QRegularExpression words(
        QStringLiteral("\\b(?:(TRACE|VERBOSE)|(DEBUG)|(INFO|NOTICE)|(WARN(?:ING)?)|(ERROR|ERR|SEVERE)|"
                       "(FATAL|CRITICAL|CRIT|EMERG|ALERT|PANIC))\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = words.matchView(line.first(std::min<qsizetype>(line.size(), 200)));
    if (!match.hasMatch())
        return {};
    for (int group = 1; group <= 6; ++group) {
        if (match.hasCaptured(group))
            return {static_cast<LogLevel>(group), match.capturedStart(group), match.capturedLength(group)};
    }
    return {};
}

} // namespace aiedit::ui
