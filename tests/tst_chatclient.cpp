#include "ui/AiPanel.h"
#include "ui/MainWindow.h"
#include "ui/AiSettings.h"
#include "ui/ChatClient.h"

#include <QCheckBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using namespace aiedit::ui;

namespace {

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
    int chunkDelayMs = 5;

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
        ++requests;
        m_buffers.remove(socket);

        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Status\r\nContent-Type: " + contentType +
                      "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
        new ChunkWriter(socket, chunks, chunkDelayMs);
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

QByteArray chunk(const QString& content, const QString& field = QStringLiteral("content"),
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

AiSettings settingsFor(const MockServer& server, AiSettings::Provider provider = AiSettings::Provider::DeepSeek)
{
    AiSettings settings;
    settings.provider = provider;
    settings.baseUrl = server.baseUrl() + QStringLiteral("/");
    settings.model = QStringLiteral("deepseek-flash");
    settings.apiKey = QStringLiteral("test-key");
    return settings;
}

QList<ChatMessage> question(const QString& text)
{
    return {{ChatMessage::Role::System, QStringLiteral("You are a test.")}, {ChatMessage::Role::User, text}};
}

} // namespace

class TestChatClient : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void streamsAnswer();
    void streamsReasoning();
    void otherProvidersGetNoThinkingField();
    void reportsHttpErrors();
    void reportsTruncation();
    void cancelStopsSignals();
    void reportsConnectionFailure();
    void settingsKeepTheKeyPrivate();
    void panelExplainsSelection();
    void panelKeepsConversation();
    void panelIdentifiesFileFromSummary();
    void panelNeedsSelectionAndSettings();
    void panelShowsStopAndErrors();
    void panelPreviewHasNoKey();
    void renderScreenshots();
};

void TestChatClient::initTestCase()
{
    // Keep the settings tests away from the user's real configuration.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTest"));
    QCoreApplication::setApplicationName(QStringLiteral("AIEditTest"));
}

void TestChatClient::streamsAnswer()
{
    MockServer server;
    const QByteArray answer = chunk(QStringLiteral("你好")) + chunk(QStringLiteral("，世界"));
    // Split the stream mid-character and add a keep-alive comment, as a slow network would.
    server.chunks = {"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n",
                     ": keep-alive\n\n", answer.left(30), answer.mid(30),
                     chunk(QString(), QStringLiteral("content"), QStringLiteral("stop")), "data: [DONE]\n\n"};

    ChatClient client;
    QString received;
    connect(&client, &ChatClient::contentReceived, this, [&](const QString& text) { received += text; });
    QSignalSpy finished(&client, &ChatClient::finished);
    QSignalSpy failed(&client, &ChatClient::failed);

    client.send(settingsFor(server), question(QStringLiteral("hi")));
    QVERIFY(client.isBusy());
    QVERIFY(finished.wait());
    QCOMPARE(received, QStringLiteral("你好，世界"));
    QCOMPARE(finished.first().first().toString(), QStringLiteral("stop"));
    QVERIFY(failed.isEmpty());
    QVERIFY(!client.isBusy());

    QCOMPARE(server.requestLine, QByteArray("POST /chat/completions HTTP/1.1"));
    QCOMPARE(server.headers.value("authorization"), QByteArray("Bearer test-key"));
    QCOMPARE(server.headers.value("content-type"), QByteArray("application/json"));
    const QJsonObject body = QJsonDocument::fromJson(server.body).object();
    QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("deepseek-flash"));
    QCOMPARE(body.value(QStringLiteral("stream")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("disabled"));
    const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages[1].toObject().value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    QCOMPARE(messages[1].toObject().value(QStringLiteral("content")).toString(), QStringLiteral("hi"));
}

