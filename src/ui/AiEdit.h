#pragma once

#include "ui/AiSettings.h"
#include "ui/ChatClient.h"
#include "ui/LogFilter.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>

namespace aiedit::ui {

// A change the AI writes for text too long to send, and that runs here on all of it: on the
// lines a filter picks (every line without one), delete them, or replace what regular
// expressions find in them.
struct EditRule
{
    struct Replacement
    {
        QString find; // a regular expression
        QString with; // \1 and so on for its groups
        bool caseSensitive = false;

        bool operator==(const Replacement&) const = default;
    };

    QString description; // what it does, in a few words
    std::optional<FilterRule> lines;
    bool deleteLines = false;
    QList<Replacement> replacements;

    // Empty if it can run; otherwise what is wrong with it.
    QString check() const;
    QJsonObject toJson() const;
    static std::optional<EditRule> fromJson(const QJsonObject& object, QString* error = nullptr);

    bool operator==(const EditRule&) const = default;
};

// What the AI is asked to change.
struct AiEditRequest
{
    QString instruction;
    QString fileName;
    QString language; // the syntax, or empty for plain text
    // A part short enough to send: the text, with some of what is around it to read.
    QString before;
    QString text;
    QString after;
    // Otherwise, lines spread over it to write a rule from.
    bool asRule = false;
    QStringList samples;
};

// Asks the AI for an edit: the text as changed, or for text too long to send a rule, much as
// FilterRuleWriter does for filters. A rule that does not parse or cannot run is sent back once
// with what is wrong with it.
class AiEditor : public QObject
{
    Q_OBJECT

public:
    using SettingsProvider = std::function<AiSettings()>;

    // Characters sent, and written back, in one answer; longer text is changed by a rule.
    static constexpr int kMaxText = 16'000;
    // Around a part: lines to read, and at most so many characters of them on each side.
    static constexpr int kContextLines = 30;
    static constexpr int kContextChars = 3'000;

    explicit AiEditor(QObject* parent = nullptr);
    ~AiEditor() override;

    // Defaults to AiSettings::load(); tests point it at a local server.
    void setSettingsProvider(SettingsProvider provider);
    AiSettings settings() const { return m_settingsProvider(); }

    QList<ChatMessage> messages(const AiEditRequest& request) const;
    // The JSON that edit() would post, for the preview.
    QByteArray previewRequest(const AiEditRequest& request) const;
    // The text in an answer: its code block. What is said around the block is the note.
    static std::optional<QString> parseText(const QString& answer, QString* note = nullptr);
    // The rule in an answer: a JSON object, in a code block or not.
    static std::optional<EditRule> parseRule(const QString& answer, QString* note = nullptr,
                                             QString* error = nullptr);

    bool isBusy() const { return m_client.isBusy(); }
    // Without a configured service it asks for the settings instead.
    void edit(const AiEditRequest& request);
    void cancel();

signals:
    void progressed(int characters); // of the answer so far
    void textWritten(const QString& text, const QString& note);
    void ruleWritten(const aiedit::ui::EditRule& rule, const QString& note);
    void failed(const QString& message);
    void settingsRequested();

private:
    void finished(const QString& finishReason);

    ChatClient m_client;
    SettingsProvider m_settingsProvider;
    AiEditRequest m_request;
    QList<ChatMessage> m_messages; // the conversation so far
    QString m_answer;
    bool m_retried = false;
};

// Backticks to fence a text with in Markdown: more than any run of them in it, and at least three.
QString codeFence(const QString& text);

// The rule run over the lines that start in [start, end) of a text (whole lines): those bytes as
// changed, or nullopt if cancelled or a line cannot be written in the codec (then `error` says
// which). Lines that do not change keep their bytes; changed ones keep their line breaks.
std::optional<QByteArray> applyEditRule(const EditRule& rule, const SearchContext& context, std::uint64_t start,
                                        std::uint64_t end, QString* error = nullptr,
                                        const std::function<bool()>& cancelled = {});

} // namespace aiedit::ui
