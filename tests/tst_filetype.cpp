#include "core/FileType.h"

#include <QTest>

#include <string>
#include <vector>

using namespace aiedit::core;

Q_DECLARE_METATYPE(ContentKind)
Q_DECLARE_METATYPE(Encoding)

namespace {

std::span<const std::uint8_t> asBytes(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

} // namespace

class TestFileType : public QObject
{
    Q_OBJECT

private slots:
    void detect_data();
    void detect();
    void detectLongUtf8CutMidCharacter();
    void utf8Validation_data();
    void utf8Validation();
    void formatForName_data();
    void formatForName();
    void contentMismatch_data();
    void contentMismatch();
};

void TestFileType::detect_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<ContentKind>("kind");
    QTest::addColumn<Encoding>("encoding");
    QTest::addColumn<bool>("hasBom");
    QTest::addColumn<QString>("format");

    using K = ContentKind;
    using E = Encoding;
    const QByteArray elfHeader("\x7F" "ELF\x02\x01\x01\x00\x00\x00\x00\x00", 12);
    const QByteArray javaClass("\xCA\xFE\xBA\xBE\x00\x00\x00\x41", 8);
    const QByteArray machOFat("\xCA\xFE\xBA\xBE\x00\x00\x00\x02", 8);
    const QByteArray peHeader("MZ\x90\x00\x03\x00\x00\x00", 8);
    const QByteArray utf16le("h\0e\0l\0l\0o\0 \0w\0o\0r\0l\0d\0\n\0", 24);
    const QByteArray utf16be("\0h\0e\0l\0l\0o\0 \0w\0o\0r\0l\0d\0\n", 24);
    const QByteArray smallInts("\x01\x00\x02\x00\x03\x00\x04\x00\x05\x00\x06\x00", 12);
    const QByteArray tarHeader = QByteArray(257, 'a') + "ustar";

    QTest::newRow("empty") << QByteArray() << K::Empty << E::Unknown << false << "";
    QTest::newRow("ascii") << QByteArray("hello\tworld\r\n") << K::Text << E::Ascii << false << "";
    QTest::newRow("ansi colours") << QByteArray("\x1B[31mERROR\x1B[0m\n") << K::Text << E::Ascii << false << "";
    QTest::newRow("utf-8 chinese") << QByteArray("你好，世界\n") << K::Text << E::Utf8 << false << "";
    QTest::newRow("utf-8 bom") << QByteArray("\xEF\xBB\xBFhi") << K::Text << E::Utf8 << true << "";
    QTest::newRow("utf-16le bom") << QByteArray("\xFF\xFEh\0i\0", 6) << K::Text << E::Utf16LE << true << "";
    QTest::newRow("utf-16be bom") << QByteArray("\xFE\xFF\0h\0i", 6) << K::Text << E::Utf16BE << true << "";
    QTest::newRow("utf-32le bom") << QByteArray("\xFF\xFE\0\0h\0\0\0", 8) << K::Text << E::Utf32LE << true << "";
    QTest::newRow("utf-32be bom") << QByteArray("\0\0\xFE\xFF\0\0\0h", 8) << K::Text << E::Utf32BE << true << "";
    QTest::newRow("utf-16le no bom") << utf16le << K::Text << E::Utf16LE << false << "";
    QTest::newRow("utf-16be no bom") << utf16be << K::Text << E::Utf16BE << false << "";
    // "你好世界" in GBK: not valid UTF-8, but clean text.
    QTest::newRow("gbk") << QByteArray("\xC4\xE3\xBA\xC3\xCA\xC0\xBD\xE7\n") << K::Text << E::Legacy << false << "";
    QTest::newRow("small ints") << smallInts << K::Binary << E::Unknown << false << "";
    QTest::newRow("elf") << elfHeader << K::Binary << E::Unknown << false << "ELF";
    QTest::newRow("png") << QByteArray("\x89PNG\r\n\x1A\n\0\0\0\x0DIHDR", 16) << K::Binary << E::Unknown << false << "PNG";
    QTest::newRow("webp") << QByteArray("RIFF\0\0\0\0WEBPVP8 ", 16) << K::Binary << E::Unknown << false << "WebP";
    QTest::newRow("java class") << javaClass << K::Binary << E::Unknown << false << "Java class";
    QTest::newRow("mach-o fat") << machOFat << K::Binary << E::Unknown << false << "Mach-O universal";
    QTest::newRow("tar") << tarHeader << K::Binary << E::Unknown << false << "tar";
    QTest::newRow("weak magic, binary") << peHeader << K::Binary << E::Unknown << false << "PE/MZ executable";
    QTest::newRow("weak magic, text") << QByteArray("MZ is not always a PE file\n") << K::Text << E::Ascii << false << "";
    QTest::newRow("control-heavy") << QByteArray("\x01\x02\x03\x04\x05\x06\x07\x08" "abc") << K::Binary << E::Unknown << false << "";
}

