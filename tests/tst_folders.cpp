#include "ui/AiPanel.h"
#include "ui/DocumentView.h"
#include "ui/FileTypeSniffer.h"
#include "ui/FolderModels.h"
#include "ui/FolderPanel.h"
#include "ui/FolderSummary.h"
#include "ui/MainWindow.h"
#include "ui/Paths.h"
#include "ui/TextView.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringEncoder>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

#include "MockChatServer.h"

#include <fstream>
#include <memory>

using namespace aiedit;

namespace {

const QByteArray kPng("\x89PNG\r\n\x1A\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x10\x08\x06\0\0\0", 33);
const QByteArray kJpeg("\xFF\xD8\xFF\xE0\0\x10JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 20);

QList<QByteArray> answer(const QString& text)
{
    return {aiedit::test::chunk(text), aiedit::test::chunk({}, QStringLiteral("content"), QStringLiteral("stop")),
            "data: [DONE]\n\n"};
}

QString canonical(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

QAction* findAction(const QWidget& window, const QString& text)
{
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == text)
            return action;
    return nullptr;
}

QStringList recentFiles()
{
    QStringList paths;
    for (const QVariant& value : QSettings().value(QStringLiteral("files/recent")).toList())
        paths << value.toMap().value(QStringLiteral("path")).toString();
    return paths;
}

// Takes back what a test put in the trash: the file and what the trash knows of it.
void removeFromTrash(const QString& pathInTrash)
{
    const QFileInfo trashed(pathInTrash);
    if (trashed.isDir())
        QDir(pathInTrash).removeRecursively();
    else
        QFile::remove(pathInTrash);
    QDir info(trashed.absolutePath());
    if (info.cdUp() && info.cd(QStringLiteral("info")))
        QFile::remove(info.filePath(trashed.fileName() + QStringLiteral(".trashinfo")));
}

} // namespace

class TestFolders : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void sniffsTypesFromContent();
    void filtersNames_data();
    void filtersNames();
    void listsMatchingFilesInOrder();
    void leavesHiddenFilesOut();
    void summarizesAFolder();

    void showsAFolder();
    void offersWhatFitsTheSelection();
    void previewsWhatTheUserMovesTo();
    void filterListsTheWholeFolder();
    void makesRenamesAndTrashesFiles();
    void remembersRecentFolders();

    void previewsInOneTab();
    void keepsAPreviewThatIsEdited();
    void previewsDoNotCrowdRecentFiles();
    void renamedFilesStayOpen();
    void keepsUnsavedFilesWhereTheyAre();
    void closesTrashedFiles();
    void opensFoldersFromThePath();
    void restoresTheFolder();
    void asksTheAiAboutAFolder();

private:
    QString write(const QString& relative, const QByteArray& content);
    QString root() const { return canonical(m_dir->path()); }
    // A small experiment: notes, settings, results in two runs, a log and two images, one of
    // them a JPEG named .png.
    void makeExperiment();

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestFolders::initTestCase()
{
    // Settings and the trash go to a test location, away from the user's own.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("AIEditTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_folders"));
}

void TestFolders::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QSettings().clear();
}

QString TestFolders::write(const QString& relative, const QByteArray& content)
{
    const QString path = QDir(root()).filePath(relative);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
    return path;
}

void TestFolders::makeExperiment()
{
    write(QStringLiteral("README.md"), "# Experiment 42\nTraining runs for the model.\n");
    write(QStringLiteral("config.yaml"), "lr: 0.001\nepochs: 10\n");
    write(QStringLiteral("data/run1/metrics.csv"), "step,loss\n1,0.9\n2,0.7\n");
    write(QStringLiteral("data/run2/metrics.csv"), "step,loss\n1,0.8\n2,0.5\n");
    write(QStringLiteral("logs/train.log"), "2026-10-16 12:00:00 INFO started\n2026-10-16 12:05:00 INFO done\n");
    write(QStringLiteral("images/plot.png"), kPng);
    write(QStringLiteral("images/fake.png"), kJpeg);
}

