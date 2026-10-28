#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QProcess>
#include <QStringDecoder>
#include <QWidget>

#include <cstdint>
#include <optional>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace aiedit::ui {

// How a script is run: the program and its arguments, the script's path among them.
struct RunCommand
{
    QString program;
    QStringList arguments;
    // What it needs, to say so when it is not installed: "python3", "node", ...
    QString interpreter;
    bool found = false; // the program is there
};

// The command for a script, by its first line (#!, except on Windows) or else its extension:
// Python, shell, JavaScript (Node), Ruby, Perl, PHP, Lua, R, Tcl, PowerShell, batch files on
// Windows, Go and single-file Java. Nothing for a file of no kind it knows.
std::optional<RunCommand> runCommandFor(const QString& path, const QByteArray& firstLine);

// Runs a script and shows what it writes: output as it comes, errors in red, a line
// typed below sent to its input, Stop and Run Again; at the end the exit code and how long it
// took. A double-clicked line naming a file and line (a Python traceback, file:line:) opens it.
class RunPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RunPanel(QWidget* parent = nullptr);
    ~RunPanel() override;

    // Runs the script, after stopping what runs. False, with the reason shown, if no program
    // can run it.
    bool run(const QString& path, const QByteArray& firstLine);
    void stop();
    bool isRunning() const { return m_process && m_process->state() != QProcess::NotRunning; }
    QString scriptPath() const { return m_path; }
    // All that is shown, as plain text; and how the last run ended. For tests.
    QString outputText() const;
    std::optional<int> exitCode() const { return m_exitCode; }
    QLineEdit* inputEdit() const { return m_input; }

signals:
    void openRequested(const QString& path, std::uint64_t line, std::uint64_t offset);
    void runningChanged(bool running);
    void finished(int exitCode);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Kind
    {
        Output,
        Error,
        Note, // what the panel itself says
    };
    void append(const QString& text, Kind kind);
    void readOutput(bool errors);
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void openLineAt(const QPoint& pos);

    QLabel* m_command = nullptr;
    QPushButton* m_again = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_clear = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QLineEdit* m_input = nullptr;
    QPointer<QProcess> m_process;
    QStringDecoder m_outDecoder{QStringDecoder::Utf8};
    QStringDecoder m_errDecoder{QStringDecoder::Utf8};
    QElapsedTimer m_clock;
    QString m_path;
    QByteArray m_firstLine;
    QString m_workingDir;
    std::optional<int> m_exitCode;
    bool m_stopping = false;
};

} // namespace aiedit::ui
