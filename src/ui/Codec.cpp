#include "ui/Codec.h"

#ifdef Q_OS_WIN
#include <algorithm>
#include <cstdint>
#include <span>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace aiedit::ui {

namespace {

#ifdef Q_OS_WIN

constexpr char16_t kReplacement = 0xFFFD;
// The most bytes, or UTF-16 code units, handed to the system in one call.
constexpr std::size_t kMaxChunk = std::size_t{1} << 20;

// A byte that the codec's structure leaves on its own and that is still a character: ASCII, and
// Shift_JIS's half-width katakana. (In single-byte codecs every byte is one.)
bool isSingleByteCharacter(core::CharScheme scheme, std::uint8_t byte)
{
    return byte < 0x80 || scheme == core::CharScheme::SingleByte ||
           (scheme == core::CharScheme::ShiftJis && byte >= 0xA1 && byte <= 0xDF);
}

// Appends whole characters as the code page maps them; false, appending nothing, if it does not
// map one of them.
bool appendMapped(unsigned codePage, const std::uint8_t* bytes, std::size_t size, QString& out)
{
    if (size == 0)
        return true;
    // A byte gives at most one UTF-16 code unit: a double-byte character gives one, the four
    // bytes of a GB18030 character at most two.
    const qsizetype at = out.size();
    out.resize(at + static_cast<qsizetype>(size));
    const int written = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes),
                                              static_cast<int>(size), reinterpret_cast<wchar_t*>(out.data() + at),
                                              static_cast<int>(size));
    out.resize(at + std::max(written, 0));
    return written > 0;
}

void appendDecoded(const core::TextCodec& codec, std::span<const std::uint8_t> data, QString& out, bool& malformed)
{
    // A character on its own, or U+FFFD if the code page has none for it.
    const auto single = [&](std::size_t at, std::size_t length) {
        if (!appendMapped(codec.codePage, data.data() + at, length, out)) {
            out += QChar(kReplacement);
            malformed = true;
        }
    };
    // Whole characters in one go, or one by one if the code page fails one of them.
    const auto run = [&](std::size_t from, std::size_t to) {
        if (appendMapped(codec.codePage, data.data() + from, to - from, out))
            return;
        for (std::size_t i = from; i < to;) {
            const std::size_t length = core::charLength(data.subspan(i, to - i), codec.scheme);
            single(i, length);
            i += length;
        }
    };

    std::size_t start = 0;
    for (std::size_t i = 0; i < data.size();) {
        const std::size_t length = core::charLength(data.subspan(i), codec.scheme);
        // Not part of a character by the codec's structure: given on its own, it cannot take the
        // byte after it (a line break) along.
        const bool stray = length == 1 && !isSingleByteCharacter(codec.scheme, data[i]);
        if (stray || i - start >= kMaxChunk) {
            run(start, i);
            start = i;
        }
        if (stray) {
            single(i, 1);
            start = i + 1;
        }
        i += length;
    }
    run(start, data.size());
}

QString decodeWithCodePage(const core::TextCodec& codec, QByteArrayView bytes, bool* malformed)
{
    QString out;
    out.reserve(bytes.size());
    bool bad = false;
    appendDecoded(codec, {reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<std::size_t>(bytes.size())},
                  out, bad);
    // Bytes a single-byte code page leaves undefined come out as private-use characters (0xD2 of
    // windows-1253 as U+F8FA), which none of them has otherwise: they are U+FFFD, as from ICU.
    if (codec.scheme == core::CharScheme::SingleByte) {
        for (QChar& c : out) {
            if (c.unicode() >= 0xE000 && c.unicode() <= 0xF8FF) {
                c = QChar(kReplacement);
                bad = true;
            }
        }
    }
    if (malformed)
        *malformed = bad;
    return out;
}

std::optional<QByteArray> encodeWithCodePage(const core::TextCodec& codec, QStringView text)
{
    QByteArray out;
    for (qsizetype at = 0; at < text.size();) {
        qsizetype size = std::min(text.size() - at, static_cast<qsizetype>(kMaxChunk));
        if (at + size < text.size() && text[at + size - 1].isHighSurrogate())
            --size; // a surrogate pair stays together
        const auto* wide = reinterpret_cast<const wchar_t*>(text.utf16() + at);
        const int length =
            ::WideCharToMultiByte(codec.codePage, 0, wide, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return std::nullopt;
        const qsizetype used = out.size();
        out.resize(used + length);
        ::WideCharToMultiByte(codec.codePage, 0, wide, static_cast<int>(size), out.data() + used, length, nullptr,
                              nullptr);
        at += size;
    }
    // What the code page lacks comes out as "?", or as a character like it ("best fit"): only
    // bytes that decode back to the text are the text.
    bool malformed = false;
    const QString back = decodeWithCodePage(codec, out, &malformed);
    if (malformed || back != text)
        return std::nullopt;
    return out;
}

#endif

} // namespace

Codec::Codec(const core::TextCodec& codec, QStringConverter::Flags flags, Engine engine)
    : m_codec(codec), m_flags(flags)
{
    if (engine == Engine::Automatic) {
        // Stateless, Qt's decoders count a character cut off at the end as malformed, instead of
        // keeping it for the next call. Its encoders are left as they are: stateless, the one
        // for Latin-1 no longer says when it writes "?" for a character it lacks.
        m_decoder.emplace(codec.name.data(), m_flags | QStringConverter::Flag::Stateless);
        if (m_decoder->isValid()) {
            m_engine = Converter::Qt;
            return;
        }
        m_decoder.reset();
    }
#ifdef Q_OS_WIN
    if (codec.codePage != 0 && ::IsValidCodePage(codec.codePage))
        m_engine = Converter::CodePage;
#endif
}

QString Codec::decode(QByteArrayView bytes, bool* malformed) const
{
    switch (m_engine) {
    case Converter::Qt: {
        // Through ICU, Qt's converters keep counting errors past resetState(): after one, start
        // afresh.
        if (m_decoder->hasError())
            m_decoder.emplace(m_codec.name.data(), m_flags | QStringConverter::Flag::Stateless);
        else
            m_decoder->resetState();
        QString text = m_decoder->decode(bytes);
        if (malformed)
            *malformed = m_decoder->hasError();
        return text;
    }
    case Converter::CodePage:
#ifdef Q_OS_WIN
        return decodeWithCodePage(m_codec, bytes, malformed);
#else
        break;
#endif
    case Converter::None:
        break;
    }
    if (malformed)
        *malformed = !bytes.isEmpty();
    return {};
}

std::optional<QByteArray> Codec::encode(QStringView text) const
{
    switch (m_engine) {
    case Converter::Qt: {
        // A high surrogate at the end would wait in the encoder for the rest of its pair.
        if (!text.isEmpty() && text.back().isHighSurrogate())
            return std::nullopt;
        if (!m_encoder || m_encoder->hasError())
            m_encoder.emplace(m_codec.name.data(), m_flags);
        else
            m_encoder->resetState();
        QByteArray bytes = m_encoder->encode(text);
        if (m_encoder->hasError())
            return std::nullopt;
        return bytes;
    }
    case Converter::CodePage:
#ifdef Q_OS_WIN
        return encodeWithCodePage(m_codec, text);
#else
        break;
#endif
    case Converter::None:
        break;
    }
    return std::nullopt;
}

} // namespace aiedit::ui
