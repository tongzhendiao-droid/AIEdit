#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

namespace aiedit::core {

// Errors specific to FileSource; OS errors are reported through std::system_category().
enum class FileError
{
    NotRegularFile = 1, // pipes, sockets, devices
    TooLargeToRead,     // a file that cannot be mapped and exceeds kMaxReadSize
};

const std::error_category& fileErrorCategory();
std::error_code make_error_code(FileError e);

// Read-only access to a file's bytes.
//
// Regular files are memory-mapped, so opening costs the same for 1 KB and 100 GB.
// Files that report size 0 but still have content (/proc, /sys) or that live on file
// systems without mmap support are read into memory instead, up to kMaxReadSize.
//
// If another process truncates a mapped file, the bytes past its new end read as zeros (see
// MappingGuard) until the file is opened again.
class FileSource
{
public:
    static constexpr std::uint64_t kMaxReadSize = 256ull * 1024 * 1024;

    static std::unique_ptr<FileSource> open(const std::filesystem::path& path, std::error_code& ec);

    ~FileSource();
    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    const std::filesystem::path& path() const { return m_path; }
    std::span<const std::uint8_t> bytes() const { return {m_data, static_cast<std::size_t>(m_size)}; }
    std::uint64_t size() const { return m_size; }
    bool isMapped() const { return m_mapping != nullptr; }

private:
    explicit FileSource(std::filesystem::path path) : m_path(std::move(path)) {}

    std::filesystem::path m_path;
    const std::uint8_t* m_data = nullptr;
    std::uint64_t m_size = 0;
    void* m_mapping = nullptr;         // base address of the mapping, if mapped
    std::vector<std::uint8_t> m_owned; // contents, if read into memory
};

} // namespace aiedit::core

template <>
struct std::is_error_code_enum<aiedit::core::FileError> : std::true_type
{
};
