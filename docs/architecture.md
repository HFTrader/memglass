# Architecture

This document describes the internal architecture of memglass.

## Overview

memglass enables real-time cross-process observation of C++ POD objects through shared memory. The design prioritizes:

1. **Non-invasive observation** - Observer reads without affecting producer performance
2. **Zero-copy access** - Direct memory reads, no serialization
3. **Type-aware introspection** - Observer understands field names, types, and structure
4. **Dynamic growth** - No fixed memory limit; regions allocated on demand

```
┌─────────────────────────────────────────────────────────────────┐
│                        PRODUCER PROCESS                         │
│                                                                 │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐          │
│  │ User Code   │───▶│ memglass    │───▶│ Shared      │          │
│  │ MyClass obj │    │ Allocator   │    │ Memory      │          │
│  └─────────────┘    └─────────────┘    │ Regions     │          │
│                                        └──────┬──────┘          │
└───────────────────────────────────────────────┼─────────────────┘
                                                │
                        ┌───────────────────────┴───────────────┐
                        │         SHARED MEMORY (shm)           │
                        │                                       │
                        │  ┌─────────┐   ┌─────────┐            │
                        │  │ Header  │──▶│ Region  │──▶ ...     │
                        │  │ Region  │   │ 2       │            │
                        │  └─────────┘   └─────────┘            │
                        │                                       │
                        │  • Type Registry (schemas)            │
                        │  • Object Directory (instances)       │
                        │  • Field Data (actual values)         │
                        └───────────────────────────────────────┘
                                                │
┌───────────────────────────────────────────────┼─────────────────┐
│                       OBSERVER PROCESS        │                 │
│                                               ▼                 │
│  ┌─────────────┐    ┌─────────────┐    ┌──────┴──────┐          │
│  │ UI / CLI    │◀───│ memglass    │◀───│ Memory      │          │
│  │ Dashboard   │    │ Observer    │    │ Mapped View │          │
│  └─────────────┘    └─────────────┘    └─────────────┘          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Shared Memory Layout

### Header Region

The header is the bootstrap region that observers map first. Named `memglass_{session}_header`.

```cpp
struct TelemetryHeader {
    uint64_t magic;              // 0x4D454D474C415353 ("MEMGLASS")
    uint32_t version;            // Protocol version
    uint32_t header_size;        // Size of this struct

    std::atomic<uint64_t> sequence;  // Incremented on structural changes

    // Type registry
    uint64_t type_registry_offset;
    uint32_t type_registry_capacity;
    std::atomic<uint32_t> type_count;

    // Field entries (shared pool)
    uint64_t field_entries_offset;
    uint32_t field_entries_capacity;
    std::atomic<uint32_t> field_count;

    // Object directory
    uint64_t object_dir_offset;
    uint32_t object_dir_capacity;
    std::atomic<uint32_t> object_count;

    // Region chain
    std::atomic<uint64_t> first_region_id;

    // Session metadata
    char session_name[64];
    uint64_t producer_pid;
    uint64_t start_timestamp;
};
```

### Type Registry

Each registered type has a `TypeEntry`:

```cpp
struct TypeEntry {
    uint32_t type_id;        // Hash of type name
    char name[64];           // Type name
    uint32_t size;           // sizeof(T)
    uint32_t alignment;      // alignof(T)
    uint32_t field_count;    // Number of fields
    uint64_t fields_offset;  // Offset to FieldEntry array
};
```

### Field Entries

Fields are stored in a shared pool referenced by types:

```cpp
struct FieldEntry {
    char name[64];           // Field name (may include dots for nesting)
    uint32_t offset;         // Offset within object
    uint32_t size;           // Size of field
    uint32_t type_id;        // PrimitiveType or user type ID
    uint32_t array_size;     // 0 for non-arrays
    uint32_t flags;          // FieldFlags bitmask
    Atomicity atomicity;     // None, Atomic, Seqlock, Locked
    bool is_nested;          // True if this is a nested struct field
};
```

### Object Directory

Each live object has an `ObjectEntry`:

```cpp
struct ObjectEntry {
    std::atomic<uint32_t> state;  // ObjectState enum
    uint32_t type_id;             // References TypeEntry
    uint64_t region_id;           // Which region contains data
    uint64_t offset;              // Offset within region
    uint64_t generation;          // ABA prevention counter
    char label[64];               // Instance label
};

