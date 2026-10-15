#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>

namespace aiedit::ui {

// Lets a second launch (a double-click in the file manager, "Open with") hand its files to the
// window that is already open, instead of starting another one.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    // One name per user, so users on the same machine never see each other's windows.
    static QString defaultName();

    explicit SingleInstance(QString name = defaultName(), QObject* parent = nullptr);

    // Sends the files (absolute paths) to an instance that is listening; true if one took them.
    bool forward(const QStringList& files) const;
    // Starts listening. A socket left behind by a crashed instance is taken over, so call this
    // only after forward() found nobody listening.
    bool listen();

signals:
    void filesReceived(const QStringList& files);

private:
    QString m_name;
    QLocalServer m_server;
};

} // namespace aiedit::ui
