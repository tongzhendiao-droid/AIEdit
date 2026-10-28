#include "ui/RunPanel.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>

namespace aiedit::ui {

namespace {

// Output kept: more lines than this drop the oldest.
constexpr int kMaxLines = 100'000;

// The first of these programs on PATH, or nothing.
QString findProgram(const QStringList& names)
{
    for (const QString& name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }
    return {};
}

} // namespace

std::optional<RunCommand> runCommandFor(const QString& path, const QByteArray& firstLine)
{
    RunCommand command;
#ifndef Q_OS_WIN
    // #!/usr/bin/env python3, #!/bin/bash -e: the script says what runs it.
    if (firstLine.startsWith("#!")) {
        QStringList words = QString::fromUtf8(firstLine.mid(2)).trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                                                Qt::SkipEmptyParts);
        if (!words.isEmpty()) {
            command.program = words.takeFirst();
            command.arguments = words;
            command.arguments << path;
            command.interpreter = QFileInfo(command.program).fileName() == u"env" && !words.isEmpty()
                                      ? words.first()
                                      : QFileInfo(command.program).fileName();
            command.found = QFileInfo(command.program).isExecutable() &&
                            (QFileInfo(command.program).fileName() != u"env" || words.isEmpty() ||
                             !QStandardPaths::findExecutable(words.first()).isEmpty());
            return command;
        }
    }
#endif
    struct Kind
    {
        QStringList programs;
        QStringList before; // arguments before the script
        QStringList after;  // and after it
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    std::optional<Kind> kind;
    if (suffix == u"py" || suffix == u"pyw") {
#ifdef Q_OS_WIN
        kind = Kind{{QStringLiteral("py"), QStringLiteral("python"), QStringLiteral("python3")}, {QStringLiteral("-u")}, {}};
#else
        kind = Kind{{QStringLiteral("python3"), QStringLiteral("python")}, {QStringLiteral("-u")}, {}};
#endif
    } else if (suffix == u"sh" || suffix == u"bash") {
        kind = Kind{{QStringLiteral("bash"), QStringLiteral("sh")}, {}, {}};
    } else if (suffix == u"zsh" || suffix == u"fish") {
        kind = Kind{{suffix}, {}, {}};
    } else if (suffix == u"js" || suffix == u"mjs" || suffix == u"cjs") {
        kind = Kind{{QStringLiteral("node")}, {}, {}};
    } else if (suffix == u"ts") {
        kind = Kind{{QStringLiteral("deno")}, {QStringLiteral("run")}, {}};
    } else if (suffix == u"rb") {
        kind = Kind{{QStringLiteral("ruby")}, {}, {}};
    } else if (suffix == u"pl") {
        kind = Kind{{QStringLiteral("perl")}, {}, {}};
    } else if (suffix == u"php") {
        kind = Kind{{QStringLiteral("php")}, {}, {}};
    } else if (suffix == u"lua") {
        kind = Kind{{QStringLiteral("lua"), QStringLiteral("lua5.4"), QStringLiteral("luajit")}, {}, {}};
    } else if (suffix == u"r") {
        kind = Kind{{QStringLiteral("Rscript")}, {}, {}};
    } else if (suffix == u"tcl") {
        kind = Kind{{QStringLiteral("tclsh")}, {}, {}};
    } else if (suffix == u"ps1") {
        kind = Kind{{QStringLiteral("pwsh"), QStringLiteral("powershell")},
                    {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                     QStringLiteral("-File")},
                    {}};
    } else if (suffix == u"go") {
        kind = Kind{{QStringLiteral("go")}, {QStringLiteral("run")}, {}};
    } else if (suffix == u"java") {
        kind = Kind{{QStringLiteral("java")}, {}, {}}; // a single file runs as it is (Java 11 on)
#ifdef Q_OS_WIN
    } else if (suffix == u"bat" || suffix == u"cmd") {
        kind = Kind{{QStringLiteral("cmd")}, {QStringLiteral("/c")}, {}};
#endif
    }
    if (!kind)
        return std::nullopt;
    command.interpreter = kind->programs.first();
    command.program = findProgram(kind->programs);
    command.found = !command.program.isEmpty();
    if (!command.found)
        command.program = command.interpreter;
    command.arguments = kind->before;
    command.arguments << path;
    command.arguments << kind->after;
    return command;
}

RunPanel::RunPanel(QWidget* parent) : QWidget(parent)
{
    m_command = new QLabel(this);
    m_command->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_command->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_again = new QPushButton(tr("Run Again"), this);
    m_stop = new QPushButton(tr("Stop"), this);
    m_clear = new QPushButton(tr("Clear"), this);
    m_again->setEnabled(false);
    m_stop->setEnabled(false);
    connect(m_again, &QPushButton::clicked, this, [this] {
        if (!m_path.isEmpty())
            run(m_path, m_firstLine);
    });
    connect(m_stop, &QPushButton::clicked, this, &RunPanel::stop);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setUndoRedoEnabled(false);
    m_output->setMaximumBlockCount(kMaxLines);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_output->viewport()->installEventFilter(this);
    m_output->setToolTip(tr("Double-click a line that names a file and a line to open it there"));
    connect(m_clear, &QPushButton::clicked, m_output, &QPlainTextEdit::clear);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Input for the program: Enter sends the line"));
    m_input->setEnabled(false);
    connect(m_input, &QLineEdit::returnPressed, this, [this] {
        if (!isRunning())
            return;
        const QString line = m_input->text();
        m_input->clear();
        append(line + u'\n', Kind::Note); // as a terminal echoes it
        m_process->write(line.toUtf8() + '\n');
    });

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(m_command, 1);
    bar->addWidget(m_again);
    bar->addWidget(m_stop);
    bar->addWidget(m_clear);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_output, 1);
    layout->addWidget(m_input);
}

