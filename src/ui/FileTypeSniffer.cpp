#include "ui/FileTypeSniffer.h"

#include "core/TextCodec.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

namespace aiedit::ui {

namespace {

// A folder can hold more files than that; the cache then starts afresh.
constexpr qsizetype kMaxCached = 50'000;
constexpr int kThreads = 2;

QString fromView(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

QString SniffedType::label() const
{
    if (unreadable)
        return FileTypeSniffer::tr("Unreadable");
    switch (type.kind) {
    case core::ContentKind::Empty:
        return FileTypeSniffer::tr("Empty");
    case core::ContentKind::Binary:
        return type.format.empty() ? FileTypeSniffer::tr("Binary") : fromView(type.format);
    case core::ContentKind::Text:
        if (type.encoding == core::Encoding::Ascii)
            return FileTypeSniffer::tr("Text · ASCII");
        return type.hasBom ? FileTypeSniffer::tr("Text · %1 with BOM").arg(codec)
                           : FileTypeSniffer::tr("Text · %1").arg(codec);
    }
    return {};
}

QString SniffedType::mismatchNote(const QString& fileName) const
{
    if (mismatch.empty())
        return {};
    const QByteArray name = fileName.toUtf8();
    const QString promised = fromView(core::formatForName({name.constData(), static_cast<std::size_t>(name.size())}));
    const QString actual = fromView(mismatch);
    const QString text = FileTypeSniffer::tr("text");
    return FileTypeSniffer::tr("The name says %1, but this is %2")
        .arg(promised == QLatin1String("text") ? text : promised, actual == QLatin1String("text") ? text : actual);
}

FileTypeSniffer::FileTypeSniffer(QObject* parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(kThreads);
}

FileTypeSniffer::~FileTypeSniffer()
{
    m_pool.clear();
    m_pool.waitForDone();
}

std::optional<SniffedType> FileTypeSniffer::type(const QFileInfo& info)
{
    if (!info.isFile())
        return std::nullopt;
    const QString path = info.absoluteFilePath();
    const qint64 size = info.size();
    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    if (const auto it = m_cache.constFind(path); it != m_cache.cend() && it->size == size && it->modified == modified)
        return it->type;
    if (m_pending.contains(path))
        return std::nullopt;
    if (m_cache.size() >= kMaxCached)
        m_cache.clear();

    m_pending.insert(path);
    const int generation = m_generation->load();
    // The files asked for last are on screen now: they go first.
    const int priority = ++m_requests;
    m_pool.start([this, path, size, modified, generation, generations = m_generation, codec = m_legacyCodec] {
        if (generations->load() != generation)
            return;
        const SniffedType found = sniff(path, codec);
        QMetaObject::invokeMethod(
            this,
            [this, path, size, modified, generation, found] {
                if (m_generation->load() != generation)
                    return;
                m_pending.remove(path);
                m_cache.insert(path, {size, modified, found});
                emit this->found(path);
            },
            Qt::QueuedConnection);
    }, priority);
    return std::nullopt;
}

void FileTypeSniffer::cancelPending()
{
    m_pool.clear();
    ++*m_generation;
    m_pending.clear();
}

SniffedType FileTypeSniffer::sniff(const QString& path, std::string_view legacyCodec)
{
    SniffedType result;
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
        result.unreadable = true;
        return result;
    }
    // One byte more than is looked at tells detectFileType() that the file goes on, so that a
    // character cut off at the end of the sample is not taken for broken UTF-8.
    const QByteArray head = file.read(static_cast<qint64>(core::kSniffSize) + 1);
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(head.constData()),
                                              static_cast<std::size_t>(head.size()));
    result.type = core::detectFileType(bytes);
    if (result.type.kind == core::ContentKind::Text) {
        const core::TextCodec& codec = core::defaultCodec(result.type, bytes, legacyCodec);
        result.codec = QString::fromLatin1(codec.name.data(), static_cast<qsizetype>(codec.name.size()));
    }
    const QByteArray name = QFileInfo(path).fileName().toUtf8();
    result.mismatch = core::contentMismatch({name.constData(), static_cast<std::size_t>(name.size())}, result.type);
    return result;
}

} // namespace aiedit::ui