void TestChatClient::streamsReasoning()
{
    MockServer server;
    server.chunks = {chunk(QStringLiteral("Let me think."), QStringLiteral("reasoning_content")),
                     chunk(QStringLiteral("42")), "data: [DONE]\n\n"};
    ChatClient client;
    QString reasoning;
    QString answer;
    connect(&client, &ChatClient::reasoningReceived, this, [&](const QString& text) { reasoning += text; });
    connect(&client, &ChatClient::contentReceived, this, [&](const QString& text) { answer += text; });
    QSignalSpy finished(&client, &ChatClient::finished);
    AiSettings settings = settingsFor(server);
    settings.thinking = true;
    client.send(settings, question(QStringLiteral("?")));
    QVERIFY(finished.wait());
    QCOMPARE(reasoning, QStringLiteral("Let me think."));
    QCOMPARE(answer, QStringLiteral("42"));
    QCOMPARE(QJsonDocument::fromJson(server.body)
                 .object()
                 .value(QStringLiteral("thinking"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("enabled"));
}

void TestChatClient::otherProvidersGetNoThinkingField()
{
    MockServer server;
    server.chunks = {chunk(QStringLiteral("ok")), "data: [DONE]\n\n"};
    ChatClient client;
    QSignalSpy finished(&client, &ChatClient::finished);
    client.send(settingsFor(server, AiSettings::Provider::OpenAiCompatible), question(QStringLiteral("?")));
    QVERIFY(finished.wait());
    QVERIFY(!QJsonDocument::fromJson(server.body).object().contains(QStringLiteral("thinking")));
}

void TestChatClient::reportsHttpErrors()
{
    MockServer server;
    server.status = 401;
    server.contentType = "application/json";
    server.chunks = {R"({"error":{"message":"Authentication Fails, Your api key is invalid","type":"authentication_error"}})"};
    ChatClient client;
    QSignalSpy failed(&client, &ChatClient::failed);
    QSignalSpy finished(&client, &ChatClient::finished);
    client.send(settingsFor(server), question(QStringLiteral("?")));
    QVERIFY(failed.wait());
    const QString message = failed.first().first().toString();
    QVERIFY2(message.contains(QStringLiteral("401")) && message.contains(QStringLiteral("Authentication Fails")),
             qPrintable(message));
    QVERIFY(finished.isEmpty());

    server.status = 402;
    server.contentType = "text/plain";
    server.chunks = {"Insufficient Balance"};
    client.send(settingsFor(server), question(QStringLiteral("?")));
    QVERIFY(failed.wait());
    QVERIFY(failed.last().first().toString().contains(QStringLiteral("Insufficient Balance")));
}

void TestChatClient::reportsTruncation()
{
    MockServer server;
    server.chunks = {chunk(QStringLiteral("partial")),
                     chunk(QString(), QStringLiteral("content"), QStringLiteral("length")), "data: [DONE]\n\n"};
    ChatClient client;
    QSignalSpy finished(&client, &ChatClient::finished);
    client.send(settingsFor(server), question(QStringLiteral("?")));
    QVERIFY(finished.wait());
    QCOMPARE(finished.first().first().toString(), QStringLiteral("length"));
}

void TestChatClient::cancelStopsSignals()
{
    MockServer server;
    server.chunkDelayMs = 100;
    server.chunks = {chunk(QStringLiteral("one ")), chunk(QStringLiteral("two ")), chunk(QStringLiteral("three")),
                     "data: [DONE]\n\n"};
    ChatClient client;
    QStringList pieces;
    connect(&client, &ChatClient::contentReceived, this, [&](const QString& text) {
        pieces << text;
        client.cancel(); // stop after the first piece, from inside the signal
    });
    QSignalSpy finished(&client, &ChatClient::finished);
    QSignalSpy failed(&client, &ChatClient::failed);
    client.send(settingsFor(server), question(QStringLiteral("?")));
    QTRY_COMPARE(pieces.size(), 1);
    QTest::qWait(400);
    QCOMPARE(pieces.size(), 1);
    QVERIFY(finished.isEmpty() && failed.isEmpty());
    QVERIFY(!client.isBusy());
}

void TestChatClient::reportsConnectionFailure()
{
    quint16 port = 0;
    {
        QTcpServer probe; // a port that was free a moment ago
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }
    AiSettings settings;
    settings.baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    settings.apiKey = QStringLiteral("test-key");
    ChatClient client;
    QSignalSpy failed(&client, &ChatClient::failed);
    client.send(settings, question(QStringLiteral("?")));
    QVERIFY(failed.wait());
    QVERIFY(!failed.first().first().toString().isEmpty());
}

void TestChatClient::settingsKeepTheKeyPrivate()
{
    qunsetenv(AiSettings::kKeyEnvironmentVariable);
    qunsetenv(AiSettings::kDeepSeekKeyEnvironmentVariable);
    QFile::remove(AiSettings::keyFilePath());

    AiSettings saved;
    saved.provider = AiSettings::Provider::OpenAiCompatible;
    saved.baseUrl = QStringLiteral("http://localhost:11434/v1");
    saved.model = QStringLiteral("qwen3");
    saved.apiKey = QStringLiteral("test-key-123");
    saved.thinking = true;
    saved.save();

    const auto permissions = QFile::permissions(AiSettings::keyFilePath());
    QVERIFY(permissions & QFileDevice::ReadOwner);
    QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup |
                             QFileDevice::WriteOther)));

    AiSettings loaded = AiSettings::load();
    QCOMPARE(loaded.provider, AiSettings::Provider::OpenAiCompatible);
    QCOMPARE(loaded.baseUrl, saved.baseUrl);
    QCOMPARE(loaded.model, saved.model);
    QCOMPARE(loaded.apiKey, saved.apiKey);
    QVERIFY(loaded.thinking);
    QVERIFY(!loaded.keyFromEnvironment);
    QCOMPARE(loaded.chatUrl(), QUrl(QStringLiteral("http://localhost:11434/v1/chat/completions")));

    // A key from the environment wins and is never written to disk.
    qputenv(AiSettings::kKeyEnvironmentVariable, "env-key");
    loaded = AiSettings::load();
    QCOMPARE(loaded.apiKey, QStringLiteral("env-key"));
    QVERIFY(loaded.keyFromEnvironment);
    loaded.save();
    qunsetenv(AiSettings::kKeyEnvironmentVariable);
    QCOMPARE(AiSettings::load().apiKey, QStringLiteral("test-key-123"));

    saved.apiKey.clear();
    saved.save();
    QVERIFY(!QFile::exists(AiSettings::keyFilePath()));
}

