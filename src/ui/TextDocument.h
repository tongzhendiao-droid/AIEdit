#pragma once

#include "core/EditBuffer.h"
#include "core/LineIndex.h"
#include "core/TextCodec.h"
#include "ui/TextDecoder.h"

#include <QObject>
#include <QTimer>

#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace aiedit::core {
class FileSource;
}

namespace aiedit::ui {

// A file seen as text in one codec: the decoder plus a line index that is built on a
// background thread. Small files are indexed synchronously so they appear complete at once.
//
// It can also be edited. The first change copies the bytes into memory (core::EditBuffer);
// from then on source() is a snapshot of the edited bytes, replaced with every change.
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
    // Shows a fresh snapshot of the file (after it changed on disk), keeping the codec. Edits
    // not saved are dropped.
    void setSource(std::shared_ptr<const core::FileSource> source);

    // Replaces [start, end) with text written in the document's codec and returns how many
    // bytes that took; nothing, changing nothing, if the codec cannot write the text.
    std::optional<std::uint64_t> replace(std::uint64_t start, std::uint64_t end, const QString& text,
                                         core::EditBuffer::Merge merge = core::EditBuffer::Merge::No);
    // The same with bytes, for changes that must leave the bytes around them exactly as they are.
    void replaceBytes(std::uint64_t start, std::uint64_t end, std::span<const std::uint8_t> bytes,
                      core::EditBuffer::Merge merge = core::EditBuffer::Merge::No);
    bool canUndo() const;
    bool canRedo() const;
    // Undo and redo return the range the step now covers (what undo restored, what redo put
    // back), to place the caret.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> undo();
    std::optional<std::pair<std::uint64_t, std::uint64_t>> redo();
    // The next change starts a new undo step.
    void breakUndoMerge();
    bool isModified() const;
    // The bytes were written to the file: unmodified from here on.
    void setSaved();
    // Text in the document's codec, if it can be written in it.
    std::optional<QByteArray> encode(const QString& text) const;
    // What Enter inserts: the line terminator the file uses.
    QString lineTerminator() const;
    // Ends every line with `terminator` ("\n", "\r\n" or "\r"), as one edit. False if every
    // line ended so already.
    bool convertLineEndings(const QString& terminator);

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
    // `removed` bytes at offset were replaced with `inserted` ones; source() and index() show
    // the result already.
    void edited(std::uint64_t offset, std::uint64_t removed, std::uint64_t inserted);
    void modificationChanged(bool modified);

private:
    // After an edit everything is indexed at once (the document is small enough to edit), so
    // that the rows at the caret are there for what comes next.
    void startIndexing(bool synchronous = false);
    void stopIndexing();
    void pollIndexer();
    // Shows the edit buffer's content after a change.
    void applyEdit(std::uint64_t offset, std::uint64_t removed, std::uint64_t inserted, bool wasModified);

    std::shared_ptr<const core::FileSource> m_source;
    const core::TextCodec* m_codec;
    std::unique_ptr<TextDecoder> m_decoder;
    std::unique_ptr<core::EditBuffer> m_edits; // from the first change on
    std::shared_ptr<core::LineIndex> m_index;
    core::LineEndings m_lineEndings;
    std::thread m_indexer;
    QTimer m_pollTimer;
    std::uint64_t m_reportedRows = 0;
};

} // namespace aiedit::ui