void TestFolders::sniffsTypesFromContent()
{
    const QString png = write(QStringLiteral("photo.png"), kPng);
    const QString fake = write(QStringLiteral("fake.png"), kJpeg);
    const QString page = write(QStringLiteral("page.png"), "<!DOCTYPE html><html><body>Not Found</body></html>\n");
    const QString utf8 = write(QStringLiteral("notes.txt"), "天下大势，分久必合，合久必分。\n");
    QString chinese;
    for (int i = 0; i < 20; ++i)
        chinese += QStringLiteral("话说天下大势，分久必合，合久必分。周末七国分争，并入于秦。\n");
    const QString legacy = write(QStringLiteral("old.txt"), QStringEncoder("GB18030").encode(chinese));
    const QString empty = write(QStringLiteral("empty.log"), {});

    using ui::FileTypeSniffer;
    QCOMPARE(FileTypeSniffer::sniff(png, "GB18030").label(), QStringLiteral("PNG"));
    QVERIFY(FileTypeSniffer::sniff(png, "GB18030").mismatch.empty());
    const ui::SniffedType jpeg = FileTypeSniffer::sniff(fake, "GB18030");
    QCOMPARE(jpeg.label(), QStringLiteral("JPEG"));
    QCOMPARE(jpeg.mismatchNote(QStringLiteral("fake.png")), QStringLiteral("The name says PNG, but this is JPEG"));
    QCOMPARE(FileTypeSniffer::sniff(page, "GB18030").mismatchNote(QStringLiteral("page.png")),
             QStringLiteral("The name says PNG, but this is text"));
    QCOMPARE(FileTypeSniffer::sniff(utf8, "GB18030").label(), QStringLiteral("Text · UTF-8"));
    QCOMPARE(FileTypeSniffer::sniff(legacy, "GB18030").label(), QStringLiteral("Text · GB18030"));
    QCOMPARE(FileTypeSniffer::sniff(empty, "GB18030").label(), QStringLiteral("Empty"));
    QVERIFY(FileTypeSniffer::sniff(m_dir->filePath(QStringLiteral("missing")), "GB18030").unreadable);

    // In the background, remembered while the file stays the same.
    FileTypeSniffer sniffer;
    QSignalSpy found(&sniffer, &FileTypeSniffer::found);
    QVERIFY(!sniffer.type(QFileInfo(png)));
    QTRY_COMPARE(found.count(), 1);
    QCOMPARE(sniffer.type(QFileInfo(png))->label(), QStringLiteral("PNG"));
    QVERIFY(!sniffer.type(QFileInfo(root()))); // folders are not read

    write(QStringLiteral("photo.png"), "now it is text\n");
    QVERIFY(!sniffer.type(QFileInfo(png))); // looked at again
    QTRY_COMPARE(found.count(), 2);
    QCOMPARE(sniffer.type(QFileInfo(png))->label(), QStringLiteral("Text · ASCII"));
}

void TestFolders::filtersNames_data()
{
    QTest::addColumn<QString>("filter");
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("matches");

    QTest::newRow("wildcard") << "*.log" << "logs/app.log" << true;
    QTest::newRow("wildcard, other") << "*.log" << "logs/app.txt" << false;
    QTest::newRow("part of the name") << "error" << "out/Error-2026.txt" << true;
    QTest::newRow("not the folder's name") << "out" << "out/Error-2026.txt" << false;
    QTest::newRow("several") << "*.log *.gz" << "old.gz" << true;
    QTest::newRow("commas") << "2026,*.csv" << "data.csv" << true;
    QTest::newRow("path") << "logs/*.gz" << "logs/old.gz" << true;
    QTest::newRow("path, elsewhere") << "logs/*.gz" << "old.gz" << false;
    QTest::newRow("part of the path") << "run1/" << "data/run1/metrics.csv" << true;
    QTest::newRow("empty") << "" << "anything" << true;
}

void TestFolders::filtersNames()
{
    QFETCH(QString, filter);
    QFETCH(QString, path);
    QFETCH(bool, matches);
    const ui::NameFilter name(filter);
    QCOMPARE(name.isEmpty(), filter.isEmpty());
    QCOMPARE(name.matches(path, path.section(QLatin1Char('/'), -1)), matches);
}

