#include "core/FileSource.h"
#include "core/MappingGuard.h"

#include <QTemporaryDir>
#include <QTest>

#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace aiedit::core;

namespace {

std::filesystem::path writeFile(const QTemporaryDir& dir, const char* name, const std::string& content)
{
    const auto path = std::filesystem::path(dir.path().toStdString()) / name;
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

} // namespace

class TestFileSource : public QObject
{
    Q_OBJECT

private slots:
    void mapsRegularFile();
    void opensEmptyFile();
    void reportsMissingFile();
    void rejectsDirectory();
#ifndef Q_OS_WIN
    void survivesTruncation();
#endif
#ifdef Q_OS_LINUX
    void readsProcFileWithZeroSize();
    void rejectsFifo();
#endif
};

void TestFileSource::mapsRegularFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string content("\x00\x01\x02hello\xFF", 9);
    const auto path = writeFile(dir, "data.bin", content);

    std::error_code ec;
    const auto source = FileSource::open(path, ec);
    QVERIFY2(source, ec.message().c_str());
    QVERIFY(!ec);
    QVERIFY(source->isMapped());
    QCOMPARE(source->size(), content.size());
    QCOMPARE(source->path(), path);
    const auto bytes = source->bytes();
    QVERIFY(std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const std::uint8_t*>(content.data())));
}

void TestFileSource::opensEmptyFile()
{
    QTemporaryDir dir;
    const auto path = writeFile(dir, "empty", "");

    std::error_code ec;
    const auto source = FileSource::open(path, ec);
    QVERIFY(source);
    QCOMPARE(source->size(), 0u);
    QVERIFY(source->bytes().empty());
}

void TestFileSource::reportsMissingFile()
{
    QTemporaryDir dir;
    std::error_code ec;
    QVERIFY(!FileSource::open(std::filesystem::path(dir.path().toStdString()) / "missing", ec));
    QCOMPARE(ec, std::errc::no_such_file_or_directory);
}

void TestFileSource::rejectsDirectory()
{
    QTemporaryDir dir;
    std::error_code ec;
    QVERIFY(!FileSource::open(dir.path().toStdString(), ec));
    QCOMPARE(ec, std::errc::is_a_directory);
}

#ifdef Q_OS_LINUX
void TestFileSource::readsProcFileWithZeroSize()
{
    std::error_code ec;
    const auto source = FileSource::open("/proc/self/status", ec);
    QVERIFY2(source, ec.message().c_str());
    QVERIFY(!source->isMapped());
    QVERIFY(source->size() > 0);
}

void TestFileSource::rejectsFifo()
{
    QTemporaryDir dir;
    const auto path = std::filesystem::path(dir.path().toStdString()) / "fifo";
    QCOMPARE(::mkfifo(path.c_str(), 0600), 0);

    // Must return at once instead of waiting for a writer.
    std::error_code ec;
    QVERIFY(!FileSource::open(path, ec));
    QCOMPARE(ec, FileError::NotRegularFile);
}
#endif

#ifndef Q_OS_WIN
void TestFileSource::survivesTruncation()
{
    // What logrotate's copytruncate does to an open log: the file shrinks under the mapping.
    // Reading past the new end would raise SIGBUS and end the process; it reads zeros instead.
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = writeFile(dir, "rotated.log", std::string(4 * page, 'x'));
    std::error_code ec;
    const auto source = FileSource::open(path, ec);
    QVERIFY(source && source->isMapped());

    std::filesystem::resize_file(path, page / 2);
    const std::uint64_t caught = truncatedReadsCaught();
    const auto bytes = source->bytes();
    QCOMPARE(bytes[0], std::uint8_t('x')); // before the new end: as it was
    QCOMPARE(bytes[2 * page], std::uint8_t(0));
    QCOMPARE(bytes[4 * page - 1], std::uint8_t(0));
    QVERIFY(truncatedReadsCaught() > caught);
}
#endif

QTEST_APPLESS_MAIN(TestFileSource)
#include "tst_filesource.moc"
