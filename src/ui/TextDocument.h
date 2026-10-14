#pragma once

#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/TextDecoder.h"

#include <QObject>
#include <QTimer>

#include <memory>
#include <thread>

namespace cppedit::core {
class FileSource;
}

namespace cppedit::ui {

// A file seen as text in one codec: the decoder plus a line index that is built on a
// background thread. Small files are indexed synchronously so they appear complete at once.
class TextDocument : public QObject
{
    Q_OBJECT

public:
    // Files up to this size are indexed before the constructor returns.
    static constexpr std::uint64_t kSynchronousIndexBytes = 4ull * 1024 * 1024;

    TextDocument(std::shared_ptr<const core::FileSource> source, const core::TextCodec& codec,
                 QObject* parent = nullptr);
    ~TextDocument() override;

    const std::shared_ptr<const core::FileSource>& source() const { return m_source; }
    std::span<const std::uint8_t> bytes() const;
    const core::TextCodec& codec() const { return *m_codec; }
    // Re-decodes and re-indexes the file; views must drop anything derived from index().
    void setCodec(const core::TextCodec& codec);
    // Shows a fresh snapshot of the file (after it changed on disk), keeping the codec.
    void setSource(std::shared_ptr<const core::FileSource> source);

    const TextDecoder& decoder() const { return *m_decoder; }
    // Replaced by setCodec(); do not keep references across codecChanged().
    const core::LineIndex& index() const { return *m_index; }
    // For background work that must keep the index alive across a codec change.
    std::shared_ptr<const core::LineIndex> sharedIndex() const { return m_index; }
    bool isIndexing() const { return m_indexer.joinable(); }
    // Line terminators counted in the first part of the file.
    const core::LineEndings& lineEndings() const { return m_lineEndings; }
    std::uint64_t textStart() const { return m_index->textStart(); }

    // The start of the character containing offset, clamped to [textStart(), size].
    std::uint64_t snapToCharStart(std::uint64_t offset) const;

signals:
    void indexGrew();
    void indexFinished();
    void codecChanged();
    void sourceChanged();

private:
    void startIndexing();
    void stopIndexing();
    void pollIndexer();

    std::shared_ptr<const core::FileSource> m_source;
    const core::TextCodec* m_codec;
    std::unique_ptr<TextDecoder> m_decoder;
    std::shared_ptr<core::LineIndex> m_index;
    core::LineEndings m_lineEndings;
    std::thread m_indexer;
    QTimer m_pollTimer;
    std::uint64_t m_reportedRows = 0;
};

} // namespace cppedit::ui
