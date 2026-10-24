// Read-only memory-mapped file (Win32 CreateFileMapping / POSIX mmap).
// The DFD streamer maps every cached sample once; pages are file-backed, so
// the OS can evict them freely — they never count as committed RAM.
#pragma once

#include "whe/core.hpp"

namespace whe {

class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept;
    MappedFile& operator=(MappedFile&& o) noexcept;
    ~MappedFile();

    [[nodiscard]] static Result<MappedFile> open(const fs::path& path);

    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr || size_ == 0; }

    // Asynchronous read-ahead hint for [offset, offset+length). Called from the
    // disk thread one region ahead of the region it is about to copy.
    void prefetch(std::size_t offset, std::size_t length) const noexcept;

private:
    void close() noexcept;
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
#ifdef _WIN32
    void* file_ = nullptr;     // HANDLE
    void* mapping_ = nullptr;  // HANDLE
#else
    int fd_ = -1;
#endif
};

}  // namespace whe
