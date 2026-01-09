#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace memglass::detail {

// Platform-agnostic shared memory handle
class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();

    // Non-copyable, movable
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;

    // Create new shared memory region (producer)
    bool create(std::string_view name, size_t size);

    // Open existing shared memory region (observer)
    bool open(std::string_view name);

    // Unlink shared memory (removes from filesystem, keeps mapped)
    void unlink();

    // Close and unmap
    void close();

    // Resize (only if created, not opened)
    bool resize(size_t new_size);

    // Accessors
    void* data() { return data_; }
    const void* data() const { return data_; }
    size_t size() const { return size_; }
    bool is_open() const { return data_ != nullptr; }
    const std::string& name() const { return name_; }
    bool is_owner() const { return is_owner_; }

private:
    void* data_ = nullptr;
    size_t size_ = 0;
    std::string name_;
    int fd_ = -1;
    bool is_owner_ = false;
};

// Generate shared memory name for session
std::string make_header_shm_name(std::string_view session_name);
std::string make_region_shm_name(std::string_view session_name, uint64_t region_id);
std::string make_overflow_shm_name(std::string_view session_name, uint64_t overflow_id);

} // namespace memglass::detail
