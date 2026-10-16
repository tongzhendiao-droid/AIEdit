#include "ui/AiEdit.h"

#include "core/FileSource.h"
#include "core/LineIndex.h"
#include "ui/FilterRuleWriter.h"
#include "ui/TextDecoder.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringEncoder>

#include <algorithm>
#include <utility>

namespace aiedit::ui {

namespace {

// In English, which models follow best; the user's own words may be in any language.
const char* const kTextPrompt =
    R"(You change text for the user of AIEdit, a text and hex editor. You get an instruction, the name and language of the file, and the text to change, with some of the lines before and after it to read.

Answer with the whole text as changed in a single fenced code block, and nothing else in the block: no line numbers, nothing of the lines before or after it, and every line that stays as it was. Change only what the instruction asks for; keep everything else exactly as it is, indentation, blank lines and spelling included. Make the fence longer than any run of backticks in the text.

Outside the block, say in one or two sentences what you changed, in the language of the instruction. If the instruction cannot be carried out on this text, say why and write no block.)";

const char* const kRulePrompt =
    R"(You write rules that change files too long to send, for AIEdit, a text and hex editor. You get an instruction and sample lines spread over the file. Answer with one JSON object in a code block:

{"description": "<what the rule does, in a few words, in the language of the instruction>",
 "lines": <optional: which lines to change, see below>,
 "delete": true or false,
 "replace": [{"find": "<PCRE pattern>", "with": "<replacement, \\1 for a group>", "case_sensitive": false}]}

How it runs, line by line, on the whole file (or the lines the user selected):
- "lines" picks the lines: {"match": "all" or "any", "conditions": [{"type": "include" or "exclude", "regex": "<PCRE pattern>", "case_sensitive": false}]}. A line is picked if it matches the include conditions (all of them, or any one with "match": "any") and none of the exclude ones. Without "lines", every line is picked.
- With "delete": true the picked lines are deleted, line breaks and all. Otherwise each replacement in turn replaces every match in a picked line; patterns never see the line break.
- Patterns ignore case unless "case_sensitive" is true. Double the backslashes inside JSON strings.

Write the patterns for the format the samples show. Outside the block, say in one sentence what the rule does, in the language of the instruction. If no such rule can do what is asked (it needs to understand the whole text), say why and write no block.)";

// The text of the first fenced code block in an answer, and the answer without it. An answer
// cut off inside its block has none.
std::optional<QString> fencedBlock(const QString& answer, QString* outside)
{
    static const QRegularExpression opening(QStringLiteral("^[ ]{0,3}(`{3,})[^`\\n]*$"),
                                            QRegularExpression::MultilineOption);
    const QRegularExpressionMatch open = opening.match(answer);
    if (!open.hasMatch())
        return std::nullopt;
    const qsizetype fence = open.capturedLength(1);
    const qsizetype contentStart = open.capturedEnd() + 1; // after the line break
    if (contentStart > answer.size())
        return std::nullopt;
    // The closing fence: a line of as many backticks or more.
    const QRegularExpression closing(QStringLiteral("^[ ]{0,3}`{%1,}[ \\t]*$").arg(fence),
                                     QRegularExpression::MultilineOption);
    const QRegularExpressionMatch close = closing.match(answer, contentStart);
    if (!close.hasMatch())
        return std::nullopt;
    QString text = answer.sliced(contentStart, close.capturedStart() - contentStart);
    if (text.endsWith(u'\n'))
        text.chop(1);
    if (text.endsWith(u'\r'))
        text.chop(1);
    if (outside)
        *outside = (answer.left(open.capturedStart()) + answer.sliced(close.capturedEnd())).trimmed();
    return text;
}

} // namespace

// EditRule ------------------------------------------------------------------------------------------

QString EditRule::check() const
{
    if (lines) {
        if (const QString problem = lines->check(); !problem.isEmpty())
            return problem;
    }
    if (deleteLines && !replacements.isEmpty())
        return QCoreApplication::translate("AiEdit", "the rule both deletes and replaces");
    if (!deleteLines && replacements.isEmpty())
        return QCoreApplication::translate("AiEdit", "the rule changes nothing: no replacement, and no deletion");
    for (qsizetype i = 0; i < replacements.size(); ++i) {
        if (replacements[i].find.isEmpty())
            return QCoreApplication::translate("AiEdit", "replacement %1 has nothing to find").arg(i + 1);
        const QRegularExpression regex(replacements[i].find);
        if (!regex.isValid())
            return QCoreApplication::translate("AiEdit", "replacement %1: invalid regular expression: %2")
                .arg(QString::number(i + 1), regex.errorString());
    }
    return {};
}

