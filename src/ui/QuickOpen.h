#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QFutureWatcher>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QAction;
class QLabel;
class QLineEdit;
class QListWidget;

namespace aiedit::ui {

// Ctrl+P and Ctrl+Shift+P, over the top of the window as in VS Code and Sublime Text: a line to
// type in, and what matches it, best first (see fuzzyMatch()).
// - Files: those open, the recent ones and every file of the folder in the folder panel (but
//   for what .gitignore names, in a repository), by name or by path. With nothing typed, the open and recent ones, with the one after the
//   current document selected: Enter goes back to it. "name:42" opens a file at line 42.
// - After ">": commands, the actions of the menus, with their shortcuts.
// - After ":": a line ("42"), or an offset ("0x1F"), in the current document.
class QuickOpen : public QFrame
{
    Q_OBJECT

public:
    struct Item
    {
        QString text;   // a command; a file's name comes from its path
        QString detail; // the command's menu
        QString path;   // a file
        QPointer<QAction> action; // a command
        QString shortcut;
        bool checkable = false;
        bool checked = false;
        bool current = false; // the document shown now
        int rank = 0;         // among close matches, higher first: open files, then recent ones
    };

    explicit QuickOpen(QWidget* parent);
    ~QuickOpen() override;

    // What it offers, asked for each time it opens: files most recently used first.
    void setFileSource(std::function<QList<Item>()> source) { m_fileSource = std::move(source); }
    void setCommandSource(std::function<QList<Item>()> source) { m_commandSource = std::move(source); }
    // The folder whose files it offers too, listed in the background; empty for none.
    void setFolder(const QString& folder);

    // Shows it over the top of its parent with `text` typed in (">" for commands), and takes
    // the keyboard.
    void open(const QString& text = {});
    QLineEdit* input() const { return m_input; }
    // What the list shows, best first, and the row selected (for tests).
    QStringList shownTexts() const;
    int currentRow() const;
    QString note() const;
    // From starting to list the folder until its files are in.
    bool isListing() const { return !m_listing.isEmpty(); }

signals:
    void fileChosen(const QString& path, std::optional<std::uint64_t> line); // the line from 0
    void positionChosen(const QString& position);                          // what followed ":"

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Something to match: its text (a name, or a command) after its detail (its folder, or its
    // menu), so that both can be matched at once.
    struct Candidate
    {
        QString combined;
        int primaryStart = 0;
        int rank = 0;
        int item = -1; // in m_items; -1 for a file of the folder, whose path is in the folder
    };
    struct FolderFiles
    {
        QString folder;
        std::vector<Candidate> files;
        bool limited = false;
        bool gitIgnored = false; // what .gitignore names was left out
    };
    class Delegate;

    void load(bool commands);
    void refilter();
    void showPosition(const QString& position);
    void accept(int row);
    void dismiss(bool restoreFocus);
    void place();
    void listFolder();
    void listed();
    // A file open or recent, by its name after its folder: within the open folder from there,
    // the home folder as ~.
    Candidate fileCandidate(const QString& path, int rank, bool* inFolder) const;

    std::function<QList<Item>()> m_fileSource;
    std::function<QList<Item>()> m_commandSource;
    QString m_folder;
    FolderFiles m_folderFiles; // as listed last
    QString m_listing;         // the folder being listed, until its files are in
    QElapsedTimer m_listedAt;
    QFutureWatcher<FolderFiles> m_listWatcher;

    bool m_commands = false; // what is offered now
    std::vector<Item> m_items;
    std::vector<Candidate> m_candidates; // of m_items
    QSet<QString> m_listedRelative;      // those of them in the folder, by path in it
    std::vector<const Candidate*> m_shown;
    // All the files the last query matched: typing on can only narrow them down.
    QString m_matchedQuery;
    std::vector<const Candidate*> m_matched;
    std::optional<std::uint64_t> m_line; // from "name:42"
    bool m_positionMode = false;          // after ":"
    QString m_position;
    QPointer<QWidget> m_returnFocus;

    QLineEdit* m_input = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_note = nullptr;
};

} // namespace aiedit::ui
