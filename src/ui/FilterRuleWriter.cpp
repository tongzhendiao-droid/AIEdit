#include "ui/FilterRuleWriter.h"

#include "core/FileSource.h"
#include "ui/TextDecoder.h"

#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <utility>

namespace aiedit::ui {

namespace {

// What the model must know to write a rule; in English, which models follow best. The user's
// own words may be in any language.
const char* const kSystemPrompt = R"(You write line filters for AIEdit, a log viewer. The user says which lines of a log they want to see, and you answer with a single JSON object and nothing else:

{"description": "<what the filter shows, in a few words, in the user's language>",
 "match": "all" or "any",
 "conditions": [
   {"type": "include" or "exclude", "regex": "<PCRE pattern>", "case_sensitive": false,
    "field": "<named group>", "op": ">", "value": 1000}
 ]}

How it runs:
- Each regex is tested against one whole line, without its line break, and may match anywhere in it; anchor it with ^ or $ if needed.
- A line is shown if it matches the include conditions (all of them with "match": "all", at least one with "any") and none of the exclude conditions. With no include conditions, every line that is not excluded is shown.
- "field", "op" and "value" are optional. To compare a number, capture it in a named group, e.g. "took (?<ms>\\d+(?:\\.\\d+)?)\\s*ms" with field "ms", op ">" and value 1000; op is one of > >= < <= == !=. The condition then holds when the regex matches and the captured number compares true. Convert units to the log's own (1 s = 1000 ms).
- Patterns ignore case unless "case_sensitive" is true.

Write the patterns for the format the sample lines show: their level words, timestamps, field names and separators, not a guess. Double the backslashes inside JSON strings. Keep the filter as simple as the request allows.)";

} // namespace

FilterRuleWriter::FilterRuleWriter(QObject* parent) : QObject(parent), m_settingsProvider(&AiSettings::load)
{
    connect(&m_client, &ChatClient::contentReceived, this, [this](const QString& text) { m_answer += text; });
    connect(&m_client, &ChatClient::finished, this, &FilterRuleWriter::finished);
    connect(&m_client, &ChatClient::failed, this, &FilterRuleWriter::failed);
    // Tools are never offered, so this does not happen; if it does, there is no rule.
    connect(&m_client, &ChatClient::toolCallsRequested, this,
            [this] { emit failed(tr("The AI answered without a rule.")); });
}

FilterRuleWriter::~FilterRuleWriter() = default;

void FilterRuleWriter::setSettingsProvider(SettingsProvider provider)
{
    m_settingsProvider = provider ? std::move(provider) : SettingsProvider(&AiSettings::load);
}

QStringList FilterRuleWriter::sampleLines(const SearchContext& context, int count)
{
    const core::LineIndex& index = *context.index;
    const std::uint64_t rowCount = index.rowCount();
    if (rowCount == 0 || count <= 0)
        return {};

    // Half from the start, where the format shows best; the rest spread over the file, for
    // what only happens later (errors, other components).
    std::vector<std::uint64_t> starts; // first rows of the chosen lines, in order
    const std::size_t head = static_cast<std::size_t>(count) / 2;
    for (std::uint64_t first = 0; first < rowCount && starts.size() < head; first += 256) {
        const std::vector<core::Row> rows = index.rows(first, 256);
        for (std::size_t k = 0; k < rows.size() && starts.size() < head; ++k) {
            if (!rows[k].continuation)
                starts.push_back(first + k);
        }
    }
    const std::uint64_t afterHead = starts.empty() ? 0 : starts.back() + 1;
    const std::uint64_t spread = static_cast<std::uint64_t>(count) - starts.size();
    if (afterHead < rowCount) {
        for (std::uint64_t i = 0; i < spread; ++i) {
            const std::uint64_t at = afterHead + (rowCount - afterHead) * (2 * i + 1) / (2 * spread);
            const core::Row r = index.row(std::min(at, rowCount - 1));
            const std::uint64_t first = r.continuation ? index.rowIndexOfLine(r.line).value_or(at) : at;
            if (starts.empty() || first > starts.back())
                starts.push_back(first);
        }
    }

    const auto data = context.source->bytes();
    const TextDecoder decoder(context.codec);
    QStringList lines;
    for (const std::uint64_t first : starts) {
        const core::Row r = index.row(first);
        const std::size_t length = static_cast<std::size_t>(r.contentEnd - r.start);
        QString text = decoder.decodePlain(data.subspan(r.start, length));
        if (text.trimmed().isEmpty())
            continue;
        if (text.size() > kSampleLineLength)
            text.truncate(kSampleLineLength);
        lines << text;
    }
    return lines;
}

QList<ChatMessage> FilterRuleWriter::messages(const QString& description, const QStringList& samples)
{
    const QString message =
        QStringLiteral("Request: %1\n\nSample lines (the start of the log, then lines from further on; long lines are "
                       "cut to %2 characters):\n```text\n%3\n```")
            .arg(description.trimmed())
            .arg(kSampleLineLength)
            .arg(samples.join(u'\n'));
    return {{ChatMessage::Role::System, QString::fromUtf8(kSystemPrompt)},
            {ChatMessage::Role::User, message}};
}

QByteArray FilterRuleWriter::previewRequest(const QString& description, const QStringList& samples) const
{
    return ChatClient::requestBody(m_settingsProvider(), messages(description, samples));
}

std::optional<FilterRule> FilterRuleWriter::parseAnswer(const QString& answer, QString* error)
{
    const qsizetype open = answer.indexOf(u'{');
    const qsizetype close = answer.lastIndexOf(u'}');
    if (open < 0 || close < open) {
        if (error)
            *error = tr("the answer holds no rule");
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument json =
        QJsonDocument::fromJson(answer.sliced(open, close - open + 1).toUtf8(), &parseError);
    if (!json.isObject()) {
        if (error)
            *error = tr("the rule is not valid JSON (%1)").arg(parseError.errorString());
        return std::nullopt;
    }
    return FilterRule::fromJson(json.object(), error);
}

void FilterRuleWriter::write(const QString& description, const QStringList& samples)
{
    cancel();
    const AiSettings settings = m_settingsProvider();
    if (!settings.isConfigured()) {
        emit failed(tr("Set up the AI service first."));
        emit settingsRequested();
        return;
    }
    m_messages = messages(description, samples);
    m_answer.clear();
    m_retried = false;
    m_client.send(settings, m_messages);
}

void FilterRuleWriter::cancel()
{
    m_client.cancel();
    m_answer.clear();
}

void FilterRuleWriter::finished(const QString& finishReason)
{
    const QString answer = std::exchange(m_answer, {});
    QString error;
    if (const std::optional<FilterRule> rule = parseAnswer(answer, &error)) {
        emit ruleWritten(*rule);
        return;
    }
    if (finishReason == u"length")
        error = tr("the answer was cut off");
    if (m_retried) {
        emit failed(tr("The AI did not write a usable rule: %1").arg(error));
        return;
    }
    // Once more, with what is wrong: models usually fix a pattern they are told about.
    m_retried = true;
    m_messages.append({ChatMessage::Role::Assistant, answer});
    m_messages.append({ChatMessage::Role::User,
                       QStringLiteral("That filter cannot be used: %1. Answer with the corrected JSON object only.")
                           .arg(error)});
    m_client.send(m_settingsProvider(), m_messages);
}

} // namespace aiedit::ui
