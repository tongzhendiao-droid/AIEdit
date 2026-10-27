#include "core/Inflate.h"
#include "core/Zip.h"

#include <QTest>

using aiedit::core::ZipArchive;

namespace {

// Made by Python's zipfile: "stored.txt" stored, "short.txt" and "Word/Document.xml" deflated.
const char kZip[] =
    "UEsDBBQAAAAAAAAAIQCc5+EcCgAAAAoAAAAKAAAAc3RvcmVkLnR4dGhlbGxvLCB6aXBQSwMEFAAAAAgAHLZbXRhILUYHAAAA"
    "CQAAAAkAAABzaG9ydC50eHRLTEpOBCMAUEsDBBQAAAAIABy2W10SHszBPgIAADovAAARAAAAV29yZC9Eb2N1bWVudC54bWyl"
    "2Llx1lAARtGcKlSCrnbRDYsBg7HZzBZBSj/U5DaYISLlP/GbL7sjvXcefv5++PVjuLm+vRrGx8OnV1fD+/vrZ2+Gpx/uvtwO"
    "L+6+Dq/v3777ONx9vvrw9/jmyfdvw/O7l48e/pl2+XS6fDpfPl0un66XT7fLp/vl0+Py6QlJSE7QUxBUUFSQVNBUEFVQVZBV"
    "0NUEXU3ynYKuJuhqgq4m6GqCriboaoKuJuhqhq5m6GqWHyB0NUNXM3Q1Q1czdDVDVzN0tUBXC3S1QFeL3KygqwW6WqCrBbpa"
    "oKsFulqhqxW6WqGrFbpa5coOXa3Q1QpdrdDVCl1t0NUGXW3Q1QZdbdDVJm9B6GqDrjboaoOuduhqh6526GqHrnboaoeudkEG"
    "6GqHrnbo6oCuDujqgK4O6OqArg7o6oCuDtEr6OqArk7o6oSuTujqhK5O6OqErk7o6oSuTmFRclGB0VFkdBQaHcVGR8HRUXR0"
    "FB4dxUdHAdJRCjN6l8II30nfid/J3wngSeCJ4MXgE4RPFD5h+MThE4hPJD6h+MTiE4xPND7h+MTjE5BPRD4h+cTkE5RPVD5h"
    "+cTlE5hPZD6h+cTmE5xPdD7h+cTnE6BPhD4h+sToE6RPlD5h+sTpE6hPpD6h+sTqE6xPtD7h+sTrE7BPxD4h+8TsE7RP1D5h"
    "+8TtE7hP5D6h+8TuE7xP9D7h+8TvE8BPBD8h/MTwE8RPFD9h/MTxE8hPJD+h/MTyE8xPND/h/MTzE9BPRD8h/f7X9P8AUEsB"
    "AhQDFAAAAAAAAAAhAJzn4RwKAAAACgAAAAoAAAAAAAAAAAAAAIABAAAAAHN0b3JlZC50eHRQSwECFAMUAAAACAActltdGEgt"
    "RgcAAAAJAAAACQAAAAAAAAAAAAAAgAEyAAAAc2hvcnQudHh0UEsBAhQDFAAAAAgAHLZbXRIezME+AgAAOi8AABEAAAAAAAAA"
    "AAAAAIABYAAAAFdvcmQvRG9jdW1lbnQueG1sUEsFBgAAAAADAAMArgAAAM0CAAAAAA==";

// zlib.compress(longText(), 9)
const char kZlib[] =
    "eNql2jly1UAUhtGcVWgJ+rvVE7thMGAwfmBjpghS9sOavA2qiEjRiVU3OyW1vr6PP38//vqx3VzfXm370+3Tm6vt48P1i3fb"
    "87vLl9vt1eXr9vbh/Yf77fL56u7v45tn379tLy+vnzz+M5rzo+X8aD0/epwfbedH+/nRcX50nh9dQEI4gacAqICoAKmAqQCq"
    "gKoAq4CrAq6KvKfAVQFXBVwVcFXAVQFXBVwVcFXBVQVXVT6A4KqCqwquKriq4KqCqwquDnB1gKsDXB1ysgJXB7g6wNUBrg5w"
    "dYCrBq4auGrgqoGrJkd2cNXAVQNXDVw1cNXBVQdXHVx1cNXBVZd/QXDVwVUHVx1cDXA1wNUAVwNcDXA1wNWQyACuBrga4GqC"
    "qwmuJria4GqCqwmuJriaUq/A1QRXC1wtcLXA1QJXC1wtcLXA1QJXS7IodVEJo7uU0V3S6C5tdJc4uksd3SWP7tJHdwmkuwiz"
    "9C7CKL5Tfaf8Tv2dAjwVeErw0uAjET6FbndEmHT4SIiPlPhIio+0+EiMj9T4SI6P9PhUukAUYZLkI00+EuUjVT6S5SNdPhLm"
    "I2U+kuZz0B21CJM6H8nzkT4fCfSRQh9J9JFGH4n0kUqfRmsQIkxCfaTUR1J9pNVHYn2k1kdyfaTXR4J9Om3aiDBp9pFoH6n2"
    "kWwf6faRcB8p95F0H2n3GbTMJcIk30f6fSTgRwp+JOFHGn4k4kcqfiTjZ9K+oAiTkh9J+ZGWH4n5kZofyfmRnh8J+pGin0Ur"
    "qf8p7A/1VNUk";

std::vector<std::uint8_t> bytes(const char* base64)
{
    const QByteArray data = QByteArray::fromBase64(base64);
    return {data.begin(), data.end()};
}

QByteArray longText()
{
    QByteArray text;
    for (int i = 0; i < 200; ++i)
        text += QStringLiteral("灵犀 line %1: the quick brown fox jumps over the lazy dog\n").arg(i).toUtf8();
    return text;
}

QByteArray toQByteArray(const std::vector<std::uint8_t>& v)
{
    return QByteArray(reinterpret_cast<const char*>(v.data()), static_cast<qsizetype>(v.size()));
}

} // namespace

