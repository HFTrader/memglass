# memglass - Live Memory Observation for C++ Objects

## Overview

A C++20 library for real-time cross-process observation of C++ POD object state using shared memory. A **producer** application constructs objects in shared memory with automatic field registration, while an **observer** process maps the same memory to inspect object state without stopping or instrumenting the producer.

**Constraints**: POD types only (trivially copyable). No `std::string`, `std::vector`, or pointer-containing types.

## Goals

1. **Non-invasive observation** - Observer reads memory without affecting producer performance
2. **Dynamic growth** - No fixed memory limit; regions allocated on demand
3. **Type-aware introspection** - Observer understands field names, types, and structure
4. **Zero boilerplate** - Automatic reflection via clang tooling, no macros needed
5. **Zero-copy** - Observer reads directly from shared memory, no serialization

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        PRODUCER PROCESS                         │
│                                                                 │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐         │
│  │ User Code   │───▶│ memglass    │───▶│ Shared      │         │
│  │ MyClass obj │    │ Allocator   │    │ Memory      │         │
│  └─────────────┘    └─────────────┘    │ Regions     │         │
│                                        └──────┬──────┘         │
└───────────────────────────────────────────────┼─────────────────┘
                                                │
                        ┌───────────────────────┴───────────────┐
                        │         SHARED MEMORY (shm)           │
                        │                                       │
                        │  ┌─────────┐   ┌─────────┐           │
                        │  │ Header  │──▶│ Region  │──▶ ...    │
                        │  │ Region  │   │ 2       │           │
                        │  └─────────┘   └─────────┘           │
                        │                                       │
                        │  • Type Registry (schemas)            │
                        │  • Object Directory (instances)       │
                        │  • Field Data (actual values)         │
                        └───────────────────────────────────────┘
                                                │
┌───────────────────────────────────────────────┼─────────────────┐
│                       OBSERVER PROCESS        │                 │
│                                               ▼                 │
│  ┌─────────────┐    ┌─────────────┐    ┌──────┴──────┐         │
│  │ UI / CLI   │◀───│ memglass    │◀───│ Memory      │         │
│  │ Dashboard   │    │ Reader      │    │ Mapped View │         │
│  └─────────────┘    └─────────────┘    └─────────────┘         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Shared Memory Layout

### Header Region (Fixed Name: `memglass_header`)

The bootstrap region that observer maps first.

```cpp
struct TelemetryHeader {
    uint64_t magic;              // 0x54454C454D455452 ("TELEMTR\0")
    uint32_t version;            // Protocol version
    uint32_t header_size;        // Size of this struct

    std::atomic<uint64_t> sequence;  // Incremented on any structural change

    // Type registry location
    uint64_t type_registry_offset;
    uint32_t type_registry_capacity;
    std::atomic<uint32_t> type_count;

    // Object directory location
    uint64_t object_dir_offset;
    uint32_t object_dir_capacity;
    std::atomic<uint32_t> object_count;

    // Linked list of additional regions
    std::atomic<uint64_t> next_region_id;  // 0 = none

    char session_name[64];       // Human-readable session identifier
    uint64_t producer_pid;       // Producer process ID
    uint64_t start_timestamp;    // When session started
};
```

### Region Descriptor

Each additional region starts with:

```cpp
struct RegionDescriptor {
    uint64_t magic;              // 0x5245474E ("REGN")
    uint64_t region_id;          // Unique ID for this region
    uint64_t size;               // Total region size
    std::atomic<uint64_t> used;  // Bytes allocated
    std::atomic<uint64_t> next_region_id;  // Next region, 0 = none
    char shm_name[64];           // Shared memory name for this region
};
```

### Type Registry Entry

Describes a registered type's schema:

```cpp
struct TypeEntry {
    uint32_t type_id;            // Unique type identifier (hash of name)
    uint32_t size;               // sizeof(T)
    uint32_t alignment;          // alignof(T)
    uint32_t field_count;
    uint64_t fields_offset;      // Offset to FieldEntry array
    char name[128];              // Type name (e.g., "MyNamespace::MyClass")
};

struct FieldEntry {
    uint32_t offset;             // Offset within object
    uint32_t size;               // Size of field
    uint32_t type_id;            // Type ID (for nested types) or primitive ID
    uint32_t flags;              // Flags (is_pointer, is_array, etc.)
    uint32_t array_size;         // For fixed arrays, element count
    char name[64];               // Field name
};
```