RunPanel::~RunPanel()
{
    if (isRunning()) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

bool RunPanel::run(const QString& path, const QByteArray& firstLine)
{
    if (isRunning()) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
        m_process->deleteLater();
    }
    m_path = path;
    m_firstLine = firstLine;
    m_exitCode.reset();
    m_again->setEnabled(true);
    const std::optional<RunCommand> command = runCommandFor(path, firstLine);
    if (!command) {
        append(tr("Do not know how to run %1: no program is known for files like it.\n").arg(QFileInfo(path).fileName()),
               Kind::Error);
        return false;
    }
    if (!command->found) {
        append(tr("%1 is needed to run %2, and it is not installed (or not on PATH).\n")
                   .arg(command->interpreter, QFileInfo(path).fileName()),
               Kind::Error);
        return false;
    }

    m_output->clear();
    m_workingDir = QFileInfo(path).absolutePath();
    QStringList shown = {QDir::toNativeSeparators(command->program)};
    for (const QString& argument : command->arguments)
        shown << (argument.contains(u' ') ? u'"' + argument + u'"' : argument);
    m_command->setText(shown.join(u' '));
    m_command->setToolTip(m_command->text());

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_workingDir);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // Python writes as it goes, in UTF-8, rather than holding it back for a pipe.
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    m_process->setProcessEnvironment(environment);
    m_outDecoder = QStringDecoder(QStringDecoder::Utf8);
    m_errDecoder = QStringDecoder(QStringDecoder::Utf8);
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] { readOutput(true); });
    connect(m_process, &QProcess::finished, this, &RunPanel::processFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            append(tr("Could not start it: %1\n").arg(m_process->errorString()), Kind::Error);
            processFinished(-1, QProcess::CrashExit);
        }
    });
    m_stopping = false;
    m_clock.start();
    m_process->start(command->program, command->arguments);
    m_stop->setEnabled(true);
    m_input->setEnabled(true);
    emit runningChanged(true);
    return true;
}

void RunPanel::stop()
{
    if (!isRunning())
        return;
    m_stopping = true;
    m_process->terminate();
    // Asked to end; ended, if it does not listen.
    QTimer::singleShot(2000, m_process, [process = m_process] {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void RunPanel::readOutput(bool errors)
{
    const QByteArray bytes = errors ? m_process->readAllStandardError() : m_process->readAllStandardOutput();
    QString text = (errors ? m_errDecoder : m_outDecoder).decode(bytes);
    text.remove(u'\r');
    append(text, errors ? Kind::Error : Kind::Output);
}

void RunPanel::processFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    readOutput(false);
    readOutput(true);
    const double seconds = static_cast<double>(m_clock.elapsed()) / 1000.0;
    QString note;
    if (m_stopping)
        note = tr("Stopped after %1 s").arg(seconds, 0, 'f', 1);
    else if (status == QProcess::CrashExit)
        note = tr("Ended abnormally after %1 s").arg(seconds, 0, 'f', 1);
    else
        note = tr("Finished with exit code %1 in %2 s").arg(exitCode).arg(seconds, 0, 'f', 1);
    const QString last = m_output->document()->lastBlock().text();
    append((last.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("— ") + note + QStringLiteral(" —\n"),
           Kind::Note);
    m_exitCode = status == QProcess::NormalExit && !m_stopping ? exitCode : -1;
    m_stop->setEnabled(false);
    m_input->setEnabled(false);
    m_process->deleteLater();
    m_process = nullptr;
    emit runningChanged(false);
    emit finished(*m_exitCode);
}

void RunPanel::append(const QString& text, Kind kind)
{
    if (text.isEmpty())
        return;
    QTextCharFormat format;
    const QPalette& palette = m_output->palette();
    if (kind == Kind::Error) {
        format.setForeground(palette.color(QPalette::Base).lightnessF() < 0.5 ? QColor(0xEF, 0x83, 0x83)
                                                                               : QColor(0xC6, 0x28, 0x28));
    } else if (kind == Kind::Note) {
        format.setForeground(palette.color(QPalette::PlaceholderText));
    }
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    const bool atBottom =
        m_output->verticalScrollBar()->value() >= m_output->verticalScrollBar()->maximum() - 2;
    cursor.insertText(text, format);
    if (atBottom)
        m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

QString RunPanel::outputText() const
{
    return m_output->toPlainText();
}

bool RunPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_output->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        openLineAt(static_cast<QMouseEvent*>(event)->position().toPoint());
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void RunPanel::openLineAt(const QPoint& pos)
{
    const QString line = m_output->cursorForPosition(pos).block().text();
    // Python's traceback (File "x.py", line 12), and file:line: as compilers, Node and most
    // tools write it.
    static const QRegularExpression python(QStringLiteral("File \"([^\"]+)\", line (\\d+)"));
    static const QRegularExpression colon(QStringLiteral("([^\\s:()\"']+\\.[A-Za-z0-9]+):(\\d+)"));
    for (const QRegularExpression* re : {&python, &colon}) {
        const QRegularExpressionMatch match = re->match(line);
        if (!match.hasMatch())
            continue;
        const QFileInfo file(QDir(m_workingDir), match.captured(1));
        if (!file.isFile())
            continue;
        const auto number = match.captured(2).toULongLong();
        emit openRequested(file.absoluteFilePath(), number > 0 ? number - 1 : 0, 0);
        return;
    }
}

} // namespace aiedit::ui
