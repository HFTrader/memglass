#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace memglass {

// Magic numbers
constexpr uint64_t HEADER_MAGIC = 0x4D454D474C415353ULL;  // "MEMGLASS"
constexpr uint64_t REGION_MAGIC = 0x5245474E4D454D47ULL;  // "REGNMEMG"
constexpr uint64_t OVERFLOW_MAGIC = 0x4F56464C574D4547ULL; // "OVFLWMEG"
constexpr uint32_t PROTOCOL_VERSION = 1;

// Primitive type IDs for reflection
enum class PrimitiveType : uint32_t {
    Unknown = 0,
    Bool = 1,
    Int8 = 2,
    UInt8 = 3,
    Int16 = 4,
    UInt16 = 5,
    Int32 = 6,
    UInt32 = 7,
    Int64 = 8,
    UInt64 = 9,
    Float32 = 10,
    Float64 = 11,
    Char = 12,
    // User types start at 0x10000
    UserTypeBase = 0x10000
};

// Atomicity levels
enum class Atomicity : uint8_t {
    None = 0,      // Direct access, may tear
    Atomic = 1,    // std::atomic<T>
    Seqlock = 2,   // Guarded<T> seqlock
    Locked = 3     // Locked<T> spinlock
};

// Object states
enum class ObjectState : uint32_t {
    Free = 0,
    Alive = 1,
    Destroyed = 2
};

// Field flags
enum class FieldFlags : uint32_t {
    None = 0,
    IsArray = 1 << 0,
    IsNested = 1 << 1,
    ReadOnly = 1 << 2
};

inline FieldFlags operator|(FieldFlags a, FieldFlags b) {
    return static_cast<FieldFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(FieldFlags a, FieldFlags b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Shared memory structures (must be trivially copyable and have standard layout)

struct FieldEntry {
    uint32_t offset;           // Offset within object
    uint32_t size;             // Size of field
    uint32_t type_id;          // Type ID (PrimitiveType or user type)
    uint32_t flags;            // FieldFlags
    uint32_t array_size;       // For arrays, element count (0 = not array)
    Atomicity atomicity;       // Atomicity level
    uint8_t padding[3];
    char name[64];             // Field name

    void set_name(std::string_view n) {
        size_t len = std::min(n.size(), sizeof(name) - 1);
        std::memcpy(name, n.data(), len);
        name[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<FieldEntry>);

struct TypeEntry {
    uint32_t type_id;          // Unique type identifier
    uint32_t size;             // sizeof(T)
    uint32_t alignment;        // alignof(T)
    uint32_t field_count;
    uint64_t fields_offset;    // Offset to FieldEntry array in registry
    char name[128];            // Type name

    void set_name(std::string_view n) {
        size_t len = std::min(n.size(), sizeof(name) - 1);
        std::memcpy(name, n.data(), len);
        name[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<TypeEntry>);

struct ObjectEntry {
    std::atomic<uint32_t> state;  // ObjectState
    uint32_t type_id;             // References TypeEntry
    uint64_t region_id;           // Which region contains the object
    uint64_t offset;              // Offset within that region
    uint64_t generation;          // Incremented on reuse (ABA prevention)
    char label[64];               // Instance label

    void set_label(std::string_view l) {
        size_t len = std::min(l.size(), sizeof(label) - 1);
        std::memcpy(label, l.data(), len);
        label[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<ObjectEntry>);

struct RegionDescriptor {
    uint64_t magic;                      // REGION_MAGIC
    uint64_t region_id;                  // Unique ID for this region
    uint64_t size;                       // Total region size
    std::atomic<uint64_t> used;          // Bytes allocated
    std::atomic<uint64_t> next_region_id;// Next region, 0 = none
    char shm_name[64];                   // Shared memory name

    void set_shm_name(std::string_view n) {
        size_t len = std::min(n.size(), sizeof(shm_name) - 1);
        std::memcpy(shm_name, n.data(), len);
        shm_name[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<RegionDescriptor>);

// Metadata overflow region descriptor - for growable type/field/object storage
struct MetadataOverflowDescriptor {
    uint64_t magic;                        // OVERFLOW_MAGIC
    uint64_t region_id;                    // Unique ID for this overflow region
    std::atomic<uint64_t> next_region_id;  // Next overflow region, 0 = end of chain

    // Object directory overflow section
    uint32_t object_entry_offset;          // Offset to ObjectEntry array
    uint32_t object_entry_capacity;        // Max entries in this region
    std::atomic<uint32_t> object_entry_count; // Current count

    // Type registry overflow section
    uint32_t type_entry_offset;            // Offset to TypeEntry array
    uint32_t type_entry_capacity;          // Max entries in this region
    std::atomic<uint32_t> type_entry_count; // Current count

    // Field entries overflow section
    uint32_t field_entry_offset;           // Offset to FieldEntry array
    uint32_t field_entry_capacity;         // Max entries in this region
    std::atomic<uint32_t> field_entry_count; // Current count

    char shm_name[64];                     // Shared memory name

    void set_shm_name(std::string_view n) {
        size_t len = std::min(n.size(), sizeof(shm_name) - 1);
        std::memcpy(shm_name, n.data(), len);
        shm_name[len] = '\0';
    }
};
static_assert(std::is_trivially_copyable_v<MetadataOverflowDescriptor>);

struct TelemetryHeader {
    uint64_t magic;                      // HEADER_MAGIC
    uint32_t version;                    // Protocol version
    uint32_t header_size;                // Size of this struct

    std::atomic<uint64_t> sequence;      // Incremented on structural change

    // Type registry location (inline in header region)
    uint64_t type_registry_offset;
    uint32_t type_registry_capacity;
    std::atomic<uint32_t> type_count;

    // Field entries location (inline in header region)
    uint64_t field_entries_offset;
    uint32_t field_entries_capacity;
    std::atomic<uint32_t> field_count;

    // Object directory location (inline in header region)
    uint64_t object_dir_offset;
    uint32_t object_dir_capacity;
    std::atomic<uint32_t> object_count;

    // First data region
    std::atomic<uint64_t> first_region_id;

    // First metadata overflow region (for growable metadata)
    std::atomic<uint64_t> first_overflow_region_id;

    char session_name[64];               // Human-readable session identifier
    uint64_t producer_pid;               // Producer process ID
    uint64_t start_timestamp;            // When session started
};
static_assert(std::is_trivially_copyable_v<TelemetryHeader>);

// Configuration
struct Config {
    size_t initial_region_size = 1024 * 1024;       // 1 MB
    size_t max_region_size = 64 * 1024 * 1024;      // 64 MB
    size_t overflow_region_size = 256 * 1024;       // 256 KB for metadata overflow
    uint32_t max_types = 256;
    uint32_t max_fields = 4096;
    uint32_t max_objects = 4096;
};

// Type trait to check if a type is observable (POD)
template<typename T>
concept Observable = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

} // namespace memglass
