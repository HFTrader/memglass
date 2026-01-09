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
// trading_types.hpp
struct [[memglass::observe]] Quote {
    int64_t bid_price;      // Price in ticks (fixed-point)
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t timestamp_ns;
};

struct [[memglass::observe]] Order {
    uint64_t order_id;
    uint32_t symbol_id;
    int64_t price;
    uint32_t quantity;
    uint32_t filled_qty;
    int8_t side;            // 1=Buy, -1=Sell
    int8_t status;          // 0=Pending, 1=Open, 2=Filled, 3=Cancelled
};
```

The `memglass-gen` tool (a clang-based code generator) parses your headers and automatically generates all reflection data:

```bash
# Run at build time (integrated into CMake)
memglass-gen --output=memglass_generated.hpp include/trading_types.hpp
```

This generates type descriptors with full field information extracted from clang's AST:

```cpp
// memglass_generated.hpp (auto-generated, do not edit)
namespace memglass::generated {

template<> struct TypeDescriptor<Quote> {
    static constexpr std::string_view name = "Quote";
    static constexpr size_t size = 32;
    static constexpr size_t alignment = 8;
    static constexpr std::array<FieldInfo, 5> fields = {{
        {"bid_price", offsetof(Quote, bid_price), sizeof(int64_t), PrimitiveType::Int64, 0},
        {"ask_price", offsetof(Quote, ask_price), sizeof(int64_t), PrimitiveType::Int64, 0},
        {"bid_size", offsetof(Quote, bid_size), sizeof(uint32_t), PrimitiveType::UInt32, 0},
        {"ask_size", offsetof(Quote, ask_size), sizeof(uint32_t), PrimitiveType::UInt32, 0},
        {"timestamp_ns", offsetof(Quote, timestamp_ns), sizeof(uint64_t), PrimitiveType::UInt64, 0},
    }};
};

template<> struct TypeDescriptor<Order> {
    static constexpr std::string_view name = "Order";
    static constexpr size_t size = 32;
    static constexpr size_t alignment = 8;
    static constexpr std::array<FieldInfo, 7> fields = {{
        {"order_id", offsetof(Order, order_id), sizeof(uint64_t), PrimitiveType::UInt64, 0},
        {"symbol_id", offsetof(Order, symbol_id), sizeof(uint32_t), PrimitiveType::UInt32, 0},
        {"price", offsetof(Order, price), sizeof(int64_t), PrimitiveType::Int64, 0},
        {"quantity", offsetof(Order, quantity), sizeof(uint32_t), PrimitiveType::UInt32, 0},
        {"filled_qty", offsetof(Order, filled_qty), sizeof(uint32_t), PrimitiveType::UInt32, 0},
        {"side", offsetof(Order, side), sizeof(int8_t), PrimitiveType::Int8, 0},
        {"status", offsetof(Order, status), sizeof(int8_t), PrimitiveType::Int8, 0},
    }};
};

inline void register_all_types() {
    memglass::registry::add<Quote>();
    memglass::registry::add<Order>();
}

} // namespace memglass::generated
```

### Object Allocation

```cpp
// Allocate in memglass-managed shared memory
Order* order = memglass::create<Order>("order_12345");

// Construct with arguments
Order* order = memglass::create<Order>("order_67890",
    Order{.order_id = 67890, .symbol_id = 1, .price = 15025, .quantity = 100, .side = 1});

// Array allocation for order book levels
Quote* quotes = memglass::create_array<Quote>("AAPL_quotes", 10);

// Destruction (marks as destroyed, memory reclaimed later)
memglass::destroy(order);
```

### Manual Memory Management (Advanced)

```cpp
// Get raw allocator for custom containers
memglass::Allocator<Order> alloc;
std::vector<Order, memglass::Allocator<Order>> orders(alloc);

// Placement new in memglass memory
void* mem = memglass::allocate(sizeof(Order), alignof(Order));
Order* o = new (mem) Order{};
memglass::register_object(o, "manual_order");
```

## Observer API

### Connecting

```cpp
#include <memglass/observer.hpp>

