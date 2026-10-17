#include "core/TextCodec.h"
#include "ui/Codec.h"
#include "ui/TextDecoder.h"

#include <QTest>

#include <algorithm>
#include <span>
#include <vector>

using namespace aiedit;
using ui::Codec;

Q_DECLARE_METATYPE(ui::Codec::Engine)

namespace {

const core::TextCodec& codec(const char* name)
{
    const core::TextCodec* found = core::findCodec(name);
    Q_ASSERT(found);
    return *found;
}

// Qt's converters (with ICU where Qt has it); on Windows also the system's code pages, which
// Qt's own builds for Windows use for every codec but Unicode and Latin-1.
std::vector<std::pair<const char*, Codec::Engine>> engines()
{
    std::vector<std::pair<const char*, Codec::Engine>> result{{"qt", Codec::Engine::Automatic}};
#ifdef Q_OS_WIN
    result.emplace_back("code pages", Codec::Engine::CodePages);
#endif
    return result;
}

std::span<const std::uint8_t> bytesOf(const QByteArray& bytes)
{
    return {reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())};
}

bool isReplacement(QChar c)
{
    return c == QChar(0xFFFD) || c == QChar(0x1A); // ICU puts U+001A for some codecs
}

} // namespace

class TestCodec : public QObject
{
    Q_OBJECT

private slots:
    void convertsEveryCodec();
    void convertsText_data();
    void convertsText();
    void refusesWhatACodecLacks_data();
    void refusesWhatACodecLacks();
    void keepsLineBreaksAfterMalformedBytes_data();
    void keepsLineBreaksAfterMalformedBytes();
    void handlesByteOrderMarks();
    void decodesCharacterByCharacter();
};

void TestCodec::convertsEveryCodec()
{
    // Every codec AIEdit offers can be read and written here.
    for (const core::TextCodec& known : core::knownCodecs()) {
        const Codec converter(known);
        QVERIFY2(converter.isValid(), known.name.data());
#ifdef Q_OS_WIN
        if (known.codePage != 0)
            QVERIFY2(Codec(known, {}, Codec::Engine::CodePages).isValid(), known.name.data());
#endif
    }
}

void TestCodec::convertsText_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<Codec::Engine>("engine");

    // Characters that ICU's tables and the Windows code pages map alike.
    const struct
    {
        const char* name;
        const char* text;
        const char* hex;
    } rows[] = {
        {"UTF-8", "aä中😀", "61 c3 a4 e4 b8 ad f0 9f 98 80"},
        {"UTF-16LE", "a中😀", "61 00 2d 4e 3d d8 00 de"},
        {"UTF-16BE", "a中😀", "00 61 4e 2d d8 3d de 00"},
        {"UTF-32LE", "a😀", "61 00 00 00 00 f6 01 00"},
        {"UTF-32BE", "a😀", "00 00 00 61 00 01 f6 00"},
        {"GB18030", "中文，€é𠀀äµ", "d6 d0 ce c4 a3 ac a2 e3 a8 a6 95 32 82 36 81 30 8a 31 81 30 85 38"},
        {"Big5", "中文，€", "a4 a4 a4 e5 a1 41 a3 e1"},
        {"Shift_JIS", "日本語ｱ～", "93 fa 96 7b 8c ea b1 81 60"},
        {"EUC-JP", "日本語ｱ", "c6 fc cb dc b8 ec 8e b1"},
        {"EUC-KR", "한국어", "c7 d1 b1 b9 be ee"},
        {"windows-1252", "café €‰", "63 61 66 e9 20 80 89"},
        {"ISO-8859-1", "café ÿ", "63 61 66 e9 20 ff"},
        {"ISO-8859-15", "€Šž", "a4 a6 b8"},
        {"windows-1250", "Łódź", "a3 f3 64 9f"},
        {"ISO-8859-2", "Łódź", "a3 f3 64 bc"},
        {"windows-1251", "Привет", "cf f0 e8 e2 e5 f2"},
        {"KOI8-R", "Привет", "f0 d2 c9 d7 c5 d4"},
        {"windows-1253", "Ελλάδα", "c5 eb eb dc e4 e1"},
        {"windows-1254", "İğş", "dd f0 fe"},
        {"windows-1256", "مرحبا", "e3 d1 cd c8 c7"},
    };
    QCOMPARE(std::size(rows), core::knownCodecs().size());
    for (const auto& [engineName, engine] : engines()) {
        for (const auto& row : rows) {
            if (engine == Codec::Engine::CodePages && codec(row.name).codePage == 0)
                continue;
            QTest::addRow("%s %s", row.name, engineName)
                << QString::fromLatin1(row.name) << QString::fromUtf8(row.text)
                << QByteArray::fromHex(row.hex) << engine;
        }
    }
}

