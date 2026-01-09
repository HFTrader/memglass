#include "memglass/memglass.hpp"

#include <chrono>
#include <unistd.h>

namespace memglass {

namespace detail {

static Context* g_context = nullptr;

Context* get_context() {
    return g_context;
}

void set_context(Context* ctx) {
    g_context = ctx;
}

} // namespace detail

// Context implementation

Context::Context() = default;

Context::~Context() {
    shutdown();
}

bool Context::init(std::string_view session_name, const Config& config) {
    if (initialized_) {
        return false;
    }

    session_name_ = std::string(session_name);
    config_ = config;

    // Calculate header size
    size_t type_registry_size = config.max_types * sizeof(TypeEntry);
    size_t field_entries_size = config.max_fields * sizeof(FieldEntry);
    size_t object_dir_size = config.max_objects * sizeof(ObjectEntry);
    size_t header_total_size = sizeof(TelemetryHeader) +
                               type_registry_size +
                               field_entries_size +
                               object_dir_size;

    // Create header shared memory
    std::string header_shm_name = detail::make_header_shm_name(session_name);
    if (!header_shm_.create(header_shm_name, header_total_size)) {
        return false;
    }

    // Initialize header
    header_ = static_cast<TelemetryHeader*>(header_shm_.data());
    std::memset(header_, 0, header_total_size);

    header_->magic = HEADER_MAGIC;
    header_->version = PROTOCOL_VERSION;
    header_->header_size = sizeof(TelemetryHeader);
    header_->sequence.store(0, std::memory_order_release);

    // Layout: [TelemetryHeader][TypeEntry...][FieldEntry...][ObjectEntry...]
    header_->type_registry_offset = sizeof(TelemetryHeader);
    header_->type_registry_capacity = config.max_types;
    header_->type_count.store(0, std::memory_order_release);

    header_->field_entries_offset = header_->type_registry_offset + type_registry_size;
    header_->field_entries_capacity = config.max_fields;
    header_->field_count.store(0, std::memory_order_release);

    header_->object_dir_offset = header_->field_entries_offset + field_entries_size;
    header_->object_dir_capacity = config.max_objects;
    header_->object_count.store(0, std::memory_order_release);

    header_->first_region_id.store(0, std::memory_order_release);
    header_->first_overflow_region_id.store(0, std::memory_order_release);

    // Set session info
    size_t name_len = std::min(session_name.size(), sizeof(header_->session_name) - 1);
    std::memcpy(header_->session_name, session_name.data(), name_len);
    header_->session_name[name_len] = '\0';
    header_->producer_pid = static_cast<uint64_t>(getpid());
    header_->start_timestamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // Create region manager
    regions_ = std::make_unique<RegionManager>(*this);
    if (!regions_->init(session_name, config.initial_region_size)) {
        header_shm_.close();
        return false;
    }

    // Create metadata manager (for overflow regions)
    metadata_ = std::make_unique<MetadataManager>(*this);
    if (!metadata_->init(session_name)) {
        regions_.reset();
        header_shm_.close();
        return false;
    }

    // Create object manager
    objects_ = std::make_unique<ObjectManager>(*this);

    // Write type registry to header
    registry::write_to_header(header_, header_shm_.data());

    initialized_ = true;
    return true;
}

void Context::shutdown() {
    if (!initialized_) return;

    objects_.reset();
    metadata_.reset();
    regions_.reset();
    header_shm_.close();

    header_ = nullptr;
    initialized_ = false;
}

// Global API implementation

bool init(std::string_view session_name, const Config& config) {
    if (detail::get_context()) {
        return false;  // Already initialized
    }

    auto* ctx = new Context();
    if (!ctx->init(session_name, config)) {
        delete ctx;
        return false;
    }

    detail::set_context(ctx);
    return true;
}

void shutdown() {
    Context* ctx = detail::get_context();
    if (ctx) {
        detail::set_context(nullptr);
        delete ctx;
    }
}

Config& config() {
    static Config default_config;
    Context* ctx = detail::get_context();
    if (ctx) {
        return const_cast<Config&>(ctx->config());
    }
    return default_config;
}

} // namespace memglass
