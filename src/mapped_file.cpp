#include "whe/mapped_file.hpp"

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

namespace whe {

MappedFile::MappedFile(MappedFile&& o) noexcept { *this = std::move(o); }

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
    if (this != &o) {
        close();
        data_ = std::exchange(o.data_, nullptr);
        size_ = std::exchange(o.size_, 0);
#ifdef _WIN32
        file_ = std::exchange(o.file_, nullptr);
        mapping_ = std::exchange(o.mapping_, nullptr);
#else
        fd_ = std::exchange(o.fd_, -1);
#endif
    }
    return *this;
}

MappedFile::~MappedFile() { close(); }

void MappedFile::close() noexcept {
#ifdef _WIN32
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_ && file_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(file_));
    mapping_ = nullptr;
    file_ = nullptr;
#else
    if (data_ && size_) munmap(const_cast<std::byte*>(data_), size_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    data_ = nullptr;
    size_ = 0;
}

Result<MappedFile> MappedFile::open(const fs::path& path) {
    MappedFile mf;
#ifdef _WIN32
    HANDLE f = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fail("cannot open " + path.string());
    mf.file_ = f;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz)) return fail("cannot stat " + path.string());
    mf.size_ = static_cast<std::size_t>(sz.QuadPart);
    if (mf.size_ == 0) return mf;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) return fail("CreateFileMapping failed for " + path.string());
    mf.mapping_ = m;
    void* view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!view) return fail("MapViewOfFile failed for " + path.string());
    mf.data_ = static_cast<const std::byte*>(view);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return fail("cannot open " + path.string());
    mf.fd_ = fd;
    struct stat st{};
    if (fstat(fd, &st) != 0) return fail("cannot stat " + path.string());
    mf.size_ = static_cast<std::size_t>(st.st_size);
    if (mf.size_ == 0) return mf;
    void* p = mmap(nullptr, mf.size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        mf.size_ = 0;
        return fail("mmap failed for " + path.string());
    }
    mf.data_ = static_cast<const std::byte*>(p);
#endif
    return mf;
}

void MappedFile::prefetch(std::size_t offset, std::size_t length) const noexcept {
    if (!data_ || offset >= size_) return;
    length = std::min(length, size_ - offset);
#ifdef _WIN32
    // PrefetchVirtualMemory (Windows 8+) issues large asynchronous reads.
    WIN32_MEMORY_RANGE_ENTRY range{const_cast<std::byte*>(data_ + offset), length};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
    const std::size_t page = 4096;
    const std::size_t aligned = offset & ~(page - 1);
    madvise(const_cast<std::byte*>(data_ + aligned), length + (offset - aligned), MADV_WILLNEED);
#endif
}

}  // namespace whe
