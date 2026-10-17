#include "ui/TabWidget.h"

#include <QEvent>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>

#include <algorithm>
#include <utility>

namespace aiedit::ui {

namespace {

constexpr int kGap = 2; // between the last tab and the +
const QString kPinName = QStringLiteral("pinTabButton");

QTabBar::ButtonPosition closeSide(const QTabBar* bar)
{
    return static_cast<QTabBar::ButtonPosition>(
        bar->style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, bar));
}

// Keeps room at the end of the tab bar for the + button.
class Room : public QWidget
{
public:
    using QWidget::QWidget;

    QSize sizeHint() const override { return m_size; }
    void setSize(QSize size)
    {
        if (size == m_size)
            return;
        m_size = size;
        updateGeometry();
    }

private:
    QSize m_size{0, 0};
};

// Where a pinned tab's close button was. It keeps that button to put back when the tab is
// unpinned, and deletes it when the tab is closed pinned.
class PinButton : public QToolButton
{
public:
    PinButton(QWidget* closeButton, QWidget* parent) : QToolButton(parent), m_closeButton(closeButton)
    {
        setObjectName(kPinName);
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(TabWidget::tr("Pinned: closing tabs together leaves it open. Click to unpin it."));
        const QSize size = closeButton ? closeButton->sizeHint() : QSize(16, 16);
        setFixedSize(size);
        setIconSize(size);
    }
    ~PinButton() override { delete m_closeButton; }

    QWidget* takeCloseButton() { return std::exchange(m_closeButton, nullptr); }

private:
    QPointer<QWidget> m_closeButton;
};

// A push pin, leaning to the right.
QIcon pinIcon(const QColor& color, QSize size, qreal ratio)
{
    QPixmap pixmap(size * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(size.width() / 2.0, size.height() / 2.0);
    painter.rotate(45);
    const qreal unit = std::min(size.width(), size.height()) / 16.0;
    painter.scale(unit, unit);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(-2.5, -6.5, 5, 6), 1, 1);         // the head
    painter.drawRoundedRect(QRectF(-4.5, -1, 9, 1.8), 0.8, 0.8);     // its rim
    painter.drawRect(QRectF(-0.6, 0.8, 1.2, 5.7));                   // the needle
    return QIcon(pixmap);
}

} // namespace

class TabWidget::Bar : public QTabBar
{
public:
    explicit Bar(TabWidget* tabs) : QTabBar(tabs), m_tabs(tabs) {}

protected:
    // Squeezed to fit, a tab keeps a dozen characters of its name, enough to tell it from the
    // others (Qt's own minimum keeps two); past that the bar scrolls.
    QSize minimumTabSizeHint(int index) const override
    {
        QSize size = QTabBar::minimumTabSizeHint(index);
        const int more = fontMetrics().horizontalAdvance(QLatin1Char('x')) * 10;
        size.setWidth(std::min(tabSizeHint(index).width(), size.width() + more));
        return size;
    }

    void tabLayoutChange() override
    {
        QTabBar::tabLayoutChange();
        m_tabs->placeNewTabButton();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton) {
            m_middlePressed = tabAt(event->position().toPoint());
            event->accept();
            return;
        }
        QTabBar::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        // Only the left button's double-click means anything. The middle button's is its
        // second click, as when closing tab after tab.
        if (event->button() == Qt::MiddleButton) {
            mousePressEvent(event);
            return;
        }
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }
        QTabBar::mouseDoubleClickEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton) {
            // The middle button closes the tab it was pressed and let go on, unless it is pinned.
            const int pressed = std::exchange(m_middlePressed, -1);
            const int index = tabAt(event->position().toPoint());
            if (index >= 0 && index == pressed && !m_tabs->isPinned(index))
                emit tabCloseRequested(index);
            event->accept();
            return;
        }
        QTabBar::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton)
            m_tabs->settleDroppedTab(currentIndex()); // the tab pressed, which a drag takes along
    }

private:
    TabWidget* m_tabs;
    int m_middlePressed = -1;
};

TabWidget::TabWidget(QWidget* parent) : QTabWidget(parent)
{
    setTabBar(new Bar(this));
    setDocumentMode(true);
    setTabsClosable(true);
    setMovable(true);
    tabBar()->setElideMode(Qt::ElideMiddle);

    m_newTab = new QToolButton(this);
    m_newTab->setAutoRaise(true);
    m_newTab->setFocusPolicy(Qt::NoFocus);
    // A plain plus, as browsers have after their tabs.
    if (const QIcon icon = QIcon::fromTheme(QStringLiteral("list-add"), QIcon::fromTheme(QStringLiteral("tab-new")));
        !icon.isNull())
        m_newTab->setIcon(icon);
    else
        m_newTab->setText(QStringLiteral("+"));
    connect(m_newTab, &QToolButton::clicked, this, &TabWidget::newTabRequested);
    m_room = new Room;
    setCornerWidget(m_room, Qt::TopRightCorner);
    placeNewTabButton();
}

