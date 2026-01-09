#include "memglass/allocator.hpp"
#include "memglass/memglass.hpp"

#include <cstring>

namespace memglass {

// RegionManager implementation

RegionManager::RegionManager(Context& ctx)
    : ctx_(ctx)
    , current_region_size_(ctx.config().initial_region_size)
{
}

RegionManager::~RegionManager() = default;

bool RegionManager::init(std::string_view session_name, size_t initial_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    session_name_ = std::string(session_name);
    current_region_size_ = initial_size;

    // Create first region
    Region* region = create_region(initial_size);
    if (!region) {
        return false;
    }

    // Update header with first region ID
    ctx_.header()->first_region_id.store(region->id, std::memory_order_release);

    return true;
}

RegionManager::Region* RegionManager::create_region(size_t size) {
    auto region = std::make_unique<Region>();
    region->id = next_region_id_++;

    std::string shm_name = detail::make_region_shm_name(session_name_, region->id);

    // Size includes RegionDescriptor at the start
    size_t total_size = sizeof(RegionDescriptor) + size;

    if (!region->shm.create(shm_name, total_size)) {
        return nullptr;
    }

    // Initialize descriptor
    region->descriptor = static_cast<RegionDescriptor*>(region->shm.data());
    region->descriptor->magic = REGION_MAGIC;
    region->descriptor->region_id = region->id;
    region->descriptor->size = total_size;
    region->descriptor->used.store(sizeof(RegionDescriptor), std::memory_order_release);
    region->descriptor->next_region_id.store(0, std::memory_order_release);
    region->descriptor->set_shm_name(shm_name);

    // Link to previous region if exists
    if (!regions_.empty()) {
        regions_.back()->descriptor->next_region_id.store(
            region->id, std::memory_order_release);
    }

    Region* ptr = region.get();
    regions_.push_back(std::move(region));
    return ptr;
}

RegionManager::Region* RegionManager::current_region() {
    if (regions_.empty()) return nullptr;
    return regions_.back().get();
}

void* RegionManager::allocate(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(mutex_);

    Region* region = current_region();
    if (!region) return nullptr;

    // Align the current position
    uint64_t current = region->descriptor->used.load(std::memory_order_acquire);
    uint64_t aligned = (current + alignment - 1) & ~(alignment - 1);
    uint64_t new_used = aligned + size;

    // Check if fits in current region
    if (new_used > region->descriptor->size) {
        // Need new region
        size_t new_size = std::max(size + sizeof(RegionDescriptor),
                                   current_region_size_ * 2);
        new_size = std::min(new_size, ctx_.config().max_region_size);
        current_region_size_ = new_size;

        region = create_region(new_size);
        if (!region) return nullptr;

        // Update header sequence
        ctx_.header()->sequence.fetch_add(1, std::memory_order_release);

        // Recalculate
        current = region->descriptor->used.load(std::memory_order_acquire);
        aligned = (current + alignment - 1) & ~(alignment - 1);
        new_used = aligned + size;
    }

    // Allocate
    region->descriptor->used.store(new_used, std::memory_order_release);

    return static_cast<char*>(region->shm.data()) + aligned;
}

void* RegionManager::get_region_data(uint64_t region_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& region : regions_) {
        if (region->id == region_id) {
            return region->shm.data();
        }
    }
    return nullptr;
}

bool RegionManager::get_location(const void* ptr, uint64_t& region_id, uint64_t& offset) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& region : regions_) {
        const char* base = static_cast<const char*>(region->shm.data());
        const char* end = base + region->descriptor->size;
        const char* p = static_cast<const char*>(ptr);

        if (p >= base && p < end) {
            region_id = region->id;
            offset = static_cast<uint64_t>(p - base);
            return true;
        }
    }
    return false;
}

// MetadataManager implementation

MetadataManager::MetadataManager(Context& ctx)
    : ctx_(ctx)
{
}

MetadataManager::~MetadataManager() = default;

bool MetadataManager::init(std::string_view session_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_name_ = std::string(session_name);
    // No overflow region created initially - will create on demand
    return true;
}

