#include "ui/AiPanel.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace aiedit::ui {

namespace {

constexpr int kRenderIntervalMs = 60;
constexpr qsizetype kMaxHistoryTurns = 8;
constexpr qsizetype kMaxShownReasoning = 600;
constexpr int kMaxToolRounds = 12;
constexpr qsizetype kMaxShownToolArguments = 120;

// The instructions are for the model, so they stay in English whatever the UI language.
const QString kSystemPrompt = QStringLiteral(
    "You are the assistant built into AIEdit, a fast viewer for text and binary files. The user may attach "
    "part of the open file (text, or a hex dump of bytes) or a summary of the whole file computed on their "
    "machine. Base your answer on what is attached, and say so when it is not enough. Be concise and concrete, "
    "give byte offsets in hex, and use Markdown. Answer in the language of the user's question.");

const QString kToolsPrompt = QStringLiteral(
    "You can call tools that run on the user's machine against the open file: get_file_info, read_bytes, "
    "read_lines, search, filter_lines and mark_fields. Files can be very large, so search and read only what you "
    "need instead of reading from the start; to count or collect lines of a log, use filter_lines. When you work "
    "out the structure of binary data, call mark_fields so the user can see the fields in the hex view.");

QString describeCall(const ToolCall& call)
{
    QString arguments = call.arguments.simplified();
    if (arguments.size() > kMaxShownToolArguments)
        arguments = arguments.left(kMaxShownToolArguments) + QStringLiteral("…");
    return QStringLiteral("`%1` %2").arg(call.name, arguments);
}

} // namespace

AiPanel::AiPanel(QWidget* parent) : QWidget(parent), m_settingsProvider(&AiSettings::load)
{
    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenLinks(false);
    connect(m_transcript, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1String("aiedit"))
            emit settingsRequested();
        else
            QDesktopServices::openUrl(url);
    });

    auto button = [this](const QString& text, const QString& tip, void (AiPanel::*slot)()) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, slot);
        return b;
    };
    m_explain = button(tr("Explain"), tr("Explain the selected text or bytes"), &AiPanel::explainSelection);
    m_translate = button(tr("Translate"), tr("Translate the selected text"), &AiPanel::translateSelection);
    m_summarize = button(tr("Summarize"), tr("Summarize the selected text"), &AiPanel::summarizeSelection);
    m_identify = button(tr("What Is This File?"),
                        tr("Identify the file from a summary computed on this machine; the file itself is not sent"),
                        &AiPanel::identifyFile);
    m_clear = button(tr("New Chat"), tr("Start a new conversation"), &AiPanel::clearConversation);

    m_attach = new QCheckBox(tr("Attach selection"), this);
    m_attach->setChecked(true);
    connect(m_attach, &QCheckBox::toggled, this, &AiPanel::refreshContext);
    m_attachLabel = new QLabel(this);
    m_attachLabel->setWordWrap(true);
    m_attachLabel->setForegroundRole(QPalette::PlaceholderText);
    m_preview = button(tr("Preview"), tr("Show exactly what will be sent"), &AiPanel::showPreview);

    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(tr("Ask about the file… (Ctrl+Enter sends)"));
    m_input->setMaximumHeight(m_input->fontMetrics().lineSpacing() * 5);
    m_input->installEventFilter(this);
    m_send = new QPushButton(tr("Send"), this);
    connect(m_send, &QPushButton::clicked, this, [this] { ask(m_input->toPlainText()); });
    m_stop = new QPushButton(tr("Stop"), this);
    connect(m_stop, &QPushButton::clicked, this, &AiPanel::stop);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    auto* actions = new QHBoxLayout;
    for (QToolButton* b : {m_explain, m_translate, m_summarize, m_identify})
        actions->addWidget(b);
    actions->addStretch();
    actions->addWidget(m_clear);

    auto* attachRow = new QHBoxLayout;
    attachRow->addWidget(m_attach);
    attachRow->addWidget(m_attachLabel, 1);
    attachRow->addWidget(m_preview);

    auto* sendRow = new QHBoxLayout;
    sendRow->addWidget(m_status, 1);
    sendRow->addWidget(m_stop);
    sendRow->addWidget(m_send);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(actions);
    layout->addWidget(m_transcript, 1);
    layout->addLayout(attachRow);
    layout->addWidget(m_input);
    layout->addLayout(sendRow);

    m_renderTimer.setSingleShot(true);
    m_renderTimer.setInterval(kRenderIntervalMs);
    connect(&m_renderTimer, &QTimer::timeout, this, &AiPanel::render);
    connect(&m_client, &ChatClient::contentReceived, this, [this](const QString& text) {
        Turn& turn = m_turns.last();
        // Text written before a tool call and the final answer are separate paragraphs.
        if (m_responseContent.isEmpty() && !turn.answer.isEmpty())
            turn.answer += QStringLiteral("\n\n");
        m_responseContent += text;
        turn.answer += text;
        if (!m_renderTimer.isActive())
            m_renderTimer.start();
    });
    connect(&m_client, &ChatClient::reasoningReceived, this, [this](const QString& text) {
        m_responseReasoning += text;
        m_turns.last().reasoning += text;
        if (!m_renderTimer.isActive())
            m_renderTimer.start();
    });
    connect(&m_client, &ChatClient::finished, this, &AiPanel::finished);
    connect(&m_client, &ChatClient::toolCallsRequested, this, &AiPanel::toolCallsRequested);
    connect(&m_client, &ChatClient::failed, this, &AiPanel::failed);

    render();
    refreshContext();
    updateState();
}

