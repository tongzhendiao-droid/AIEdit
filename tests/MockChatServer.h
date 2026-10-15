#pragma once

// A local stand-in for an OpenAI-style chat service, for tests that must not reach the network.

#include "ui/AiSettings.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace aiedit::test {

// Writes a response body in pieces, with a pause between them, then closes the connection.
class ChunkWriter : public QObject
{
public:
    ChunkWriter(QTcpSocket* socket, QList<QByteArray> chunks, int delayMs)
        : QObject(socket), m_socket(socket), m_chunks(std::move(chunks)), m_delayMs(delayMs)
    {
        writeNext();
    }

private:
    void writeNext()
    {
        if (m_next >= m_chunks.size()) {
            m_socket->disconnectFromHost();
            return;
        }
        m_socket->write(m_chunks[m_next++]);
        m_socket->flush();
        QTimer::singleShot(m_delayMs, this, [this] { writeNext(); });
    }

    QTcpSocket* m_socket;
    QList<QByteArray> m_chunks;
    int m_delayMs;
    qsizetype m_next = 0;
};

// A minimal HTTP/1.1 server standing in for the chat API.
class MockServer : public QObject
{
public:
    int status = 200;
    QByteArray contentType = "text/event-stream";
    QList<QByteArray> chunks;
    QList<QList<QByteArray>> queue; // responses for successive requests, before falling back to `chunks`
    int chunkDelayMs = 5;
    QList<QByteArray> bodies;       // every request body received

    QByteArray requestLine;
    QMap<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;
    int requests = 0;

    MockServer()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = m_server.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        });
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

private:
    void read(QTcpSocket* socket)
    {
        QByteArray& buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        QMap<QByteArray, QByteArray> parsed;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0)
                parsed.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const qsizetype length = parsed.value("content-length").toLongLong();
        if (buffer.size() < headerEnd + 4 + length)
            return;

        requestLine = lines.first().trimmed();
        headers = parsed;
        body = buffer.mid(headerEnd + 4, length);
        bodies << body;
        ++requests;
        m_buffers.remove(socket);

        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Status\r\nContent-Type: " + contentType +
                      "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
        new ChunkWriter(socket, queue.isEmpty() ? chunks : queue.takeFirst(), chunkDelayMs);
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

inline QByteArray chunk(const QString& content, const QString& field = QStringLiteral("content"),
                 const QString& finishReason = {})
{
    QJsonObject choice{{QStringLiteral("index"), 0},
                       {QStringLiteral("delta"), QJsonObject{{field, content}}},
                       {QStringLiteral("finish_reason"),
                        finishReason.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(finishReason)}};
    const QJsonObject object{{QStringLiteral("object"), QStringLiteral("chat.completion.chunk")},
                             {QStringLiteral("choices"), QJsonArray{choice}}};
    return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n";
}

inline ui::AiSettings settingsFor(const MockServer& server,
                                  ui::AiSettings::Provider provider = ui::AiSettings::Provider::DeepSeek)
{
    ui::AiSettings settings;
    settings.provider = provider;
    settings.baseUrl = server.baseUrl() + QStringLiteral("/");
    settings.model = QStringLiteral("deepseek-flash");
    settings.apiKey = QStringLiteral("test-key");
    return settings;
}

} // namespace aiedit::test