int main() {
    // Connect to running session
    memglass::Observer observer("trading_engine");

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

### Reading and Writing Field Values

The observer uses an intuitive `operator[]` syntax. Atomicity is handled automatically based on field metadata.

```cpp
    auto order = observer.find("order_12345");
    if (order) {
        // Read fields - type inferred from registry, atomicity automatic
        int64_t price = order["price"];
        int8_t status = order["status"];

        // Write fields - atomicity automatic
        order["filled_qty"] = 50;
        order["status"] = 2;  // Filled

        // Nested fields via chaining
        int64_t bid = order["last_quote"]["bid_price"];
        order["last_quote"]["bid_price"] = 15025;

        // Or dot notation (equivalent)
        int64_t ask = order["last_quote.ask_price"];
        order["last_quote.ask_size"] = 100;

        // Array access (order book levels)
        int64_t best_bid = order["bid_levels"][0];
        order["ask_levels"][2] = 15030;

        // Read entire struct (copies to local memory)
        Quote quote = order["last_quote"].as<Quote>();
        Order local_copy = order.as<Order>();

        // Write entire struct
        order["last_quote"] = Quote{.bid_price=15020, .ask_price=15025, .bid_size=100, .ask_size=200};
    }
```

### FieldProxy Implementation

The `operator[]` returns a `FieldProxy` that handles reads/writes:

```cpp
class FieldProxy {
    ObjectView& obj_;
    std::string path_;
    const FieldInfo& info_;

public:
    // Implicit conversion for reads (type from registry)
    template<typename T>
    operator T() const {
        // Dispatch based on atomicity annotation
        switch (info_.atomicity) {
            case Atomicity::None:    return read_direct<T>();
            case Atomicity::Atomic:  return read_atomic<T>();
            case Atomicity::Seqlock: return read_seqlock<T>();
            case Atomicity::Locked:  return read_locked<T>();
        }
    }

    // Assignment for writes
    template<typename T>
    FieldProxy& operator=(const T& value) {
        switch (info_.atomicity) {
            case Atomicity::None:    write_direct(value); break;
            case Atomicity::Atomic:  write_atomic(value); break;
            case Atomicity::Seqlock: write_seqlock(value); break;
            case Atomicity::Locked:  write_locked(value); break;
        }
        return *this;
    }

    // Chaining for nested access
    FieldProxy operator[](std::string_view name) const {
        return obj_.field(path_ + "." + std::string(name));
    }

    FieldProxy operator[](size_t index) const {
        return obj_.field(path_ + "[" + std::to_string(index) + "]");
    }

    // Explicit type conversion when needed
    template<typename T>
    T as() const { return static_cast<T>(*this); }
};
```

### Path Syntax Reference

| Syntax | Example | Description |
|--------|---------|-------------|
| `field` | `order["price"]` | Direct field access |
| `field.nested` | `order["last_quote.bid_price"]` | Nested field (dot notation) |
| `field[i]` | `order["bid_levels"][0]` | Array element |
| `field[i].nested` | `order["fills"][0]["price"]` | Array of structs |
| Chained | `order["last_quote"]["bid_price"]` | Equivalent to dot notation |

### Advanced: Explicit Atomicity Control

For special cases where you need to override the default behavior:

```cpp
    // Force non-atomic read (for debugging, slightly faster)
    int64_t qty = position["quantity"].unsafe();

    // Force atomic even if not annotated
    int64_t qty = position["quantity"].atomic();

    // Try-read for seqlock (non-blocking, returns nullopt if write in progress)
    auto quote = position["last_quote"].try_get<Quote>();
    if (quote) {
        // Got consistent value
    }

    // Read-modify-write for locked fields
    position["error_msg"].update([](char* buf) {
        std::strcat(buf, " (retry)");
    });
```

### Raw Access (Advanced)

```cpp
    // Raw pointer access (bypasses atomicity)
    const void* raw = order.data();
    void* mutable_raw = order.mutable_data();  // Use with caution

    // Field info for reflection
    FieldInfo info = order["price"].info();
    std::cout << "Type: " << info.type_name
              << ", Offset: " << info.offset
              << ", Atomicity: " << info.atomicity << "\n";
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
        auto position = observer.find("AAPL_position");
        if (position) {
            std::cout << "Quantity: " << position["quantity"] << "\n";
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

### Atomicity Levels

memglass provides multiple levels of consistency for field access:

| Level | Mechanism | Overhead | Use Case |
|-------|-----------|----------|----------|
| None | Direct read/write | Zero | Debugging, non-critical data |
| Atomic | `std::atomic<T>` | Low | Single primitive values |
| Seqlock | `memglass::Guarded<T>` | Medium | Compound types (structs) |
| Mutex | `memglass::Locked<T>` | High | Complex operations, RMW |

### Specifying Atomicity via Annotations

Use the `@atomic` annotation to specify consistency requirements:

```cpp
struct [[memglass::observe]] Position {
    uint32_t symbol_id;               // No consistency (default)
    int64_t quantity;                 // @atomic - use std::atomic internally
    Quote last_quote;                 // @seqlock - wrap in Guarded<T>
    char error_msg[256];              // @locked - mutex-protected
    uint64_t update_count;            // @atomic
};
```

The generator produces appropriate wrappers:

```cpp
// memglass_generated.hpp
struct Position_Storage {
    uint32_t symbol_id;               // Direct storage
    std::atomic<int64_t> quantity;    // Atomic wrapper
    memglass::Guarded<Quote> last_quote;  // Seqlock wrapper
    memglass::Locked<char[256]> error_msg;  // Mutex wrapper
    std::atomic<uint64_t> update_count;
};
```

### Atomic Primitives (`std::atomic<T>`)

For single primitive values that fit in a register:

```cpp
struct [[memglass::observe]] OrderStats {
    uint64_t order_count;  // @atomic
};

// Producer
stats->order_count.store(42, std::memory_order_release);
stats->order_count.fetch_add(1, std::memory_order_relaxed);

// Observer
uint64_t count = stats_view["order_count"];  // automatic atomic read
stats_view["order_count"] = 100;             // automatic atomic write
```

**Supported atomic types:** `bool`, `int8-64`, `uint8-64`, `float`, `double` (if lock-free on platform)

### Seqlock Wrapper (`Guarded<T>`)

For compound types that must be read/written atomically as a unit:

```cpp
template<typename T>
struct Guarded {
    static_assert(std::is_trivially_copyable_v<T>);

    mutable std::atomic<uint32_t> seq{0};
    T value;

    // Producer write
    void write(const T& v) {
        seq.fetch_add(1, std::memory_order_release);  // Odd = writing
        std::memcpy(&value, &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq.fetch_add(1, std::memory_order_release);  // Even = stable
    }

    // Observer read (spins until consistent)
    T read() const {
        T result;
        uint32_t s1, s2;
        do {
            s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1) {
                // Write in progress, spin
                _mm_pause();  // or std::this_thread::yield()
                continue;
            }
            std::memcpy(&result, &value, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = seq.load(std::memory_order_acquire);
        } while (s1 != s2);
        return result;
    }

    // Try read without spinning (returns nullopt if write in progress)
    std::optional<T> try_read() const {
        uint32_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) return std::nullopt;

        T result;
        std::memcpy(&result, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_acquire);

        uint32_t s2 = seq.load(std::memory_order_acquire);
        if (s1 != s2) return std::nullopt;

        return result;
    }
};
```

**Usage in struct:**

```cpp
struct [[memglass::observe]] MarketData {
    memglass::Guarded<Quote> best_quote;     // Or use: // @seqlock
    memglass::Guarded<Quote> last_trade;
    memglass::Guarded<OHLC> daily_bar;
};

// Producer
market_data->best_quote.write({.bid_price=15020, .ask_price=15025, .bid_size=100, .ask_size=200});

// Observer (automatic seqlock handling)
Quote quote = market_data_view["best_quote"];
market_data_view["best_quote"] = Quote{.bid_price=15021, .ask_price=15026};
```

### Mutex Wrapper (`Locked<T>`)

For operations requiring read-modify-write or complex updates:

```cpp
template<typename T>
struct Locked {
    static_assert(std::is_trivially_copyable_v<T>);

    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    T value;

    void write(const T& v) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
        value = v;
        lock_.clear(std::memory_order_release);
    }

    T read() const {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
        T result = value;
        lock_.clear(std::memory_order_release);
        return result;
    }

    // Read-modify-write operation
    template<typename F>
    void update(F&& func) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
        func(value);
        lock_.clear(std::memory_order_release);
    }
};
```

**Usage:**

```cpp
struct [[memglass::observe]] TradingStatus {
    memglass::Locked<char[256]> last_error;  // Or use: // @locked
    memglass::Locked<char[128]> connection_state;
};

