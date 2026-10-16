#pragma once

#include <QTabWidget>

class QToolButton;

namespace aiedit::ui {

// The tabs of the documents, as other editors have them: a + right after the last tab makes a
// new document, the middle button closes a tab, and pinned tabs come first, with a pin where
// their close button was. Clicking the pin unpins a tab; dragging a tab in among the pinned
// ones pins it, and dragging one out of them unpins it.
class TabWidget : public QTabWidget
{
    Q_OBJECT

public:
    explicit TabWidget(QWidget* parent = nullptr);

    QToolButton* newTabButton() const { return m_newTab; }

    bool isPinned(int index) const;
    int pinnedCount() const;
    // A tab pinned goes after the pinned ones; one unpinned, just after them.
    void setPinned(int index, bool pinned);
    // Moves a tab, but not out of the pinned ones or in among them; returns where it went.
    int moveTab(int from, int to);

signals:
    void newTabRequested();
    void pinnedChanged(int index, bool pinned);

protected:
    void changeEvent(QEvent* event) override;

private:
    class Bar;

    void placeNewTabButton();
    // Pins or unpins a tab where it is.
    void markPinned(int index, bool pinned);
    // After a drag, the tab dropped takes the side it was dropped on.
    void settleDroppedTab(int index);
    void updatePinIcons();

    QToolButton* m_newTab = nullptr;
    QWidget* m_room = nullptr; // at the end of the bar, where the + goes when the tabs fill it
};

} // namespace aiedit::ui