class TestZip : public QObject
{
    Q_OBJECT

private slots:
    void entries();
    void readEntries();
    void limits();
    void zlib();
    void brokenData();
    void notAnArchive();
};

void TestZip::entries()
{
    const auto data = bytes(kZip);
    const auto archive = ZipArchive::open(data);
    QVERIFY(archive);
    QCOMPARE(archive->entries().size(), 3u);
    QCOMPARE(archive->entries()[0].name, std::string("stored.txt"));
    QCOMPARE(archive->entries()[0].method, 0);
    QCOMPARE(archive->entries()[2].method, 8);
    QCOMPARE(archive->entries()[2].size, static_cast<std::uint64_t>(longText().size()));
}

void TestZip::readEntries()
{
    const auto data = bytes(kZip);
    const auto archive = ZipArchive::open(data);
    QVERIFY(archive);
    auto stored = archive->read("stored.txt", 1024);
    QVERIFY(stored);
    QCOMPARE(toQByteArray(*stored), QByteArray("hello, zip"));
    auto fixed = archive->read("short.txt", 1024);
    QVERIFY(fixed);
    QCOMPARE(toQByteArray(*fixed), QByteArray("abcabcabc"));
    // Found ignoring case, as Office files are written by tools that disagree on it.
    auto dynamic = archive->read("word/document.xml", 1 << 20);
    QVERIFY(dynamic);
    QCOMPARE(toQByteArray(*dynamic), longText());
    QVERIFY(!archive->find("missing.txt"));
}

void TestZip::limits()
{
    const auto data = bytes(kZip);
    const auto archive = ZipArchive::open(data);
    QVERIFY(archive);
    QVERIFY(!archive->read("word/document.xml", 100));
}

void TestZip::zlib()
{
    const auto data = bytes(kZlib);
    std::vector<std::uint8_t> out;
    QVERIFY(aiedit::core::zlibInflate(data, 1 << 20, out));
    QCOMPARE(toQByteArray(out), longText());

    // Cut short, as PDF streams sometimes are: false, with what came before kept.
    out.clear();
    const std::span<const std::uint8_t> cut(data.data(), data.size() / 2);
    QVERIFY(!aiedit::core::zlibInflate(cut, 1 << 20, out));
    QVERIFY(!out.empty());
    QVERIFY(longText().startsWith(toQByteArray(out)));

    QCOMPARE(aiedit::core::crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("123456789"), 9)),
             0xCBF43926u);
}

void TestZip::brokenData()
{
    auto data = bytes(kZip);
    // A flipped byte inside the deflated entry: the checksum, if nothing else, catches it.
    const auto archive = ZipArchive::open(data);
    QVERIFY(archive);
    const auto* entry = archive->find("Word/Document.xml");
    QVERIFY(entry);
    auto copy = data;
    copy[static_cast<std::size_t>(entry->localHeaderOffset) + 30 + entry->name.size() + 40] ^= 0x55;
    const auto broken = ZipArchive::open(copy);
    QVERIFY(broken);
    QVERIFY(!broken->read("Word/Document.xml", 1 << 20));
}

void TestZip::notAnArchive()
{
    const std::vector<std::uint8_t> text(1000, 'a');
    QVERIFY(!ZipArchive::open(text));
    QVERIFY(!ZipArchive::open({}));
    std::vector<std::uint8_t> out;
    QVERIFY(!aiedit::core::zlibInflate(text, 1000, out));
}

QTEST_GUILESS_MAIN(TestZip)
#include "tst_zip.moc"