// Producer
status->last_error.write("Exchange connection timeout");

// Observer - read-modify-write
status["last_error"].update([](char* buf) {
    std::strcat(buf, " (retrying)");
});
```

### memglass-view Integration

The viewer automatically uses the correct access method:

```
┌─ AAPL_position : Position ──────────────────┐
│ symbol_id  uint32   1                       │
│ quantity   int64    500          [atomic]   │  ← Shows consistency level
│ last_quote Quote    ▼            [seqlock]  │
│   bid_price  int64    15020                 │
│   ask_price  int64    15025                 │
│   bid_size   uint32   100                   │
│   ask_size   uint32   200                   │
│ error_msg  char[256] ""          [locked]   │
└─────────────────────────────────────────────┘
```

When editing, the viewer uses the same `operator[]` API which automatically handles synchronization.

### Performance Considerations

| Access Type | Read Latency | Write Latency | Contention Behavior |
|-------------|--------------|---------------|---------------------|
| Direct | ~1 ns | ~1 ns | May tear |
| `std::atomic` | ~5-20 ns | ~5-20 ns | Lock-free |
| `Guarded<T>` | ~10-50 ns | ~10-30 ns | Reader spins on write |
| `Locked<T>` | ~20-100 ns | ~20-100 ns | Exclusive access |

**Guidelines:**
- Use `@atomic` for frequently-updated scalars (counters, flags, quantities)
- Use `@seqlock` for compound values read often, written rarely (quotes, OHLC bars)
- Use `@locked` for strings or values needing RMW operations
- Default (none) for debugging data or where tearing is acceptable

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
┌────────────────┐     ┌─────────────┐     ┌───────────────────┐
│trading_types.h │────▶│ memglass-gen│────▶│ memglass_generated│
│                │     │ (libclang)  │     │ .hpp              │
└────────────────┘     └─────────────┘     └───────────────────┘
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
7. **Comment metadata extraction** - Parses inline comments for field annotations

### CMake Integration

```cmake
find_package(memglass REQUIRED)