void TestCodec::convertsText()
{
    QFETCH(QString, name);
    QFETCH(QString, text);
    QFETCH(QByteArray, bytes);
    QFETCH(Codec::Engine, engine);

    const Codec converter(codec(name.toLatin1().constData()), {}, engine);
    QVERIFY(converter.isValid());
    bool malformed = true;
    QCOMPARE(converter.decode(bytes, &malformed), text);
    QVERIFY(!malformed);
    QCOMPARE(converter.encode(text), std::optional<QByteArray>(bytes));
    // Again with the same converter: nothing is left over from the first time.
    QCOMPARE(converter.decode(bytes), text);
    QCOMPARE(converter.encode(text), std::optional<QByteArray>(bytes));
}

void TestCodec::refusesWhatACodecLacks_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("text");
    QTest::addColumn<Codec::Engine>("engine");

    const struct
    {
        const char* name;
        QString text;
    } rows[] = {
        {"UTF-8", QString(QChar(0xDC00)) + QLatin1Char('a')}, // half of a surrogate pair
        {"UTF-8", QStringLiteral("a") + QChar(0xD800)},
        {"GB18030", QStringLiteral("a") + QChar(0xDC00)},
        {"Big5", QStringLiteral("😀")},
        {"Big5", QStringLiteral("한")},
        {"Shift_JIS", QStringLiteral("한")},
        {"Shift_JIS", QStringLiteral("¥")}, // not even as the backslash Windows would make of it
        {"EUC-JP", QStringLiteral("한")},
        {"EUC-KR", QStringLiteral("똠")}, // only in Windows' larger code page 949
        {"windows-1252", QStringLiteral("中")},
        {"windows-1252", QStringLiteral("ā")}, // not as "a", either
        {"ISO-8859-1", QStringLiteral("€")},
        {"windows-1251", QStringLiteral("中")},
        {"KOI8-R", QStringLiteral("é")},
    };
    for (const auto& [engineName, engine] : engines()) {
        for (const auto& row : rows) {
            if (engine == Codec::Engine::CodePages && codec(row.name).codePage == 0)
                continue;
            QTest::addRow("%s %s %s", row.name, qPrintable(row.text), engineName)
                << QString::fromLatin1(row.name) << row.text << engine;
        }
    }
}

void TestCodec::refusesWhatACodecLacks()
{
    QFETCH(QString, name);
    QFETCH(QString, text);
    QFETCH(Codec::Engine, engine);

    const Codec converter(codec(name.toLatin1().constData()), {}, engine);
    QCOMPARE(converter.encode(text), std::nullopt);
    // What it has it still writes.
    QCOMPARE(converter.encode(QStringLiteral("ok")), std::optional<QByteArray>("ok"));
}

void TestCodec::keepsLineBreaksAfterMalformedBytes_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<Codec::Engine>("engine");

    // The first byte of a character, then a line break where the rest should be.
    const struct
    {
        const char* name;
        QByteArray bytes;
    } rows[] = {
        {"UTF-8", QByteArray("a\xE4\xB8\nb")},      {"GB18030", QByteArray("a\x81\nb")},
        {"Big5", QByteArray("a\xA4\nb")},           {"Shift_JIS", QByteArray("a\x81\nb")},
        {"EUC-JP", QByteArray("a\x8E\nb")},         {"EUC-JP", QByteArray("a\x8F\xB0\nb")},
        {"EUC-KR", QByteArray("a\xC7\nb")},
    };
    for (const auto& [engineName, engine] : engines()) {
        for (const auto& row : rows)
            QTest::addRow("%s %s %s", row.name, row.bytes.toHex(' ').constData(), engineName)
                << QString::fromLatin1(row.name) << row.bytes << engine;
    }
}

