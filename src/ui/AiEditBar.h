#pragma once

#include <QList>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QToolButton;

namespace aiedit::ui {

// Above a document (Ctrl+I): what the AI is to change in it, and how far it has got. Enter sends,
// Esc closes; while the AI writes, Stop stops it.
class AiEditBar : public QWidget
{
    Q_OBJECT

public:
    explicit AiEditBar(QWidget* parent = nullptr);

    QLineEdit* input() const { return m_input; }
    QString instruction() const;
    // What will be changed: the selection, or the whole document.
    void setScope(const QString& scope);
    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }
    // Under the line: how far the AI has got, or what went wrong.
    void showMessage(const QString& text, bool error = false);
    // Shows the bar and puts the cursor in the line, its text selected.
    void activate();
    // Requests asked for often, under the line: clicked, one is sent as it is.
    struct Suggestion
    {
        QString label;
        QString instruction;
    };
    void setSuggestions(const QList<Suggestion>& suggestions);
    const QList<Suggestion>& suggestions() const { return m_suggestions; }

signals:
    void editRequested(const QString& instruction);
    void previewRequested(const QString& instruction);
    void stopRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void send();
    void updateButtons();

    QLabel* m_scope = nullptr;
    QLineEdit* m_input = nullptr;
    QToolButton* m_send = nullptr;
    QToolButton* m_stop = nullptr;
    QToolButton* m_preview = nullptr;
    QToolButton* m_close = nullptr;
    QLabel* m_message = nullptr;
    QWidget* m_suggestionRow = nullptr;
    QHBoxLayout* m_suggestionLayout = nullptr;
    QList<Suggestion> m_suggestions;
    bool m_busy = false;
};

} // namespace aiedit::ui
