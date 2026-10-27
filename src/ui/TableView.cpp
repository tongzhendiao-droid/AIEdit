#include "ui/TableView.h"

#include "core/Csv.h"
#include "core/FileSource.h"
#include "ui/Codec.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace aiedit::ui {

namespace {

// JSON is read whole; larger files are shown as text only.
constexpr std::uint64_t kMaxJson = 64ull * 1024 * 1024;
constexpr int kMaxJsonColumns = 500;
constexpr int kMaxJsonNodes = 1'000'000;
// Sorting more rows than this would keep the window busy too long.
constexpr int kMaxSortedRows = 200'000;
constexpr int kMaxColumnWidth = 400;

// Rows of a CSV file, their cells decoded when shown.
class CsvModel : public QAbstractTableModel
{
public:
    CsvModel(std::shared_ptr<const core::FileSource> source, std::vector<std::uint8_t> converted,
             const core::TextCodec& codec, core::CsvTable table)
        : m_source(std::move(source)), m_converted(std::move(converted)), m_codec(codec), m_table(std::move(table))
    {
        m_header = looksLikeHeader();
    }

    bool hasOffsets() const { return m_converted.empty(); }
    bool truncated() const { return m_table.truncated; }

    int rowCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_table.rowCount()) - (m_header ? 1 : 0);
    }
    int columnCount(const QModelIndex& parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_table.columns);
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid())
            return {};
        const std::size_t row = static_cast<std::size_t>(index.row()) + (m_header ? 1 : 0);
        if (role == Qt::UserRole)
            return hasOffsets() ? QVariant::fromValue<qulonglong>(m_table.rowOffsets[row]) : QVariant();
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        QString text = cell(row, static_cast<std::size_t>(index.column()));
        if (role == Qt::ToolTipRole)
            return text.size() > 40 || text.contains(u'\n') ? text.left(2000) : QVariant();
        text.replace(u'\n', QStringLiteral(" ⏎ "));
        return text;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole)
            return {};
        if (orientation == Qt::Vertical)
            return section + 1;
        if (m_header) {
            const QString name = cell(0, static_cast<std::size_t>(section));
            if (!name.isEmpty())
                return name;
        }
        return section + 1;
    }

private:
    std::span<const std::uint8_t> bytes() const
    {
        return m_converted.empty() ? m_source->bytes() : std::span<const std::uint8_t>(m_converted);
    }

    QString cell(std::size_t row, std::size_t column) const
    {
        const auto cells = m_table.row(row);
        if (column >= cells.size())
            return {};
        const std::string raw = core::csvCellBytes(bytes(), cells[column]);
        return m_codec.decode(QByteArrayView(raw.data(), static_cast<qsizetype>(raw.size())));
    }

    // A first row of names: every cell filled and none a number, under rows that are not all
    // like that.
    bool looksLikeHeader() const
    {
        if (m_table.rowCount() < 2)
            return false;
        auto isNumber = [](const QString& s) {
            bool ok = false;
            s.trimmed().toDouble(&ok);
            return ok;
        };
        const auto first = m_table.row(0);
        for (std::size_t c = 0; c < first.size(); ++c) {
            const QString text = cell(0, c);
            if (text.trimmed().isEmpty() || isNumber(text))
                return false;
        }
        return true;
    }

    std::shared_ptr<const core::FileSource> m_source;
    std::vector<std::uint8_t> m_converted; // the text as UTF-8, when the file's codec is not ASCII-based
    Codec m_codec;
    core::CsvTable m_table;
    bool m_header = false;
};

// Numbers as numbers, everything else as the locale sorts text.
class SortProxy : public QSortFilterProxyModel
{
protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        const QString a = left.data().toString().trimmed();
        const QString b = right.data().toString().trimmed();
        bool aNumber = false;
        bool bNumber = false;
        const double x = a.toDouble(&aNumber);
        const double y = b.toDouble(&bNumber);
        if (aNumber && bNumber)
            return x < y;
        if (aNumber != bNumber)
            return aNumber; // numbers before text
        return QString::localeAwareCompare(a, b) < 0;
    }
};

QString jsonText(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Null:
        return QStringLiteral("null");
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: {
        const double v = value.toDouble();
        if (std::floor(v) == v && std::abs(v) < 1e15)
            return QString::number(static_cast<qlonglong>(v));
        return QString::number(v, 'g', 15);
    }
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Array:
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)).left(500);
    case QJsonValue::Object:
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)).left(500);
    case QJsonValue::Undefined:
        break;
    }
    return {};
}