# Automatically generate reflection code for your headers
memglass_generate(
    TARGET trading_engine
    HEADERS
        include/trading_types.hpp
        include/market_data.hpp
    OUTPUT ${CMAKE_BINARY_DIR}/generated/memglass_generated.hpp
)

target_link_libraries(trading_engine PRIVATE memglass::memglass)
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
- `clang_Cursor_getRawCommentText()` - Extract inline comments for metadata

### Field Metadata via Comments

Inline comments on struct fields can contain annotations that memglass-gen extracts for use by memglass-view. This enables validation, formatting hints, and editing constraints without additional boilerplate.

**Syntax:**

```cpp
struct [[memglass::observe]] Order {
    uint64_t order_id;        // @readonly
    int64_t price;            // @min(0) @format("%.2f") @unit("ticks")
    uint32_t quantity;        // @range(1, 1000000) @step(100)
    uint32_t filled_qty;      // @readonly @format("%d")
    char symbol[16];          // @regex("[A-Z]{1,5}")
    int8_t side;              // @enum(BUY=1, SELL=-1)
    int8_t status;            // @enum(PENDING=0, OPEN=1, FILLED=2, CANCELLED=3, REJECTED=4)
    uint32_t flags;           // @flags(IOC=1, FOK=2, POST_ONLY=4, REDUCE_ONLY=8)
};
```

**Supported Annotations:**

| Annotation | Description | Example |
|------------|-------------|---------|
| `@readonly` | Field cannot be modified via memglass-view | `// @readonly` |
| `@range(min, max)` | Numeric bounds for validation | `// @range(0, 100)` |
| `@min(val)` | Minimum value only | `// @min(0)` |
| `@max(val)` | Maximum value only | `// @max(1000)` |
| `@step(val)` | Increment step for +/- adjustment | `// @step(0.1)` |
| `@regex(pattern)` | Regex validation for strings | `// @regex("[a-z]+")` |
| `@enum(name=val,...)` | Named values for integers | `// @enum(OFF=0, ON=1)` |
| `@flags(name=bit,...)` | Bitfield with named flags | `// @flags(A=1, B=2, C=4)` |
| `@format(fmt)` | printf-style display format | `// @format("%.2f")` |
| `@unit(str)` | Display unit suffix | `// @unit("m/s")` |
| `@desc(str)` | Description tooltip | `// @desc("Player health points")` |
| `@atomic` | Use `std::atomic<T>` for field | `// @atomic` |
| `@seqlock` | Use `Guarded<T>` seqlock wrapper | `// @seqlock` |
| `@locked` | Use `Locked<T>` mutex wrapper | `// @locked` |

