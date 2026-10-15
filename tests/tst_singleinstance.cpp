#include "ui/SingleInstance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTest>

using aiedit::ui::SingleInstance;

class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void forwardsFilesToTheRunningInstance();
    void forwardFailsWithoutInstance();
    void takesOverAStaleSocket();

private:
    // A name of our own, so the test never talks to a real AIEdit window.
    static QString uniqueName(const char* suffix)
    {
        return QStringLiteral("aiedit-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(QLatin1String(suffix));
    }
};

void TestSingleInstance::forwardsFilesToTheRunningInstance()
{
    SingleInstance running(uniqueName("forward"));
    QVERIFY(running.listen());
    QSignalSpy received(&running, &SingleInstance::filesReceived);

    const SingleInstance second(uniqueName("forward"));
    const QStringList files{QStringLiteral("/tmp/a.log"), QStringLiteral("/home/user/文档/说明.txt")};
    QVERIFY(second.forward(files));
    QVERIFY(received.wait());
    QCOMPARE(received.first().first().toStringList(), files);

    // An empty list (launched without files) still reaches it, to bring the window forward.
    QVERIFY(second.forward({}));
    QVERIFY(received.wait());
    QVERIFY(received.last().first().toStringList().isEmpty());
}

void TestSingleInstance::forwardFailsWithoutInstance()
{
    const SingleInstance nobody(uniqueName("nobody"));
    QVERIFY(!nobody.forward({QStringLiteral("/tmp/x")}));
}

void TestSingleInstance::takesOverAStaleSocket()
{
    // What a crashed instance leaves behind: a socket file nobody listens on.
    const QString name = uniqueName("stale");
    const QString path = QDir::tempPath() + QLatin1Char('/') + name;
    QFile stale(path);
    QVERIFY(stale.open(QIODevice::WriteOnly));
    stale.close();

    SingleInstance instance(name);
    QVERIFY(!instance.forward({}));
    QVERIFY(instance.listen());
    QSignalSpy received(&instance, &SingleInstance::filesReceived);
    QVERIFY(SingleInstance(name).forward({QStringLiteral("/tmp/after-crash")}));
    QVERIFY(received.wait());
}

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_singleinstance.moc"
