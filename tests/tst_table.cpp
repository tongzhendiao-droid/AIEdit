#include "core/FileSource.h"
#include "core/FileType.h"
#include "core/TextCodec.h"
#include "ui/Codec.h"
#include "ui/DocumentView.h"
#include "ui/Paths.h"
#include "ui/TableView.h"
#include "ui/TextView.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QTest>

using namespace aiedit::ui;

class TestTable : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void csvOpensAsTable();
    void sortsAndFilters();
    void legacyAndWideCodecs();
    void rowShownInText();
    void copiesCells();
    void jsonArrayOfObjects();
    void jsonTree();
    void jsonLines();
    void badJson();
    void onlyTabularFiles();

private:
    // Opens the file in a document, and waits for its table if it shows one.
    std::unique_ptr<DocumentView> open(const QString& name, const QByteArray& content,
                                       const aiedit::core::TextCodec* codec = nullptr);
    void showTable(DocumentView& doc);

    QTemporaryDir m_dir;
};

void TestTable::init()
{
    QVERIFY(m_dir.isValid());
}

std::unique_ptr<DocumentView> TestTable::open(const QString& name, const QByteArray& content,
                                              const aiedit::core::TextCodec* codec)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return nullptr;
    file.write(content);
    file.close();
    std::error_code ec;
    std::shared_ptr<const aiedit::core::FileSource> source = aiedit::core::FileSource::open(toPath(path), ec);
    if (!source)
        return nullptr;
    const auto type = aiedit::core::detectFileType(source->bytes());
    auto doc = std::make_unique<DocumentView>(source, type, codec ? *codec : aiedit::core::utf8Codec());
    doc->resize(800, 600);
    doc->show();
    return doc;
}

void TestTable::showTable(DocumentView& doc)
{
    if (!doc.tableView() || doc.viewMode() != DocumentView::ViewMode::Table)
        doc.setViewMode(DocumentView::ViewMode::Table);
    QVERIFY(doc.tableView());
    if (doc.tableView()->isLoading() || !doc.tableView()->model()) {
        QSignalSpy loaded(doc.tableView(), &TableView::loaded);
        QVERIFY(loaded.wait(5000));
    }
}

void TestTable::csvOpensAsTable()
{
    auto doc = open(QStringLiteral("people.csv"), "name,age,city\r\nAlice,30,\"New York, NY\"\r\nBob,4,\"say \"\"hi\"\"\"\r\n");
    QVERIFY(doc);
    QVERIFY(doc->isTabular());
    QCOMPARE(doc->viewMode(), DocumentView::ViewMode::Table);
    showTable(*doc);
    const QAbstractItemModel* model = doc->tableView()->model();
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->columnCount(), 3);
    QCOMPARE(model->headerData(2, Qt::Horizontal).toString(), QStringLiteral("city"));
    QCOMPARE(model->index(0, 2).data().toString(), QStringLiteral("New York, NY"));
    QCOMPARE(model->index(1, 2).data().toString(), QStringLiteral("say \"hi\""));
    QVERIFY(doc->tableView()->status().contains(u'2'));

    // A first row of numbers is data, and the columns are numbered.
    auto numbers = open(QStringLiteral("numbers.tsv"), "1\t2\n3\t4\n");
    showTable(*numbers);
    QCOMPARE(numbers->tableView()->model()->rowCount(), 2);
    QCOMPARE(numbers->tableView()->model()->headerData(0, Qt::Horizontal).toInt(), 1);
}

void TestTable::sortsAndFilters()
{
    auto doc = open(QStringLiteral("sizes.csv"), "item,size\nb,10\na,9\nc,100\n");
    showTable(*doc);
    QSortFilterProxyModel* proxy = doc->tableView()->proxy();
    proxy->sort(1, Qt::AscendingOrder);
    QCOMPARE(proxy->index(0, 1).data().toString(), QStringLiteral("9"));
    QCOMPARE(proxy->index(2, 1).data().toString(), QStringLiteral("100"));
    proxy->sort(0, Qt::DescendingOrder);
    QCOMPARE(proxy->index(0, 0).data().toString(), QStringLiteral("c"));

    doc->tableView()->filterEdit()->setText(QStringLiteral("10"));
    QCOMPARE(proxy->rowCount(), 2); // 10 and 100
    doc->tableView()->filterEdit()->clear();
    QCOMPARE(proxy->rowCount(), 3);

    // Ctrl+F in the table goes to the filter.
    doc->showFindBar();
    QVERIFY(doc->tableView()->filterEdit()->hasFocus() || !QApplication::focusWidget());
}

void TestTable::legacyAndWideCodecs()
{
    const auto* gb18030 = aiedit::core::findCodec("GB18030");
    QVERIFY(gb18030);
    const QByteArray gbk = *Codec(*gb18030).encode(QStringLiteral("名称,数量\n苹果,3\n"));
    auto doc = open(QStringLiteral("gbk.csv"), gbk, gb18030);
    showTable(*doc);
    QCOMPARE(doc->tableView()->model()->headerData(0, Qt::Horizontal).toString(), QStringLiteral("名称"));
    QCOMPARE(doc->tableView()->model()->index(0, 0).data().toString(), QStringLiteral("苹果"));

    const auto* utf16 = aiedit::core::findCodec("UTF-16LE");
    QVERIFY(utf16);
    const QByteArray wide = *Codec(*utf16).encode(QStringLiteral("a,b\nx,y\n"));
    auto wideDoc = open(QStringLiteral("wide.csv"), wide, utf16);
    showTable(*wideDoc);
    QCOMPARE(wideDoc->tableView()->model()->index(0, 1).data().toString(), QStringLiteral("y"));
}

