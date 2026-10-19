#pragma once

#include "core/TextCodec.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QStringView>

#include <optional>

namespace aiedit::ui {

// Text to and from the bytes of one of AIEdit's codecs (core::knownCodecs()), a call at a time:
// nothing carries over from one call to the next, so a character cut off at the end of the
// bytes is malformed.
//
// Qt converts Unicode and Latin-1 itself, and the other codecs through ICU where it has ICU.
// Its builds for Windows do not, so there the system's code pages convert the rest (GB18030 is
// code page 54936, Big5 950 and so on). Characters are then found by the codec's byte structure
// (core::charLength) and only mapped by the system, so that a malformed byte never takes the
// line break after it along.
//
// Like Qt's converters, a Codec is for one thread at a time.
class Codec
{
public:
    enum class Engine
    {
        Automatic, // Qt where it can, else the system's code pages
        CodePages, // the system's code pages only (Windows), for testing them where Qt could convert
    };

    // `flags` are Qt's, for the byte order marks of UTF-16 and UTF-32: ConvertInitialBom keeps
    // one as U+FEFF (it is skipped otherwise), WriteBom writes one. No other codec has them.
    explicit Codec(const core::TextCodec& codec, QStringConverter::Flags flags = QStringConverter::Flag::Default,
                   Engine engine = Engine::Automatic);

    const core::TextCodec& textCodec() const { return m_codec; }
    // False if neither Qt nor the system converts this codec here.
    bool isValid() const { return m_engine != Converter::None; }
    bool usesCodePage() const { return m_engine == Converter::CodePage; }

    // Malformed bytes become U+FFFD (or, from ICU for some codecs, U+001A); `malformed`, if
    // given, says whether there were any.
    QString decode(QByteArrayView bytes, bool* malformed = nullptr) const;
    // nullopt if some character cannot be written in the codec.
    std::optional<QByteArray> encode(QStringView text) const;

private:
    enum class Converter
    {
        None,
        Qt,
        CodePage,
    };

    core::TextCodec m_codec;
    QStringConverter::Flags m_flags;
    Converter m_engine = Converter::None;
    mutable std::optional<QStringDecoder> m_decoder;
    mutable std::optional<QStringEncoder> m_encoder;
};

} // namespace aiedit::ui
