#include "ui/SingleInstance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTest>

#include <future>

using aiedit::ui::SingleInstance;

namespace {

// What a second launch does, from another thread: it is another process, and this thread has to
// take the connection meanwhile. (A Windows pipe holds little, so a write waits for the reader.)
std::future<bool> forwardFromElsewhere(const SingleInstance& instance, const QStringList& files)
{
    return std::async(std::launch::async, [&instance, files] { return instance.forward(files); });
}

} // namespace

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
    std::future<bool> forwarded = forwardFromElsewhere(second, files);
    QVERIFY(received.wait());
    QVERIFY(forwarded.get());
    QCOMPARE(received.first().first().toStringList(), files);

    // An empty list (launched without files) still reaches it, to bring the window forward.
    forwarded = forwardFromElsewhere(second, {});
    QVERIFY(received.wait());
    QVERIFY(forwarded.get());
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
    const SingleInstance second(name);
    std::future<bool> forwarded = forwardFromElsewhere(second, {QStringLiteral("/tmp/after-crash")});
    QVERIFY(received.wait());
    QVERIFY(forwarded.get());
}

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_singleinstance.moc"
