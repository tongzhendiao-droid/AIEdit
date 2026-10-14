#include "ui/FindBar.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPromise>
#include <QStyle>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

namespace cppedit::ui {

FindBar::FindBar(QWidget* parent) : QWidget(parent)
{
    m_query = new QLineEdit(this);
    m_query->setClearButtonEnabled(true);
    m_query->installEventFilter(this);

    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Text"), static_cast<int>(SearchMode::Text));
    m_mode->addItem(tr("Regex"), static_cast<int>(SearchMode::Regex));
    m_mode->addItem(tr("Hex"), static_cast<int>(SearchMode::Hex));
    connect(m_mode, &QComboBox::currentIndexChanged, this, &FindBar::modeChanged);

    m_caseSensitive = new QCheckBox(tr("Match case"), this);

    auto button = [this](QStyle::StandardPixmap icon, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    m_previous = button(QStyle::SP_ArrowUp, tr("Find previous (Shift+Enter, Shift+F3)"));
    m_next = button(QStyle::SP_ArrowDown, tr("Find next (Enter, F3)"));
    m_close = button(QStyle::SP_DialogCloseButton, tr("Close (Esc)"));
    connect(m_previous, &QToolButton::clicked, this, [this] { emit searchRequested(false); });
    connect(m_next, &QToolButton::clicked, this, [this] { emit searchRequested(true); });
    connect(m_close, &QToolButton::clicked, this, &FindBar::closeRequested);

    m_status = new QLabel(this);
    m_status->setMinimumWidth(fontMetrics().horizontalAdvance(QLatin1Char('0')) * 16);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 3, 3);
    layout->addWidget(new QLabel(tr("Find:"), this));
    layout->addWidget(m_query, 1);
    layout->addWidget(m_mode);
    layout->addWidget(m_caseSensitive);
    layout->addWidget(m_previous);
    layout->addWidget(m_next);
    layout->addWidget(m_status);
    layout->addWidget(m_close);

    connect(&m_watcher, &QFutureWatcher<SearchOutcome>::finished, this, &FindBar::searchFinished);
    connect(&m_watcher, &QFutureWatcher<SearchOutcome>::progressValueChanged, this, [this](int permille) {
        showStatus(tr("Searching… %1%").arg(permille / 10));
    });
    modeChanged();
}

FindBar::~FindBar()
{
    // The search owns what it reads, so it may finish on its own; just stop it early.
    cancel();
}

SearchMode FindBar::mode() const
{
    return static_cast<SearchMode>(m_mode->currentData().toInt());
}

void FindBar::setMode(SearchMode mode)
{
    m_mode->setCurrentIndex(m_mode->findData(static_cast<int>(mode)));
}

QString FindBar::query() const
{
    return m_query->text();
}

void FindBar::setQuery(const QString& query)
{
    m_query->setText(query);
}

bool FindBar::caseSensitive() const
{
    return m_caseSensitive->isChecked();
}

void FindBar::setCaseSensitive(bool caseSensitive)
{
    m_caseSensitive->setChecked(caseSensitive);
}

QString FindBar::statusText() const
{
    return m_status->text();
}

void FindBar::activate()
{
    show();
    m_query->setFocus(Qt::ShortcutFocusReason);
    m_query->selectAll();
}

void FindBar::start(const SearchRequest& request, const SearchContext& context)
{
    cancel();
    showStatus(tr("Searching…"));
    m_watcher.setFuture(QtConcurrent::run(
        [request, context](QPromise<SearchOutcome>& promise) { runSearch(promise, request, context); }));
}

void FindBar::cancel()
{
    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        showStatus({});
    }
}

void FindBar::searchFinished()
{
    if (m_watcher.isCanceled() || m_watcher.future().resultCount() == 0)
        return;
    const SearchOutcome outcome = m_watcher.result();
    switch (outcome.status) {
    case SearchOutcome::Status::Found:
        showStatus(outcome.wrapped ? tr("Wrapped around") : QString());
        emit matchFound(outcome.start, outcome.end, outcome.wrapped);
        break;
    case SearchOutcome::Status::NotFound:
        showStatus(tr("Not found"), true);
        break;
    case SearchOutcome::Status::InvalidQuery:
        showStatus(outcome.error, true);
        break;
    case SearchOutcome::Status::Cancelled:
        showStatus({});
        break;
    }
}

void FindBar::modeChanged()
{
    const bool hex = mode() == SearchMode::Hex;
    m_caseSensitive->setEnabled(!hex);
    m_query->setPlaceholderText(hex ? tr("Hex bytes, e.g. 4D 5A ?? 00") : mode() == SearchMode::Regex
                                                                              ? tr("Regular expression")
                                                                              : tr("Text to find"));
    showStatus({});
}

void FindBar::showStatus(const QString& text, bool error)
{
    m_status->setText(text);
    QPalette pal = palette();
    if (error) {
        const bool dark = pal.color(QPalette::Window).lightness() < 128;
        pal.setColor(QPalette::WindowText, dark ? QColor(0xEF, 0x9A, 0x9A) : QColor(0xC6, 0x28, 0x28));
    }
    m_status->setPalette(pal);
}

bool FindBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            emit searchRequested(!(key->modifiers() & Qt::ShiftModifier));
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            emit closeRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace cppedit::ui
