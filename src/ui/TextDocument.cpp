#include "ui/TextDocument.h"

#include "core/FileSource.h"
#include "core/FileType.h"

#include <QStringEncoder>

#include <algorithm>

namespace aiedit::ui {

namespace {

constexpr int kPollIntervalMs = 100;

} // namespace

TextDocument::TextDocument(std::shared_ptr<const core::FileSource> source, const core::TextCodec& codec,
                           QObject* parent)
    : QObject(parent), m_source(std::move(source)), m_codec(&codec), m_decoder(std::make_unique<TextDecoder>(codec))
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &TextDocument::pollIndexer);
    startIndexing();
}

TextDocument::~TextDocument()
{
    stopIndexing();
}

std::span<const std::uint8_t> TextDocument::bytes() const
{
    return m_source->bytes();
}

void TextDocument::setCodec(const core::TextCodec& codec)
{
    stopIndexing();
    m_codec = &codec;
    m_decoder = std::make_unique<TextDecoder>(codec);
    startIndexing();
    emit codecChanged();
}

void TextDocument::setSource(std::shared_ptr<const core::FileSource> source)
{
    const bool wasModified = isModified();
    stopIndexing();
    m_source = std::move(source);
    m_edits.reset();
    startIndexing();
    emit sourceChanged();
    if (wasModified)
        emit modificationChanged(false);
}

std::optional<QByteArray> TextDocument::encode(const QString& text) const
{
    QStringEncoder encoder(m_codec->name.data());
    QByteArray bytes = encoder.encode(text);
    if (encoder.hasError())
        return std::nullopt;
    return bytes;
}

QString TextDocument::lineTerminator() const
{
    const core::LineEndings& endings = m_lineEndings;
    if (endings.crlf > endings.lf && endings.crlf >= endings.cr)
        return QStringLiteral("\r\n");
    if (endings.cr > endings.lf && endings.cr > endings.crlf)
        return QStringLiteral("\r");
    return QStringLiteral("\n");
}

std::optional<std::uint64_t> TextDocument::replace(std::uint64_t start, std::uint64_t end, const QString& text,
                                                   core::EditBuffer::Merge merge)
{
    const std::optional<QByteArray> encoded = encode(text);
    if (!encoded)
        return std::nullopt;
    replaceBytes(start, end,
                 {reinterpret_cast<const std::uint8_t*>(encoded->constData()), static_cast<std::size_t>(encoded->size())},
                 merge);
    return static_cast<std::uint64_t>(encoded->size());
}

void TextDocument::replaceBytes(std::uint64_t start, std::uint64_t end, std::span<const std::uint8_t> bytes,
                                core::EditBuffer::Merge merge)
{
    if (!m_edits)
        m_edits = std::make_unique<core::EditBuffer>(this->bytes());
    const bool wasModified = m_edits->isModified();
    end = std::max(start, end);
    m_edits->replace(start, end - start, bytes, merge);
    applyEdit(start, end - start, bytes.size(), wasModified);
}

bool TextDocument::convertLineEndings(const QString& terminator)
{
    const std::optional<QByteArray> encoded = encode(terminator);
    if (!encoded)
        return false;
    if (!m_index->isComplete()) { // just opened and still indexing in the background
        stopIndexing();
        startIndexing(true);
    }

    // One change from the first terminator that differs to the last, keeping the text between.
    const auto data = bytes();
    const std::span<const std::uint8_t> target(reinterpret_cast<const std::uint8_t*>(encoded->constData()),
                                               static_cast<std::size_t>(encoded->size()));
    std::vector<std::uint8_t> replacement;
    std::optional<std::uint64_t> start;
    std::uint64_t end = 0;
    constexpr std::size_t kRowsAtOnce = 4096;
    for (std::uint64_t first = 0; first < m_index->rowCount(); first += kRowsAtOnce) {
        for (const core::Row& row : m_index->rows(first, kRowsAtOnce)) {
            const auto current = data.subspan(row.contentEnd, static_cast<std::size_t>(row.end - row.contentEnd));
            if (current.empty() || std::equal(current.begin(), current.end(), target.begin(), target.end()))
                continue;
            if (!start)
                start = end = row.contentEnd;
            replacement.insert(replacement.end(), data.begin() + static_cast<std::ptrdiff_t>(end),
                               data.begin() + static_cast<std::ptrdiff_t>(row.contentEnd));
            replacement.insert(replacement.end(), target.begin(), target.end());
            end = row.end;
        }
    }
    if (!start)
        return false;
    breakUndoMerge();
    replaceBytes(*start, end, replacement);
    return true;
}