void TestFileType::detect()
{
    QFETCH(QByteArray, content);
    QFETCH(ContentKind, kind);
    QFETCH(Encoding, encoding);
    QFETCH(bool, hasBom);
    QFETCH(QString, format);

    const auto info = detectFileType(
        {reinterpret_cast<const std::uint8_t*>(content.constData()), static_cast<std::size_t>(content.size())});
    QCOMPARE(info.kind, kind);
    QCOMPARE(info.encoding, encoding);
    QCOMPARE(info.hasBom, hasBom);
    QCOMPARE(QString::fromUtf8(info.format.data(), static_cast<qsizetype>(info.format.size())), format);
}

void TestFileType::detectLongUtf8CutMidCharacter()
{
    // kSniffSize is not a multiple of 3, so the sniffed prefix ends inside a character.
    // That must not demote the file to a legacy encoding.
    static_assert(kSniffSize % 3 != 0);
    std::string content;
    while (content.size() < kSniffSize + 16)
        content += "中";
    QCOMPARE(detectFileType(asBytes(content)).encoding, Encoding::Utf8);
}

void TestFileType::utf8Validation_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("truncatedOk");
    QTest::addColumn<bool>("valid");

    QTest::newRow("ascii") << QByteArray("abc") << false << true;
    QTest::newRow("two byte") << QByteArray("\xC3\xA9") << false << true;
    QTest::newRow("four byte") << QByteArray("\xF0\x9F\x98\x80") << false << true;
    QTest::newRow("overlong") << QByteArray("\xC0\xAF") << false << false;
    QTest::newRow("surrogate") << QByteArray("\xED\xA0\x80") << false << false;
    QTest::newRow("above U+10FFFF") << QByteArray("\xF4\x90\x80\x80") << false << false;
    QTest::newRow("bad continuation") << QByteArray("\xE4\xB8x") << false << false;
    QTest::newRow("truncated, strict") << QByteArray("\xE4\xB8") << false << false;
    QTest::newRow("truncated, allowed") << QByteArray("\xE4\xB8") << true << true;
}

void TestFileType::utf8Validation()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, truncatedOk);
    QFETCH(bool, valid);

    const std::span<const std::uint8_t> span{reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                                             static_cast<std::size_t>(bytes.size())};
    QCOMPARE(isValidUtf8(span, truncatedOk), valid);
}

void TestFileType::formatForName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("format");

    QTest::newRow("png") << "photo.png" << "PNG";
    QTest::newRow("upper case") << "PHOTO.JPG" << "JPEG";
    QTest::newRow("office") << "report.docx" << "ZIP";
    QTest::newRow("text") << "notes.txt" << "text";
    QTest::newRow("compressed tar") << "src.tar.gz" << "gzip";
    QTest::newRow("unknown") << "data.dat" << "";
    QTest::newRow("no extension") << "Makefile" << "";
    QTest::newRow("dot file") << ".bashrc" << "";
    QTest::newRow("trailing dot") << "odd." << "";
}

void TestFileType::formatForName()
{
    QFETCH(QString, name);
    QFETCH(QString, format);
    const QByteArray utf8 = name.toUtf8();
    const std::string_view found =
        aiedit::core::formatForName({utf8.constData(), static_cast<std::size_t>(utf8.size())});
    QCOMPARE(QString::fromUtf8(found.data(), static_cast<qsizetype>(found.size())), format);
}

void TestFileType::contentMismatch_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<QString>("mismatch");

    const QByteArray png("\x89PNG\r\n\x1A\n\0\0\0\rIHDR", 16);
    const QByteArray jpeg("\xFF\xD8\xFF\xE0\0\x10JFIF\0", 11);
    const QByteArray zip("PK\x03\x04\x14\0\0\0\x08\0", 10);
    const QByteArray html("<!DOCTYPE html><html><body>404 Not Found</body></html>\n");
    QByteArray unknown(64, '\0');
    unknown[1] = '\x42';

    QTest::newRow("as named") << "photo.png" << png << "";
    QTest::newRow("JPEG named png") << "photo.png" << jpeg << "JPEG";
    QTest::newRow("error page named png") << "photo.png" << html << "text";
    QTest::newRow("docx is a zip") << "report.docx" << zip << "";
    QTest::newRow("zip named txt") << "notes.txt" << zip << "ZIP";
    QTest::newRow("text named txt") << "notes.txt" << html << "";
    QTest::newRow("unknown binary named txt") << "notes.txt" << unknown << "";
    QTest::newRow("unknown binary named png") << "photo.png" << unknown << "";
    QTest::newRow("empty") << "photo.png" << QByteArray() << "";
    QTest::newRow("no promise") << "data.dat" << zip << "";
}

void TestFileType::contentMismatch()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, content);
    QFETCH(QString, mismatch);
    const QByteArray utf8 = name.toUtf8();
    const FileTypeInfo type = detectFileType(
        {reinterpret_cast<const std::uint8_t*>(content.constData()), static_cast<std::size_t>(content.size())});
    const std::string_view found =
        aiedit::core::contentMismatch({utf8.constData(), static_cast<std::size_t>(utf8.size())}, type);
    QCOMPARE(QString::fromUtf8(found.data(), static_cast<qsizetype>(found.size())), mismatch);
}

QTEST_APPLESS_MAIN(TestFileType)
#include "tst_filetype.moc"