AiPanel::~AiPanel() = default;

void AiPanel::setContextProvider(ContextProvider provider)
{
    m_contextProvider = std::move(provider);
    refreshContext();
}

void AiPanel::setSettingsProvider(SettingsProvider provider)
{
    m_settingsProvider = std::move(provider);
    render();
}

void AiPanel::refreshContext()
{
    const auto attachment = selectionAttachment();
    if (!m_attach->isChecked())
        m_attachLabel->setText(tr("Nothing is attached."));
    else if (attachment)
        m_attachLabel->setText(tr("Will attach %1.").arg(attachment->label));
    else
        m_attachLabel->setText(tr("Nothing selected: only your question is sent."));
}

void AiPanel::settingsChanged()
{
    render();
}

std::optional<AiPanel::Attachment> AiPanel::selectionAttachment() const
{
    if (!m_contextProvider)
        return std::nullopt;
    const std::optional<AiContext> context = m_contextProvider();
    if (!context || context->selectionContent.isEmpty())
        return std::nullopt;

    // A fence longer than any backtick run inside keeps the content from closing it early.
    QString fence = QStringLiteral("```");
    while (context->selectionContent.contains(fence))
        fence += QLatin1Char('`');
    Attachment attachment;
    attachment.label = context->truncated ? tr("the beginning of %1 of %2").arg(context->selectionLabel, context->fileName)
                                          : tr("%1 of %2").arg(context->selectionLabel, context->fileName);
    attachment.text = QStringLiteral("Selection from \"%1\" (%2), %3:\n%4%5\n%6\n%4")
                          .arg(context->fileName, context->fileDescription, context->selectionLabel, fence,
                               context->selectionFence, context->selectionContent);
    if (context->truncated)
        attachment.text += QStringLiteral("\n(The selection is longer; only its beginning is included.)");
    attachment.binary = context->selectionIsBinary;
    return attachment;
}

QString AiPanel::composeMessage(const QString& question, const std::optional<Attachment>& attachment) const
{
    return attachment ? question + QStringLiteral("\n\n") + attachment->text : question;
}

QList<ChatMessage> AiPanel::conversation(const QString& nextMessage, bool withTools) const
{
    QList<ChatMessage> messages{
        {ChatMessage::Role::System, withTools ? kSystemPrompt + QLatin1Char(' ') + kToolsPrompt : kSystemPrompt}};
    // Earlier answered turns give the model context for follow-up questions.
    QList<ChatMessage> history;
    for (const Turn& turn : m_turns) {
        if (turn.streaming || turn.answer.isEmpty())
            continue;
        history.append({ChatMessage::Role::User, turn.message});
        history.append({ChatMessage::Role::Assistant, turn.answer});
    }
    messages += history.mid(std::max<qsizetype>(0, history.size() - 2 * kMaxHistoryTurns));
    messages.append({ChatMessage::Role::User, nextMessage});
    return messages;
}

QByteArray AiPanel::previewRequest(const QString& question) const
{
    const auto attachment = m_attach->isChecked() ? selectionAttachment() : std::nullopt;
    const std::optional<AiContext> context = m_contextProvider ? m_contextProvider() : std::nullopt;
    const bool withTools = context && context->toolTarget;
    return ChatClient::requestBody(m_settingsProvider(), conversation(composeMessage(question, attachment), withTools),
                                   withTools ? tools::definitions() : QJsonArray());
}