namespace {

AiContext sampleContext()
{
    AiContext context;
    context.fileName = QStringLiteral("notes.txt");
    context.fileDescription = QStringLiteral("text, decoded as UTF-8, 120 bytes");
    context.selectionLabel = QStringLiteral("line 3 (11 bytes)");
    context.selectionContent = QStringLiteral("hello world");
    context.selectionFence = QStringLiteral("text");
    context.fileSummary = [] { return QStringLiteral("File: notes.txt (local summary)"); };
    return context;
}

QJsonArray sentMessages(const MockServer& server)
{
    return QJsonDocument::fromJson(server.body).object().value(QStringLiteral("messages")).toArray();
}

} // namespace

void TestChatClient::panelExplainsSelection()
{
    MockServer server;
    server.chunks = {chunk(QStringLiteral("It is a ")), chunk(QStringLiteral("**greeting**.")), "data: [DONE]\n\n"};
    AiPanel panel;
    panel.setSettingsProvider([&] { return settingsFor(server); });
    panel.setContextProvider([] { return std::optional<AiContext>(sampleContext()); });

    panel.explainSelection();
    QVERIFY(panel.isBusy());
    QTRY_VERIFY(!panel.isBusy());
    const QString transcript = panel.transcript();
    QVERIFY2(transcript.contains(QStringLiteral("It is a **greeting**.")), qPrintable(transcript));
    QVERIFY(transcript.contains(QStringLiteral("line 3 (11 bytes) of notes.txt")));

    const QJsonArray messages = sentMessages(server);
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages[0].toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
    const QString user = messages[1].toObject().value(QStringLiteral("content")).toString();
    QVERIFY(user.startsWith(QStringLiteral("Explain this.")));
    QVERIFY(user.contains(QStringLiteral("```text\nhello world\n```")));
    QVERIFY(user.contains(QStringLiteral("notes.txt")));
}

void TestChatClient::panelKeepsConversation()
{
    MockServer server;
    server.chunks = {chunk(QStringLiteral("first answer")), "data: [DONE]\n\n"};
    AiPanel panel;
    panel.setSettingsProvider([&] { return settingsFor(server); });
    panel.ask(QStringLiteral("first question"));
    QTRY_VERIFY(!panel.isBusy());

    server.chunks = {chunk(QStringLiteral("second answer")), "data: [DONE]\n\n"};
    panel.ask(QStringLiteral("and then?"));
    QTRY_VERIFY(!panel.isBusy());
    const QJsonArray messages = sentMessages(server);
    QCOMPARE(messages.size(), 4); // system, first question, first answer, follow-up
    QCOMPARE(messages[2].toObject().value(QStringLiteral("content")).toString(), QStringLiteral("first answer"));
    QCOMPARE(messages[3].toObject().value(QStringLiteral("content")).toString(), QStringLiteral("and then?"));
    QVERIFY(panel.transcript().contains(QStringLiteral("second answer")));

    panel.clearConversation();
    QVERIFY(!panel.transcript().contains(QStringLiteral("first answer")));
}

void TestChatClient::panelIdentifiesFileFromSummary()
{
    MockServer server;
    server.chunks = {chunk(QStringLiteral("A text file.")), "data: [DONE]\n\n"};
    AiPanel panel;
    panel.setSettingsProvider([&] { return settingsFor(server); });
    panel.setContextProvider([] { return std::optional<AiContext>(sampleContext()); });
    panel.identifyFile();
    QTRY_VERIFY(!panel.isBusy());
    const QString user = sentMessages(server)[1].toObject().value(QStringLiteral("content")).toString();
    QVERIFY(user.contains(QStringLiteral("File: notes.txt (local summary)")));
    QVERIFY(!user.contains(QStringLiteral("hello world"))); // the selection is not part of it
}

