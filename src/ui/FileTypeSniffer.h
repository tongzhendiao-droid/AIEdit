#pragma once

#include "core/FileType.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <memory>
#include <optional>
#include <string_view>

class QFileInfo;

namespace aiedit::ui {

// What a file holds, told from its first bytes as opening it would tell.
struct SniffedType
{
    core::FileTypeInfo type;
    QString codec;             // for text: the encoding it would be read in
    std::string_view mismatch; // what it is instead, when its name says otherwise (core::contentMismatch)
    bool unreadable = false;

    // For a column: "PNG", "Text · UTF-8", "Empty", …
    QString label() const;
    // What is wrong with the name, if anything: "The name says PNG, but this is JPEG".
    QString mismatchNote(const QString& fileName) const;
};

// Tells the types of files on worker threads, a few at a time, for the folder panel's type
// column, and remembers them while a file keeps its size and time of change. Only regular
// files are read: never pipes or devices, which could block.
class FileTypeSniffer : public QObject
{
    Q_OBJECT

public:
    explicit FileTypeSniffer(QObject* parent = nullptr);
    ~FileTypeSniffer() override; // waits for the files being read

    // The legacy encoding to try first for text that is not UTF-8.
    void setLegacyCodec(std::string_view codec) { m_legacyCodec = codec; }
    // What is known of the file. If nothing is yet, it is looked at, and found() follows.
    std::optional<SniffedType> type(const QFileInfo& info);
    // Drops the files still waiting to be looked at, such as those of a folder no longer shown.
    void cancelPending();

    // Reads the first bytes of a file and tells its type, on the calling thread.
    static SniffedType sniff(const QString& path, std::string_view legacyCodec);

signals:
    void found(const QString& path);

private:
    struct Entry
    {
        qint64 size = 0;
        qint64 modified = 0;
        SniffedType type;
    };

    QThreadPool m_pool;
    QHash<QString, Entry> m_cache;
    QSet<QString> m_pending;
    int m_requests = 0;
    // Bumped by cancelPending(), so that answers to what was asked before are dropped.
    std::shared_ptr<std::atomic<int>> m_generation = std::make_shared<std::atomic<int>>(0);
    std::string_view m_legacyCodec = "windows-1252";
};

} // namespace aiedit::ui
