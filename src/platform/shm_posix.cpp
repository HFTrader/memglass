#include "memglass/detail/shm.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
#include <stdexcept>

namespace memglass::detail {

SharedMemory::~SharedMemory() {
    close();
}

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
    , name_(std::move(other.name_))
    , fd_(other.fd_)
    , is_owner_(other.is_owner_)
{
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.is_owner_ = false;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        close();
        data_ = other.data_;
        size_ = other.size_;
        name_ = std::move(other.name_);
        fd_ = other.fd_;
        is_owner_ = other.is_owner_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
        other.is_owner_ = false;
    }
    return *this;
}

bool SharedMemory::create(std::string_view name, size_t size) {
    if (data_) {
        close();
    }

    name_ = std::string(name);

    // Create shared memory object
    fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd_ == -1) {
        // Try to open existing and truncate
        fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
        if (fd_ == -1) {
            return false;
        }
    }

    // Set size
    if (ftruncate(fd_, static_cast<off_t>(size)) == -1) {
        ::close(fd_);
        fd_ = -1;
        shm_unlink(name_.c_str());
        return false;
    }

    // Map memory
    data_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        shm_unlink(name_.c_str());
        return false;
    }

    size_ = size;
    is_owner_ = true;
    return true;
}

bool SharedMemory::open(std::string_view name) {
    if (data_) {
        close();
    }

    name_ = std::string(name);

    // Open existing shared memory object
    fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
    if (fd_ == -1) {
        return false;
    }

    // Get size
    struct stat sb;
    if (fstat(fd_, &sb) == -1) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    size_ = static_cast<size_t>(sb.st_size);

    // Map memory
    data_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    is_owner_ = false;
    return true;
}

void SharedMemory::unlink() {
    if (!name_.empty()) {
        shm_unlink(name_.c_str());
    }
}

void SharedMemory::close() {
    if (data_) {
        munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
    if (is_owner_ && !name_.empty()) {
        shm_unlink(name_.c_str());
    }
    size_ = 0;
    is_owner_ = false;
}

bool SharedMemory::resize(size_t new_size) {
    if (!is_owner_ || fd_ == -1) {
        return false;
    }

    // Unmap current
    if (data_) {
        munmap(data_, size_);
        data_ = nullptr;
    }

    // Resize
    if (ftruncate(fd_, static_cast<off_t>(new_size)) == -1) {
        return false;
    }

    // Remap
    data_ = mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        return false;
    }

    size_ = new_size;
    return true;
}

std::string make_header_shm_name(std::string_view session_name) {
    return fmt::format("/memglass_{}_header", session_name);
}

std::string make_region_shm_name(std::string_view session_name, uint64_t region_id) {
    return fmt::format("/memglass_{}_region_{:04d}", session_name, region_id);
}

std::string make_overflow_shm_name(std::string_view session_name, uint64_t overflow_id) {
    return fmt::format("/memglass_{}_overflow_{:04d}", session_name, overflow_id);
}

} // namespace memglass::detail