void TestChatClient::panelNeedsSelectionAndSettings()
{
    MockServer server;
    AiPanel panel;
    panel.setSettingsProvider([&] { return settingsFor(server); });
    panel.setContextProvider([] {
        AiContext context = sampleContext();
        context.selectionContent.clear(); // nothing selected
        return std::optional<AiContext>(context);
    });
    panel.explainSelection();
    QVERIFY(!panel.isBusy());

    QSignalSpy settingsRequested(&panel, &AiPanel::settingsRequested);
    panel.setSettingsProvider([] { return AiSettings(); }); // no key
    panel.ask(QStringLiteral("anything"));
    QCOMPARE(settingsRequested.count(), 1);
    QVERIFY(!panel.isBusy());
    QTest::qWait(50);
    QCOMPARE(server.requests, 0);
}

void TestChatClient::panelShowsStopAndErrors()
{
    MockServer server;
    server.chunkDelayMs = 200;
    server.chunks = {chunk(QStringLiteral("slow ")), chunk(QStringLiteral("answer")), "data: [DONE]\n\n"};
    AiPanel panel;
    panel.setSettingsProvider([&] { return settingsFor(server); });
    panel.ask(QStringLiteral("take your time"));
    QTRY_VERIFY(panel.transcript().contains(QStringLiteral("slow")));
    panel.stop();
    QVERIFY(!panel.isBusy());
    QVERIFY(panel.transcript().contains(QStringLiteral("Stopped.")));

    MockServer failing;
    failing.status = 402;
    failing.contentType = "application/json";
    failing.chunks = {R"({"error":{"message":"Insufficient Balance"}})"};
    panel.setSettingsProvider([&] { return settingsFor(failing); });
    panel.ask(QStringLiteral("again"));
    QTRY_VERIFY(!panel.isBusy());
    QVERIFY(panel.transcript().contains(QStringLiteral("Insufficient Balance")));
}

void TestChatClient::panelPreviewHasNoKey()
{
    MockServer server;
    AiPanel panel;
    panel.setSettingsProvider([&] { return settingsFor(server); });
    panel.setContextProvider([] { return std::optional<AiContext>(sampleContext()); });
    QByteArray preview = panel.previewRequest(QStringLiteral("what is it?"));
    QVERIFY(preview.contains("hello world"));
    QVERIFY(!preview.contains("test-key"));

    panel.findChild<QCheckBox*>()->setChecked(false); // "Attach selection"
    preview = panel.previewRequest(QStringLiteral("what is it?"));
    QVERIFY(!preview.contains("hello world"));
}

void TestChatClient::renderScreenshots()
{
    // Set AIEDIT_SCREENSHOT_DIR to look at the rendering; otherwise this only checks that it paints.
    const QByteArray dir = qgetenv("AIEDIT_SCREENSHOT_DIR");
    MockServer server;
    const QString answer = QStringLiteral(
        "这是一个 **ELF 64 位** 可执行文件的文件头：\n\n"
        "| 偏移 | 字段 | 值 |\n|---|---|---|\n| 0x00 | 魔数 | `7F 45 4C 46` |\n| 0x04 | 位数 | 2（64 位） |\n\n"
        "- 小端序\n- 目标平台：x86-64\n\n```c\nElf64_Ehdr header; /* 64 字节 */\n```");
    server.chunks = {chunk(answer), "data: [DONE]\n\n"};
    AiPanel panel;
    panel.resize(460, 640);
    panel.setSettingsProvider([&] { return settingsFor(server); });
    AiContext context = sampleContext();
    context.selectionLabel = QStringLiteral("bytes 0x0–0x3F (64 bytes)");
    context.selectionIsBinary = true;
    panel.setContextProvider([context] { return std::optional<AiContext>(context); });
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.explainSelection();
    QTRY_VERIFY(!panel.isBusy());
    QTest::qWait(100);
    QPixmap shot = panel.grab();
    QVERIFY(!shot.isNull());
    if (!dir.isEmpty())
        QVERIFY(shot.save(QString::fromLocal8Bit(dir) + QStringLiteral("/aipanel.png")));

    MainWindow window;
    window.resize(1200, 560);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    shot = window.grab();
    if (!dir.isEmpty())
        QVERIFY(shot.save(QString::fromLocal8Bit(dir) + QStringLiteral("/mainwindow-ai.png")));
}

QTEST_MAIN(TestChatClient)
#include "tst_chatclient.moc"