**Generated Metadata:**

```cpp
// memglass_generated.hpp (excerpt)
template<> struct TypeDescriptor<Order> {
    // ... fields array ...

    static constexpr std::array<FieldMeta, 8> metadata = {{
        {.readonly = true},                                           // order_id
        {.range_min = 0, .format = "%.2f", .unit = "ticks"},          // price
        {.range = {1, 1000000}, .step = 100},                         // quantity
        {.readonly = true, .format = "%d"},                           // filled_qty
        {.regex = "[A-Z]{1,5}"},                                      // symbol
        {.enum_values = {{"BUY",1},{"SELL",-1}}},                     // side
        {.enum_values = {{"PENDING",0},{"OPEN",1},{"FILLED",2},{"CANCELLED",3},{"REJECTED",4}}}, // status
        {.flags = {{"IOC",1},{"FOK",2},{"POST_ONLY",4},{"REDUCE_ONLY",8}}}, // flags
    }};
};
```

**Comment Parsing Implementation:**

```cpp
FieldMeta parse_field_comment(CXCursor field_cursor) {
    FieldMeta meta{};

    CXString comment = clang_Cursor_getRawCommentText(field_cursor);
    const char* text = clang_getCString(comment);
    if (!text) return meta;

    std::string_view sv(text);

    // Parse @readonly
    if (sv.find("@readonly") != std::string_view::npos) {
        meta.readonly = true;
    }

    // Parse @range(min, max)
    static const std::regex range_re(R"(@range\s*\(\s*([^,]+)\s*,\s*([^)]+)\s*\))");
    std::smatch match;
    std::string str(sv);
    if (std::regex_search(str, match, range_re)) {
        meta.range_min = std::stod(match[1]);
        meta.range_max = std::stod(match[2]);
        meta.has_range = true;
    }

    // Parse @regex(pattern)
    static const std::regex regex_re(R"(@regex\s*\(\s*"([^"]+)"\s*\))");
    if (std::regex_search(str, match, regex_re)) {
        meta.regex_pattern = match[1];
    }

    // Parse @enum(NAME=val, ...)
    static const std::regex enum_re(R"(@enum\s*\(([^)]+)\))");
    if (std::regex_search(str, match, enum_re)) {
        meta.enum_values = parse_enum_list(match[1]);
    }

    // ... similar for @flags, @format, @step, etc.

    clang_disposeString(comment);
    return meta;
}

## memglass-view: ncurses Memory Visualizer

An interactive terminal-based tool for real-time visualization of shared memory state.

### Screenshot (Mockup)

```
┌─ memglass-view ── trading_engine ── PID:12345 ── 3 regions ── 47 objects ──┐
│                                                                             │
│ ┌─ Objects ─────────────────────┐ ┌─ order_12345 : Order ─────────────────┐│
│ │ ▶ order_12345     Order       │ │ order_id   uint64   12345             ││
│ │   order_12346     Order       │ │ symbol_id  uint32   1                 ││
│ │   order_12347     Order       │ │ price      int64    15025             ││
│ │   order_12348     Order       │ │ quantity   uint32   100               ││
│ │   order_12349     Order       │ │ filled_qty uint32   50                ││
│ │   AAPL_position   Position    │ │ side       int8     1        [BUY]    ││
│ │   MSFT_position   Position    │ │ status     int8     1        [OPEN]   ││
│ │   GOOG_position   Position    │ │                                       ││
│ │   AAPL_quote      Quote       │ │ Offset: 0x1A40  Size: 32 bytes        ││
│ │   MSFT_quote      Quote       │ │ Region: 1       Gen: 1                ││
│ │   engine_stats    TradingStats│ └───────────────────────────────────────┘│
│ └───────────────────────────────┘                                          │
│ ┌─ Memory Map ─────────────────────────────────────────────────────────────┐│
│ │ Region 1 [████████████░░░░░░░░░░░░░░░░░░░░] 38% (389KB / 1MB)           ││
│ │ Region 2 [██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]  6% (128KB / 2MB)           ││
│ └───────────────────────────────────────────────────────────────────────────┘│
├─────────────────────────────────────────────────────────────────────────────┤
│ [q]uit [r]efresh [f]ilter [/]search [h]ex [w]atch [Tab]switch  Rate: 60Hz  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Features