void TestFolders::listsMatchingFilesInOrder()
{
    for (const char* name :
         {"b.log", "a.log", "notes.txt", "run2/x.log", "run10/x.log", "run1/x.log", "run1/deep/y.log"})
        write(QString::fromLatin1(name), "line\n");

    QStringList listed;
    ui::FileListHooks hooks;
    hooks.found = [&listed](std::vector<ui::ListedFile>&& files, int) {
        for (const ui::ListedFile& file : files)
            listed << file.relativePath;
    };
    const ui::FileListSummary summary = ui::listFiles(root(), ui::NameFilter(QStringLiteral("*.log")), false, hooks);
    // A folder's files, then its folders, in natural order.
    QCOMPARE(listed, (QStringList{"a.log", "b.log", "run1/x.log", "run1/deep/y.log", "run2/x.log", "run10/x.log"}));
    QCOMPARE(summary.matched, 6);
    QVERIFY(!summary.limited);
    QCOMPARE(summary.visited, 11); // 7 files and 4 folders

    listed.clear();
    const ui::FileListSummary first =
        ui::listFiles(root(), ui::NameFilter(QStringLiteral("*.log")), false, hooks, 2);
    QCOMPARE(listed, (QStringList{"a.log", "b.log"}));
    QVERIFY(first.limited);
}

void TestFolders::leavesHiddenFilesOut()
{
    write(QStringLiteral(".hidden/a.log"), "line\n");
    write(QStringLiteral(".b.log"), "line\n");
    write(QStringLiteral("c.log"), "line\n");
    QStringList listed;
    ui::FileListHooks hooks;
    hooks.found = [&listed](std::vector<ui::ListedFile>&& files, int) {
        for (const ui::ListedFile& file : files)
            listed << file.relativePath;
    };
    ui::listFiles(root(), ui::NameFilter(QStringLiteral("*.log")), false, hooks);
    QCOMPARE(listed, QStringList{QStringLiteral("c.log")});
    listed.clear();
    ui::listFiles(root(), ui::NameFilter(QStringLiteral("*.log")), true, hooks);
    QCOMPARE(listed.size(), 3);
}

void TestFolders::summarizesAFolder()
{
    makeExperiment();
    const QString summary = ui::summarizeFolder(root(), "GB18030", false);
    QVERIFY2(summary.contains(QStringLiteral("Looked at: 7 files and 5 folders")), qPrintable(summary));
    QVERIFY(summary.contains(QStringLiteral("- data/ (2 files, 2 folders")));
    QVERIFY(summary.contains(QStringLiteral("  - data/run1/ (1 file, ")));
    QVERIFY(summary.contains(QStringLiteral("- README.md (")));
    QVERIFY(summary.contains(QStringLiteral("- .csv: 2 files")));
    QVERIFY(summary.contains(QStringLiteral("- PNG: 1 file, ")));
    QVERIFY(summary.contains(QStringLiteral("- JPEG: 1 file, ")));
    QVERIFY(summary.contains(QStringLiteral("- images/fake.png: the name says PNG, but the content is JPEG")));
    // The README comes first among the files whose start is shown.
    const qsizetype readme = summary.indexOf(QStringLiteral("--- README.md"));
    QVERIFY(readme > 0);
    QVERIFY(summary.indexOf(QStringLiteral("--- config.yaml")) > readme);
    QVERIFY(summary.contains(QStringLiteral("Training runs for the model.")));

    QVERIFY(ui::summarizeFolder(root(), "GB18030", false, [] { return true; }).isEmpty()); // stopped

    ui::FolderSummaryLimits limits;
    limits.maxEntries = 3;
    QVERIFY(ui::summarizeFolder(root(), "GB18030", false, {}, limits)
                .contains(QStringLiteral("Only the first 3 entries")));
}

