#include "ui/TextDocument.h"

#include "core/FileSource.h"
#include "core/FileType.h"

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
    stopIndexing();
    m_source = std::move(source);
    startIndexing();
    emit sourceChanged();
}

void TextDocument::startIndexing()
{
    const auto data = bytes();
    const core::CharScheme scheme = m_codec->scheme;
    m_lineEndings = core::countLineEndings(data.first(std::min(data.size(), core::kSniffSize)), scheme);
    m_index = std::make_shared<core::LineIndex>(data, scheme, m_lineEndings.lineBreak(), core::bomLength(data, scheme));

    // Enough for the first screens at once; small files are finished right here.
    const bool complete = m_index->buildSome(kSynchronousIndexBytes);
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