QJsonObject EditRule::toJson() const
{
    QJsonObject object{{QStringLiteral("description"), description}};
    if (lines) {
        QJsonObject filter = lines->toJson();
        filter.remove(QStringLiteral("description"));
        object.insert(QStringLiteral("lines"), filter);
    }
    if (deleteLines)
        object.insert(QStringLiteral("delete"), true);
    QJsonArray list;
    for (const Replacement& replacement : replacements) {
        QJsonObject item{{QStringLiteral("find"), replacement.find}, {QStringLiteral("with"), replacement.with}};
        if (replacement.caseSensitive)
            item.insert(QStringLiteral("case_sensitive"), true);
        list.append(item);
    }
    if (!list.isEmpty())
        object.insert(QStringLiteral("replace"), list);
    return object;
}

std::optional<EditRule> EditRule::fromJson(const QJsonObject& object, QString* error)
{
    EditRule rule;
    rule.description = object.value(QStringLiteral("description")).toString();
    if (const QJsonValue lines = object.value(QStringLiteral("lines")); lines.isObject()) {
        QString problem;
        rule.lines = FilterRule::fromJson(lines.toObject(), &problem);
        if (!rule.lines) {
            if (error)
                *error = QCoreApplication::translate("AiEdit", "which lines: %1").arg(problem);
            return std::nullopt;
        }
    }
    rule.deleteLines = object.value(QStringLiteral("delete")).toBool();
    for (const QJsonValue& value : object.value(QStringLiteral("replace")).toArray()) {
        const QJsonObject item = value.toObject();
        rule.replacements.append({item.value(QStringLiteral("find")).toString(),
                                  item.value(QStringLiteral("with")).toString(),
                                  item.value(QStringLiteral("case_sensitive")).toBool()});
    }
    if (const QString problem = rule.check(); !problem.isEmpty()) {
        if (error)
            *error = problem;
        return std::nullopt;
    }
    return rule;
}

// AiEditor ------------------------------------------------------------------------------------------

AiEditor::AiEditor(QObject* parent) : QObject(parent), m_settingsProvider(&AiSettings::load)
{
    connect(&m_client, &ChatClient::contentReceived, this, [this](const QString& text) {
        m_answer += text;
        emit progressed(static_cast<int>(m_answer.size()));
    });
    connect(&m_client, &ChatClient::finished, this, &AiEditor::finished);
    connect(&m_client, &ChatClient::failed, this, &AiEditor::failed);
    // No tools are offered, so this does not happen; if it does, there is no edit.
    connect(&m_client, &ChatClient::toolCallsRequested, this,
            [this] { emit failed(tr("The AI answered without a change.")); });
}

AiEditor::~AiEditor() = default;

void AiEditor::setSettingsProvider(SettingsProvider provider)
{
    m_settingsProvider = provider ? std::move(provider) : SettingsProvider(&AiSettings::load);
}