enum class ObjectState : uint32_t {
    Free = 0,
    Alive = 1,
    Destroyed = 2
};
```

### Data Regions

Additional regions for object data, named `memglass_{session}_region_{id}`:

```cpp
struct RegionDescriptor {
    uint64_t magic;              // 0x5245474E ("REGN")
    uint64_t region_id;
    uint64_t size;
    std::atomic<uint64_t> used;
    std::atomic<uint64_t> next_region_id;  // Linked list
    char shm_name[64];
};
```

---

## Type System

### Primitive Types

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
    Char = 12,
    // User types start at 0x10000
};
```

### Atomicity Levels

```cpp
enum class Atomicity : uint8_t {
    None = 0,    // Direct read/write
    Atomic = 1,  // std::atomic<T>
    Seqlock = 2, // Guarded<T>
    Locked = 3   // Locked<T>
};
```

### Nested Type Handling

Nested structs are flattened in the field entries. For example:

```cpp
struct Inner { int x; int y; };
struct Outer { Inner inner; int z; };
```

Generates fields:
- `inner.x` at offset 0
- `inner.y` at offset 4
- `z` at offset 8

---

## Synchronization

### Producer Side

- **Type registration**: Protected by internal mutex, happens at startup
- **Object creation**: Atomic updates to object directory, sequence increment
- **Field writes**: No locking by default; atomicity controlled by annotations

### Observer Side

- **Structural reads**: Check sequence before/after; refresh if changed
- **Field reads**: Respect atomicity annotation (atomic load, seqlock spin, etc.)
- **Region mapping**: Lazy mapping as new regions are discovered

### Sequence Counter

The header's `sequence` counter is incremented on:
- Type registration
- Object creation
- Object destruction
- Region allocation

Observers watch this to detect when they need to refresh their view.

---

## Memory Allocation

### Bump Allocator

Objects are allocated using a simple bump allocator within regions:

```cpp
void* allocate(size_t size, size_t alignment) {
    // Round up current offset to alignment
    uint64_t aligned = (current_offset + alignment - 1) & ~(alignment - 1);

    // Check if fits in current region
    if (aligned + size > region_size) {
        // Allocate new region
        allocate_new_region();
        return allocate(size, alignment);
    }

    void* ptr = region_base + aligned;
    current_offset = aligned + size;
    return ptr;
}
```

### Region Growth

1. Initial region created at session start (default 1MB)
2. When region fills, new region allocated (grows 2x up to max)
3. Regions linked via `next_region_id`
4. Observer discovers new regions by following chain

### Memory Reclamation

Destroyed objects are marked but memory not immediately freed:

1. `memglass::destroy()` sets state to `Destroyed`
2. Memory remains accessible (observers may be reading)
3. Future: background reclamation after grace period

---

## Code Generator

### Overview

`memglass-gen` uses libclang to parse C++ and generate reflection data:

```
Source.hpp → libclang AST → Generator → Generated.hpp
```

### Key libclang Functions

```cpp
clang_parseTranslationUnit()   // Parse source file
clang_visitChildren()          // Walk AST
clang_getCursorKind()          // Identify structs
clang_Cursor_hasAttrs()        // Check for [[memglass::observe]]
clang_Type_getSizeOf()         // Get type sizes
clang_Cursor_getOffsetOfField() // Get field offsets
clang_Cursor_getRawCommentText() // Extract annotations from comments
```

### Generation Process

1. Parse translation unit with system includes
2. Visit all top-level declarations
3. Find structs/classes with `[[memglass::observe]]` attribute
4. For each type:
   - Extract name, size, alignment
   - Visit fields recursively
   - Parse comment annotations (`@atomic`, etc.)
   - Handle nested structs (flatten with dot notation)
