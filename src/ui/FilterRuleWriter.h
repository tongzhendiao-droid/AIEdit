#pragma once

#include "ui/AiSettings.h"
#include "ui/ChatClient.h"
#include "ui/LogFilter.h"

#include <QObject>
#include <QStringList>

#include <functional>
#include <optional>

namespace aiedit::ui {

// Asks the AI for a filter rule from a sentence, such as "errors slower than a second, without
// the heartbeats". It sees the sentence and a sample of lines to learn the log's format, never
// the whole file; the rule it writes then runs here, on every line.
class FilterRuleWriter : public QObject
{
    Q_OBJECT

public:
    using SettingsProvider = std::function<AiSettings()>;

    static constexpr int kSampleLines = 80;
    static constexpr int kSampleLineLength = 200; // characters

    explicit FilterRuleWriter(QObject* parent = nullptr);
    ~FilterRuleWriter() override;

    // Defaults to AiSettings::load(); tests point it at a local server.
    void setSettingsProvider(SettingsProvider provider);
    AiSettings settings() const { return m_settingsProvider(); }

    // Lines to show the AI: the start of the file, then lines spread over the rest of what is
    // indexed, each cut to kSampleLineLength characters. Empty lines are left out.
    static QStringList sampleLines(const SearchContext& context, int count = kSampleLines);
    // The conversation write() starts.
    static QList<ChatMessage> messages(const QString& description, const QStringList& samples);
    // The JSON that write() would post now, for the preview.
    QByteArray previewRequest(const QString& description, const QStringList& samples) const;
    // The rule in an answer: a JSON object, perhaps in a code fence or after a few words.
    static std::optional<FilterRule> parseAnswer(const QString& answer, QString* error = nullptr);

    bool isBusy() const { return m_client.isBusy(); }
    // Asks for a rule. Without a configured service it asks for the settings instead. A rule
    // that does not parse or compile is sent back once with what is wrong with it.
    void write(const QString& description, const QStringList& samples);
    void cancel();

signals:
    void ruleWritten(const aiedit::ui::FilterRule& rule);
    void failed(const QString& message);
    void settingsRequested();

private:
    void finished(const QString& finishReason);

    ChatClient m_client;
    SettingsProvider m_settingsProvider;
    QList<ChatMessage> m_messages; // the conversation so far
    QString m_answer;
    bool m_retried = false;
};

} // namespace aiedit::ui
