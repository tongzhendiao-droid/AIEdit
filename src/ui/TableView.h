#pragma once

#include "core/TextCodec.h"

#include <QFutureWatcher>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

class QAbstractItemModel;
class QAbstractItemView;
class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;
class QTreeView;

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

// CSV, TSV and JSON shown as a table (M10): a CSV row is a row; JSON that is an array of
// objects, or JSON Lines, has a column for each key; other JSON is a tree. Read in the
// background; columns sort by clicking their header (numbers as numbers), and the box above
// keeps the rows that contain what is typed.
class TableView : public QWidget
{
    Q_OBJECT

public:
    enum class Format
    {
        Csv,       // the separator found from the content
        Json,      // one value
        JsonLines, // a value on each line
    };
    // What a file is shown as a table as, by its name; nothing for other files.
    static std::optional<Format> formatForName(const QString& fileName);

    explicit TableView(QWidget* parent = nullptr);
    ~TableView() override;

    // Reads the table from the bytes, which are text in `codec`, replacing what was shown.
    void setSource(std::shared_ptr<const core::FileSource> source, const core::TextCodec& codec, Format format);
    bool isLoading() const { return m_watcher.isRunning(); }
    // The table or tree on screen, and what it shows (before sorting and filtering).
    QAbstractItemView* view() const;
    QAbstractItemModel* model() const { return m_model.get(); }
    QSortFilterProxyModel* proxy() const { return m_proxy; }
    QLineEdit* filterEdit() const { return m_filter; }
    // How many rows, or why there is no table.
    QString status() const;
    // The selected cells, a tab between cells and a line break between rows.
    QString selectedText() const;
    void copy();

signals:
    // A row was double-clicked; it starts at this offset of the bytes (CSV only).
    void rowActivated(std::uint64_t offset);
    void loaded();

public:
    // What reading the bytes made of them, handed from the worker thread.
    struct Result;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showResult();

    QLineEdit* m_filter = nullptr;
    QLabel* m_status = nullptr;
    QStackedWidget* m_stack = nullptr;
    QTableView* m_table = nullptr;
    QTreeView* m_tree = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    std::unique_ptr<QAbstractItemModel> m_model;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QFutureWatcher<std::shared_ptr<Result>> m_watcher;
};

} // namespace aiedit::ui