void TestFolders::showsAFolder()
{
    makeExperiment();
    ui::FolderPanel panel;
    panel.resize(500, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QVERIFY(!panel.setFolder(QDir(root()).filePath(QStringLiteral("README.md")))); // not a folder
    QVERIFY(panel.folder().isEmpty());

    QSignalSpy changed(&panel, &ui::FolderPanel::folderChanged);
    QVERIFY(panel.setFolder(root()));
    QCOMPARE(panel.folder(), root());
    QCOMPARE(changed.count(), 1);
    ui::FolderModel* model = panel.model();
    const QModelIndex top = panel.tree()->rootIndex();
    QTRY_COMPARE(model->rowCount(top), 5); // data, images, logs, config.yaml, README.md
    QCOMPARE(model->filePath(top), root());

    // Names only: no header, no other columns, however narrow the panel.
    QVERIFY(panel.tree()->isHeaderHidden());
    for (const int column : {ui::SizeColumn, ui::TypeColumn, ui::ModifiedColumn})
        QVERIFY(panel.tree()->isColumnHidden(column));
    QVERIFY(!panel.tree()->isColumnHidden(ui::NameColumn));

    // Types come from the content; a name that says otherwise gets a warning for its icon, and
    // its tooltip says why.
    const QString plot = QDir(root()).filePath(QStringLiteral("images/plot.png"));
    const QString fake = QDir(root()).filePath(QStringLiteral("images/fake.png"));
    panel.select(fake); // opens images
    QTRY_COMPARE(model->index(fake, ui::TypeColumn).data().toString(), QStringLiteral("JPEG"));
    QTRY_COMPARE(model->index(plot, ui::TypeColumn).data().toString(), QStringLiteral("PNG"));
    const QString fakeTip = model->index(fake, ui::NameColumn).data(Qt::ToolTipRole).toString();
    QVERIFY2(fakeTip.contains(QStringLiteral("Type: JPEG")), qPrintable(fakeTip));
    QVERIFY(fakeTip.contains(QStringLiteral("The name says PNG, but this is JPEG")));
    QVERIFY(fakeTip.contains(QDir::toNativeSeparators(fake)));
    const QString plotTip = model->index(plot, ui::NameColumn).data(Qt::ToolTipRole).toString();
    QVERIFY(plotTip.contains(QStringLiteral("Type: PNG")));
    QVERIFY(!plotTip.contains(QStringLiteral("The name says")));
    const auto icon = [&](const QString& path) {
        return model->index(path, ui::NameColumn).data(Qt::DecorationRole).value<QIcon>().pixmap(16).toImage();
    };
    const QImage warning = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(16).toImage();
    QCOMPARE(icon(fake), warning);
    QVERIFY(icon(plot) != warning);
    QCOMPARE(model->index(QDir(root()).filePath(QStringLiteral("logs")), ui::TypeColumn).data().toString(),
             QStringLiteral("Folder"));
    QCOMPARE(panel.selectedPaths(), QStringList{fake});

    panel.closeFolder();
    QVERIFY(panel.folder().isEmpty());
    QCOMPARE(changed.count(), 2);
}

void TestFolders::offersWhatFitsTheSelection()
{
    makeExperiment();
    ui::FolderPanel panel;
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QVERIFY(panel.setFolder(root()));
    QTRY_COMPARE(panel.model()->rowCount(panel.tree()->rootIndex()), 5);
    const auto action = [&panel](const char* text) {
        for (QAction* a : panel.findChildren<QAction*>())
            if (a->text() == QString::fromUtf8(text))
                return a;
        return static_cast<QAction*>(nullptr);
    };
    const QList<QAction*> folderOnly = {action("&Find in This Folder…"), action("&What Is in This Folder?"),
                                        action("New &File…"), action("New Fol&der…"), action("Open as &Folder")};
    const QList<QAction*> fileOnly = {action("&Open")};
    const QList<QAction*> either = {action("&Rename…"), action("Move to &Trash"), action("&Copy Path"),
                                    action("Copy Re&lative Path"), action("Show in File &Manager")};
    for (QAction* a : folderOnly + fileOnly + either)
        QVERIFY(a);
    const auto enabled = [](const QList<QAction*>& actions) {
        return std::all_of(actions.begin(), actions.end(), [](QAction* a) { return a->isEnabled(); });
    };
    const auto disabled = [](const QList<QAction*>& actions) {
        return std::none_of(actions.begin(), actions.end(), [](QAction* a) { return a->isEnabled(); });
    };

    // A file: what concerns folders is greyed out.
    panel.select(QDir(root()).filePath(QStringLiteral("README.md")));
    QVERIFY(disabled(folderOnly));
    QVERIFY(enabled(fileOnly));
    QVERIFY(enabled(either));

    // A folder: the other way round.
    panel.select(QDir(root()).filePath(QStringLiteral("logs")));
    QVERIFY(enabled(folderOnly));
    QVERIFY(disabled(fileOnly));
    QVERIFY(enabled(either));

    // Nothing selected: the folder shown, which is no item to rename or move.
    panel.tree()->clearSelection();
    QVERIFY(enabled({action("&Find in This Folder…"), action("New &File…"), action("&Copy Path")}));
    QVERIFY(disabled({action("Open as &Folder"), action("&Open"), action("&Rename…"), action("Move to &Trash")}));

    // The panel's own menu is about the folder shown, even with a file selected.
    panel.select(QDir(root()).filePath(QStringLiteral("README.md")));
    QVERIFY(!action("New &File…")->isEnabled());
    emit panel.findChild<QToolButton*>()->menu()->aboutToShow();
    QVERIFY(action("New &File…")->isEnabled());
    QSignalSpy find(&panel, &ui::FolderPanel::findInFolderRequested);
    action("&Find in This Folder…")->trigger();
    QCOMPARE(find.count(), 1);
    QCOMPARE(find.last().first().toString(), root());
}

void TestFolders::previewsWhatTheUserMovesTo()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    write(QStringLiteral("sub/c.txt"), "c\n");
    ui::FolderPanel panel;
    panel.resize(500, 600);
    panel.show();
    panel.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    QVERIFY(panel.setFolder(root()));
    QTRY_COMPARE(panel.model()->rowCount(panel.tree()->rootIndex()), 3);

    QSignalSpy previewed(&panel, &ui::FolderPanel::previewRequested);
    QSignalSpy opened(&panel, &ui::FolderPanel::openRequested);
    // Selected by the program: nothing is shown.
    panel.select(a);
    QTest::qWait(300);
    QCOMPARE(previewed.count(), 0);

    // Moved to by the user: shown a moment later; Enter opens it.
    QTreeView* tree = panel.tree();
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_Down);
    QTRY_COMPARE(previewed.count(), 1);
    QCOMPARE(previewed.last().first().toString(), b);
    QTest::keyClick(tree, Qt::Key_Return);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.last().first().toString(), b);

    // A folder opens and closes on Enter, and is not previewed.
    QTest::keyClick(tree, Qt::Key_Home);
    QCOMPARE(panel.selectedPaths(), QStringList{QDir(root()).filePath(QStringLiteral("sub"))});
    QTest::keyClick(tree, Qt::Key_Return);
    QVERIFY(tree->isExpanded(tree->currentIndex()));
    QTest::qWait(300);
    QCOMPARE(previewed.count(), 1);
    QCOMPARE(opened.count(), 1);
}