1. **Object Browser** - Scrollable list of all live objects with type info
2. **Field Inspector** - Hierarchical view of selected object's fields
3. **Live Updates** - Values refresh in real-time (configurable rate)
4. **Memory Map** - Visual representation of region utilization
5. **Hex View** - Raw memory dump of selected object (toggle with `h`)
6. **Search/Filter** - Find objects by label or type (press `/` or `f`)
7. **Watch List** - Pin specific fields to always-visible panel
8. **Change Highlighting** - Flash values that changed since last frame
9. **Field Editing** - Modify values directly with validation from metadata
10. **Enum/Flags Picker** - Visual selector for annotated enum and bitfield types

### Keyboard Controls

| Key | Action |
|-----|--------|
| `↑/↓` or `j/k` | Navigate object/field list |
| `←/→` or `h/l` | Navigate between panels |
| `Enter` | Expand/collapse nested struct, or edit field |
| `e` | Edit selected field (opens input dialog) |
| `Tab` | Switch focus between panels |
| `h` | Toggle hex view for selected object |
| `w` | Add field to watch list |
| `f` | Filter objects by type |
| `/` | Search objects by label |
| `r` | Force refresh |
| `+/-` | Adjust numeric field value by step (or refresh rate in list) |
| `Space` | Toggle bool field, or cycle enum values |
| `Esc` | Cancel edit, close dialog |
| `q` | Quit |

### Command Line Usage

```bash
# Connect to a session
memglass-view trading_engine

# Custom refresh rate (default 10 Hz)
memglass-view --rate 60 trading_engine

# Filter to specific type on startup
memglass-view --type Order trading_engine

# Hex view mode by default
memglass-view --hex trading_engine

# Watch specific fields
memglass-view --watch "AAPL_position.quantity" --watch "AAPL_quote.bid_price" trading_engine
```

### Architecture

```cpp
// Simplified structure
class App {
    memglass::Observer observer_;

    // UI panels
    ObjectListPanel object_list_;
    ObjectDetailPanel detail_;
    MemoryMapPanel memory_map_;
    HexViewPanel hex_view_;
    WatchPanel watch_;

    // State
    std::string selected_object_;
    std::vector<std::string> watch_list_;
    int refresh_rate_hz_ = 10;

public:
    void run() {
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);

        while (!quit_) {
            handle_input();
            if (should_refresh()) {
                observer_.refresh();
                render();
            }
            std::this_thread::sleep_for(1ms);
        }

        endwin();
    }

    void render() {
        object_list_.render(observer_.objects());
        detail_.render(observer_.find(selected_object_));
        memory_map_.render(observer_.regions());
        // ...
        refresh();
    }
};
```

### Value Formatting

The tool automatically formats values based on type:

| Type | Format |
|------|--------|
| `bool` | `true` / `false` |
| `int8/16/32/64` | Decimal with thousands separator |
| `uint8/16/32/64` | Decimal (hex with `h` modifier) |
| `float/double` | 6 significant digits |
| `char[N]` | String (escaped non-printable) |
| `T[N]` | `[N elements]` (expandable) |
| Nested struct | `▶ {...}` (expandable) |

### Change Detection

Fields that changed since last frame are highlighted:

```
│ health     float    87.5  →  82.3   │  (value shown in bold/color)
```

Configuration via command line:
```bash
# Highlight duration in frames
memglass-view --highlight-frames 30 game_server

# Disable highlighting
memglass-view --no-highlight game_server
```

### Field Editing

memglass-view can write values directly to shared memory. Editing respects metadata annotations.

**Edit Modes by Type:**

| Type | Edit Behavior |
|------|---------------|
| `bool` | Space toggles, or type `true`/`false`/`0`/`1` |
| `int/uint` | Direct input, +/- adjusts by step (default 1) |
| `float/double` | Direct input, +/- adjusts by step (default 0.1) |
| `char[N]` | Text input with length limit, regex validation |
| `@enum` | Dropdown picker or cycle with Space |
| `@flags` | Checkbox list, toggle individual bits |

**Edit Dialog (mockup):**

```
┌─ Edit: order_12345.quantity ────────────────┐
│                                             │
│  Type:   uint32                             │
│  Range:  1 - 1000000                        │
│  Step:   100                                │
│                                             │
│  Current: 100                               │
│  New:     [200______________]               │
│                                             │
│  [Enter] Apply  [Esc] Cancel  [+/-] Adjust  │
└─────────────────────────────────────────────┘
```

**Enum Picker:**

