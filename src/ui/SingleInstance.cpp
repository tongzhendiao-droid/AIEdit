#include "ui/SingleInstance.h"

#include "ui/Desktop.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>

#include <memory>

namespace aiedit::ui {

namespace {

constexpr int kConnectTimeoutMs = 500;
constexpr int kWriteTimeoutMs = 2000;

} // namespace

QString SingleInstance::defaultName()
{
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    return QStringLiteral("aiedit-") + user;
}

SingleInstance::SingleInstance(QString name, QObject* parent) : QObject(parent), m_name(std::move(name))
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = m_server.nextPendingConnection()) {
            // The sender writes one JSON array and disconnects. What arrives is kept as it
            // comes, not read at the end: a Windows pipe may have dropped it by then.
            const auto received = std::make_shared<QByteArray>();
            connect(socket, &QLocalSocket::readyRead, this, [socket, received] { *received += socket->readAll(); });
            const auto finish = [this, socket, received] {
                *received += socket->readAll();
                socket->disconnect(this);
                socket->deleteLater();
                const QJsonDocument document = QJsonDocument::fromJson(*received);
                if (!document.isArray())
                    return;
                QStringList files;
                for (const QJsonValue& value : document.array())
                    files << value.toString();
                emit filesReceived(files);
            };
            connect(socket, &QLocalSocket::disconnected, this, finish);
            if (socket->state() == QLocalSocket::UnconnectedState) // gone before it was picked up
                finish();
        }
    });
}

bool SingleInstance::forward(const QStringList& files) const
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(kConnectTimeoutMs))
        return false;
    allowActivationByOthers(); // so that the window that takes the files can come to the front
    socket.write(QJsonDocument(QJsonArray::fromStringList(files)).toJson(QJsonDocument::Compact));
    if (!socket.waitForBytesWritten(kWriteTimeoutMs))
        return false;
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(kWriteTimeoutMs);
    return true;
}

bool SingleInstance::listen()
{
    if (m_server.listen(m_name))
        return true;
    // A previous instance crashed and left its socket file; nobody answered forward(), so it is ours.
    QLocalServer::removeServer(m_name);
    return m_server.listen(m_name);
}

} // namespace aiedit::ui