void TabWidget::placeNewTabButton()
{
    if (!m_newTab)
        return; // the bar is being set up
    const QTabBar* bar = tabBar();
    const QRect area = bar->geometry();
    // Square, and no taller than the bar (some styles' tool buttons are), so that it sits in the
    // middle of it.
    const QSize hint = m_newTab->sizeHint();
    int side = std::max(hint.width(), hint.height());
    if (area.height() > 0)
        side = std::min(side, area.height());
    static_cast<Room*>(m_room)->setSize(QSize(side + 2 * kGap, side));

    int x = area.left();
    if (count() > 0)
        x += bar->tabRect(count() - 1).right() + 1;
    // Tabs that do not fit scroll, with buttons at the end of the bar; the + goes after those,
    // in the room kept for it. (The bar's own tool buttons are the scroll buttons.)
    bool scrolling = false;
    for (const QToolButton* button : bar->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        scrolling = scrolling || (button->objectName() != kPinName && button->isVisibleTo(bar));
    if (scrolling || x > area.right() + 1)
        x = area.right() + 1;
    m_newTab->setGeometry(x + kGap, area.top() + (area.height() - side) / 2, side, side);
    m_newTab->raise();
}

bool TabWidget::isPinned(int index) const
{
    return tabBar()->tabData(index).toBool();
}

int TabWidget::pinnedCount() const
{
    int pinned = 0;
    for (int i = 0; i < count(); ++i)
        pinned += isPinned(i) ? 1 : 0;
    return pinned;
}

void TabWidget::setPinned(int index, bool pinned)
{
    if (index < 0 || index >= count() || isPinned(index) == pinned)
        return;
    const int to = pinned ? pinnedCount() : pinnedCount() - 1;
    tabBar()->moveTab(index, to);
    markPinned(to, pinned);
}

int TabWidget::moveTab(int from, int to)
{
    if (from < 0 || from >= count())
        return from;
    const int pinned = pinnedCount();
    to = isPinned(from) ? std::clamp(to, 0, pinned - 1) : std::clamp(to, pinned, count() - 1);
    tabBar()->moveTab(from, to);
    return to;
}

void TabWidget::markPinned(int index, bool pinned)
{
    QTabBar* bar = tabBar();
    const QTabBar::ButtonPosition side = closeSide(bar);
    QWidget* button = bar->tabButton(index, side);
    bar->setTabData(index, pinned ? QVariant(true) : QVariant());
    if (pinned) {
        auto* pin = new PinButton(button, bar);
        pin->setIcon(pinIcon(bar->palette().color(QPalette::WindowText), pin->iconSize(), devicePixelRatioF()));
        connect(pin, &QToolButton::clicked, this, [this, pin] {
            for (int i = 0; i < count(); ++i) {
                if (tabBar()->tabButton(i, closeSide(tabBar())) == pin) {
                    setPinned(i, false);
                    return;
                }
            }
        });
        bar->setTabButton(index, side, pin);
    } else if (button && button->objectName() == kPinName) {
        auto* pin = static_cast<PinButton*>(button);
        bar->setTabButton(index, side, pin->takeCloseButton());
        pin->deleteLater();
    }
    emit pinnedChanged(index, pinned);
}

void TabWidget::settleDroppedTab(int index)
{
    if (index < 0 || index >= count())
        return;
    // The other tabs kept their order, pinned ones first, so only this one can be out of place.
    if (isPinned(index)) {
        for (int i = 0; i < index; ++i) {
            if (!isPinned(i)) {
                markPinned(index, false);
                return;
            }
        }
    } else {
        for (int i = index + 1; i < count(); ++i) {
            if (isPinned(i)) {
                markPinned(index, true);
                return;
            }
        }
    }
}

void TabWidget::updatePinIcons()
{
    const QTabBar* bar = tabBar();
    const QColor color = bar->palette().color(QPalette::WindowText);
    for (QToolButton* pin : bar->findChildren<QToolButton*>(kPinName, Qt::FindDirectChildrenOnly))
        pin->setIcon(pinIcon(color, pin->iconSize(), devicePixelRatioF()));
}

void TabWidget::changeEvent(QEvent* event)
{
    QTabWidget::changeEvent(event);
    // The pins are drawn in the colour of the text, which follows the colour scheme.
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        updatePinIcons();
}

} // namespace aiedit::ui
