#pragma once

#include "ui/AiSettings.h"
#include "ui/ChatClient.h"

#include <QTimer>
#include <QWidget>

#include <functional>
#include <optional>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QToolButton;

namespace aiedit::ui {

// What the assistant may attach to a question, taken from the current document. Nothing is
// sent until the user asks, and the panel always shows what will go along.
struct AiContext
{
    QString fileName;
    QString fileDescription; // e.g. "Text · UTF-8, 12 KB"
    QString selectionLabel;  // e.g. "lines 3–5, 120 bytes"; empty without a selection
    QString selectionContent; // the selected text, or a hex dump of the selected bytes
    QString selectionFence;   // code-fence language for the content: a syntax name or "hexdump"
    bool selectionIsBinary = false;
    bool truncated = false; // only the start of a large selection is included
    // A local description of the whole file (type, entropy, strings, first bytes), built on demand.
    std::function<QString()> fileSummary;
};

// The AI assistant: a conversation about the open file, streamed from the configured service.
class AiPanel : public QWidget
{
    Q_OBJECT

public:
    using ContextProvider = std::function<std::optional<AiContext>()>;
    using SettingsProvider = std::function<AiSettings()>;

    explicit AiPanel(QWidget* parent = nullptr);
    ~AiPanel() override;

    void setContextProvider(ContextProvider provider);
    // Defaults to AiSettings::load(); tests point it at a local server.
    void setSettingsProvider(SettingsProvider provider);
    // Updates the "will be attached" line, e.g. after the selection changed.
    void refreshContext();
    // Re-reads the settings, e.g. to drop the "not set up" hint after they were saved.
    void settingsChanged();

    bool isBusy() const { return m_client.isBusy(); }
    QString transcript() const; // the conversation as Markdown
    // The exact JSON that asking `question` now would send.
    QByteArray previewRequest(const QString& question) const;

public slots:
    void ask(const QString& question);
    void explainSelection();
    void translateSelection();
    void summarizeSelection();
    void identifyFile();
    void stop();
    void clearConversation();

signals:
    void settingsRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Turn
    {
        QString question;
        QString attachmentLabel;
        QString message; // what was sent as the user message
        QString answer;
        QString reasoning;
        QString note;  // an error or a remark such as "cut off"
        bool streaming = false;
    };

    struct Attachment
    {
        QString label;
        QString text;
        bool binary = false;
    };

    std::optional<Attachment> selectionAttachment() const;
    QString composeMessage(const QString& question, const std::optional<Attachment>& attachment) const;
    QList<ChatMessage> conversation(const QString& nextMessage) const;
    void start(const QString& question, const std::optional<Attachment>& attachment);
    void askAboutSelection(const QString& textQuestion, const QString& bytesQuestion);
    void finished(const QString& finishReason);
    void failed(const QString& message);
    void render();
    void updateState();
    void showPreview();

    ChatClient m_client;
    QList<Turn> m_turns;
    ContextProvider m_contextProvider;
    SettingsProvider m_settingsProvider;
    QTimer m_renderTimer; // coalesces re-rendering while an answer streams in

    QTextBrowser* m_transcript = nullptr;
    QToolButton* m_explain = nullptr;
    QToolButton* m_translate = nullptr;
    QToolButton* m_summarize = nullptr;
    QToolButton* m_identify = nullptr;
    QCheckBox* m_attach = nullptr;
    QLabel* m_attachLabel = nullptr;
    QToolButton* m_preview = nullptr;
    QPlainTextEdit* m_input = nullptr;
    QPushButton* m_send = nullptr;
    QPushButton* m_stop = nullptr;
    QToolButton* m_clear = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace aiedit::ui