5. Emit registration functions

---

## Observer Implementation

### Connection

```cpp
bool Observer::connect() {
    // Open header shared memory
    std::string name = "memglass_" + session_name_ + "_header";
    if (!header_shm_.open(name)) return false;

    header_ = static_cast<TelemetryHeader*>(header_shm_.data());

    // Verify magic and version
    if (header_->magic != HEADER_MAGIC) return false;
    if (header_->version != PROTOCOL_VERSION) return false;

    // Load initial state
    load_types();
    load_regions();

    return true;
}
```

### Field Access

The `FieldProxy` class handles type-aware field access:

```cpp
template<typename T>
T FieldProxy::read() const {
    switch (field_->atomicity) {
        case Atomicity::Atomic:
            return reinterpret_cast<std::atomic<T>*>(data_)
                ->load(std::memory_order_acquire);

        case Atomicity::Seqlock:
            return reinterpret_cast<Guarded<T>*>(data_)->read();

        case Atomicity::Locked:
            return reinterpret_cast<Locked<T>*>(data_)->read();

        default:
            return *reinterpret_cast<T*>(data_);
    }
}
```

### Region Discovery

```cpp
void Observer::load_regions() {
    uint64_t region_id = header_->first_region_id.load();

    while (region_id != 0) {
        if (region_shms_.contains(region_id)) {
            // Already loaded, follow chain
            auto* desc = get_region_descriptor(region_id);
            region_id = desc->next_region_id.load();
            continue;
        }

        // Map new region
        std::string name = fmt::format("memglass_{}_region_{:04d}",
                                       session_name_, region_id);
        SharedMemory shm;
        if (!shm.open(name)) break;

        auto* desc = static_cast<RegionDescriptor*>(shm.data());
        region_id = desc->next_region_id.load();
        region_shms_[region_id] = std::move(shm);
    }
}
```

---

## Platform Abstraction

### POSIX (Linux, macOS)

```cpp
// Create shared memory
int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
ftruncate(fd, size);
void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);

// Open existing
int fd = shm_open(name, O_RDONLY, 0);
struct stat st;
fstat(fd, &st);
void* ptr = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

// Cleanup
munmap(ptr, size);
shm_unlink(name);
```

### Windows (Planned)

```cpp
// Create
HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                              PAGE_READWRITE, 0, size, name);
void* ptr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);

// Open
HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
void* ptr = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);

// Cleanup
UnmapViewOfFile(ptr);
CloseHandle(h);
```

---

## File Structure

```
memglass/
├── include/memglass/
│   ├── memglass.hpp       # Producer API
│   ├── observer.hpp       # Observer API
│   ├── types.hpp          # Core types (enums, structs)
│   ├── allocator.hpp      # Shared memory allocator
│   ├── registry.hpp       # Type registration
│   └── detail/
│       ├── shm.hpp        # Platform shm abstraction
│       └── seqlock.hpp    # Guarded<T>, Locked<T>
├── src/
│   ├── memglass.cpp       # Producer implementation
│   ├── observer.cpp       # Observer implementation
│   ├── allocator.cpp      # Allocator implementation
│   ├── registry.cpp       # Registry implementation
│   └── platform/
│       └── shm_posix.cpp  # POSIX shm implementation
├── tools/
│   ├── memglass.cpp       # TUI/Web observer (see memglass CLI Tool below)
│   └── memglass-gen/      # Code generator
│       ├── main.cpp
│       ├── generator.hpp
│       └── generator.cpp
├── examples/
│   ├── trading_producer.cpp
│   ├── trading_observer.cpp
│   └── trading_types.hpp
└── tests/
    ├── test_allocator.cpp
    ├── test_registry.cpp
    ├── test_seqlock.cpp
    └── test_integration.cpp
```

---

## memglass CLI Tool

The `memglass` command-line tool provides interactive observation of any memglass session. It supports two modes of operation: a terminal-based TUI and a web-based browser.

### Usage