### Primitive Type IDs

```cpp
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
    Pointer = 12,      // Observed as raw address, not followed
    StdString = 13,    // Special handling
    // User types start at 0x10000
};
```

### Object Directory Entry

Tracks each live object instance:

```cpp
struct ObjectEntry {
    std::atomic<uint32_t> state;  // 0=free, 1=alive, 2=destroyed
    uint32_t type_id;             // References TypeEntry
    uint64_t region_id;           // Which region contains the object
    uint64_t offset;              // Offset within that region
    uint64_t generation;          // Incremented on reuse (ABA prevention)
    char label[64];               // Optional instance label
};
```

## Producer API

### Initialization

```cpp
#include <memglass/memglass.hpp>

int main() {
    // Initialize memglass with session name
    memglass::init("my_application");

    // Optional: configure initial region size (default 1MB)
    memglass::config().initial_region_size = 4 * 1024 * 1024;

    // ... application code ...

    memglass::shutdown();
}
```

### Type Registration (Automatic via Clang Tooling)

Simply mark your structs with an attribute - no macros, no manual field listing:

```cpp
// game_types.hpp
struct [[memglass::observe]] Vec3 {
    float x, y, z;
};

struct [[memglass::observe]] Player {
    uint32_t id;
    Vec3 position;
    Vec3 velocity;
    float health;
    bool is_active;
};
```

The `memglass-gen` tool (a clang-based code generator) parses your headers and automatically generates all reflection data:

```bash
# Run at build time (integrated into CMake)
memglass-gen --output=memglass_generated.hpp include/game_types.hpp
```

This generates type descriptors with full field information extracted from clang's AST:

```cpp
// memglass_generated.hpp (auto-generated, do not edit)
namespace memglass::generated {

template<> struct TypeDescriptor<Vec3> {
    static constexpr std::string_view name = "Vec3";
    static constexpr size_t size = 12;
    static constexpr size_t alignment = 4;
    static constexpr std::array<FieldInfo, 3> fields = {{
        {"x", offsetof(Vec3, x), sizeof(float), PrimitiveType::Float32, 0},
        {"y", offsetof(Vec3, y), sizeof(float), PrimitiveType::Float32, 0},
        {"z", offsetof(Vec3, z), sizeof(float), PrimitiveType::Float32, 0},
    }};
};

template<> struct TypeDescriptor<Player> {
    static constexpr std::string_view name = "Player";
    static constexpr size_t size = 28;
    static constexpr size_t alignment = 4;
    static constexpr std::array<FieldInfo, 5> fields = {{
        {"id", offsetof(Player, id), sizeof(uint32_t), PrimitiveType::UInt32, 0},
        {"position", offsetof(Player, position), sizeof(Vec3), TypeId<Vec3>, 0},
        {"velocity", offsetof(Player, velocity), sizeof(Vec3), TypeId<Vec3>, 0},
        {"health", offsetof(Player, health), sizeof(float), PrimitiveType::Float32, 0},
        {"is_active", offsetof(Player, is_active), sizeof(bool), PrimitiveType::Bool, 0},
    }};
};

inline void register_all_types() {
    memglass::registry::add<Vec3>();
    memglass::registry::add<Player>();
}

} // namespace memglass::generated
```

### Object Allocation

```cpp
// Allocate in memglass-managed shared memory
Player* player = memglass::create<Player>("player_1");

// Construct with arguments
Player* player = memglass::create<Player>("player_2",
    Player{.id = 42, .position = {0,0,0}, .health = 100.0f});

// Array allocation
Player* players = memglass::create_array<Player>("all_players", 100);

// Destruction (marks as destroyed, memory reclaimed later)
memglass::destroy(player);
```

### Manual Memory Management (Advanced)

```cpp
// Get raw allocator for custom containers
memglass::Allocator<Player> alloc;
std::vector<Player, memglass::Allocator<Player>> players(alloc);

// Placement new in memglass memory
void* mem = memglass::allocate(sizeof(Player), alignof(Player));
Player* p = new (mem) Player{};
memglass::register_object(p, "manual_player");
```

## Observer API

### Connecting

