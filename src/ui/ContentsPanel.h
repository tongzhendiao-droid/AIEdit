#pragma once

#include "ui/Contents.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace aiedit::ui {

class DocumentView;
class FilterRuleWriter;

// The contents of a book: its headings (see Contents.h), found in the background for the
// current document. Picking one goes there; the one being read is marked as the text
// scrolls. Where the built-in rule finds nothing, or the wrong things, the AI can write
// one for the book, which is then remembered for that file. A chapter can go to the AI to
// be summarized, translated or explained.
class ContentsPanel : public QWidget
{
    Q_OBJECT

public:
    enum class ChapterAction
    {
        Summarize,
        Translate,
        Explain,
    };

    explicit ContentsPanel(QWidget* parent = nullptr);
    ~ContentsPanel() override;

    // The document to show the contents of; nullptr for none.
    void setDocument(DocumentView* document);
    const std::vector<Heading>& headings() const { return m_headings; }
    // From starting until the headings found are shown.
    bool isScanning() const { return m_scanning; }
    QTreeWidget* tree() const { return m_tree; }
    FilterRuleWriter* ruleWriter() const { return m_writer; }
    QString statusText() const;
    // The rule the headings are found with: the AI's for this file, or the built-in one.
    FilterRule rule() const;
    // The chapter under a byte offset, as the AI gets it: its text (up to a limit) and whether
    // that was cut short. -1 for the index when the offset is before the first heading.
    int chapterAt(std::uint64_t offset) const;
    QString chapterText(int index, bool* cut = nullptr) const;

public slots:
    // Asks the AI for a rule that finds this book's headings, or stops it.
    void findWithAi();
    // Goes back to the built-in rule for this file.
    void useBuiltInRule();
    // Sends the chapter being read to the AI; once the headings are found, if they are not yet.
    void askAboutCurrentChapter(ChapterAction action);

signals:
    void chapterAction(ContentsPanel::ChapterAction action, const QString& title, const QString& text, bool cut);
    void aiSettingsRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void scan();
    void scanned();
    void rebuildTree();
    void followReading();
    void activate(QTreeWidgetItem* item);
    void showMenu(const QPoint& pos);
    void ruleWritten(const FilterRule& rule);
    void showStatus(const QString& text);
    void updateButtons();
    QString fileKey() const;
    // The AI wrote the rule the headings are found with, for this file.
    bool hasCustomRule() const;

    QPointer<DocumentView> m_document;
    std::vector<Heading> m_headings;
    QFutureWatcher<std::vector<Heading>> m_watcher;
    bool m_stale = true; // the document changed since its headings were found
    bool m_scanning = false;
    QTimer m_followTimer; // marks the chapter being read, a moment after scrolling
    QTimer m_rescanTimer; // finds the headings again, a moment after a change
    std::optional<FilterRule> m_aiRule; // the AI's, for the document shown
    std::optional<ChapterAction> m_pending; // asked for before the headings were found
    FilterRuleWriter* m_writer = nullptr;
    QString m_aiNote;

    QPushButton* m_ai = nullptr;
    QToolButton* m_builtIn = nullptr;
    QLabel* m_status = nullptr;
    QTreeWidget* m_tree = nullptr;
};

} // namespace aiedit::ui
