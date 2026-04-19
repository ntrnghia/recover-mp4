#pragma once
/// RAII memory-mapped file wrapper — cross-platform (POSIX + Windows).

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace recover {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
#ifdef _WIN32
        file_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open file: " + path);

        LARGE_INTEGER li;
        if (!GetFileSizeEx(file_, &li)) {
            CloseHandle(file_);
            throw std::runtime_error("Cannot get file size: " + path);
        }
        size_ = static_cast<size_t>(li.QuadPart);

        mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) {
            CloseHandle(file_);
            throw std::runtime_error("Cannot create file mapping: " + path);
        }

        data_ = static_cast<const uint8_t*>(
            MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) {
            CloseHandle(mapping_);
            CloseHandle(file_);
            throw std::runtime_error("Cannot map file: " + path);
        }
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0)
            throw std::runtime_error("Cannot open file: " + path);

        struct stat st{};
        if (fstat(fd_, &st) < 0) {
            ::close(fd_);
            throw std::runtime_error("Cannot stat file: " + path);
        }
        size_ = static_cast<size_t>(st.st_size);

        data_ = static_cast<const uint8_t*>(
            mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            ::close(fd_);
            throw std::runtime_error("Cannot mmap file: " + path);
        }

        // Hint sequential access for kernel readahead
        madvise(const_cast<uint8_t*>(data_), size_, MADV_SEQUENTIAL);
#endif
    }

    ~MappedFile() { release(); }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& o) noexcept
        : data_(o.data_), size_(o.size_)
#ifdef _WIN32
        , file_(o.file_), mapping_(o.mapping_)
#else
        , fd_(o.fd_)
#endif
    {
        o.data_ = nullptr;
        o.size_ = 0;
#ifdef _WIN32
        o.file_ = INVALID_HANDLE_VALUE;
        o.mapping_ = nullptr;
#else
        o.fd_ = -1;
#endif
    }

    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            release();
            data_ = o.data_;
            size_ = o.size_;
#ifdef _WIN32
            file_ = o.file_;
            mapping_ = o.mapping_;
            o.file_ = INVALID_HANDLE_VALUE;
            o.mapping_ = nullptr;
#else
            fd_ = o.fd_;
            o.fd_ = -1;
#endif
            o.data_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] std::span<const uint8_t> span() const noexcept {
        return {data_, size_};
    }

private:
    void release() noexcept {
        if (!data_) return;
#ifdef _WIN32
        UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        mapping_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
#else
        munmap(const_cast<uint8_t*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
        data_ = nullptr;
        size_ = 0;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace recover
