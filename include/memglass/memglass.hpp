#pragma once

#include "types.hpp"
#include "registry.hpp"
#include "allocator.hpp"
#include "detail/seqlock.hpp"

#include <memory>
#include <string_view>
#include <type_traits>

namespace memglass {

// Forward declarations
class Context;

// Global context management
namespace detail {
    Context* get_context();
    void set_context(Context* ctx);
}

// Context - holds all state for a memglass session
class Context {
public:
    Context();
    ~Context();

    // Initialize the context
    bool init(std::string_view session_name, const Config& config = {});

    // Shutdown and cleanup
    void shutdown();

    // Check if initialized
    bool is_initialized() const { return initialized_; }

    // Access components
    TelemetryHeader* header() { return header_; }
    RegionManager& regions() { return *regions_; }
    MetadataManager& metadata() { return *metadata_; }
    ObjectManager& objects() { return *objects_; }
    const Config& config() const { return config_; }
    const std::string& session_name() const { return session_name_; }

    // Direct access to header shared memory
    detail::SharedMemory& header_shm() { return header_shm_; }

private:
    bool initialized_ = false;
    std::string session_name_;
    Config config_;

    detail::SharedMemory header_shm_;
    TelemetryHeader* header_ = nullptr;

    std::unique_ptr<RegionManager> regions_;
    std::unique_ptr<MetadataManager> metadata_;
    std::unique_ptr<ObjectManager> objects_;
};

// Initialize memglass (must be called before any other functions)
bool init(std::string_view session_name, const Config& config = {});

// Shutdown memglass
void shutdown();

// Get current configuration
Config& config();

// Create an object in shared memory
template<Observable T>
T* create(std::string_view label) {
    auto* ctx = detail::get_context();
    if (!ctx || !ctx->is_initialized()) return nullptr;

    // Get type ID
    uint32_t type_id = registry::get_type_id(typeid(T).name());
    if (type_id == 0) {
        // Type not registered, try to find by demangled name
        // For MVP, require explicit registration
        return nullptr;
    }

    // Allocate memory
    void* ptr = ctx->regions().allocate(sizeof(T), alignof(T));
    if (!ptr) return nullptr;

    // Construct object
    T* obj = new (ptr) T{};

    // Register in object directory
    ctx->objects().register_object(ptr, type_id, label);

    return obj;
}

// Create an object with initial value
template<Observable T>
T* create(std::string_view label, const T& initial) {
    auto* ctx = detail::get_context();
    if (!ctx || !ctx->is_initialized()) return nullptr;

    uint32_t type_id = registry::get_type_id(typeid(T).name());
    if (type_id == 0) return nullptr;

    void* ptr = ctx->regions().allocate(sizeof(T), alignof(T));
    if (!ptr) return nullptr;

    T* obj = new (ptr) T(initial);
    ctx->objects().register_object(ptr, type_id, label);

    return obj;
}

// Create an array of objects
template<Observable T>
T* create_array(std::string_view label, size_t count) {
    auto* ctx = detail::get_context();
    if (!ctx || !ctx->is_initialized()) return nullptr;

    uint32_t type_id = registry::get_type_id(typeid(T).name());
    if (type_id == 0) return nullptr;

    void* ptr = ctx->regions().allocate(sizeof(T) * count, alignof(T));
    if (!ptr) return nullptr;

    T* arr = static_cast<T*>(ptr);
    for (size_t i = 0; i < count; ++i) {
        new (&arr[i]) T{};
    }

    ctx->objects().register_object(ptr, type_id, label);

    return arr;
}

// Destroy an object
template<Observable T>
void destroy(T* obj) {
    if (!obj) return;

    auto* ctx = detail::get_context();
    if (!ctx || !ctx->is_initialized()) return;

    obj->~T();
    ctx->objects().destroy_object(obj);
}

} // namespace memglass