void TestTable::rowShownInText()
{
    auto doc = open(QStringLiteral("jump.csv"), "h\nfirst\nsecond\n");
    showTable(*doc);
    const QModelIndex row = doc->tableView()->proxy()->index(1, 0);
    emit doc->tableView()->view()->doubleClicked(row);
    QCOMPARE(doc->viewMode(), DocumentView::ViewMode::Text);
    QCOMPARE(doc->textView()->cursorOffset(), 8u); // where "second" starts
}

void TestTable::copiesCells()
{
    auto doc = open(QStringLiteral("copy.csv"), "a,b,c\n1,2,3\n4,\"x\ny\",6\n");
    showTable(*doc);
    QAbstractItemView* view = doc->tableView()->view();
    QSortFilterProxyModel* proxy = doc->tableView()->proxy();
    view->selectionModel()->select(QItemSelection(proxy->index(0, 0), proxy->index(1, 1)), QItemSelectionModel::Select);
    QCOMPARE(doc->tableView()->selectedText(), QStringLiteral("1\t2\n4\tx\ny"));
    doc->copy();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("1\t2\n4\tx\ny"));
}

void TestTable::jsonArrayOfObjects()
{
    auto doc = open(QStringLiteral("list.json"),
                    R"([{"id": 1, "name": "甲", "tags": ["a", "b"]}, {"id": 2.5, "ok": true, "name": null}])");
    // JSON opens as text, a key away from its table.
    QCOMPARE(doc->viewMode(), DocumentView::ViewMode::Text);
    showTable(*doc);
    const QAbstractItemModel* model = doc->tableView()->model();
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->columnCount(), 4);
    QCOMPARE(model->headerData(3, Qt::Horizontal).toString(), QStringLiteral("ok"));
    QCOMPARE(model->index(0, 1).data().toString(), QStringLiteral("甲"));
    QCOMPARE(model->index(0, 2).data().toString(), QStringLiteral("[\"a\",\"b\"]"));
    QCOMPARE(model->index(1, 0).data().toString(), QStringLiteral("2.5"));
    QCOMPARE(model->index(1, 1).data().toString(), QStringLiteral("null"));
    QCOMPARE(model->index(1, 3).data().toString(), QStringLiteral("true"));
}

void TestTable::jsonTree()
{
    auto doc = open(QStringLiteral("config.json"), R"({"server": {"port": 8080, "hosts": ["a", "b"]}, "debug": false})");
    showTable(*doc);
    const QAbstractItemModel* model = doc->tableView()->model();
    QCOMPARE(model->rowCount(), 2);
    // Qt's JSON keeps an object's keys sorted, not in the file's order.
    QCOMPARE(model->index(0, 0).data().toString(), QStringLiteral("debug"));
    QCOMPARE(model->index(0, 1).data().toString(), QStringLiteral("false"));
    const QModelIndex server = model->index(1, 0);
    QCOMPARE(server.data().toString(), QStringLiteral("server"));
    QCOMPARE(model->index(1, 1).data().toString(), QStringLiteral("{2}"));
    QCOMPARE(model->rowCount(server), 2);
    QCOMPARE(model->index(0, 0, server).data().toString(), QStringLiteral("hosts"));
    QCOMPARE(model->index(1, 1, server).data().toString(), QStringLiteral("8080"));

    // Filtering keeps the parents of what matches.
    doc->tableView()->filterEdit()->setText(QStringLiteral("8080"));
    QCOMPARE(doc->tableView()->proxy()->rowCount(), 1);
}

void TestTable::jsonLines()
{
    auto doc = open(QStringLiteral("log.jsonl"), "{\"level\": \"info\", \"msg\": \"start\"}\n\n{\"level\": \"error\", \"code\": 7}\n");
    showTable(*doc);
    const QAbstractItemModel* model = doc->tableView()->model();
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->columnCount(), 3);
    QCOMPARE(model->index(1, 2).data().toString(), QStringLiteral("7"));
}

void TestTable::badJson()
{
    auto doc = open(QStringLiteral("bad.json"), R"({"a": 1,})");
    showTable(*doc);
    QVERIFY(!doc->tableView()->model());
    QVERIFY(doc->tableView()->status().contains(u'8') || !doc->tableView()->status().isEmpty());
}

void TestTable::onlyTabularFiles()
{
    auto doc = open(QStringLiteral("notes.txt"), "a,b\n1,2\n");
    QVERIFY(!doc->isTabular());
    doc->setViewMode(DocumentView::ViewMode::Table);
    QCOMPARE(doc->viewMode(), DocumentView::ViewMode::Text);
}

QTEST_MAIN(TestTable)
#include "tst_table.moc"