QString jsonType(const QJsonValue& value)
{
    switch (value.type()) {
    case QJsonValue::Null:
        return QStringLiteral("null");
    case QJsonValue::Bool:
        return QCoreApplication::translate("TableView", "boolean");
    case QJsonValue::Double:
        return QCoreApplication::translate("TableView", "number");
    case QJsonValue::String:
        return QCoreApplication::translate("TableView", "text");
    case QJsonValue::Array:
        return QCoreApplication::translate("TableView", "array");
    case QJsonValue::Object:
        return QCoreApplication::translate("TableView", "object");
    case QJsonValue::Undefined:
        break;
    }
    return {};
}

QStandardItem* readOnly(const QString& text)
{
    auto* item = new QStandardItem(text);
    item->setEditable(false);
    return item;
}

} // namespace

struct TableView::Result
{
    std::unique_ptr<QAbstractItemModel> model;
    bool tree = false;
    QString status;
    bool canSort = true;
};

namespace {

// Objects as rows, a column for each key in the order first met.
std::unique_ptr<QStandardItemModel> objectTable(const QList<QJsonObject>& objects, bool* truncated)
{
    QStringList keys;
    QHash<QString, int> column;
    for (const QJsonObject& object : objects) {
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!column.contains(it.key()) && keys.size() < kMaxJsonColumns) {
                column.insert(it.key(), static_cast<int>(keys.size()));
                keys.append(it.key());
            } else if (!column.contains(it.key())) {
                *truncated = true;
            }
        }
    }
    auto model = std::make_unique<QStandardItemModel>(static_cast<int>(objects.size()), static_cast<int>(keys.size()));
    model->setHorizontalHeaderLabels(keys);
    for (int r = 0; r < objects.size(); ++r) {
        for (auto it = objects[r].begin(); it != objects[r].end(); ++it) {
            if (const auto c = column.find(it.key()); c != column.end())
                model->setItem(r, *c, readOnly(jsonText(it.value())));
        }
    }
    return model;
}

void addTreeNode(QStandardItem* parent, const QString& name, const QJsonValue& value, int* nodes)
{
    if (++*nodes > kMaxJsonNodes)
        return;
    QString shown = jsonText(value);
    if (value.isArray())
        shown = QStringLiteral("[%L1]").arg(value.toArray().size());
    else if (value.isObject())
        shown = QStringLiteral("{%L1}").arg(value.toObject().size());
    QList<QStandardItem*> row = {readOnly(name), readOnly(shown), readOnly(jsonType(value))};
    parent->appendRow(row);
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (qsizetype i = 0; i < array.size(); ++i)
            addTreeNode(row.front(), QStringLiteral("[%1]").arg(i), array[i], nodes);
    } else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            addTreeNode(row.front(), it.key(), it.value(), nodes);
    }
}