```
┌─ Edit: order_12345.status ──────────────────┐
│                                             │
│  ○ PENDING    (0)                           │
│  ● OPEN       (1)  ← current                │
│  ○ FILLED     (2)                           │
│  ○ CANCELLED  (3)                           │
│  ○ REJECTED   (4)                           │
│                                             │
│  [Enter] Select  [Esc] Cancel               │
└─────────────────────────────────────────────┘
```

**Flags Editor:**

```
┌─ Edit: order_12345.flags ───────────────────┐
│                                             │
│  [x] IOC         (bit 0)                    │
│  [ ] FOK         (bit 1)                    │
│  [ ] POST_ONLY   (bit 2)                    │
│  [ ] REDUCE_ONLY (bit 3)                    │
│                                             │
│  Value: 1 (0x01)                            │
│                                             │
│  [Space] Toggle  [Enter] Done  [Esc] Cancel │
└─────────────────────────────────────────────┘
```

**Validation:**

- `@readonly` fields show "Read-only" and reject edits
- `@range` validates before applying; shows error if out of bounds
- `@regex` validates string input; shows pattern on mismatch
- Invalid input highlights the field in red

**Write Implementation:**

```cpp
class FieldEditor {
    memglass::Observer& observer_;
    const FieldMeta& meta_;

public:
    bool write_value(ObjectView& obj, const std::string& field_path,
                     const std::string& input) {
        if (meta_.readonly) {
            show_error("Field is read-only");
            return false;
        }

        // Parse and validate based on type
        auto value = parse_input(input, field_info_.type);
        if (!value) {
            show_error("Invalid input format");
            return false;
        }

        // Check range constraints
        if (meta_.has_range) {
            double v = std::get<double>(*value);
            if (v < meta_.range_min || v > meta_.range_max) {
                show_error(fmt::format("Value must be in range [{}, {}]",
                                       meta_.range_min, meta_.range_max));
                return false;
            }
        }

        // Check regex for strings
        if (!meta_.regex_pattern.empty()) {
            std::regex re(meta_.regex_pattern);
            if (!std::regex_match(std::get<std::string>(*value), re)) {
                show_error(fmt::format("Must match pattern: {}",
                                       meta_.regex_pattern));
                return false;
            }
        }

        // Write to shared memory
        obj.write(field_path, *value);
        return true;
    }
};
```

**Command Line Options:**

```bash
# Read-only mode (disable editing)
memglass-view --readonly trading_engine

# Allow editing (default)
memglass-view --edit trading_engine
```

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
│   ├── memglass-gen/
│   │   ├── CMakeLists.txt     # Links against libclang
│   │   ├── main.cpp           # CLI entry point
│   │   ├── generator.hpp      # Generator class
│   │   ├── generator.cpp      # AST traversal, code emission
│   │   └── type_mapper.cpp    # C++ type to PrimitiveType mapping
│   └── memglass-view/
│       ├── CMakeLists.txt     # Links against ncurses
│       ├── main.cpp           # CLI entry point
│       ├── app.hpp            # Application state
│       ├── app.cpp            # Main loop, input handling
│       ├── ui/
│       │   ├── layout.hpp         # Window layout management
│       │   ├── object_list.cpp    # Object browser panel
│       │   ├── object_detail.cpp  # Field inspector panel
│       │   ├── memory_map.cpp     # Region visualization
│       │   ├── hex_view.cpp       # Raw memory hex dump
│       │   ├── edit_dialog.cpp    # Field edit dialog
│       │   ├── enum_picker.cpp    # Enum value selector
│       │   └── flags_editor.cpp   # Bitfield checkbox editor
│       ├── editor.hpp         # Field editing logic
│       ├── editor.cpp         # Validation, write operations
│       └── format.cpp         # Value formatting utilities
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

### Producer (Trading Engine)

```cpp
// trading_types.hpp
#pragma once
#include <cstdint>
#include <atomic>

struct [[memglass::observe]] Quote {
    int64_t bid_price;      // @seqlock - Price in ticks
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t timestamp_ns;
};

struct [[memglass::observe]] Position {
    uint32_t symbol_id;
    int64_t quantity;       // @atomic
    int64_t avg_price;
    int64_t realized_pnl;
    int64_t unrealized_pnl;
    Quote last_quote;       // @seqlock
};

struct [[memglass::observe]] Order {
    uint64_t order_id;      // @readonly
    uint32_t symbol_id;
    int64_t price;
    uint32_t quantity;
    uint32_t filled_qty;
    int8_t side;            // @enum(BUY=1, SELL=-1)
    int8_t status;          // @enum(PENDING=0, OPEN=1, FILLED=2, CANCELLED=3)
};
```

