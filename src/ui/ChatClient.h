#pragma once

#include "core/Sse.h"
#include "ui/AiSettings.h"

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;

namespace aiedit::ui {

// A function the model asked to call; `arguments` is JSON text, as the model wrote it.
struct ToolCall
{
    QString id;
    QString name;
    QString arguments;
};

struct ChatMessage
{
    enum class Role
    {
        System,
        User,
        Assistant,
        Tool,
    };

    ChatMessage() = default;
    ChatMessage(Role messageRole, QString messageContent) : role(messageRole), content(std::move(messageContent)) {}

    Role role = Role::User;
    QString content;
    QList<ToolCall> toolCalls; // assistant: the tools it called
    QString toolCallId;        // tool: the call this result answers
    QString reasoning;         // assistant: DeepSeek needs its thinking back when tools are involved
};

// Streams one chat completion from an OpenAI-style /chat/completions endpoint (DeepSeek and
// most other providers speak it). Answers arrive piece by piece through contentReceived().
class ChatClient : public QObject
{
    Q_OBJECT

public:
    explicit ChatClient(QObject* parent = nullptr);
    ~ChatClient() override;

    // Starts a request, cancelling one still running. With `tools` (OpenAI function
    // definitions) the model may answer with toolCallsRequested() instead of finished().
    void send(const AiSettings& settings, const QList<ChatMessage>& messages, const QJsonArray& tools = {});
    // Stops the request in flight; no further signals arrive for it.
    void cancel();
    bool isBusy() const { return m_reply != nullptr; }

    // The JSON that send() posts: what the send preview shows (the key is in a header, not here).
    static QByteArray requestBody(const AiSettings& settings, const QList<ChatMessage>& messages,
                                  const QJsonArray& tools = {});

signals:
    void reasoningReceived(const QString& text); // pieces of the model's thinking, if it thinks
    void contentReceived(const QString& text);   // pieces of the answer
    void finished(const QString& finishReason);  // "stop", or e.g. "length" when cut off
    // The response ended by calling tools; run them and send the results in a new request.
    void toolCallsRequested(const QList<aiedit::ui::ToolCall>& calls);
    void failed(const QString& message);

private:
    void readStream();
    void replyFinished();
    void handleEvent(const core::SseEvent& event);
    QString describeHttpError(int status, const QByteArray& body) const;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    core::SseParser m_parser;
    QString m_finishReason;
    QMap<int, ToolCall> m_toolCalls; // assembled from streamed fragments, by index
    QString m_streamError;
    QByteArray m_errorBody;
};

} // namespace aiedit::ui
