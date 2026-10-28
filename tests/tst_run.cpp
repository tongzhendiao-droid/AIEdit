#include "ui/MainWindow.h"
#include "ui/RunPanel.h"

#include <QDockWidget>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace aiedit::ui;

class TestRun : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void commands();
    void runsAndShowsOutput();
    void sendsInput();
    void stops();
    void opensTracebackLines();
    void runsFromTheWindow();

private:
    QString write(const QString& name, const QByteArray& content);
    QTemporaryDir m_dir;
};

void TestRun::init()
{
    QVERIFY(m_dir.isValid());
}

QString TestRun::write(const QString& name, const QByteArray& content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
    return path;
}

void TestRun::commands()
{
    const auto python = runCommandFor(QStringLiteral("/x/a.py"), {});
    QVERIFY(python);
    QVERIFY(python->arguments.contains(QStringLiteral("-u")));
    QCOMPARE(python->arguments.last(), QStringLiteral("/x/a.py"));
#ifndef Q_OS_WIN
    QCOMPARE(python->interpreter, QStringLiteral("python3"));
    const auto shell = runCommandFor(QStringLiteral("/x/a.sh"), {});
    QVERIFY(shell && shell->found);
    // The first line says what runs it, whatever the name.
    const auto shebang = runCommandFor(QStringLiteral("/x/tool"), "#!/bin/sh -e");
    QVERIFY(shebang && shebang->found);
    QCOMPARE(shebang->program, QStringLiteral("/bin/sh"));
    QCOMPARE(shebang->arguments, (QStringList{QStringLiteral("-e"), QStringLiteral("/x/tool")}));
    const auto missing = runCommandFor(QStringLiteral("/x/tool"), "#!/nowhere/interpreter");
    QVERIFY(missing && !missing->found);
#else
    QCOMPARE(python->interpreter, QStringLiteral("py"));
    const auto batch = runCommandFor(QStringLiteral("C:/x/a.bat"), {});
    QVERIFY(batch && batch->found);
#endif
    QVERIFY(!runCommandFor(QStringLiteral("/x/notes.txt"), {}));
    QVERIFY(!runCommandFor(QStringLiteral("/x/data.csv"), {}));
}

void TestRun::runsAndShowsOutput()
{
#ifdef Q_OS_WIN
    const QString script = write(QStringLiteral("hello.bat"), "@echo off\r\necho hello\r\necho oops 1>&2\r\nexit /b 3\r\n");
#else
    const QString script = write(QStringLiteral("hello.sh"), "echo hello\necho oops >&2\nexit 3\n");
#endif
    RunPanel panel;
    QSignalSpy finished(&panel, &RunPanel::finished);
    QVERIFY(panel.run(script, {}));
    QVERIFY(panel.isRunning());
    QVERIFY(finished.wait(10000));
    QCOMPARE(panel.exitCode(), 3);
    QVERIFY2(panel.outputText().contains(QStringLiteral("hello")), qPrintable(panel.outputText()));
    QVERIFY(panel.outputText().contains(QStringLiteral("oops")));
    QVERIFY(panel.outputText().contains(QStringLiteral("3"))); // the exit code, said at the end
    QVERIFY(!panel.isRunning());

    // A file of no kind it can run says so, and nothing starts.
    QVERIFY(!panel.run(write(QStringLiteral("notes.txt"), "x"), {}));
    QVERIFY(!panel.isRunning());
}

void TestRun::sendsInput()
{
#ifdef Q_OS_WIN
    const QString script = write(QStringLiteral("ask.bat"), "@echo off\r\nset /p name=\r\necho got %name%\r\n");
#else
    const QString script = write(QStringLiteral("ask.sh"), "read name\necho \"got $name\"\n");
#endif
    RunPanel panel;
    QSignalSpy finished(&panel, &RunPanel::finished);
    QVERIFY(panel.run(script, {}));
    panel.inputEdit()->setText(QStringLiteral("灵犀"));
    QMetaObject::invokeMethod(panel.inputEdit(), "returnPressed");
    QVERIFY(finished.wait(10000));
#ifdef Q_OS_WIN
    // cmd reads in the console's code page: the word itself may not come back as typed.
    QVERIFY2(panel.outputText().contains(QStringLiteral("got ")), qPrintable(panel.outputText()));
#else
    QVERIFY2(panel.outputText().contains(QStringLiteral("got 灵犀")), qPrintable(panel.outputText()));
#endif
}

void TestRun::stops()
{
#ifdef Q_OS_WIN
    const QString script = write(QStringLiteral("wait.bat"), "@echo off\r\nping -n 30 127.0.0.1 >nul\r\n");
#else
    const QString script = write(QStringLiteral("wait.sh"), "sleep 30\n");
#endif
    RunPanel panel;
    QSignalSpy finished(&panel, &RunPanel::finished);
    QVERIFY(panel.run(script, {}));
    QTest::qWait(200);
    panel.stop();
    QVERIFY(finished.wait(10000));
    QCOMPARE(panel.exitCode(), -1);
    QVERIFY(panel.outputText().contains(QStringLiteral("Stopped")));
}

void TestRun::opensTracebackLines()
{
    if (QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty() &&
        QStandardPaths::findExecutable(QStringLiteral("python")).isEmpty())
        QSKIP("no Python here");
    const QString script = write(QStringLiteral("boom.py"), "print('start')\n\nraise ValueError('boom')\n");
    RunPanel panel;
    QSignalSpy finished(&panel, &RunPanel::finished);
    QSignalSpy opened(&panel, &RunPanel::openRequested);
    QVERIFY(panel.run(script, {}));
    QVERIFY(finished.wait(20000));
    QCOMPARE(panel.exitCode(), 1);
    QVERIFY(panel.outputText().contains(QStringLiteral("ValueError: boom")));
    // Double-clicking the traceback's line opens the script at line 3 (2 from 0).
    auto* output = panel.findChild<QPlainTextEdit*>();
    QVERIFY(output);
    output->show();
    panel.resize(800, 400);
    panel.show();
    QTextCursor cursor = output->document()->find(QStringLiteral("line 3"));
    QVERIFY(!cursor.isNull());
    const QRect rect = output->cursorRect(cursor);
    QTest::mouseDClick(output->viewport(), Qt::LeftButton, {}, rect.center());
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.first().at(0).toString(), QFileInfo(script).absoluteFilePath());
    QCOMPARE(opened.first().at(1).toULongLong(), 2u);
}

void TestRun::runsFromTheWindow()
{
#ifdef Q_OS_WIN
    const QString script = write(QStringLiteral("window.bat"), "@echo off\r\necho from the window\r\n");
#else
    const QString script = write(QStringLiteral("window.sh"), "echo from the window\n");
#endif
    MainWindow window;
    QVERIFY(window.openFile(script));
    auto* panel = window.findChild<RunPanel*>();
    QVERIFY(panel);
    QSignalSpy finished(panel, &RunPanel::finished);
    QAction* run = nullptr;
    for (QAction* action : window.findChildren<QAction*>())
        if (action->text() == QStringLiteral("&Run") && !action->menu())
            run = action;
    QVERIFY(run && run->isEnabled());
    run->trigger();
    QVERIFY(finished.wait(10000));
    QVERIFY(panel->outputText().contains(QStringLiteral("from the window")));
    QVERIFY(window.findChild<QDockWidget*>(QStringLiteral("runDock"))->isVisibleTo(&window));
}

QTEST_MAIN(TestRun)
#include "tst_run.moc"