void TestCodec::keepsLineBreaksAfterMalformedBytes()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, bytes);
    QFETCH(Codec::Engine, engine);

    const Codec converter(codec(name.toLatin1().constData()), {}, engine);
    bool malformed = false;
    const QString text = converter.decode(bytes, &malformed);
    QVERIFY(malformed);
    QVERIFY2(text.startsWith(u'a') && text.endsWith(QStringLiteral("\nb")) && text.count(u'\n') == 1,
             qPrintable(text));
    const QStringView middle = QStringView(text).sliced(1, text.size() - 3);
    QVERIFY(!middle.isEmpty());
    QVERIFY(std::all_of(middle.begin(), middle.end(), isReplacement));

    // Nor does a character cut off by the end of the bytes go unnoticed.
    malformed = false;
    converter.decode(bytes.first(bytes.indexOf('\n')), &malformed);
    QVERIFY(malformed);
}

void TestCodec::handlesByteOrderMarks()
{
    // A byte order mark is skipped unless asked otherwise, and written for UTF-16 if asked.
    const QByteArray withBom("\xEF\xBB\xBF"
                             "a");
    QCOMPARE(Codec(core::utf8Codec()).decode(withBom), QStringLiteral("a"));
    QCOMPARE(Codec(core::utf8Codec(), QStringConverter::Flag::ConvertInitialBom).decode(withBom),
             QStringLiteral("﻿a"));
    QCOMPARE(Codec(codec("UTF-16LE"), QStringConverter::Flag::WriteBom).encode(QStringLiteral("a")),
             std::optional<QByteArray>(QByteArray("\xFF\xFE"
                                                  "a\0",
                                                  4)));
    QCOMPARE(Codec(codec("UTF-16LE")).encode(QStringLiteral("a")), std::optional<QByteArray>(QByteArray("a\0", 2)));
    // GB18030's mark is a character like any other.
    for (const auto& [engineName, engine] : engines())
        QCOMPARE(Codec(codec("GB18030"), {}, engine).decode(QByteArray("\x84\x31\x95\x33"
                                                                       "a")),
                 QStringLiteral("﻿a"));
}

void TestCodec::decodesCharacterByCharacter()
{
    // The text view's decoder: malformed bytes and characters a codec leaves undefined are U+FFFD
    // (ICU's U+001A included), each with its place in the bytes.
    const ui::TextDecoder gb(codec("GB18030"));
    const ui::DecodedText text = gb.decode(bytesOf(QByteArray("\xD6\xD0\x81\nA")));
    QCOMPARE(text.text, QStringLiteral("中�␊A"));
    QCOMPARE(text.byteOffsets, (std::vector<std::uint32_t>{0, 2, 3, 4, 5}));
    QCOMPARE(gb.decodePlain(bytesOf(QByteArray("\xD6\xD0\x81\nA"))).left(1), QStringLiteral("中"));

    const ui::TextDecoder sjis(codec("Shift_JIS"));
    QCOMPARE(sjis.decode(bytesOf(QByteArray("\x85\x40\x81\x7F"))).text, QStringLiteral("��␡"));
    const ui::TextDecoder kr(codec("EUC-KR"));
    QCOMPARE(kr.decode(bytesOf(QByteArray("\xA2\xE8\xC7\xD1"))).text, QStringLiteral("�한"));
    const ui::TextDecoder greek(codec("windows-1253"));
    QCOMPARE(greek.decode(bytesOf(QByteArray("\xC5\xD2"))).text, QStringLiteral("Ε�"));
}

QTEST_GUILESS_MAIN(TestCodec)
#include "tst_codec.moc"