QString codeFence(const QString& text)
{
    qsizetype longest = 0;
    qsizetype run = 0;
    for (const QChar c : text) {
        run = c == u'`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return QString(std::max<qsizetype>(3, longest + 1), u'`');
}

QList<ChatMessage> AiEditor::messages(const AiEditRequest& request) const
{
    // Put together rather than with arg(): the user's words could hold a %1.
    const auto block = [](const QString& text) {
        const QString fence = codeFence(text);
        return fence + QLatin1Char('\n') + text + QLatin1Char('\n') + fence;
    };
    QString file = request.fileName.isEmpty() ? QStringLiteral("a new document") : request.fileName;
    if (!request.language.isEmpty())
        file += QStringLiteral(" (") + request.language + QLatin1Char(')');
    QString message = QStringLiteral("Instruction: ") + request.instruction.trimmed() +
                      QStringLiteral("\nFile: ") + file + QStringLiteral("\n\n");
    if (request.asRule) {
        message += QStringLiteral("Sample lines (the start, then lines from further on; long lines are cut to ") +
                   QString::number(FilterRuleWriter::kSampleLineLength) + QStringLiteral(" characters):\n") +
                   block(request.samples.join(u'\n'));
    } else {
        if (!request.before.isEmpty())
            message += QStringLiteral("Before the text, to read only (do not change or repeat it):\n") +
                       block(request.before) + QStringLiteral("\n\n");
        message += QStringLiteral("The text to change:\n") + block(request.text);
        if (!request.after.isEmpty())
            message += QStringLiteral("\n\nAfter the text, to read only (do not change or repeat it):\n") +
                       block(request.after);
    }
    return {{ChatMessage::Role::System, QString::fromUtf8(request.asRule ? kRulePrompt : kTextPrompt)},
            {ChatMessage::Role::User, message}};
}

QByteArray AiEditor::previewRequest(const AiEditRequest& request) const
{
    return ChatClient::requestBody(m_settingsProvider(), messages(request));
}

std::optional<QString> AiEditor::parseText(const QString& answer, QString* note)
{
    QString outside;
    std::optional<QString> text = fencedBlock(answer, &outside);
    if (note)
        *note = text ? outside : answer.trimmed();
    return text;
}

std::optional<EditRule> AiEditor::parseRule(const QString& answer, QString* note, QString* error)
{
    QString outside;
    QString json;
    if (const std::optional<QString> block = fencedBlock(answer, &outside)) {
        json = *block;
    } else {
        // A bare object, perhaps after a few words.
        const qsizetype open = answer.indexOf(u'{');
        const qsizetype close = answer.lastIndexOf(u'}');
        if (open < 0 || close < open) {
            if (note)
                *note = answer.trimmed();
            if (error)
                *error = tr("the answer holds no rule");
            return std::nullopt;
        }
        json = answer.sliced(open, close - open + 1);
        outside = (answer.left(open) + answer.sliced(close + 1)).trimmed();
    }
    if (note)
        *note = outside;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (!document.isObject()) {
        if (error)
            *error = tr("the rule is not valid JSON (%1)").arg(parseError.errorString());
        return std::nullopt;
    }
    return EditRule::fromJson(document.object(), error);
}

void AiEditor::edit(const AiEditRequest& request)
{
    cancel();
    const AiSettings settings = m_settingsProvider();
    if (!settings.isConfigured()) {
        emit failed(tr("Set up the AI service first."));
        emit settingsRequested();
        return;
    }
    m_request = request;
    m_messages = messages(request);
    m_retried = false;
    m_client.send(settings, m_messages);
}

void AiEditor::cancel()
{
    m_client.cancel();
    m_answer.clear();
}

void AiEditor::finished(const QString& finishReason)
{
    const QString answer = std::exchange(m_answer, {});
    const bool cutOff = finishReason == u"length";
    QString note;
    if (!m_request.asRule) {
        if (const std::optional<QString> text = parseText(answer, &note)) {
            emit textWritten(*text, note);
            return;
        }
        if (cutOff)
            emit failed(tr("The answer was cut off before the end of the text. Select less of it."));
        else if (note.isEmpty())
            emit failed(tr("The AI gave no changed text."));
        else // what it said instead, such as why it cannot
            emit failed(tr("The AI: %1").arg(note.size() > 400 ? note.left(400) + QStringLiteral("…") : note));
        return;
    }
    QString error;
    if (const std::optional<EditRule> rule = parseRule(answer, &note, &error)) {
        emit ruleWritten(*rule, note);
        return;
    }
    // No rule at all and words instead: it says why it cannot; asking again would not help.
    const bool explained = !answer.contains(u'{') && !note.isEmpty();
    if (cutOff)
        error = tr("the answer was cut off");
    if (m_retried || explained) {
        emit failed(explained ? tr("The AI: %1").arg(note.size() > 400 ? note.left(400) + QStringLiteral("…") : note)
                              : tr("The AI's rule cannot be used: %1").arg(error));
        return;
    }
    // Once more, with what is wrong.
    m_retried = true;
    m_messages.append({ChatMessage::Role::Assistant, answer});
    m_messages.append({ChatMessage::Role::User,
                       QStringLiteral("That rule cannot be used: ") + error +
                           QStringLiteral(". Answer again with the whole rule, corrected.")});
    m_client.send(m_settingsProvider(), m_messages);
}

// Running a rule, and keeping some changes --------------------------------------------------------------

std::optional<QByteArray> applyEditRule(const EditRule& rule, const SearchContext& context, std::uint64_t start,
                                        std::uint64_t end, QString* error, const std::function<bool()>& cancelled)
{
    const core::LineIndex& index = *context.index;
    const auto data = context.source->bytes();
    end = std::min<std::uint64_t>(end, data.size());
    if (index.rowCount() == 0 || start >= end)
        return QByteArray();
    const std::uint64_t firstRow = index.rowIndexAt(start);

    // The lines to change, when a filter picks them: those starting in the range.
    std::vector<std::uint64_t> picked;
    if (rule.lines) {
        bool past = false;
        FilterHooks hooks;
        hooks.cancelled = [&] { return past || (cancelled && cancelled()); };
        hooks.found = [&](std::vector<FilterMatch>&& batch) {
            for (const FilterMatch& match : batch) {
                if (match.start >= end) {
                    past = true;
                    break;
                }
                if (match.start >= start)
                    picked.push_back(match.line);
            }
        };
        filterLines(*rule.lines, context, firstRow, hooks);
        if (cancelled && cancelled())
            return std::nullopt;
    }
    std::vector<QRegularExpression> patterns;
    for (const EditRule::Replacement& replacement : rule.replacements)
        patterns.emplace_back(replacement.find, replacement.caseSensitive ? QRegularExpression::NoPatternOption
                                                                          : QRegularExpression::CaseInsensitiveOption);

    const TextDecoder decoder(context.codec);
    QStringEncoder encoder(context.codec.name.data());
    const auto bytesOf = [&data](std::uint64_t from, std::uint64_t to) {
        return QByteArrayView(reinterpret_cast<const char*>(data.data()) + from, static_cast<qsizetype>(to - from));
    };
    QByteArray out;
    // A line, put together from its rows (a long one is split into several), and what becomes of it.
    struct Line
    {
        std::uint64_t number = 0;
        std::uint64_t start = 0;
        std::uint64_t contentEnd = 0;
        std::uint64_t end = 0;
    };
    const auto change = [&](const Line& line) {
        const bool chosen = !rule.lines || std::binary_search(picked.begin(), picked.end(), line.number);
        if (!chosen) {
            out += bytesOf(line.start, line.end);
            return true;
        }
        if (rule.deleteLines)
            return true;
        const QString text = decoder.decodePlain(data.subspan(line.start, line.contentEnd - line.start));
        QString changed = text;
        for (std::size_t i = 0; i < patterns.size(); ++i)
            changed.replace(patterns[i], rule.replacements[static_cast<qsizetype>(i)].with);
        if (changed == text) {
            out += bytesOf(line.start, line.end);
            return true;
        }
        const QByteArray encoded = encoder.encode(changed);
        if (encoder.hasError()) {
            if (error)
                *error = QCoreApplication::translate("AiEdit", "line %1 as changed cannot be written in %2")
                             .arg(QString::number(line.number + 1), QString::fromLatin1(context.codec.name.data()));
            return false;
        }
        out += encoded;
        out += bytesOf(line.contentEnd, line.end);
        return true;
    };
    constexpr std::size_t kRowsAtOnce = 4096;
    std::optional<Line> line;
    bool past = false; // a line starting at the end or after it
    for (std::uint64_t row = firstRow; row < index.rowCount() && !past;) {
        if (cancelled && cancelled())
            return std::nullopt;
        const std::vector<core::Row> rows = index.rows(row, kRowsAtOnce);
        if (rows.empty())
            break;
        for (const core::Row& r : rows) {
            if (r.continuation && line) {
                line->contentEnd = r.contentEnd;
                line->end = r.end;
                continue;
            }
            if (line && !change(*line))
                return std::nullopt;
            line.reset();
            if (r.start >= end) {
                past = true;
                break;
            }
            line = Line{r.line, r.start, r.contentEnd, r.end};
        }
        row += rows.size();
    }
    if (line && !change(*line))
        return std::nullopt;
    return out;
}

} // namespace aiedit::ui