std::shared_ptr<TableView::Result> readJson(const QByteArray& utf8, bool lines)
{
    auto result = std::make_shared<TableView::Result>();
    QList<QJsonValue> values;
    if (lines) {
        int lineNumber = 0;
        for (const QByteArray& line : utf8.split('\n')) {
            ++lineNumber;
            if (line.trimmed().isEmpty())
                continue;
            QJsonParseError error;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError) {
                result->status = QCoreApplication::translate("TableView", "Line %1 is not JSON: %2").arg(lineNumber).arg(error.errorString());
                return result;
            }
            values.append(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()));
        }
    } else {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(utf8, &error);
        if (error.error != QJsonParseError::NoError) {
            result->status = QCoreApplication::translate("TableView", "This JSON has an error at character %1: %2")
                                 .arg(error.offset)
                                 .arg(error.errorString());
            return result;
        }
        if (doc.isArray()) {
            for (const QJsonValue& v : doc.array())
                values.append(v);
        } else {
            // One object: a tree.
            auto model = std::make_unique<QStandardItemModel>(0, 3);
            model->setHorizontalHeaderLabels({QCoreApplication::translate("TableView", "Name"), QCoreApplication::translate("TableView", "Value"), QCoreApplication::translate("TableView", "Type")});
            int nodes = 0;
            const QJsonObject object = doc.object();
            for (auto it = object.begin(); it != object.end(); ++it)
                addTreeNode(model->invisibleRootItem(), it.key(), it.value(), &nodes);
            result->status = nodes > kMaxJsonNodes ? QCoreApplication::translate("TableView", "The first %L1 values").arg(kMaxJsonNodes)
                                                   : QCoreApplication::translate("TableView", "%L1 values").arg(nodes);
            result->model = std::move(model);
            result->tree = true;
            return result;
        }
    }

    const bool allObjects =
        !values.isEmpty() && std::all_of(values.cbegin(), values.cend(), [](const QJsonValue& v) { return v.isObject(); });
    if (allObjects) {
        QList<QJsonObject> objects;
        objects.reserve(values.size());
        for (const QJsonValue& v : std::as_const(values))
            objects.append(v.toObject());
        bool truncated = false;
        result->model = objectTable(objects, &truncated);
        result->status = QCoreApplication::translate("TableView", "%L1 rows, %L2 columns")
                             .arg(result->model->rowCount())
                             .arg(result->model->columnCount());
        if (truncated)
            result->status += QLatin1Char(' ') + QCoreApplication::translate("TableView", "(only the first %L1 keys are columns)").arg(kMaxJsonColumns);
        result->canSort = result->model->rowCount() <= kMaxSortedRows;
        return result;
    }
    // Anything else in an array: a tree of its items.
    auto model = std::make_unique<QStandardItemModel>(0, 3);
    model->setHorizontalHeaderLabels({QCoreApplication::translate("TableView", "Name"), QCoreApplication::translate("TableView", "Value"), QCoreApplication::translate("TableView", "Type")});
    int nodes = 0;
    for (qsizetype i = 0; i < values.size(); ++i)
        addTreeNode(model->invisibleRootItem(), QStringLiteral("[%1]").arg(i), values[i], &nodes);
    result->status = QCoreApplication::translate("TableView", "%L1 values").arg(std::min(nodes, kMaxJsonNodes));
    result->model = std::move(model);
    result->tree = true;
    return result;
}

} // namespace

std::optional<TableView::Format> TableView::formatForName(const QString& fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == u"csv" || suffix == u"tsv" || suffix == u"tab")
        return Format::Csv;
    if (suffix == u"json" || suffix == u"geojson")
        return Format::Json;
    if (suffix == u"jsonl" || suffix == u"ndjson")
        return Format::JsonLines;
    return std::nullopt;
}

TableView::TableView(QWidget* parent) : QWidget(parent)
{
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter rows"));
    m_filter->setClearButtonEnabled(true);
    m_status = new QLabel(this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_proxy = new SortProxy;
    m_proxy->setParent(this);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);
    m_proxy->setRecursiveFilteringEnabled(true);
    m_proxy->setSortRole(Qt::DisplayRole);
    connect(m_filter, &QLineEdit::textChanged, m_proxy, &QSortFilterProxyModel::setFilterFixedString);

    m_table = new QTableView(this);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setWordWrap(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_table->horizontalHeader()->setResizeContentsPrecision(100);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_tree = new QTreeView(this);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setUniformRowHeights(true);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_table);
    m_stack->addWidget(m_tree);
    for (QAbstractItemView* view : {static_cast<QAbstractItemView*>(m_table), static_cast<QAbstractItemView*>(m_tree)})
        view->installEventFilter(this);

    connect(m_table, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
        const QVariant offset = m_proxy->mapToSource(index).data(Qt::UserRole);
        if (offset.isValid())
            emit rowActivated(offset.toULongLong());
    });

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 4, 4, 4);
    bar->addWidget(m_filter, 1);
    bar->addWidget(m_status);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_stack, 1);

    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<Result>>::finished, this, &TableView::showResult);
    setFocusProxy(m_table);
}

TableView::~TableView()
{
    if (m_cancel)
        m_cancel->store(true);
    m_watcher.waitForFinished();
}