MetadataManager::OverflowRegion* MetadataManager::create_overflow_region() {
    auto region = std::make_unique<OverflowRegion>();
    region->id = next_overflow_id_++;

    std::string shm_name = detail::make_overflow_shm_name(session_name_, region->id);

    const Config& cfg = ctx_.config();
    size_t region_size = cfg.overflow_region_size;

    // Calculate capacities for this overflow region
    // Split space roughly equally between objects, types, and fields
    size_t header_size = sizeof(MetadataOverflowDescriptor);
    size_t available = region_size - header_size;

    // Allocate: 50% for objects, 10% for types, 40% for fields
    uint32_t object_capacity = static_cast<uint32_t>((available * 50 / 100) / sizeof(ObjectEntry));
    uint32_t type_capacity = static_cast<uint32_t>((available * 10 / 100) / sizeof(TypeEntry));
    uint32_t field_capacity = static_cast<uint32_t>((available * 40 / 100) / sizeof(FieldEntry));

    size_t object_size = object_capacity * sizeof(ObjectEntry);
    size_t type_size = type_capacity * sizeof(TypeEntry);
    size_t field_size = field_capacity * sizeof(FieldEntry);

    size_t total_size = header_size + object_size + type_size + field_size;

    if (!region->shm.create(shm_name, total_size)) {
        return nullptr;
    }

    // Initialize descriptor
    region->descriptor = static_cast<MetadataOverflowDescriptor*>(region->shm.data());
    std::memset(region->descriptor, 0, sizeof(MetadataOverflowDescriptor));

    region->descriptor->magic = OVERFLOW_MAGIC;
    region->descriptor->region_id = region->id;
    region->descriptor->next_region_id.store(0, std::memory_order_release);

    // Object entries section
    region->descriptor->object_entry_offset = static_cast<uint32_t>(header_size);
    region->descriptor->object_entry_capacity = object_capacity;
    region->descriptor->object_entry_count.store(0, std::memory_order_release);

    // Type entries section
    region->descriptor->type_entry_offset = static_cast<uint32_t>(header_size + object_size);
    region->descriptor->type_entry_capacity = type_capacity;
    region->descriptor->type_entry_count.store(0, std::memory_order_release);

    // Field entries section
    region->descriptor->field_entry_offset = static_cast<uint32_t>(header_size + object_size + type_size);
    region->descriptor->field_entry_capacity = field_capacity;
    region->descriptor->field_entry_count.store(0, std::memory_order_release);

    region->descriptor->set_shm_name(shm_name);

    // Link to previous overflow region if exists
    if (!overflow_regions_.empty()) {
        overflow_regions_.back()->descriptor->next_region_id.store(
            region->id, std::memory_order_release);
    } else {
        // First overflow region - link from header
        ctx_.header()->first_overflow_region_id.store(region->id, std::memory_order_release);
    }

    OverflowRegion* ptr = region.get();
    overflow_regions_.push_back(std::move(region));

    // Increment sequence for observers to detect new region
    ctx_.header()->sequence.fetch_add(1, std::memory_order_release);

    return ptr;
}

MetadataManager::OverflowRegion* MetadataManager::current_overflow_region() {
    if (overflow_regions_.empty()) return nullptr;
    return overflow_regions_.back().get();
}

ObjectEntry* MetadataManager::allocate_object_entry() {
    std::lock_guard<std::mutex> lock(mutex_);

    TelemetryHeader* header = ctx_.header();

    // First try to allocate from header
    uint32_t count = header->object_count.load(std::memory_order_acquire);
    if (count < header->object_dir_capacity) {
        auto* entries = reinterpret_cast<ObjectEntry*>(
            static_cast<char*>(ctx_.header_shm().data()) + header->object_dir_offset);
        header->object_count.store(count + 1, std::memory_order_release);
        return &entries[count];
    }

    // Header full - try overflow regions
    OverflowRegion* region = current_overflow_region();
    if (region) {
        uint32_t overflow_count = region->descriptor->object_entry_count.load(std::memory_order_acquire);
        if (overflow_count < region->descriptor->object_entry_capacity) {
            auto* entries = reinterpret_cast<ObjectEntry*>(
                static_cast<char*>(region->shm.data()) + region->descriptor->object_entry_offset);
            region->descriptor->object_entry_count.store(overflow_count + 1, std::memory_order_release);
            return &entries[overflow_count];
        }
    }

    // Need new overflow region
    region = create_overflow_region();
    if (!region) return nullptr;

    auto* entries = reinterpret_cast<ObjectEntry*>(
        static_cast<char*>(region->shm.data()) + region->descriptor->object_entry_offset);
    region->descriptor->object_entry_count.store(1, std::memory_order_release);
    return &entries[0];
}

TypeEntry* MetadataManager::allocate_type_entry() {
    std::lock_guard<std::mutex> lock(mutex_);

    TelemetryHeader* header = ctx_.header();

    // First try to allocate from header
    uint32_t count = header->type_count.load(std::memory_order_acquire);
    if (count < header->type_registry_capacity) {
        auto* entries = reinterpret_cast<TypeEntry*>(
            static_cast<char*>(ctx_.header_shm().data()) + header->type_registry_offset);
        header->type_count.store(count + 1, std::memory_order_release);
        return &entries[count];
    }

    // Header full - try overflow regions
    OverflowRegion* region = current_overflow_region();
    if (region) {
        uint32_t overflow_count = region->descriptor->type_entry_count.load(std::memory_order_acquire);
        if (overflow_count < region->descriptor->type_entry_capacity) {
            auto* entries = reinterpret_cast<TypeEntry*>(
                static_cast<char*>(region->shm.data()) + region->descriptor->type_entry_offset);
            region->descriptor->type_entry_count.store(overflow_count + 1, std::memory_order_release);
            return &entries[overflow_count];
        }
    }

    // Need new overflow region
    region = create_overflow_region();
    if (!region) return nullptr;

    auto* entries = reinterpret_cast<TypeEntry*>(
        static_cast<char*>(region->shm.data()) + region->descriptor->type_entry_offset);
    region->descriptor->type_entry_count.store(1, std::memory_order_release);
    return &entries[0];
}

