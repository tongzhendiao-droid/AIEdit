#pragma once

#include "ui/FileSearch.h"

#include <QFutureWatcher>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string_view>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace aiedit::ui {

// Find in Files: the lines of every file under a folder that contain a text, match a regular
// expression, or pass the filter rule used last, grouped by file. The files are searched in
// the background; picking a line opens its file there.
class FindInFilesPanel : public QWidget
{
    Q_OBJECT

public:
    enum class Mode
    {
        Text,
        Regex,
        FilterRule, // the rule the filter panel ran last
    };

    explicit FindInFilesPanel(QWidget* parent = nullptr);
    ~FindInFilesPanel() override;

    QString folder() const;
    void setFolder(const QString& folder);
    void setQuery(const QString& query);
    void setMode(Mode mode);
    void setNameFilters(const QString& filters);
    void setLegacyCodec(std::string_view codec) { m_legacyCodec = codec; }
    // Puts the cursor in the query, with what it holds selected.
    void focusQuery();

    // From starting until the last results are shown.
    bool isSearching() const { return m_inbox != nullptr; }
    QTreeWidget* results() const { return m_results; }
    QString statusText() const;

public slots:
    // Searches with what the fields hold, or stops the search under way.
    void search();
    void stop();

signals:
    // Open this file at this 0-based line (which starts at `offset`).
    void openRequested(const QString& path, std::uint64_t line, std::uint64_t offset);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Inbox;

    void modeChanged();
    void browse();
    void takeResults();
    void searchFinished();
    void showStatus(const QString& text, bool error = false);
    void updateButton();
    void activate(QTreeWidgetItem* item);
    void saveSettings() const;

    QLineEdit* m_query = nullptr;
    QComboBox* m_mode = nullptr;
    QCheckBox* m_caseSensitive = nullptr;
    QPushButton* m_search = nullptr;
    QLineEdit* m_folder = nullptr;
    QToolButton* m_browse = nullptr;
    QLineEdit* m_filters = nullptr;
    QCheckBox* m_subfolders = nullptr;
    QLabel* m_status = nullptr;
    QTreeWidget* m_results = nullptr;

    QFutureWatcher<FileSearchSummary> m_watcher;
    std::shared_ptr<Inbox> m_inbox;
    QString m_searchedFolder; // results are shown relative to it
    std::string_view m_legacyCodec = "windows-1252";
    std::size_t m_matches = 0;
    int m_files = 0;
};

} // namespace aiedit::ui
