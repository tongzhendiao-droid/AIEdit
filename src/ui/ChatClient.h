#pragma once

#include "core/Sse.h"
#include "ui/AiSettings.h"

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;

namespace aiedit::ui {

struct ChatMessage
{
    enum class Role
    {
        System,
        User,
        Assistant,
    };

    Role role;
    QString content;
};

// Streams one chat completion from an OpenAI-style /chat/completions endpoint (DeepSeek and
// most other providers speak it). Answers arrive piece by piece through contentReceived().
class ChatClient : public QObject
{
    Q_OBJECT

public:
    explicit ChatClient(QObject* parent = nullptr);
    ~ChatClient() override;

    // Starts a request, cancelling one still running.
    void send(const AiSettings& settings, const QList<ChatMessage>& messages);
    // Stops the request in flight; no further signals arrive for it.
    void cancel();
    bool isBusy() const { return m_reply != nullptr; }

    // The JSON that send() posts: what the send preview shows (the key is in a header, not here).
    static QByteArray requestBody(const AiSettings& settings, const QList<ChatMessage>& messages);

signals:
    void reasoningReceived(const QString& text); // pieces of the model's thinking, if it thinks
    void contentReceived(const QString& text);   // pieces of the answer
    void finished(const QString& finishReason);  // "stop", or e.g. "length" when cut off
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
    QString m_streamError;
    QByteArray m_errorBody;
};

} // namespace aiedit::ui