```cpp
#include <memglass/observer.hpp>

int main() {
    // Connect to running session
    memglass::Observer observer("my_application");

    if (!observer.connect()) {
        std::cerr << "Failed to connect\n";
        return 1;
    }

    std::cout << "Connected to PID: " << observer.producer_pid() << "\n";
```

### Listing Types and Objects

```cpp
    // Enumerate registered types
    for (const auto& type : observer.types()) {
        std::cout << "Type: " << type.name << " (" << type.size << " bytes)\n";
        for (const auto& field : type.fields) {
            std::cout << "  " << field.name << ": " << field.type_name << "\n";
        }
    }

    // Enumerate live objects
    for (const auto& obj : observer.objects()) {
        std::cout << "Object: " << obj.label
                  << " [" << obj.type_name << "] "
                  << " @ region " << obj.region_id << "+" << obj.offset << "\n";
    }
```

### Reading Object State

```cpp
    // Find object by label
    auto player_view = observer.find("player_1");
    if (player_view) {
        // Read fields by name
        float health = player_view.read<float>("health");
        bool active = player_view.read<bool>("is_active");

        // Read nested struct
        auto pos = player_view.field("position");
        float x = pos.read<float>("x");

        // Read entire object (copies to local memory)
        Player local_copy = player_view.read_as<Player>();

        // Raw memory access
        const void* raw = player_view.data();
    }
```

### Watching for Changes

```cpp
    // Poll for structural changes (new types, objects)
    uint64_t last_seq = 0;
    while (running) {
        if (observer.sequence() != last_seq) {
            last_seq = observer.sequence();
            // Re-enumerate objects, types may have changed
            observer.refresh();
        }

        // Read current values
        auto player = observer.find("player_1");
        if (player) {
            std::cout << "Health: " << player.read<float>("health") << "\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
```

## Synchronization Strategy

### Producer Side

- **Type registration**: Happens once at startup, protected by mutex
- **Object creation/destruction**: Updates object directory atomically
- **Field writes**: No locking (observer may see torn reads for multi-word values)
- **Sequence counter**: Incremented after structural changes complete

### Observer Side

- **Structural reads**: Check sequence before and after; retry if changed
- **Field reads**: Accept that non-atomic fields may have torn values
- **Memory mapping**: Lazily map new regions as they appear

### Optional Strong Consistency

For fields requiring atomic reads:

```cpp
struct Player {
    std::atomic<float> health;  // Observer sees consistent value
    memglass::Guarded<Vec3> position;  // Seqlock-protected compound value
};
```

The `Guarded<T>` wrapper uses a seqlock:

```cpp
template<typename T>
struct Guarded {
    std::atomic<uint32_t> seq;
    T value;

    void write(const T& v) {
        seq.fetch_add(1, std::memory_order_release);
        value = v;
        seq.fetch_add(1, std::memory_order_release);
    }

    T read() const {
        T result;
        uint32_t s;
        do {
            s = seq.load(std::memory_order_acquire);
            if (s & 1) continue;  // Write in progress
            result = value;
        } while (seq.load(std::memory_order_acquire) != s);
        return result;
    }
};
```

## Region Management

### Allocation Strategy

1. Producer starts with header region + initial data region
2. When region fills, allocate new region with 2x size (up to max)
3. Link new region via `next_region_id` in previous region
4. Update header's region chain atomically

### Region Naming

```
memglass_{session_name}_header
memglass_{session_name}_region_0001
memglass_{session_name}_region_0002
...
```

### Observer Region Discovery

```cpp
void Observer::refresh() {
    // Start from header
    uint64_t next_id = header_->next_region_id.load();

    while (next_id != 0 && !regions_.contains(next_id)) {
        // Construct expected shm name
        std::string name = fmt::format("memglass_{}_region_{:04d}",
                                       session_name_, next_id);

        // Map the new region
        auto region = map_region(name);
        regions_[next_id] = std::move(region);

        // Follow the chain
        next_id = regions_[next_id]->next_region_id.load();
    }
}
```

## Memory Reclamation

### Deferred Destruction

When `memglass::destroy()` is called:

1. Object state set to `destroyed`
2. Memory not immediately freed (observer might be reading)
3. Background thread periodically scans for destroyed objects
4. Objects destroyed for >N seconds have memory reclaimed
5. ObjectEntry marked as `free` for reuse