```cpp
// producer.cpp
#include <memglass/memglass.hpp>
#include "trading_types.hpp"
#include "memglass_generated.hpp"  // Auto-generated by memglass-gen
#include <thread>
#include <random>

int main() {
    memglass::init("trading_engine");
    memglass::generated::register_all_types();  // Register discovered types

    // Create positions for tracked symbols
    const char* symbols[] = {"AAPL", "MSFT", "GOOG", "AMZN", "META"};
    std::vector<Position*> positions;
    std::vector<Quote*> quotes;

    for (int i = 0; i < 5; i++) {
        auto* pos = memglass::create<Position>(fmt::format("{}_position", symbols[i]));
        pos->symbol_id = i;
        pos->quantity = 0;
        pos->avg_price = 0;
        positions.push_back(pos);

        auto* quote = memglass::create<Quote>(fmt::format("{}_quote", symbols[i]));
        quote->bid_price = 15000 + i * 1000;  // Starting prices
        quote->ask_price = quote->bid_price + 5;
        quote->bid_size = 100;
        quote->ask_size = 100;
        quotes.push_back(quote);
    }

    // Market simulation loop
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> price_delta(-10, 10);

    while (true) {
        for (int i = 0; i < 5; i++) {
            // Update quotes with price movement
            quotes[i]->bid_price += price_delta(gen);
            quotes[i]->ask_price = quotes[i]->bid_price + 5;
            quotes[i]->timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();

            // Update position P&L
            positions[i]->last_quote = *quotes[i];
            if (positions[i]->quantity != 0) {
                int64_t mark = quotes[i]->bid_price;
                positions[i]->unrealized_pnl =
                    (mark - positions[i]->avg_price) * positions[i]->quantity;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    memglass::shutdown();
}
```

### Observer (Monitoring Tool)

```cpp
#include <memglass/observer.hpp>
#include <iostream>

int main() {
    memglass::Observer obs("trading_engine");

    if (!obs.connect()) {
        std::cerr << "Cannot connect to trading_engine\n";
        return 1;
    }

    while (true) {
        obs.refresh();

        std::cout << "\033[2J\033[H";  // Clear screen
        std::cout << "=== Trading Engine Telemetry ===\n\n";

        for (const auto& obj : obs.objects()) {
            if (obj.type_name == "Position") {
                auto view = obs.get(obj);

                std::cout << fmt::format("{}: qty={} avg_px={} pnl={}\n",
                    obj.label,
                    (int64_t)view["quantity"],
                    (int64_t)view["avg_price"],
                    (int64_t)view["unrealized_pnl"]
                );
            } else if (obj.type_name == "Quote") {
                auto view = obs.get(obj);

                std::cout << fmt::format("{}: {} @ {} x {} @ {}\n",
                    obj.label,
                    (uint32_t)view["bid_size"],
                    (int64_t)view["bid_price"],
                    (int64_t)view["ask_price"],
                    (uint32_t)view["ask_size"]
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

**Core library:**
- C++20 compiler (GCC 10+, Clang 12+, MSVC 19.29+)
- fmt (formatting, can be replaced with std::format on C++23)
- Optional: Boost.Interprocess (alternative allocator backend)

**memglass-gen:**
- libclang 12+ (LLVM/Clang development libraries)
  - Minimum: Clang/LLVM 12 (matches C++20 compiler requirement)
  - Recommended: Clang/LLVM 14+ (improved C++20 AST handling)
  - Required packages: `libclang-dev` (Debian/Ubuntu), `clang-devel` (Fedora), `llvm` (macOS Homebrew)

**memglass-view:**
- ncurses (ncursesw for wide character support)

## Open Questions

1. **String handling** - Should `std::string` be supported directly, or require fixed-size char arrays?
2. **Containers** - Support `std::vector` in shared memory? (Complex due to allocator requirements)
3. **Inheritance** - Support polymorphic types? (Would need RTTI-like metadata)
4. **Atomics by default** - Should all primitive fields use `std::atomic` for consistency?

---

*This proposal is ready for implementation. Estimated scope: ~2000-3000 lines of C++ for core functionality.*
