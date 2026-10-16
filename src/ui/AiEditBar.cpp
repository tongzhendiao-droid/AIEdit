#include "ui/AiEditBar.h"

#include <QBoxLayout>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QToolButton>

namespace aiedit::ui {

AiEditBar::AiEditBar(QWidget* parent) : QWidget(parent)
{
    auto* title = new QLabel(tr("AI edit:"), this);
    m_scope = new QLabel(this);
    m_scope->setForegroundRole(QPalette::PlaceholderText);

    m_input = new QLineEdit(this);
    m_input->setClearButtonEnabled(true);
    m_input->setPlaceholderText(tr("What to change, such as: translate the comments into English"));
    m_input->installEventFilter(this);
    connect(m_input, &QLineEdit::textChanged, this, &AiEditBar::updateButtons);

    const auto button = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    m_send = button(tr("Edit"), tr("Ask the AI for the change (Enter); you review it before anything is written"));
    m_stop = button(tr("Stop"), tr("Stop the AI"));
    m_preview = button(tr("Preview"), tr("See the request that would be sent"));
    m_close = new QToolButton(this);
    m_close->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    m_close->setToolTip(tr("Close (Esc)"));
    m_close->setAutoRaise(true);
    connect(m_send, &QToolButton::clicked, this, &AiEditBar::send);
    connect(m_stop, &QToolButton::clicked, this, &AiEditBar::stopRequested);
    connect(m_preview, &QToolButton::clicked, this, [this] { emit previewRequested(instruction()); });
    connect(m_close, &QToolButton::clicked, this, &AiEditBar::closeRequested);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message->hide();

    auto* row = new QHBoxLayout;
    row->addWidget(title);
    row->addWidget(m_input, 1);
    row->addWidget(m_scope);
    row->addWidget(m_send);
    row->addWidget(m_stop);
    row->addWidget(m_preview);
    row->addWidget(m_close);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(2);
    layout->addLayout(row);
    layout->addWidget(m_message);
    updateButtons();
}

QString AiEditBar::instruction() const
{
    return m_input->text().trimmed();
}

void AiEditBar::setScope(const QString& scope)
{
    m_scope->setText(scope);
}

void AiEditBar::setBusy(bool busy)
{
    m_busy = busy;
    m_input->setReadOnly(busy);
    updateButtons();
}

void AiEditBar::showMessage(const QString& text, bool error)
{
    // In red for an error, as the find bar has it; otherwise dimmed.
    m_message->setText(text);
    QPalette pal = palette();
    if (error) {
        const bool dark = pal.color(QPalette::Window).lightness() < 128;
        pal.setColor(QPalette::WindowText, dark ? QColor(0xEF, 0x9A, 0x9A) : QColor(0xC6, 0x28, 0x28));
    } else {
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
    }
    m_message->setPalette(pal);
    m_message->setVisible(!text.isEmpty());
}

void AiEditBar::activate()
{
    show();
    m_input->setFocus();
    m_input->selectAll();
}

void AiEditBar::send()
{
    if (!m_busy && !instruction().isEmpty())
        emit editRequested(instruction());
}

void AiEditBar::updateButtons()
{
    const bool typed = !instruction().isEmpty();
    m_send->setVisible(!m_busy);
    m_send->setEnabled(typed);
    m_stop->setVisible(m_busy);
    m_preview->setEnabled(typed && !m_busy);
}

bool AiEditBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            send();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            emit closeRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace aiedit::ui