void AiPanel::ask(const QString& question)
{
    const QString trimmed = question.trimmed();
    if (trimmed.isEmpty() || isBusy())
        return;
    const auto attachment = m_attach->isChecked() ? selectionAttachment() : std::nullopt;
    m_input->clear();
    start(trimmed, attachment);
}

void AiPanel::askAboutSelection(const QString& textQuestion, const QString& bytesQuestion)
{
    const auto attachment = selectionAttachment();
    if (!attachment) {
        m_status->setText(tr("Select some text or bytes first."));
        return;
    }
    start(attachment->binary ? bytesQuestion : textQuestion, attachment);
}

void AiPanel::explainSelection()
{
    askAboutSelection(tr("Explain this."), tr("Explain these bytes: what they are and how they are structured."));
}

void AiPanel::translateSelection()
{
    const QString language = QLocale().nativeLanguageName();
    askAboutSelection(tr("Translate this into %1. If it is already in %1, translate it into English.").arg(language),
                      tr("Decode any text in these bytes and translate it into %1.").arg(language));
}

void AiPanel::summarizeSelection()
{
    askAboutSelection(tr("Summarize this: the main points, and anything unusual."),
                      tr("Summarize what these bytes contain."));
}

void AiPanel::identifyFile()
{
    const std::optional<AiContext> context = m_contextProvider ? m_contextProvider() : std::nullopt;
    if (!context || !context->fileSummary) {
        m_status->setText(tr("Open a file first."));
        return;
    }
    Attachment summary;
    summary.label = tr("a summary of %1 computed on this machine").arg(context->fileName);
    summary.text = context->fileSummary();
    start(tr("What kind of file is this? Name the format and what it is used for, and point out anything notable."),
          summary);
}

void AiPanel::start(const QString& question, const std::optional<Attachment>& attachment)
{
    if (isBusy())
        return;
    const AiSettings settings = m_settingsProvider();
    if (!settings.isConfigured()) {
        m_status->setText(tr("Set up the AI service first."));
        emit settingsRequested();
        return;
    }

    Turn turn;
    turn.question = question;
    turn.attachmentLabel = attachment ? attachment->label : QString();
    turn.message = composeMessage(question, attachment);
    turn.streaming = true;
    if (m_contextProvider) {
        if (const std::optional<AiContext> context = m_contextProvider())
            turn.target = context->toolTarget;
    }
    m_turns.append(turn);
    m_working = true;
    sendRequest();
    updateState();
}

void AiPanel::sendRequest()
{
    const AiSettings settings = m_settingsProvider();
    const Turn& turn = m_turns.last();
    const bool withTools = turn.target.has_value();
    QList<ChatMessage> messages = conversation(turn.message, withTools);
    messages += turn.exchange;
    m_responseContent.clear();
    m_responseReasoning.clear();
    m_client.send(settings, messages, withTools ? tools::definitions() : QJsonArray());
    m_status->setText(tr("Asking %1…").arg(settings.chatUrl().host()));
    render();
}

void AiPanel::toolCallsRequested(const QList<ToolCall>& calls)
{
    Turn& turn = m_turns.last();
    if (!turn.target || ++turn.toolRounds > kMaxToolRounds) {
        turn.streaming = false;
        turn.note = tr("Stopped after %1 rounds of tool calls.").arg(kMaxToolRounds);
        m_working = false;
        m_status->clear();
        render();
        updateState();
        return;
    }
    ChatMessage request(ChatMessage::Role::Assistant, m_responseContent);
    request.toolCalls = calls;
    request.reasoning = m_responseReasoning;
    turn.exchange.append(request);
    m_pendingCalls = calls;
    runNextTool();
}

void AiPanel::runNextTool()
{
    if (!m_working)
        return;
    if (m_pendingCalls.isEmpty()) {
        sendRequest(); // all results are in: let the model continue
        return;
    }
    const ToolCall call = m_pendingCalls.takeFirst();
    Turn& turn = m_turns.last();
    turn.toolLog << describeCall(call);
    m_status->setText(tr("Running %1…").arg(call.name));
    render();
    m_tools.run(call, *turn.target, [this, id = call.id](const QString& result) {
        ChatMessage reply(ChatMessage::Role::Tool, result);
        reply.toolCallId = id;
        m_turns.last().exchange.append(reply);
        runNextTool();
    });
}

void AiPanel::stop()
{
    if (!isBusy())
        return;
    m_client.cancel();
    m_tools.cancel();
    m_pendingCalls.clear();
    m_working = false;
    m_turns.last().streaming = false;
    m_turns.last().note = tr("Stopped.");
    m_status->clear();
    render();
    updateState();
}