void TestFolders::filterListsTheWholeFolder()
{
    for (const char* name : {"a.log", "notes.txt", "x/b.log", "x/y/c.log"})
        write(QString::fromLatin1(name), "line\n");
    ui::FolderPanel panel;
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QVERIFY(panel.setFolder(root()));
    QVERIFY(!panel.showsList());

    panel.setFilter(QStringLiteral("*.log"));
    QVERIFY(panel.showsList());
    QTRY_VERIFY(!panel.isListing());
    ui::FileListModel* list = panel.listModel();
    QCOMPARE(list->rowCount(), 3);
    QCOMPARE(list->index(1, ui::NameColumn).data().toString(), QDir::toNativeSeparators(QStringLiteral("x/b.log")));
    QCOMPARE(list->filePath(list->index(2, ui::NameColumn)), QDir(root()).filePath(QStringLiteral("x/y/c.log")));
    QCOMPARE(panel.statusText(), QStringLiteral("3 file(s) match"));
    QTRY_COMPARE(list->index(0, ui::TypeColumn).data().toString(), QStringLiteral("Text · ASCII"));

    panel.setFilter(QStringLiteral("nothing-like-it"));
    QTRY_VERIFY(!panel.isListing());
    QCOMPARE(list->rowCount(), 0);
    QCOMPARE(panel.statusText(), QStringLiteral("No file names match"));

    // Selecting what the list does not show goes back to the tree.
    panel.setFilter(QStringLiteral("*.log"));
    QTRY_VERIFY(!panel.isListing());
    const QString notes = QDir(root()).filePath(QStringLiteral("notes.txt"));
    panel.select(notes);
    QVERIFY(!panel.showsList());
    QVERIFY(panel.filter().isEmpty());
    QVERIFY(panel.statusText().isEmpty());
    QCOMPARE(panel.selectedPaths(), QStringList{notes});
}

