#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QThreadPool>

#include <memory>
#include <span>
#include <vector>

class QLockFile;

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

// Keeps the unsaved content of documents on disk, so that it outlives quitting AIEdit and
// crashes, as Notepad++'s session snapshots do.
//
// Every window writes to a folder of its own under root(), locked while the window runs.
// A folder that is no longer locked belongs to a window that has ended, normally or not;
// the next window to start takes its backups over and brings the documents back. Nothing is
// written to disk until the first backup.
class BackupStore
{
public:
    // One document's backup: what describes it, as written, and where its bytes are.
    struct Entry
    {
        QString id;
        QJsonObject meta;
        QString contentPath;
    };

    BackupStore();
    ~BackupStore(); // finishes writing; the backups stay for the next start

    BackupStore(const BackupStore&) = delete;
    BackupStore& operator=(const BackupStore&) = delete;

    static QString root();

    // Saves a document's bytes and what describes it, on a worker thread. Writes and removals
    // happen in the order they are asked for, so the latest one for an id wins.
    void write(const QString& id, const QJsonObject& meta, std::shared_ptr<const core::FileSource> content);
    // The same, done when it returns; false if it could not be written.
    bool writeNow(const QString& id, const QJsonObject& meta, std::span<const std::uint8_t> bytes);
    void remove(const QString& id);
    // Waits until everything asked for is done.
    void flush();

    // The backups of windows that have ended, oldest first. Their folders stay locked by this
    // store, so that no other window takes them too, until discardTaken().
    QList<Entry> takeOrphans();
    // What was taken is backed up here now (or was given up): delete it.
    void discardTaken();

private:
    bool ensureFolder();

    QThreadPool m_pool; // one thread, so that operations keep their order
    QString m_folder;   // empty until the first write
    std::unique_ptr<QLockFile> m_lock;
    std::vector<std::unique_ptr<QLockFile>> m_takenLocks;
    QStringList m_taken; // folders of ended windows, taken over
};

} // namespace aiedit::ui