```bash
memglass [OPTIONS] <session_name>

Options:
  -h, --help           Show help message
  -w, --web [PORT]     Run as web server (default port: 8080)
```

### TUI Mode (Default)

When run without flags, memglass presents an interactive terminal UI:

```
=== Memglass Browser ===
PID: 12345  Objects: 3  Seq: 42  t:12345
--------------------------------------------------------------------------------
[-] AAPL (Security)
      id                           =              1
  [-] quote
        bid_price                  =         150.25 [atomic]
        ask_price                  =         150.30 [atomic]
        bid_size                   =            100
        ask_size                   =            200
[+] GOOGL (Security)
[+] MSFT (Security)
--------------------------------------------------------------------------------
h/? for help | q to quit
```

**Controls:**
- `Up/Down` or `j/k` - Navigate
- `Enter` or `Space` - Expand/collapse object or field group
- `r` - Refresh object list
- `h` or `?` - Toggle help
- `q` - Quit

The display auto-refreshes every 500ms to show live field values.

### Web Server Mode

With the `-w` or `--web` flag, memglass starts an HTTP server that serves a browser-based UI:

```bash
# Start on default port 8080
memglass --web trading

# Start on custom port
memglass --web 9000 trading
```

Then open `http://localhost:8080` in any modern browser.

**Features:**
- Dark-themed responsive UI
- Expandable/collapsible tree view for objects and nested field groups
- Auto-refresh toggle (500ms interval)
- Value change highlighting with flash animation
- Atomicity badges showing synchronization level (atomic, seqlock, locked)
- Expand All / Collapse All buttons
- Live connection status indicator
- Session info display (PID, object count, sequence number)

### Architecture

The web server is built with [cpp-httplib](https://github.com/yhirose/cpp-httplib), a header-only HTTP library.

```
┌─────────────────────────────────────────────────────────────────────┐
│                           BROWSER                                   │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  JavaScript UI                                                │  │
│  │  - Fetches /api/data every 500ms                              │  │
│  │  - Renders dynamic tree with expand/collapse                  │  │
│  │  - Highlights changed values                                  │  │
│  └───────────────────────────────────────────────────────────────┘  │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ HTTP
┌──────────────────────────────▼──────────────────────────────────────┐
│                      memglass --web                                 │
│                                                                     │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐              │
│  │ HTTP Server │◀───│ WebServer   │◀───│ Observer    │              │
│  │ (httplib)   │    │ Class       │    │ API         │              │
│  └─────────────┘    └─────────────┘    └──────┬──────┘              │
│                                               │                     │
│  Endpoints:                                   │                     │
│  GET /         → Embedded HTML/JS/CSS         │                     │
│  GET /api/data → JSON snapshot                │                     │
└───────────────────────────────────────────────┼─────────────────────┘
                                                │
                         ┌──────────────────────▼─────────────────────┐
                         │            SHARED MEMORY                   │
                         │   memglass_{session}_header                │
                         │   memglass_{session}_region_*              │
                         └────────────────────────────────────────────┘
```

### JSON API

The `/api/data` endpoint returns a JSON snapshot of the session:

```json
{
  "pid": 12345,
  "sequence": 42,
  "types": [
    {
      "name": "Security",
      "type_id": 65537,
      "size": 128,
      "field_count": 8
    }
  ],
  "objects": [
    {
      "label": "AAPL",
      "type_name": "Security",
      "type_id": 65537,
      "fields": [
        {
          "name": "id",
          "value": 1,
          "atomicity": "none"
        },
        {
          "name": "quote.bid_price",
          "value": 150.25,
          "atomicity": "atomic"
        }
      ]
    }
  ]
}
```

### Build Configuration

The web server support is controlled by the `MEMGLASS_BUILD_WEB` CMake option:

```cmake
option(MEMGLASS_BUILD_WEB "Build memglass with web server support" ON)
```

To build without web support (reduces dependencies):

```bash
cmake -DMEMGLASS_BUILD_WEB=OFF ..
```

When web support is disabled, the `--web` option is not available and the binary size is smaller.
