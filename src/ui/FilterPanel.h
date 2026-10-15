#pragma once

#include "ui/LogFilter.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QList>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace aiedit::ui {

class ConditionEditor;
class FilterResultsView;
class FilterRuleWriter;
class TextDocument;

// Shows only the lines of a document that pass a rule (see LogFilter), under the document
// itself. The rule is a few conditions, typed in or written by the AI from a sentence; either
// way it runs here, over the whole file, and keeps up with a log that grows. Picking a result
// goes to its line.
class FilterPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FilterPanel(TextDocument* document, QWidget* parent = nullptr);
    ~FilterPanel() override;

    // The rule as the editor shows it. Conditions left empty are not part of it; a number test
    // that does not parse is reported in `error`.
    FilterRule rule(QString* error = nullptr) const;
    // Shows a rule in the editor and runs it.
    void setRule(const FilterRule& rule);
    // Runs the rule in the editor; false, with the reason shown, if it cannot run.
    bool apply();

    bool isFiltering() const { return m_watcher.isRunning(); }
    const std::vector<FilterMatch>& matches() const;
    QString statusText() const;
    FilterResultsView* resultsView() const { return m_results; }
    FilterRuleWriter* ruleWriter() const { return m_writer; }
    QLineEdit* descriptionEdit() const { return m_description; }
    int conditionCount() const { return static_cast<int>(m_conditions.size()); }
    // A condition's pattern and number test fields.
    QLineEdit* patternEdit(int condition) const;
    QLineEdit* comparisonEdit(int condition) const;

    void setResultsFont(const QFont& font);
    // Focus where typing makes sense: the first pattern, or the AI field while there is none.
    void focusEditor();

public slots:
    // Asks the AI for a rule from the description field, or stops it while it is writing.
    void writeRule();

signals:
    // Show this 0-based line of the document (a result became current).
    void lineSelected(std::uint64_t line);
    // Go to the line and work there (Enter, double click).
    void lineActivated(std::uint64_t line);
    void closeRequested();
    void aiSettingsRequested();
    void statusMessage(const QString& message);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ConditionEditor* addCondition(const FilterCondition& condition = {});
    void removeCondition(ConditionEditor* editor);
    void ruleEdited();
    void run(const FilterRule& rule, std::uint64_t firstRow = 0);
    void stop();
    void takeResults();
    void filterFinished();
    void sourceChanged();
    void documentChanged();
    void showMessage(const QString& message, bool error = false);
    void updateStatus();
    void updateNotice();
    void updateWriteButton();
    void showPreview();
    void ruleWritten(const FilterRule& rule);
    void writerFailed(const QString& message);

    struct Inbox;

    TextDocument* m_document;
    FilterRuleWriter* m_writer = nullptr;
    QFutureWatcher<void> m_watcher;
    std::shared_ptr<Inbox> m_inbox; // where the running filter puts what it finds
    std::optional<FilterRule> m_shown; // the rule of the results shown, finished or not
    bool m_complete = false;           // the results cover the whole document
    bool m_capped = false;             // stopped at the most results shown
    bool m_stale = false;              // the document changed while hidden
    int m_progress = 0;                // permille, while running
    QString m_message;                 // a problem with the rule
    bool m_messageIsError = false;
    QString m_aiNote;                  // what the AI is doing, or did
    QString m_ruleDescription;         // the AI's few words on the rule
    // What the last complete run saw, to only read what was appended when a log grows.
    std::uint64_t m_scannedSize = 0;
    QByteArray m_scannedTail;
    int m_scannedLineBreak = 0;
    std::uint64_t m_lastLineRow = 0; // first row of the last line, which may go on
    std::uint64_t m_lastLine = 0;
    QTimer m_runTimer; // runs the rule a moment after it was typed

    QLineEdit* m_description = nullptr;
    QPushButton* m_write = nullptr;
    QToolButton* m_preview = nullptr;
    QToolButton* m_close = nullptr;
    QLabel* m_notice = nullptr;
    QVBoxLayout* m_conditionsLayout = nullptr;
    QList<ConditionEditor*> m_conditions;
    QToolButton* m_add = nullptr;
    QCheckBox* m_any = nullptr;
    QLabel* m_status = nullptr;
    FilterResultsView* m_results = nullptr;
};

} // namespace aiedit::ui
