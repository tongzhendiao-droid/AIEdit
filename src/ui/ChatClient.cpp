#include "ui/ChatClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace aiedit::ui {

namespace {

constexpr qsizetype kMaxErrorBodyBytes = 64 * 1024;
constexpr qsizetype kShownErrorChars = 300;

QString roleName(ChatMessage::Role role)
{
    switch (role) {
    case ChatMessage::Role::System:
        return QStringLiteral("system");
    case ChatMessage::Role::User:
        return QStringLiteral("user");
    case ChatMessage::Role::Assistant:
        return QStringLiteral("assistant");
    case ChatMessage::Role::Tool:
        return QStringLiteral("tool");
    }
    return {};
}

int httpStatus(const QNetworkReply* reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool isHttpError(int status)
{
    return status != 0 && (status < 200 || status >= 300);
}

} // namespace

ChatClient::ChatClient(QObject* parent) : QObject(parent) {}

ChatClient::~ChatClient()
{
    cancel();
}

QByteArray ChatClient::requestBody(const AiSettings& settings, const QList<ChatMessage>& messages,
                                   const QJsonArray& tools)
{
    QJsonArray list;
    for (const ChatMessage& message : messages) {
        QJsonObject object{{QStringLiteral("role"), roleName(message.role)},
                           {QStringLiteral("content"), message.content}};
        if (!message.toolCalls.isEmpty()) {
            QJsonArray calls;
            for (const ToolCall& call : message.toolCalls)
                calls.append(QJsonObject{{QStringLiteral("id"), call.id},
                                         {QStringLiteral("type"), QStringLiteral("function")},
                                         {QStringLiteral("function"),
                                          QJsonObject{{QStringLiteral("name"), call.name},
                                                      {QStringLiteral("arguments"), call.arguments}}}});
            object.insert(QStringLiteral("tool_calls"), calls);
        }
        if (message.role == ChatMessage::Role::Tool)
            object.insert(QStringLiteral("tool_call_id"), message.toolCallId);
        if (!message.reasoning.isEmpty() && settings.provider == AiSettings::Provider::DeepSeek)
            object.insert(QStringLiteral("reasoning_content"), message.reasoning);
        list.append(object);
    }
    QJsonObject body{{QStringLiteral("model"), settings.model},
                     {QStringLiteral("messages"), list},
                     {QStringLiteral("stream"), true}};
    if (!tools.isEmpty())
        body.insert(QStringLiteral("tools"), tools);
    // DeepSeek thinks by default; other services do not know the field.
    if (settings.provider == AiSettings::Provider::DeepSeek)
        body.insert(QStringLiteral("thinking"),
                    QJsonObject{{QStringLiteral("type"),
                                 settings.thinking ? QStringLiteral("enabled") : QStringLiteral("disabled")}});
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

void ChatClient::send(const AiSettings& settings, const QList<ChatMessage>& messages, const QJsonArray& tools)
{
    cancel();
    m_parser = core::SseParser();
    m_finishReason.clear();
    m_toolCalls.clear();
    m_streamError.clear();
    m_errorBody.clear();

    QNetworkRequest request(settings.chatUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("Authorization", "Bearer " + settings.apiKey.toUtf8());
    // No transfer timeout: a queued request may wait minutes, kept open by keep-alive comments.
    m_reply = m_network.post(request, requestBody(settings, messages, tools));
    connect(m_reply, &QNetworkReply::readyRead, this, &ChatClient::readStream);
    connect(m_reply, &QNetworkReply::finished, this, &ChatClient::replyFinished);
}

void ChatClient::cancel()
{
    if (!m_reply)
        return;
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void ChatClient::readStream()
{
    if (!m_reply)
        return;
    const QByteArray chunk = m_reply->readAll();
    if (isHttpError(httpStatus(m_reply))) {
        // An error body is a message for the user, not a stream.
        m_errorBody += chunk.left(kMaxErrorBodyBytes - m_errorBody.size());
        return;
    }
    for (const core::SseEvent& event : m_parser.feed(std::string_view(chunk.constData(), static_cast<std::size_t>(chunk.size())))) {
        handleEvent(event);
        if (!m_reply) // a slot cancelled the request
            return;
    }
}

void ChatClient::handleEvent(const core::SseEvent& event)
{
    const QByteArray data = QByteArray::fromStdString(event.data).trimmed();
    if (data == "[DONE]")
        return;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return; // not ours to understand; the stream goes on
    const QJsonObject object = document.object();

    if (const QJsonValue error = object.value(QStringLiteral("error")); !error.isUndefined()) {
        m_streamError = error.toObject().value(QStringLiteral("message")).toString();
        if (m_streamError.isEmpty())
            m_streamError = QString::fromUtf8(data).left(kShownErrorChars);
        return;
    }

    const QJsonArray choices = object.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject choice = choices.first().toObject();
    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
    if (const QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString(); !reasoning.isEmpty())
        emit reasoningReceived(reasoning);
    if (const QString content = delta.value(QStringLiteral("content")).toString(); !content.isEmpty())
        emit contentReceived(content);
    // Tool calls arrive in fragments: the first carries the id and name, later ones add
    // pieces of the arguments; `index` says which call a fragment belongs to.
    for (const QJsonValue& fragment : delta.value(QStringLiteral("tool_calls")).toArray()) {
        const QJsonObject part = fragment.toObject();
        ToolCall& call = m_toolCalls[part.value(QStringLiteral("index")).toInt()];
        if (const QString id = part.value(QStringLiteral("id")).toString(); !id.isEmpty())
            call.id = id;
        const QJsonObject function = part.value(QStringLiteral("function")).toObject();
        if (const QString name = function.value(QStringLiteral("name")).toString(); !name.isEmpty())
            call.name = name;
        call.arguments += function.value(QStringLiteral("arguments")).toString();
    }
    if (const QJsonValue reason = choice.value(QStringLiteral("finish_reason")); reason.isString())
        m_finishReason = reason.toString();
}

void ChatClient::replyFinished()
{
    if (!m_reply)
        return;
    readStream(); // whatever arrived with the end of the reply
    QNetworkReply* reply = m_reply;
    if (!reply)
        return;
    m_reply = nullptr;
    reply->deleteLater();

    const int status = httpStatus(reply);
    if (isHttpError(status)) {
        emit failed(describeHttpError(status, m_errorBody));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emit failed(tr("Network error: %1").arg(reply->errorString()));
        return;
    }
    for (const core::SseEvent& event : m_parser.finish())
        handleEvent(event);
    if (!m_streamError.isEmpty())
        emit failed(m_streamError);
    else if (!m_toolCalls.isEmpty())
        emit toolCallsRequested(m_toolCalls.values());
    else
        emit finished(m_finishReason.isEmpty() ? QStringLiteral("stop") : m_finishReason);
}

QString ChatClient::describeHttpError(int status, const QByteArray& body) const
{
    // Providers word their error bodies differently; take a message wherever it is.
    QString detail;
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (document.isObject()) {
        const QJsonObject object = document.object();
        const QJsonValue error = object.value(QStringLiteral("error"));
        detail = error.isObject() ? error.toObject().value(QStringLiteral("message")).toString() : error.toString();
        if (detail.isEmpty())
            detail = object.value(QStringLiteral("message")).toString();
    }
    if (detail.isEmpty())
        detail = QString::fromUtf8(body).trimmed().left(kShownErrorChars);

    QString summary;
    switch (status) {
    case 401:
        summary = tr("The API key was rejected.");
        break;
    case 402:
        summary = tr("The account balance is insufficient.");
        break;
    case 429:
        summary = tr("Too many requests; try again in a moment.");
        break;
    case 400:
    case 404:
    case 422:
        summary = tr("The service did not accept the request; check the address and model name.");
        break;
    default:
        summary = status >= 500 ? tr("The service has a problem; try again in a moment.") : tr("The request failed.");
        break;
    }
    return detail.isEmpty() ? tr("%1 (HTTP %2)").arg(summary).arg(status)
                            : tr("%1 (HTTP %2: %3)").arg(summary).arg(status).arg(detail);
}

} // namespace aiedit::ui
