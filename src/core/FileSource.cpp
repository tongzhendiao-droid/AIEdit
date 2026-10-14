#include "core/FileSource.h"

#include <cerrno>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static_assert(sizeof(void*) == 8, "AIEdit maps whole files and requires a 64-bit address space");

namespace aiedit::core {

namespace {

class FileErrorCategory final : public std::error_category
{
public:
    const char* name() const noexcept override { return "aiedit.file"; }

    std::string message(int ev) const override
    {
        switch (static_cast<FileError>(ev)) {
        case FileError::NotRegularFile:
            return "not a regular file";
        case FileError::TooLargeToRead:
            return "file cannot be memory-mapped and is too large to read into memory";
        }
        return "unknown error";
    }
};

constexpr std::size_t kReadChunk = 64 * 1024;

#ifdef _WIN32

std::error_code lastError()
{
    return {static_cast<int>(::GetLastError()), std::system_category()};
}

struct HandleCloser
{
    HANDLE handle;
    ~HandleCloser()
    {
        if (handle && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};

bool readAll(HANDLE file, std::vector<std::uint8_t>& out, std::error_code& ec)
{
    for (;;) {
        const std::size_t used = out.size();
        out.resize(used + kReadChunk);
        DWORD n = 0;
        if (!::ReadFile(file, out.data() + used, static_cast<DWORD>(kReadChunk), &n, nullptr)) {
            ec = lastError();
            return false;
        }
        out.resize(used + n);
        if (n == 0)
            return true;
        if (out.size() > FileSource::kMaxReadSize) {
            ec = FileError::TooLargeToRead;
            return false;
        }
    }
}

#else

std::error_code lastError()
{
    return {errno, std::system_category()};
}

struct FdCloser
{
    int fd;
    ~FdCloser()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

bool readAll(int fd, std::vector<std::uint8_t>& out, std::error_code& ec)
{
    for (;;) {
        const std::size_t used = out.size();
        out.resize(used + kReadChunk);
        const ssize_t n = ::read(fd, out.data() + used, kReadChunk);
        if (n < 0) {
            out.resize(used);
            if (errno == EINTR)
                continue;
            ec = lastError();
            return false;
        }
        out.resize(used + static_cast<std::size_t>(n));
        if (n == 0)
            return true;
        if (out.size() > FileSource::kMaxReadSize) {
            ec = FileError::TooLargeToRead;
            return false;
        }
    }
}

#endif

} // namespace

const std::error_category& fileErrorCategory()
{
    static const FileErrorCategory category;
    return category;
}

std::error_code make_error_code(FileError e)
{
    return {static_cast<int>(e), fileErrorCategory()};
}

#ifdef _WIN32

std::unique_ptr<FileSource> FileSource::open(const std::filesystem::path& path, std::error_code& ec)
{
    ec.clear();
    std::error_code ignored;
    if (std::filesystem::is_directory(path, ignored)) {
        ec = std::make_error_code(std::errc::is_a_directory);
        return nullptr;
    }

    // Share write/delete so that programs can keep appending to a log while it is open here.
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ec = lastError();
        return nullptr;
    }
    const HandleCloser fileCloser{file};

    if (::GetFileType(file) != FILE_TYPE_DISK) {
        ec = FileError::NotRegularFile;
        return nullptr;
    }
    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(file, &fileSize)) {
        ec = lastError();
        return nullptr;
    }

    std::unique_ptr<FileSource> source(new FileSource(path));
    const auto size = static_cast<std::uint64_t>(fileSize.QuadPart);
    if (size > 0) {
        std::error_code mapError;
        if (const HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            // The view keeps the mapping object alive, so both handles can be closed right away.
            const HandleCloser mappingCloser{mapping};
            if (void* base = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) {
                source->m_mapping = base;
                source->m_data = static_cast<const std::uint8_t*>(base);
                source->m_size = size;
                return source;
            }
            mapError = lastError();
        } else {
            mapError = lastError();
        }
        if (size > kMaxReadSize) {
            ec = mapError;
            return nullptr;
        }
    }

    if (!readAll(file, source->m_owned, ec))
        return nullptr;
    source->m_data = source->m_owned.data();
    source->m_size = source->m_owned.size();
    return source;
}

FileSource::~FileSource()
{
    if (m_mapping)
        ::UnmapViewOfFile(m_mapping);
}

#else

std::unique_ptr<FileSource> FileSource::open(const std::filesystem::path& path, std::error_code& ec)
{
    ec.clear();
    // O_NONBLOCK keeps open() from waiting for a writer on a FIFO (or carrier on a tty); such
    // files are rejected below anyway. It has no effect on reads from regular files.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        ec = lastError();
        return nullptr;
    }
    const FdCloser fdCloser{fd};

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ec = lastError();
        return nullptr;
    }
    if (S_ISDIR(st.st_mode)) {
        ec = std::make_error_code(std::errc::is_a_directory);
        return nullptr;
    }
    if (!S_ISREG(st.st_mode)) {
        ec = FileError::NotRegularFile;
        return nullptr;
    }

    std::unique_ptr<FileSource> source(new FileSource(path));
    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size > 0) {
        // The mapping stays valid after the descriptor is closed.
        void* base = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (base != MAP_FAILED) {
            source->m_mapping = base;
            source->m_data = static_cast<const std::uint8_t*>(base);
            source->m_size = size;
            return source;
        }
        if (size > kMaxReadSize) {
            ec = lastError();
            return nullptr;
        }
    }

    // Size 0 does not mean empty for /proc and /sys entries, so read until EOF.
    if (!readAll(fd, source->m_owned, ec))
        return nullptr;
    source->m_data = source->m_owned.data();
    source->m_size = source->m_owned.size();
    return source;
}

FileSource::~FileSource()
{
    if (m_mapping)
        ::munmap(m_mapping, static_cast<std::size_t>(m_size));
}

#endif

} // namespace aiedit::core
