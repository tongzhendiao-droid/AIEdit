#include "ui/BackupStore.h"

#include "core/FileSource.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace aiedit::ui {

namespace {

constexpr int kMaxLockAttempts = 5;

QString contentFile(const QString& folder, const QString& id)
{
    return folder + QLatin1Char('/') + id + QStringLiteral(".bin");
}

QString metaFile(const QString& folder, const QString& id)
{
    return folder + QLatin1Char('/') + id + QStringLiteral(".json");
}

// What was typed is for the user's eyes only.
void restrictToOwner(const QString& folder)
{
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

bool writeFiles(const QString& folder, const QString& id, const QJsonObject& meta,
                std::span<const std::uint8_t> bytes)
{
    // The bytes first, so that a description always has its content beside it. Each file is
    // replaced whole, so a crash halfway leaves the previous backup.
    QSaveFile content(contentFile(folder, id));
    const auto size = static_cast<qint64>(bytes.size());
    if (!content.open(QIODevice::WriteOnly) ||
        content.write(reinterpret_cast<const char*>(bytes.data()), size) != size || !content.commit())
        return false;
    QSaveFile info(metaFile(folder, id));
    const QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    return info.open(QIODevice::WriteOnly) && info.write(json) == json.size() && info.commit();
}

std::unique_ptr<QLockFile> lockFor(const QString& path)
{
    auto lock = std::make_unique<QLockFile>(path);
    lock->setStaleLockTime(0); // stale only when the window holding it has ended
    return lock;
}

} // namespace

BackupStore::BackupStore()
{
    m_pool.setMaxThreadCount(1);
}

BackupStore::~BackupStore()
{
    flush();
    // Nothing kept: no folder left behind. Otherwise the next start finds it unlocked.
    if (!m_folder.isEmpty() && QDir(m_folder).isEmpty())
        QDir().rmdir(m_folder);
}

QString BackupStore::root()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/backup");
}

bool BackupStore::ensureFolder()
{
    if (!m_folder.isEmpty())
        return true;
    const QString base = root();
    if (!QDir().mkpath(base))
        return false;
    restrictToOwner(base);
    for (int attempt = 0; attempt < kMaxLockAttempts; ++attempt) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::unique_ptr<QLockFile> lock = lockFor(base + QLatin1Char('/') + id + QStringLiteral(".lock"));
        if (!lock->tryLock(0))
            continue;
        // Locked before the folder exists, so that no other window ever finds it unlocked.
        const QString folder = base + QLatin1Char('/') + id;
        if (!QDir().mkpath(folder))
            return false;
        restrictToOwner(folder);
        m_lock = std::move(lock);
        m_folder = folder;
        return true;
    }
    return false;
}

void BackupStore::write(const QString& id, const QJsonObject& meta,
                        std::shared_ptr<const core::FileSource> content)
{
    if (!ensureFolder())
        return;
    m_pool.start([folder = m_folder, id, meta, content = std::move(content)] {
        writeFiles(folder, id, meta, content->bytes());
    });
}

bool BackupStore::writeNow(const QString& id, const QJsonObject& meta, std::span<const std::uint8_t> bytes)
{
    flush();
    return ensureFolder() && writeFiles(m_folder, id, meta, bytes);
}

void BackupStore::remove(const QString& id)
{
    if (m_folder.isEmpty())
        return; // nothing was ever written
    m_pool.start([folder = m_folder, id] {
        // The description first: content without one is never restored.
        QFile::remove(metaFile(folder, id));
        QFile::remove(contentFile(folder, id));
    });
}

void BackupStore::flush()
{
    m_pool.waitForDone();
}

QList<BackupStore::Entry> BackupStore::takeOrphans()
{
    QList<Entry> entries;
    const QDir base(root());
    if (!base.exists())
        return entries;
    const QString own = m_folder.isEmpty() ? QString() : QFileInfo(m_folder).fileName();
    const QFileInfoList folders =
        base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed); // oldest first
    for (const QFileInfo& folder : folders) {
        if (folder.fileName() == own || m_taken.contains(folder.absoluteFilePath()))
            continue;
        std::unique_ptr<QLockFile> lock = lockFor(base.filePath(folder.fileName() + QStringLiteral(".lock")));
        if (!lock->tryLock(0))
            continue; // its window is still running
        const QDir dir(folder.absoluteFilePath());
        QList<Entry> found;
        for (const QFileInfo& meta : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
            Entry entry;
            entry.id = meta.completeBaseName();
            entry.contentPath = contentFile(dir.absolutePath(), entry.id);
            QFile file(meta.absoluteFilePath());
            if (!QFileInfo::exists(entry.contentPath) || !file.open(QIODevice::ReadOnly))
                continue;
            entry.meta = QJsonDocument::fromJson(file.readAll()).object();
            found << entry;
        }
        std::stable_sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
            return a.meta.value(QStringLiteral("order")).toInt() < b.meta.value(QStringLiteral("order")).toInt();
        });
        entries += found;
        m_taken << folder.absoluteFilePath();
        m_takenLocks.push_back(std::move(lock));
    }
    // Locks whose window ended before it made a folder.
    for (const QFileInfo& file : base.entryInfoList({QStringLiteral("*.lock")}, QDir::Files)) {
        if (!QFileInfo::exists(base.filePath(file.completeBaseName())) && file.completeBaseName() != own) {
            if (std::unique_ptr<QLockFile> lock = lockFor(file.absoluteFilePath()); lock->tryLock(0))
                lock->unlock(); // removes it
        }
    }
    return entries;
}

void BackupStore::discardTaken()
{
    for (const QString& folder : std::as_const(m_taken))
        QDir(folder).removeRecursively();
    m_taken.clear();
    m_takenLocks.clear(); // unlocking removes the lock files
}

} // namespace aiedit::ui