### Graceful Shutdown

```cpp
memglass::shutdown();
// 1. Stop accepting new allocations
// 2. Wait for pending observers (optional timeout)
// 3. Unlink shared memory regions
```

## memglass-gen: Clang-Based Code Generator

The `memglass-gen` tool uses libclang to parse C++ headers and extract struct layouts automatically.

### How It Works

```
┌──────────────┐     ┌─────────────┐     ┌───────────────────┐
│ game_types.h │────▶│ memglass-gen│────▶│ memglass_generated│
│              │     │ (libclang)  │     │ .hpp              │
└──────────────┘     └─────────────┘     └───────────────────┘
       │                    │                      │
       │                    ▼                      │
       │            ┌─────────────┐                │
       │            │ Parse AST   │                │
       │            │ Find [[memglass::observe]]   │
       │            │ Extract fields│              │
       │            │ Compute offsets│             │
       │            └─────────────┘                │
       │                                           │
       └───────────────────────────────────────────┘
                    Compile together
```

### Generator Features

1. **Attribute detection** - Finds structs marked `[[memglass::observe]]`
2. **Recursive type resolution** - Handles nested structs automatically
3. **POD validation** - Warns/errors on non-trivially-copyable types
4. **Offset computation** - Uses clang's layout info for accurate offsets
5. **Array support** - Detects `T[N]` and `std::array<T,N>`
6. **Namespace preservation** - Generates fully qualified names

### CMake Integration

```cmake
find_package(memglass REQUIRED)

# Automatically generate reflection code for your headers
memglass_generate(
    TARGET my_game
    HEADERS
        include/game_types.hpp
        include/player.hpp
    OUTPUT ${CMAKE_BINARY_DIR}/generated/memglass_generated.hpp
)

target_link_libraries(my_game PRIVATE memglass::memglass)
```

### Command Line Usage

```bash
# Basic usage
memglass-gen -o memglass_generated.hpp input.hpp

# With include paths
memglass-gen -I/usr/include -I./include -o out.hpp src/*.hpp

# Verbose mode (shows discovered types)
memglass-gen -v -o out.hpp input.hpp

# Dry run (parse only, no output)
memglass-gen --dry-run input.hpp
```

### Generator Implementation

The generator is built using libclang's C API (~600 lines):

```cpp
// Simplified structure of memglass-gen
class MemglassGenerator {
    CXIndex index_;
    std::vector<TypeInfo> discovered_types_;

public:
    void parse(const std::string& filename,
               const std::vector<std::string>& args);
    void visit_cursor(CXCursor cursor);
    bool has_memglass_attribute(CXCursor cursor);
    TypeInfo extract_struct_info(CXCursor cursor);
    FieldInfo extract_field_info(CXCursor field);
    void emit_header(std::ostream& out);
};
```

Key libclang functions used:
- `clang_parseTranslationUnit()` - Parse the source file
- `clang_visitChildren()` - Walk the AST
- `clang_getCursorKind()` - Identify structs/classes
- `clang_Cursor_hasAttrs()` - Check for attributes
- `clang_Type_getSizeOf()` - Get type sizes
- `clang_Type_getOffsetOf()` - Get field offsets

## File Structure

```
memglass/
├── CMakeLists.txt
├── include/
│   └── memglass/
│       ├── memglass.hpp       # Main producer header
│       ├── observer.hpp       # Observer header
│       ├── allocator.hpp      # Shared memory allocator
│       ├── registry.hpp       # Type registration (runtime)
│       ├── types.hpp          # Common types, primitives
│       ├── attribute.hpp      # [[memglass::observe]] definition
│       └── detail/
│           ├── shm.hpp        # Platform shared memory abstraction
│           ├── region.hpp     # Region management
│           └── seqlock.hpp    # Seqlock implementation
├── src/
│   ├── memglass.cpp
│   ├── observer.cpp
│   ├── allocator.cpp
│   ├── registry.cpp
│   └── platform/
│       ├── shm_posix.cpp
│       └── shm_windows.cpp
├── tools/
│   └── memglass-gen/
│       ├── CMakeLists.txt     # Links against libclang
│       ├── main.cpp           # CLI entry point
│       ├── generator.hpp      # Generator class
│       ├── generator.cpp      # AST traversal, code emission
│       └── type_mapper.cpp    # C++ type to PrimitiveType mapping
├── cmake/
│   ├── FindLibClang.cmake
│   └── MemglassGenerate.cmake # memglass_generate() function
├── examples/
│   ├── producer/
│   │   ├── CMakeLists.txt
│   │   ├── game_types.hpp     # Structs with [[memglass::observe]]
│   │   └── producer.cpp
│   └── observer/
│       ├── CMakeLists.txt
│       └── observer_cli.cpp
└── tests/
    ├── test_allocator.cpp
    ├── test_registry.cpp
    ├── test_generator.cpp     # Tests for memglass-gen
    └── test_integration.cpp
```