FieldEntry* MetadataManager::allocate_field_entries(uint32_t count) {
    if (count == 0) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);

    TelemetryHeader* header = ctx_.header();

    // First try to allocate from header
    uint32_t current = header->field_count.load(std::memory_order_acquire);
    if (current + count <= header->field_entries_capacity) {
        auto* entries = reinterpret_cast<FieldEntry*>(
            static_cast<char*>(ctx_.header_shm().data()) + header->field_entries_offset);
        header->field_count.store(current + count, std::memory_order_release);
        return &entries[current];
    }

    // Header full - try overflow regions
    OverflowRegion* region = current_overflow_region();
    if (region) {
        uint32_t overflow_count = region->descriptor->field_entry_count.load(std::memory_order_acquire);
        if (overflow_count + count <= region->descriptor->field_entry_capacity) {
            auto* entries = reinterpret_cast<FieldEntry*>(
                static_cast<char*>(region->shm.data()) + region->descriptor->field_entry_offset);
            region->descriptor->field_entry_count.store(overflow_count + count, std::memory_order_release);
            return &entries[overflow_count];
        }
    }

    // Need new overflow region
    region = create_overflow_region();
    if (!region) return nullptr;

    // Check if new region can hold all requested fields
    if (count > region->descriptor->field_entry_capacity) {
        return nullptr;  // Request too large for a single region
    }

    auto* entries = reinterpret_cast<FieldEntry*>(
        static_cast<char*>(region->shm.data()) + region->descriptor->field_entry_offset);
    region->descriptor->field_entry_count.store(count, std::memory_order_release);
    return &entries[0];
}

uint32_t MetadataManager::total_object_count() const {
    uint32_t total = ctx_.header()->object_count.load(std::memory_order_acquire);
    for (const auto& region : overflow_regions_) {
        total += region->descriptor->object_entry_count.load(std::memory_order_acquire);
    }
    return total;
}

uint32_t MetadataManager::total_type_count() const {
    uint32_t total = ctx_.header()->type_count.load(std::memory_order_acquire);
    for (const auto& region : overflow_regions_) {
        total += region->descriptor->type_entry_count.load(std::memory_order_acquire);
    }
    return total;
}

uint32_t MetadataManager::total_field_count() const {
    uint32_t total = ctx_.header()->field_count.load(std::memory_order_acquire);
    for (const auto& region : overflow_regions_) {
        total += region->descriptor->field_entry_count.load(std::memory_order_acquire);
    }
    return total;
}

// ObjectManager implementation

ObjectManager::ObjectManager(Context& ctx)
    : ctx_(ctx)
{
}

ObjectEntry* ObjectManager::register_object(void* ptr, uint32_t type_id, std::string_view label) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get location
    uint64_t region_id, offset;
    if (!ctx_.regions().get_location(ptr, region_id, offset)) {
        return nullptr;
    }

    // Allocate entry via MetadataManager (handles overflow automatically)
    ObjectEntry* entry = ctx_.metadata().allocate_object_entry();
    if (!entry) {
        return nullptr;
    }

    // Initialize entry
    entry->state.store(static_cast<uint32_t>(ObjectState::Alive), std::memory_order_release);
    entry->type_id = type_id;
    entry->region_id = region_id;
    entry->offset = offset;
    entry->generation = 1;
    entry->set_label(label);

    // Increment sequence for observers
    ctx_.header()->sequence.fetch_add(1, std::memory_order_release);

    ptr_to_entry_[ptr] = entry;

    return entry;
}

void ObjectManager::destroy_object(void* ptr) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = ptr_to_entry_.find(ptr);
    if (it != ptr_to_entry_.end()) {
        it->second->state.store(static_cast<uint32_t>(ObjectState::Destroyed),
                                std::memory_order_release);
        ctx_.header()->sequence.fetch_add(1, std::memory_order_release);
        ptr_to_entry_.erase(it);
    }
}

ObjectEntry* ObjectManager::find_object(std::string_view label) {
    std::lock_guard<std::mutex> lock(mutex_);

    TelemetryHeader* header = ctx_.header();
    uint32_t count = header->object_count.load(std::memory_order_acquire);

    auto* entries = reinterpret_cast<ObjectEntry*>(
        static_cast<char*>(ctx_.header_shm().data()) + header->object_dir_offset);

    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].state.load(std::memory_order_acquire) ==
                static_cast<uint32_t>(ObjectState::Alive) &&
            std::string_view(entries[i].label) == label) {
            return &entries[i];
        }
    }
    return nullptr;
}

std::vector<ObjectEntry*> ObjectManager::get_all_objects() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ObjectEntry*> result;
    TelemetryHeader* header = ctx_.header();
    uint32_t count = header->object_count.load(std::memory_order_acquire);

    auto* entries = reinterpret_cast<ObjectEntry*>(
        static_cast<char*>(ctx_.header_shm().data()) + header->object_dir_offset);

    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].state.load(std::memory_order_acquire) ==
                static_cast<uint32_t>(ObjectState::Alive)) {
            result.push_back(&entries[i]);
        }
    }
    return result;
}

} // namespace memglass