void TestFolders::makesRenamesAndTrashesFiles()
{
    ui::FolderPanel panel;
    QVERIFY(panel.setFolder(root()));
    QString error;
    const QString made = panel.createFile(root(), QStringLiteral("new.txt"), &error);
    QVERIFY2(!made.isEmpty(), qPrintable(error));
    QVERIFY(QFileInfo(made).isFile());
    QVERIFY(panel.createFile(root(), QStringLiteral("new.txt"), &error).isEmpty());
    QCOMPARE(error, QStringLiteral("new.txt already exists."));
    QVERIFY(panel.createFile(root(), QStringLiteral("a/b"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("not a valid name")));
    const QString folder = panel.createFolder(root(), QStringLiteral("sub"), &error);
    QVERIFY(QFileInfo(folder).isDir());

    QSignalSpy renamed(&panel, &ui::FolderPanel::pathRenamed);
    const QString moved = QDir(root()).filePath(QStringLiteral("renamed.txt"));
    QVERIFY2(panel.rename(made, QStringLiteral("renamed.txt"), &error), qPrintable(error));
    QCOMPARE(renamed.count(), 1);
    QCOMPARE(renamed.last().at(0).toString(), made);
    QCOMPARE(renamed.last().at(1).toString(), moved);
    QVERIFY(!QFileInfo::exists(made));
    QVERIFY(QFileInfo::exists(moved));
    QVERIFY(!panel.rename(moved, QStringLiteral("sub"), &error)); // taken
    QCOMPARE(error, QStringLiteral("sub already exists."));

    // Unsaved changes keep a file where it is.
    panel.setUnsavedCheck([&moved](const QString& path) {
        return path == moved ? QStringLiteral("renamed.txt") : QString();
    });
    QVERIFY(!panel.rename(moved, QStringLiteral("other.txt"), &error));
    QCOMPARE(error, QStringLiteral("renamed.txt has unsaved changes. Save or close it first."));
    QVERIFY(!panel.moveToTrash(moved, &error));
    QVERIFY(QFileInfo::exists(moved));
    panel.setUnsavedCheck({});

    QSignalSpy trashed(&panel, &ui::FolderPanel::pathTrashed);
    QString inTrash;
    QVERIFY2(panel.moveToTrash(moved, &error, &inTrash), qPrintable(error));
    QVERIFY(!QFileInfo::exists(moved));
    QVERIFY(QFileInfo::exists(inTrash));
    QCOMPARE(trashed.count(), 1);
    removeFromTrash(inTrash);
}

void TestFolders::remembersRecentFolders()
{
    QVERIFY(QDir(root()).mkpath(QStringLiteral("a")));
    QVERIFY(QDir(root()).mkpath(QStringLiteral("b")));
    const QString first = canonical(QDir(root()).filePath(QStringLiteral("a")));
    const QString second = canonical(QDir(root()).filePath(QStringLiteral("b")));
    ui::FolderPanel panel;
    QVERIFY(panel.setFolder(first));
    QVERIFY(panel.setFolder(second));
    QCOMPARE(panel.recentFolders(), (QStringList{second, first}));
    QVERIFY(panel.setFolder(first));
    QCOMPARE(panel.recentFolders(), (QStringList{first, second}));
    panel.goUp();
    QCOMPARE(panel.folder(), root());
    QCOMPARE(panel.selectedPaths(), QStringList{first});
    QCOMPARE(panel.recentFolders(), (QStringList{root(), first, second}));
    panel.forgetFolder(second);
    QCOMPARE(panel.recentFolders(), (QStringList{root(), first}));
    panel.clearRecentFolders();
    QVERIFY(panel.recentFolders().isEmpty());
}

void TestFolders::previewsInOneTab()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    const QString c = write(QStringLiteral("c.txt"), "c\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* tabs = window.findChild<QTabWidget*>();

    QVERIFY(window.previewFile(a));
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.previewDocument(), window.currentDocument());
    QVERIFY(tabs->tabBar()->tabTextColor(0).isValid()); // dimmed
    QVERIFY(window.previewFile(b));
    QCOMPARE(window.documentCount(), 1); // in the same tab
    QCOMPARE(window.currentDocument()->filePath(), b);

    // Opening it keeps it.
    QVERIFY(window.openFile(b));
    QCOMPARE(window.documentCount(), 1);
    QVERIFY(!window.previewDocument());
    QVERIFY(!tabs->tabBar()->tabTextColor(0).isValid());
    QVERIFY(window.previewFile(c));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.previewDocument()->filePath(), c);

    // A file that is open already is just shown.
    QVERIFY(window.previewFile(b));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.currentDocument()->filePath(), b);
    QCOMPARE(window.previewDocument()->filePath(), c);

    // Double-clicking its tab keeps it too.
    const int index = tabs->indexOf(window.previewDocument());
    QTest::mouseDClick(tabs->tabBar(), Qt::LeftButton, {}, tabs->tabBar()->tabRect(index).center());
    QVERIFY(!window.previewDocument());

    // What cannot be shown says so in the status bar, without a dialog in the way.
    QVERIFY(!window.previewFile(QDir(root()).filePath(QStringLiteral("missing.txt"))));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("missing.txt")));
    QCOMPARE(window.documentCount(), 2);
}