void AiPanel::clearConversation()
{
    m_client.cancel();
    m_tools.cancel();
    m_pendingCalls.clear();
    m_working = false;
    m_turns.clear();
    m_status->clear();
    render();
    updateState();
}

void AiPanel::focusInput()
{
    m_input->setFocus(Qt::OtherFocusReason);
}

void AiPanel::finished(const QString& finishReason)
{
    Turn& turn = m_turns.last();
    turn.streaming = false;
    m_working = false;
    if (finishReason == QLatin1String("length"))
        turn.note = tr("The answer was cut off at the length limit.");
    else if (finishReason != QLatin1String("stop"))
        turn.note = tr("The answer ended early (%1).").arg(finishReason);
    m_status->clear();
    render();
    updateState();
}

void AiPanel::failed(const QString& message)
{
    Turn& turn = m_turns.last();
    turn.streaming = false;
    m_working = false;
    turn.note = message;
    m_status->setText(tr("The request failed."));
    render();
    updateState();
}

QString AiPanel::transcript() const
{
    if (m_turns.isEmpty()) {
        QString intro = tr("Select text or bytes and choose **Explain**, **Translate** or **Summarize**, ask "
                           "**What Is This File?**, or type a question below.");
        if (!m_settingsProvider().isConfigured())
            intro += QStringLiteral("\n\n") +
                     tr("The AI service is not set up yet: [open the AI settings](aiedit:settings).");
        return intro;
    }

    QString markdown;
    for (const Turn& turn : m_turns) {
        markdown += QStringLiteral("**%1:** %2\n\n").arg(tr("You"), turn.question);
        if (!turn.attachmentLabel.isEmpty())
            markdown += QStringLiteral("> %1\n\n").arg(tr("Attached: %1").arg(turn.attachmentLabel));
        markdown += QStringLiteral("**%1:**\n\n").arg(tr("AI"));
        for (const QString& call : turn.toolLog)
            markdown += QStringLiteral("> %1\n\n").arg(tr("Tool: %1").arg(call));
        if (!turn.reasoning.isEmpty()) {
            QString reasoning = turn.reasoning.simplified();
            if (reasoning.size() > kMaxShownReasoning)
                reasoning = QStringLiteral("…") + reasoning.right(kMaxShownReasoning);
            markdown += QStringLiteral("> *%1* %2\n\n").arg(tr("Thinking:"), reasoning);
        }
        markdown += turn.answer.isEmpty() && turn.streaming ? QStringLiteral("…") : turn.answer;
        if (!turn.note.isEmpty())
            markdown += QStringLiteral("\n\n*%1*").arg(turn.note);
        markdown += QStringLiteral("\n\n---\n\n");
    }
    return markdown;
}

void AiPanel::render()
{
    QScrollBar* scroll = m_transcript->verticalScrollBar();
    const bool atBottom = scroll->value() >= scroll->maximum() - 4;
    m_transcript->setMarkdown(transcript());
    if (atBottom || isBusy())
        scroll->setValue(scroll->maximum());
}

void AiPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    // Markdown is turned into text with the link colour of the moment: a new palette (a colour
    // scheme) needs it done again.
    if (event->type() == QEvent::PaletteChange && m_transcript)
        render();
}

void AiPanel::updateState()
{
    const bool busy = isBusy();
    m_send->setEnabled(!busy);
    m_stop->setEnabled(busy);
    for (QToolButton* b : {m_explain, m_translate, m_summarize, m_identify})
        b->setEnabled(!busy);
}

void AiPanel::showPreview()
{
    const QString typed = m_input->toPlainText().trimmed();
    showRequestPreview(this, m_settingsProvider(), previewRequest(typed.isEmpty() ? tr("(your question)") : typed));
}

void showRequestPreview(QWidget* parent, const AiSettings& settings, const QByteArray& body)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(AiPanel::tr("What Will Be Sent"));
    auto* intro = new QLabel(AiPanel::tr("A POST request to %1 with the body below. The API key travels in a header "
                                         "and is not shown.")
                                 .arg(settings.chatUrl().toString()),
                             &dialog);
    intro->setWordWrap(true);
    auto* text = new QPlainTextEdit(QString::fromUtf8(QJsonDocument::fromJson(body).toJson(QJsonDocument::Indented)),
                                    &dialog);
    text->setReadOnly(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(intro);
    layout->addWidget(text, 1);
    layout->addWidget(buttons);
    dialog.resize(720, 520);
    dialog.exec();
}

bool AiPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && (key->modifiers() & Qt::ControlModifier)) {
            ask(m_input->toPlainText());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace aiedit::ui