## Example: Complete Usage

### Producer (Game Server)

```cpp
// game_types.hpp
#pragma once
#include <cstdint>
#include <atomic>

struct [[memglass::observe]] Vec3 {
    float x, y, z;
};

struct [[memglass::observe]] Entity {
    uint32_t id;
    Vec3 position;
    Vec3 velocity;
    float health;
    std::atomic<bool> is_active;
};
```

```cpp
// producer.cpp
#include <memglass/memglass.hpp>
#include "game_types.hpp"
#include "memglass_generated.hpp"  // Auto-generated by memglass-gen
#include <thread>
#include <cmath>

int main() {
    memglass::init("game_server");
    memglass::generated::register_all_types();  // Register discovered types

    // Create some entities
    std::vector<Entity*> entities;
    for (int i = 0; i < 10; i++) {
        auto* e = memglass::create<Entity>(fmt::format("entity_{}", i));
        e->id = i;
        e->position = {float(i), 0.0f, 0.0f};
        e->health = 100.0f;
        e->is_active = true;
        entities.push_back(e);
    }

    // Simulation loop
    float t = 0;
    while (true) {
        for (auto* e : entities) {
            e->position.x = std::sin(t + e->id) * 10.0f;
            e->position.z = std::cos(t + e->id) * 10.0f;
        }
        t += 0.016f;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    memglass::shutdown();
}
```

### Observer (Debug Tool)

```cpp
#include <memglass/observer.hpp>
#include <iostream>

int main() {
    memglass::Observer obs("game_server");

    if (!obs.connect()) {
        std::cerr << "Cannot connect to game_server\n";
        return 1;
    }

    while (true) {
        obs.refresh();

        std::cout << "\033[2J\033[H";  // Clear screen
        std::cout << "=== Game Server Telemetry ===\n\n";

        for (const auto& obj : obs.objects()) {
            if (obj.type_name == "Entity") {
                auto view = obs.get(obj);
                auto pos = view.field("position");

                std::cout << fmt::format("{}: pos=({:.1f}, {:.1f}, {:.1f}) health={:.0f}\n",
                    obj.label,
                    pos.read<float>("x"),
                    pos.read<float>("y"),
                    pos.read<float>("z"),
                    view.read<float>("health")
                );
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

## Platform Support

| Feature | Linux | macOS | Windows |
|---------|-------|-------|---------|
| POSIX shm_open | ✓ | ✓ | - |
| Windows CreateFileMapping | - | - | ✓ |
| Memory mapping | mmap | mmap | MapViewOfFile |
| Atomic operations | ✓ | ✓ | ✓ |

## Future Extensions

1. **Compression** - Optional compression for large objects
2. **Versioning** - Schema evolution support
3. **Network observer** - TCP transport for remote observation
4. **Recording** - Snapshot and playback of memglass streams
5. **Annotations** - User-defined metadata on objects/fields
6. **Triggers** - Observer callbacks on value changes

## Dependencies

- C++20 compiler (GCC 10+, Clang 12+, MSVC 19.29+)
- fmt (formatting, can be replaced with std::format on C++23)
- Optional: Boost.Interprocess (alternative allocator backend)

## Open Questions

1. **String handling** - Should `std::string` be supported directly, or require fixed-size char arrays?
2. **Containers** - Support `std::vector` in shared memory? (Complex due to allocator requirements)
3. **Inheritance** - Support polymorphic types? (Would need RTTI-like metadata)
4. **Atomics by default** - Should all primitive fields use `std::atomic` for consistency?

---

*This proposal is ready for implementation. Estimated scope: ~2000-3000 lines of C++ for core functionality.*
