#pragma once

#include "ui/SearchJob.h"

#include <QFutureWatcher>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace aiedit::ui {

// The search bar under a document: query, mode and options, plus the background search,
// and a second row for replacing. The document view decides where to search from and what to
// do with a match or a replacement.
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget* parent = nullptr);
    ~FindBar() override;

    SearchMode mode() const;
    void setMode(SearchMode mode);
    QString query() const;
    void setQuery(const QString& query);
    bool caseSensitive() const;
    void setCaseSensitive(bool caseSensitive);
    QString replacement() const;
    void setReplacement(const QString& replacement);
    QString statusText() const;
    void showMessage(const QString& text, bool error = false);

    // Shows the bar and focuses the query, with its text selected. With `replace`, the replace
    // row shows too; without, it stays as it was if the bar was already open.
    void activate(bool replace = false);
    bool isReplaceVisible() const;
    // Off for documents that cannot be edited; `reason` becomes the tooltip.
    void setReplaceAvailable(bool available, const QString& reason = {});
    bool isSearching() const { return m_watcher.isRunning(); }
    // Starts a search, cancelling one still running.
    void start(const SearchRequest& request, const SearchContext& context);
    void cancel();

signals:
    void searchRequested(bool forward);
    void matchFound(std::uint64_t start, std::uint64_t end, bool wrapped);
    void replaceRequested();
    void replaceAllRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void searchFinished();
    void modeChanged();
    void updateReplaceEnabled();

    QLineEdit* m_query = nullptr;
    QComboBox* m_mode = nullptr;
    QCheckBox* m_caseSensitive = nullptr;
    QToolButton* m_previous = nullptr;
    QToolButton* m_next = nullptr;
    QToolButton* m_close = nullptr;
    QLabel* m_status = nullptr;
    QWidget* m_replaceRow = nullptr;
    QLineEdit* m_replacement = nullptr;
    QToolButton* m_replace = nullptr;
    QToolButton* m_replaceAll = nullptr;
    bool m_replaceAvailable = true;
    QString m_replaceUnavailableReason;
    QFutureWatcher<SearchOutcome> m_watcher;
};

} // namespace aiedit::ui