bool TextDocument::canUndo() const
{
    return m_edits && m_edits->canUndo();
}

bool TextDocument::canRedo() const
{
    return m_edits && m_edits->canRedo();
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> TextDocument::undo()
{
    if (!canUndo())
        return std::nullopt;
    const bool wasModified = m_edits->isModified();
    const core::Edit edit = *m_edits->undo();
    applyEdit(edit.offset, edit.removed.size(), edit.inserted.size(), wasModified);
    return std::pair{edit.offset, edit.offset + edit.inserted.size()};
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> TextDocument::redo()
{
    if (!canRedo())
        return std::nullopt;
    const bool wasModified = m_edits->isModified();
    const core::Edit edit = *m_edits->redo();
    applyEdit(edit.offset, edit.removed.size(), edit.inserted.size(), wasModified);
    return std::pair{edit.offset, edit.offset + edit.inserted.size()};
}

void TextDocument::breakUndoMerge()
{
    if (m_edits)
        m_edits->breakMerge();
}

bool TextDocument::isModified() const
{
    return m_edits && m_edits->isModified();
}

void TextDocument::setSaved()
{
    if (!isModified())
        return;
    m_edits->setSaved();
    emit modificationChanged(false);
}

void TextDocument::applyEdit(std::uint64_t offset, std::uint64_t removed, std::uint64_t inserted, bool wasModified)
{
    stopIndexing();
    m_source = core::FileSource::fromBytes(m_source->path(), m_edits->content());
    startIndexing(true);
    emit edited(offset, removed, inserted);
    if (m_edits->isModified() != wasModified)
        emit modificationChanged(!wasModified);
}

void TextDocument::startIndexing(bool synchronous)
{
    const auto data = bytes();
    const core::CharScheme scheme = m_codec->scheme;
    m_lineEndings = core::countLineEndings(data.first(std::min(data.size(), core::kSniffSize)), scheme);
    m_index = std::make_shared<core::LineIndex>(data, scheme, m_lineEndings.lineBreak(), core::bomLength(data, scheme));

    // Enough for the first screens at once; small files are finished right here.
    const bool complete = synchronous ? m_index->build() : m_index->buildSome(kSynchronousIndexBytes);
    m_reportedRows = m_index->rowCount();
    if (complete)
        return;
    m_indexer = std::thread([index = m_index.get()] { index->build(); });
    m_pollTimer.start();
}

void TextDocument::stopIndexing()
{
    m_pollTimer.stop();
    if (m_indexer.joinable()) {
        m_index->cancel();
        m_indexer.join();
    }
}

void TextDocument::pollIndexer()
{
    const bool complete = m_index->isComplete();
    const std::uint64_t rows = m_index->rowCount();
    if (rows != m_reportedRows) {
        m_reportedRows = rows;
        emit indexGrew();
    }
    if (complete) {
        m_pollTimer.stop();
        if (m_indexer.joinable())
            m_indexer.join();
        emit indexFinished();
    }
}

std::uint64_t TextDocument::snapToCharStart(std::uint64_t offset) const
{
    return core::snapToCharStart(*m_index, offset);
}

} // namespace aiedit::ui
