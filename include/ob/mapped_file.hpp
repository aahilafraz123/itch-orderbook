// Read-only memory-mapped file (POSIX: Linux and macOS).
//
// Why mmap instead of reading into a std::vector: a full ITCH day is ~10 GB
// uncompressed. mmap makes the file look like one big array without copying it;
// the OS pages bytes in on first touch and can drop them under memory pressure.
// Benchmarks should touch every page once (a warm-up pass) before timing so they
// measure parsing, not disk reads.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace ob {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) fail("open", path);

        struct stat st{};
        if (::fstat(fd, &st) != 0) { ::close(fd); fail("fstat", path); }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) { ::close(fd); fail("mmap", path); }
            data_ = static_cast<const std::uint8_t*>(p);
            // We read front to back; tell the kernel so it reads ahead aggressively.
            ::madvise(const_cast<std::uint8_t*>(data_), size_, MADV_SEQUENTIAL);
        }
        ::close(fd);  // the mapping stays valid after the descriptor is closed
    }

    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
    }

    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const std::uint8_t> bytes() const noexcept { return {data_, size_}; }

private:
    [[noreturn]] static void fail(const char* what, const std::string& path) {
        throw std::runtime_error(std::string(what) + "(" + path + "): " + std::strerror(errno));
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t         size_ = 0;
};

}  // namespace ob