void TestFolders::keepsAPreviewThatIsEdited()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.previewFile(a));
    ui::DocumentView* doc = window.previewDocument();
    doc->textView()->setFocus();
    QTest::keyClicks(doc->textView(), QStringLiteral("x"));
    QVERIFY(doc->isModified());
    QVERIFY(!window.previewDocument());
    QVERIFY(window.previewFile(b));
    QCOMPARE(window.documentCount(), 2);
    QCOMPARE(window.document(0), doc);
}

void TestFolders::previewsDoNotCrowdRecentFiles()
{
    const QString a = write(QStringLiteral("a.txt"), "one\ntwo\nthree\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    const QString c = write(QStringLiteral("c.txt"), "c\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(a));
    QVERIFY(window.openFile(c));
    QCOMPARE(recentFiles(), (QStringList{c, a}));
    window.findChild<QTabWidget*>()->tabCloseRequested(0);
    window.findChild<QTabWidget*>()->tabCloseRequested(0);
    QCOMPARE(window.documentCount(), 0);

    // Previews come and go without moving to the front of the list.
    QVERIFY(window.previewFile(b));
    QVERIFY(window.previewFile(a));
    QTRY_VERIFY(window.currentDocument()->textView()->goToLine(2));
    QVERIFY(window.previewFile(c));
    QCOMPARE(recentFiles(), (QStringList{c, a}));
    // … but where a file was left is still remembered.
    const QVariantMap remembered = QSettings().value(QStringLiteral("files/recent")).toList().at(1).toMap();
    QCOMPARE(remembered.value(QStringLiteral("cursor")).toULongLong(), 8u);

    QVERIFY(window.openFile(b));
    QCOMPARE(recentFiles().first(), b);
}

