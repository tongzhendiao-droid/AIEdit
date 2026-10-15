#include "ui/FindBar.h"

#include <QCheckBox>
#include <QComboBox>
#include <QBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPromise>
#include <QStyle>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace aiedit::ui {

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

    // The replace row, under the find row.
    m_replaceRow = new QWidget(this);
    m_replacement = new QLineEdit(m_replaceRow);
    m_replacement->setClearButtonEnabled(true);
    m_replacement->installEventFilter(this);
    m_replace = new QToolButton(m_replaceRow);
    m_replace->setText(tr("Replace"));
    m_replace->setToolTip(tr("Replace this match and find the next (Enter)"));
    m_replaceAll = new QToolButton(m_replaceRow);
    m_replaceAll->setText(tr("Replace All"));
    m_replaceAll->setToolTip(tr("Replace every match in the file, as one step to undo"));
    connect(m_replace, &QToolButton::clicked, this, &FindBar::replaceRequested);
    connect(m_replaceAll, &QToolButton::clicked, this, &FindBar::replaceAllRequested);

    auto* findLabel = new QLabel(tr("Find:"), this);
    auto* replaceLabel = new QLabel(tr("Replace:"), m_replaceRow);
    const int labelWidth = std::max(findLabel->sizeHint().width(), replaceLabel->sizeHint().width());
    findLabel->setMinimumWidth(labelWidth);
    replaceLabel->setMinimumWidth(labelWidth);

    auto* findRow = new QHBoxLayout;
    findRow->addWidget(findLabel);
    findRow->addWidget(m_query, 1);
    findRow->addWidget(m_mode);
    findRow->addWidget(m_caseSensitive);
    findRow->addWidget(m_previous);
    findRow->addWidget(m_next);
    findRow->addWidget(m_status);
    findRow->addWidget(m_close);
    auto* replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->addWidget(replaceLabel);
    replaceRow->addWidget(m_replacement, 1);
    replaceRow->addWidget(m_replace);
    replaceRow->addWidget(m_replaceAll);
    // Lines the buttons up under the find row's controls.
    replaceRow->addSpacing(m_status->minimumWidth() + m_close->sizeHint().width());
    m_replaceRow->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 3, 3, 3);
    layout->setSpacing(3);
    layout->addLayout(findRow);
    layout->addWidget(m_replaceRow);

    connect(&m_watcher, &QFutureWatcher<SearchOutcome>::finished, this, &FindBar::searchFinished);
    connect(&m_watcher, &QFutureWatcher<SearchOutcome>::progressValueChanged, this, [this](int permille) {
        showMessage(tr("Searching… %1%").arg(permille / 10));
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

QString FindBar::replacement() const
{
    return m_replacement->text();
}

void FindBar::setReplacement(const QString& replacement)
{
    m_replacement->setText(replacement);
}

bool FindBar::isReplaceVisible() const
{
    return isVisible() && m_replaceRow->isVisible();
}

void FindBar::setReplaceAvailable(bool available, const QString& reason)
{
    m_replaceAvailable = available;
    m_replaceUnavailableReason = reason;
    updateReplaceEnabled();
}

void FindBar::updateReplaceEnabled()
{
    const bool hex = mode() == SearchMode::Hex;
    const bool enabled = m_replaceAvailable && !hex;
    for (QWidget* widget : {static_cast<QWidget*>(m_replacement), static_cast<QWidget*>(m_replace),
                            static_cast<QWidget*>(m_replaceAll)})
        widget->setEnabled(enabled);
    m_replaceRow->setToolTip(!m_replaceAvailable ? m_replaceUnavailableReason
                             : hex               ? tr("Bytes are not replaced in hex mode")
                                                 : QString());
}

QString FindBar::statusText() const
{
    return m_status->text();
}

void FindBar::activate(bool replace)
{
    if (replace)
        m_replaceRow->show();
    else if (!isVisible())
        m_replaceRow->hide();
    show();
    m_query->setFocus(Qt::ShortcutFocusReason);
    m_query->selectAll();
}

void FindBar::start(const SearchRequest& request, const SearchContext& context)
{
    cancel();
    showMessage(tr("Searching…"));
    m_watcher.setFuture(QtConcurrent::run(
        [request, context](QPromise<SearchOutcome>& promise) { runSearch(promise, request, context); }));
}

void FindBar::cancel()
{
    if (m_watcher.isRunning()) {
        m_watcher.cancel();
        showMessage({});
    }
}

void FindBar::searchFinished()
{
    if (m_watcher.isCanceled() || m_watcher.future().resultCount() == 0)
        return;
    const SearchOutcome outcome = m_watcher.result();
    switch (outcome.status) {
    case SearchOutcome::Status::Found:
        showMessage(outcome.wrapped ? tr("Wrapped around") : QString());
        emit matchFound(outcome.start, outcome.end, outcome.wrapped);
        break;
    case SearchOutcome::Status::NotFound:
        showMessage(tr("Not found"), true);
        break;
    case SearchOutcome::Status::InvalidQuery:
        showMessage(outcome.error, true);
        break;
    case SearchOutcome::Status::Cancelled:
        showMessage({});
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
    m_replacement->setPlaceholderText(mode() == SearchMode::Regex ? tr("Replace with: $1 or \\1 for groups, \\n for a line break")
                                                                  : tr("Replace with"));
    updateReplaceEnabled();
    showMessage({});
}

void FindBar::showMessage(const QString& text, bool error)
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
    if ((watched == m_query || watched == m_replacement) && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (watched == m_replacement)
                emit replaceRequested();
            else
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

} // namespace aiedit::ui