void TableView::setSource(std::shared_ptr<const core::FileSource> source, const core::TextCodec& codec, Format format)
{
    if (m_cancel)
        m_cancel->store(true);
    m_watcher.waitForFinished();
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_status->setText(tr("Reading…"));
    // What the worker needs, copied: the codec by value, the bytes kept alive by the source.
    const core::TextCodec textCodec = codec;
    const bool asciiBased = codec.scheme != core::CharScheme::Utf16LE && codec.scheme != core::CharScheme::Utf16BE &&
                            codec.scheme != core::CharScheme::Utf32LE && codec.scheme != core::CharScheme::Utf32BE;
    auto task = [source = std::move(source), textCodec, asciiBased, format,
                 cancel = m_cancel]() -> std::shared_ptr<Result> {
        auto result = std::make_shared<Result>();
        const auto bytes = source->bytes();
        if (format == Format::Csv) {
            std::vector<std::uint8_t> converted;
            core::TextCodec shownCodec = textCodec;
            if (!asciiBased) {
                // UTF-16 and UTF-32 have zero bytes in every character: read as UTF-8.
                const QByteArray utf8 =
                    Codec(textCodec)
                        .decode(QByteArrayView(reinterpret_cast<const char*>(bytes.data()),
                                               static_cast<qsizetype>(bytes.size())))
                        .toUtf8();
                converted.assign(utf8.begin(), utf8.end());
                shownCodec = core::utf8Codec();
            }
            const auto csv = converted.empty() ? bytes : std::span<const std::uint8_t>(converted);
            core::CsvTable table = core::readCsv(csv, core::detectCsvSeparator(csv), 2'000'000, 40'000'000, cancel.get());
            auto model = std::make_unique<CsvModel>(source, std::move(converted), shownCodec, std::move(table));
            result->status = QCoreApplication::translate("TableView", "%L1 rows, %L2 columns").arg(model->rowCount()).arg(model->columnCount());
            if (model->truncated())
                result->status += QLatin1Char(' ') + QCoreApplication::translate("TableView", "(the rest of the file is left out)");
            result->canSort = model->rowCount() <= kMaxSortedRows;
            result->model = std::move(model);
        } else if (bytes.size() > kMaxJson) {
            result->status = QCoreApplication::translate("TableView", "Too large to show as a table");
        } else {
            const QString text = Codec(textCodec).decode(
                QByteArrayView(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
            result = readJson(text.toUtf8(), format == Format::JsonLines);
        }
        // Made here, used on the window's thread.
        if (result->model)
            result->model->moveToThread(QCoreApplication::instance()->thread());
        return result;
    };
    m_watcher.setFuture(QtConcurrent::run(std::move(task)));
}

void TableView::showResult()
{
    if (m_watcher.isCanceled() || (m_cancel && m_cancel->load()))
        return;
    const std::shared_ptr<Result> result = m_watcher.result();
    QAbstractItemView* view = result->tree ? static_cast<QAbstractItemView*>(m_tree) : m_table;
    m_table->setSortingEnabled(false);
    m_tree->setSortingEnabled(false);
    m_proxy->setSourceModel(result->model.get());
    m_model = std::move(result->model); // after the proxy lets go of the old one
    m_table->setModel(result->tree ? nullptr : m_proxy);
    m_tree->setModel(result->tree ? m_proxy : nullptr);
    m_stack->setCurrentWidget(view);
    setFocusProxy(view);
    if (m_model) {
        m_proxy->sort(-1); // the file's order until a header is clicked
        if (result->canSort) {
            m_table->setSortingEnabled(!result->tree);
            m_tree->setSortingEnabled(result->tree);
            m_table->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
            m_tree->header()->setSortIndicator(-1, Qt::AscendingOrder);
        }
        if (result->tree) {
            m_tree->expandToDepth(0);
            m_tree->resizeColumnToContents(0);
        } else {
            m_table->resizeColumnsToContents();
            for (int c = 0; c < m_table->model()->columnCount(); ++c)
                m_table->setColumnWidth(c, std::min(m_table->columnWidth(c), kMaxColumnWidth));
        }
    }
    m_status->setText(result->status);
    emit loaded();
}

QAbstractItemView* TableView::view() const
{
    return static_cast<QAbstractItemView*>(m_stack->currentWidget());
}

QString TableView::status() const
{
    return m_status->text();
}

QString TableView::selectedText() const
{
    const QAbstractItemView* current = view();
    if (!current->selectionModel())
        return {};
    QModelIndexList indexes = current->selectionModel()->selectedIndexes();
    std::sort(indexes.begin(), indexes.end(), [](const QModelIndex& a, const QModelIndex& b) {
        return a.row() != b.row() ? a.row() < b.row() : a.column() < b.column();
    });
    QString text;
    int row = -1;
    for (const QModelIndex& index : std::as_const(indexes)) {
        if (row != -1)
            text += index.row() == row ? u'\t' : u'\n';
        row = index.row();
        // The whole text, line breaks and all, not the shortened display.
        const QVariant tip = index.data(Qt::ToolTipRole);
        text += tip.isValid() ? tip.toString() : index.data().toString();
    }
    return text;
}

void TableView::copy()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

bool TableView::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->matches(QKeySequence::Copy)) {
        copy();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace aiedit::ui