void TestFolders::renamedFilesStayOpen()
{
    const QString a = write(QStringLiteral("a.txt"), "one\ntwo\nthree\n");
    const QString x = write(QStringLiteral("sub/x.txt"), "x\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFolder(root()));
    auto* panel = window.findChild<ui::FolderPanel*>();
    QVERIFY(window.openFile(x));
    QVERIFY(window.openFile(a));
    QTRY_VERIFY(window.currentDocument()->textView()->goToLine(2));

    QString error;
    QVERIFY2(panel->rename(a, QStringLiteral("b.txt"), &error), qPrintable(error));
    const QString b = QDir(root()).filePath(QStringLiteral("b.txt"));
    QCOMPARE(window.currentDocument()->filePath(), b);
    QCOMPARE(window.currentDocument()->title(), QStringLiteral("b.txt"));
    QCOMPARE(window.findChild<QTabWidget*>()->tabText(1), QStringLiteral("b.txt"));
    QTRY_COMPARE(window.currentDocument()->textView()->cursorLine(), 2u); // where it was

    // Folders take their files along.
    QVERIFY(panel->rename(QDir(root()).filePath(QStringLiteral("sub")), QStringLiteral("moved"), &error));
    QCOMPARE(window.document(0)->filePath(), QDir(root()).filePath(QStringLiteral("moved/x.txt")));
    QVERIFY(recentFiles().contains(QDir(root()).filePath(QStringLiteral("moved/x.txt"))));
}

void TestFolders::keepsUnsavedFilesWhereTheyAre()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFolder(root()));
    auto* panel = window.findChild<ui::FolderPanel*>();
    QVERIFY(window.openFile(a));
    window.currentDocument()->textView()->setFocus();
    QTest::keyClicks(window.currentDocument()->textView(), QStringLiteral("x"));
    QVERIFY(window.currentDocument()->isModified());

    QString error;
    QVERIFY(!panel->rename(a, QStringLiteral("b.txt"), &error));
    QCOMPARE(error, QStringLiteral("a.txt has unsaved changes. Save or close it first."));
    QVERIFY(!panel->rename(root(), QStringLiteral("elsewhere"), &error)); // nor the folder it is in
    QVERIFY(!panel->moveToTrash(a, &error));
    QVERIFY(QFileInfo::exists(a));
}

void TestFolders::closesTrashedFiles()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFolder(root()));
    auto* panel = window.findChild<ui::FolderPanel*>();
    QVERIFY(window.openFile(a));
    QVERIFY(window.openFile(b));
    QString error;
    QString inTrash;
    QVERIFY2(panel->moveToTrash(a, &error, &inTrash), qPrintable(error));
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.currentDocument()->filePath(), b);
    QVERIFY(!recentFiles().contains(a));
    removeFromTrash(inTrash);
}

void TestFolders::opensFoldersFromThePath()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* panel = window.findChild<ui::FolderPanel*>();
    QVERIFY(!panel->isVisible());
    QVERIFY(window.openPath(root()));
    QCOMPARE(panel->folder(), root());
    QVERIFY(panel->isVisible());
    QVERIFY(window.openPath(a));
    QCOMPARE(window.currentDocument()->filePath(), a);
    QVERIFY(findAction(window, QStringLiteral("What Is in This Fol&der?"))->isEnabled());
}

void TestFolders::restoresTheFolder()
{
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    {
        ui::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openFolder(root()));
        QVERIFY(window.openFile(b));
        QVERIFY(window.previewFile(a));
        QVERIFY(window.close());
    }
    ui::MainWindow window;
    window.restoreSession();
    QCOMPARE(window.findChild<ui::FolderPanel*>()->folder(), root());
    QCOMPARE(window.documentCount(), 2);
    QVERIFY(window.previewDocument());
    QCOMPARE(window.previewDocument()->filePath(), a);
}

void TestFolders::asksTheAiAboutAFolder()
{
    makeExperiment();
    aiedit::test::MockServer server;
    server.chunks = answer(QStringLiteral("这是一个模型训练的实验目录。"));
    ui::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.findChild<ui::AiPanel*>()->setSettingsProvider([&server] { return aiedit::test::settingsFor(server); });
    QAction* ask = findAction(window, QStringLiteral("What Is in This Fol&der?"));
    QVERIFY(!ask->isEnabled()); // no folder yet
    QVERIFY(window.openFolder(root()));
    QVERIFY(ask->isEnabled());

    ask->trigger();
    QTRY_COMPARE(server.requests, 1);
    const QString body = QString::fromUtf8(server.body);
    QVERIFY(body.contains(QStringLiteral("What is in this folder?")));
    QVERIFY(body.contains(QStringLiteral("Looked at: 7 files and 5 folders")));
    QVERIFY(body.contains(QStringLiteral("Training runs for the model.")));
    QVERIFY(!body.contains(QStringLiteral("1,0.9"))); // not every file's content
    QTRY_VERIFY(window.findChild<ui::AiPanel*>()->transcript().contains(QStringLiteral("实验目录")));
}

QTEST_MAIN(TestFolders)
#include "tst_folders.moc"
