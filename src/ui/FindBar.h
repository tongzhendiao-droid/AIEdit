#pragma once

#include "ui/SearchJob.h"

#include <QFutureWatcher>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace cppedit::ui {

// The search bar under a document: query, mode and options, plus the background search.
// The document view decides where to search from and what to do with a match.
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
    QString statusText() const;

    // Shows the bar and focuses the query, with its text selected.
    void activate();
    bool isSearching() const { return m_watcher.isRunning(); }
    // Starts a search, cancelling one still running.
    void start(const SearchRequest& request, const SearchContext& context);
    void cancel();

signals:
    void searchRequested(bool forward);
    void matchFound(std::uint64_t start, std::uint64_t end, bool wrapped);
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void searchFinished();
    void modeChanged();
    void showStatus(const QString& text, bool error = false);

    QLineEdit* m_query = nullptr;
    QComboBox* m_mode = nullptr;
    QCheckBox* m_caseSensitive = nullptr;
    QToolButton* m_previous = nullptr;
    QToolButton* m_next = nullptr;
    QToolButton* m_close = nullptr;
    QLabel* m_status = nullptr;
    QFutureWatcher<SearchOutcome> m_watcher;
};

} // namespace cppedit::ui
