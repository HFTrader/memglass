# Advanced Guide

> **⚠️ IMPORTANT: Synchronization Requirements**
>
> For correct operation in multi-core/multi-process environments, you **must** use proper synchronization wrappers (`std::atomic<T>`, `Guarded<T>`, `Locked<T>`) for fields accessed concurrently. Comment annotations like `@atomic` and `@seqlock` are metadata only - they tell observers how to read, but do not provide synchronization themselves.
>
> **See [MEMORY_MODEL.md](MEMORY_MODEL.md) for complete details on memory ordering and synchronization requirements.**

This guide covers advanced features of memglass including nested structs, synchronization primitives, field annotations, and the code generator.

## Table of Contents

- [Nested Structs](#nested-structs)
- [Synchronization Primitives](#synchronization-primitives)
- [Field Annotations](#field-annotations)
- [Code Generator](#code-generator)
- [Observer API Details](#observer-api-details)
- [Memory Management](#memory-management)

---

## Nested Structs

memglass supports nested POD structs. Fields are flattened with dot notation in the generated metadata.

### Defining Nested Types

```cpp
struct [[memglass::observe]] Quote {
    int64_t bid_price;   // @seqlock
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t timestamp_ns;
};

struct [[memglass::observe]] Position {
    uint32_t symbol_id;
    int64_t quantity;    // @atomic
    int64_t avg_price;
    int64_t realized_pnl;
    int64_t unrealized_pnl;
};

struct [[memglass::observe]] Security {
    Quote quote;
    Position position;
};
```

### Generated Field Names

The generator flattens nested structs using dot notation:

```cpp
// Fields for Security:
// quote.bid_price
// quote.ask_price
// quote.bid_size
// quote.ask_size
// quote.timestamp_ns
// position.symbol_id
// position.quantity
// position.avg_price
// position.realized_pnl
// position.unrealized_pnl
```

### Accessing Nested Fields

From the observer:

```cpp
auto view = obs.find("AAPL");
if (view) {
    // Access with dot notation
    int64_t bid = view["quote.bid_price"];
    int64_t qty = view["position.quantity"];

    // Or chained access
    int64_t ask = view["quote"]["ask_price"];
}
```

### TUI Display

The memglass viewer shows nested structs as expandable groups:

```
[-] AAPL (Security)
  [-] quote
        bid_price        =          15023 [seqlock]
        ask_price        =          15028
        bid_size         =            142
        ask_size         =             98
        timestamp_ns     = 1704825432123456789
  [-] position
        symbol_id        =              0
        quantity         =            500 [atomic]
        avg_price        =          14998
        realized_pnl     =              0
        unrealized_pnl   =          12500
```

---

## Synchronization Primitives

memglass provides three levels of synchronization for field access, specified via comment annotations.

### Overview

| Level | Annotation | Mechanism | Use Case |
|-------|------------|-----------|----------|
| None | (default) | Direct read/write | Non-critical data, debugging |
| Atomic | `@atomic` | `std::atomic<T>` | Single primitive values |
| Seqlock | `@seqlock` | `Guarded<T>` | Compound types, read-heavy |
| Locked | `@locked` | `Locked<T>` | Complex operations, RMW |

### Atomic Fields

For single primitive values that fit in a register, use `std::atomic<T>`:

```cpp
struct [[memglass::observe]] Stats {
    std::atomic<uint64_t> counter;        // @atomic
    std::atomic<int64_t> last_value;      // @atomic
};
```

**Producer usage:**
```cpp
stats->counter.fetch_add(1, std::memory_order_relaxed);  // Atomic increment
// or
stats->last_value.store(42, std::memory_order_release);  // Atomic store
```

**Observer usage:**
```cpp
uint64_t c = view["counter"];  // Atomic load with acquire semantics
```

**IMPORTANT:** The field must be declared as `std::atomic<T>` in the struct definition. 
The `@atomic` annotation alone is insufficient - it only tells the observer how to read; 
the producer must use `std::atomic<T>` to ensure proper synchronization.

Supported atomic types: `bool`, `int8-64`, `uint8-64`, `float`, `double`

### Seqlock Fields (`Guarded<T>`)

For compound types that must be read/written atomically as a unit, use `Guarded<T>`:

```cpp
struct [[memglass::observe]] MarketData {
    Guarded<Quote> best_quote;        // @seqlock
    Guarded<Quote> last_trade;        // @seqlock
};
```

**How it works:**

1. Writer increments sequence to odd (signals write in progress)
2. Writer copies data with memory barriers
3. Writer increments sequence to even (signals write complete)
4. Reader checks sequence before and after read; retries if changed

**Producer usage:**
```cpp
// The Guarded<T> wrapper handles seqlock automatically
market_data->best_quote.write(Quote{.bid_price=15020, .ask_price=15025});
```

**Observer usage:**
```cpp
// Automatic seqlock handling
Quote q = view["best_quote"];

// Non-blocking try-read (returns nullopt if write in progress)
auto maybe_quote = view["best_quote"].try_get<Quote>();
if (maybe_quote) {
    // Got consistent value
}
```

**IMPORTANT:** The field must be declared as `Guarded<T>` in the struct definition.
The `@seqlock` annotation alone is insufficient - it only tells the observer how to read;
the producer must use `Guarded<T>` to ensure proper synchronization and prevent torn reads.

**Memory overhead:** Each `Guarded<T>` field adds 8 bytes for the sequence counter.

### Locked Fields (`Locked<T>`)

For operations requiring mutual exclusion, use `Locked<T>`:

```cpp
struct [[memglass::observe]] Status {
    Locked<char[256]> error_msg;     // @locked
};
```

**Producer usage:**
```cpp
status->error_msg.write("Connection timeout");

// Read-modify-write
status->error_msg.update([](char* buf) {
    std::strcat(buf, " (retrying)");
});
```

**IMPORTANT:** The field must be declared as `Locked<T>` in the struct definition.
The `@locked` annotation alone is insufficient - it only tells the observer how to read;
the producer must use `Locked<T>` to ensure proper synchronization.

**Memory overhead:** Each `Locked<T>` field adds 1 byte for the spinlock.

### Performance Characteristics

| Access Type | Read Latency | Write Latency | Contention |
|-------------|--------------|---------------|------------|
| Direct | ~1 ns | ~1 ns | May tear |
| Atomic | ~5-20 ns | ~5-20 ns | Lock-free |
| Seqlock | ~10-50 ns | ~10-30 ns | Reader spins |
| Locked | ~20-100 ns | ~20-100 ns | Exclusive |

**Guidelines:**
- Use `@atomic` for frequently-updated scalars (counters, flags, quantities)
- Use `@seqlock` for compound values read often, written rarely (quotes)
- Use `@locked` for strings or values needing read-modify-write
- Default (none) for debugging data or where tearing is acceptable

---

## Field Annotations

> **⚠️ CRITICAL:** Annotations are **metadata only** - they tell observers how to read fields but do not provide synchronization. The producer must use matching wrapper types (`std::atomic<T>`, `Guarded<T>`, `Locked<T>`) for proper synchronization. See [MEMORY_MODEL.md](MEMORY_MODEL.md).

Field annotations are specified in comments and parsed by memglass-gen.

### Synchronization Annotations

```cpp
struct Data {
    std::atomic<int64_t> counter;  // @atomic - matches std::atomic wrapper
    Guarded<Quote> quote;          // @seqlock - matches Guarded wrapper
    Locked<char[256]> message;     // @locked - matches Locked wrapper
    int32_t debug_value;           // (no annotation - plain access, no sync)
};
```

**Annotation-to-Type Mapping:**
- `@atomic` → Field type must be `std::atomic<T>`
- `@seqlock` → Field type must be `Guarded<T>`
- `@locked` → Field type must be `Locked<T>`
- (no annotation) → Plain type `T` (no synchronization)

### Future Annotations (Planned)

These annotations are planned for future memglass-view integration:

```cpp
struct Order {
    uint64_t order_id;        // @readonly
    int64_t price;            // @min(0) @format("%.2f") @unit("ticks")
    uint32_t quantity;        // @range(1, 1000000) @step(100)
    char symbol[16];          // @regex("[A-Z]{1,5}")
    int8_t side;              // @enum(BUY=1, SELL=-1)
    int8_t status;            // @enum(PENDING=0, OPEN=1, FILLED=2, CANCELLED=3)
    uint32_t flags;           // @flags(IOC=1, FOK=2, POST_ONLY=4)
};
```

| Annotation | Description |
|------------|-------------|
| `@readonly` | Field cannot be modified via viewer |
| `@range(min, max)` | Numeric bounds for validation |
| `@min(val)` / `@max(val)` | Single-sided bounds |
| `@step(val)` | Increment step for adjustment |
| `@regex(pattern)` | Regex validation for strings |
| `@enum(name=val,...)` | Named values for integers |
| `@flags(name=bit,...)` | Bitfield with named flags |
| `@format(fmt)` | printf-style display format |
| `@unit(str)` | Display unit suffix |
| `@desc(str)` | Description tooltip |

---

## Code Generator

The `memglass-gen` tool uses libclang to parse C++ headers and generate type registration code.

### How It Works

```
┌────────────────┐     ┌─────────────┐     ┌───────────────────┐
│ my_types.hpp   │────▶│ memglass-gen│────▶│ my_types_generated│
│                │     │ (libclang)  │     │ .hpp              │
└────────────────┘     └─────────────┘     └───────────────────┘
```

1. Parses the AST using libclang
2. Finds structs with `[[memglass::observe]]` attribute
3. Extracts field names, types, offsets, sizes
4. Parses comment annotations (`@atomic`, `@seqlock`, etc.)
5. Generates type descriptors and registration functions

### Command Line Usage

```bash
# Basic usage
memglass-gen input.hpp -o output_generated.hpp

# With include paths
memglass-gen -I/usr/include -I./include input.hpp -o output.hpp

# Process multiple headers
memglass-gen header1.hpp header2.hpp -o generated.hpp
```

### Generated Code Structure

```cpp
// my_types_generated.hpp (auto-generated)
#pragma once
#include <memglass/registry.hpp>
#include "my_types.hpp"

namespace memglass::generated {

inline uint32_t register_Counter() {
    memglass::TypeDescriptor desc;
    desc.name = "Counter";
    desc.size = sizeof(Counter);
    desc.alignment = alignof(Counter);
    desc.fields = {
        {"value", 0, 8, memglass::PrimitiveType::UInt64, 0, 0,
         memglass::Atomicity::Atomic, false},
        {"timestamp", 8, 8, memglass::PrimitiveType::UInt64, 0, 0,
         memglass::Atomicity::None, false},
    };
    return memglass::registry::register_type_for<Counter>(desc);
}

inline void register_all_types() {
    register_Counter();
    // ... other types
}

} // namespace memglass::generated
```

### CMake Integration

```cmake
# In examples/CMakeLists.txt
set(GENERATED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/my_types_generated.hpp)

add_custom_command(
    OUTPUT ${GENERATED_HEADER}
    COMMAND memglass-gen
        ${CMAKE_CURRENT_SOURCE_DIR}/my_types.hpp
        -I ${CMAKE_SOURCE_DIR}/include
        -o ${GENERATED_HEADER}
    DEPENDS memglass-gen ${CMAKE_CURRENT_SOURCE_DIR}/my_types.hpp
    COMMENT "Generating type registration code"
)

add_executable(my_producer producer.cpp)
target_include_directories(my_producer PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(my_producer trading_types_gen)
```

---

## Observer API Details

### Connecting

```cpp
memglass::Observer obs("session_name");

if (!obs.connect()) {
    // Session doesn't exist or producer not running
}

std::cout << "Connected to PID: " << obs.producer_pid() << "\n";
```

### Listing Objects

```cpp
for (const auto& obj : obs.objects()) {
    std::cout << obj.label << " : " << obj.type_name << "\n";
}
```

### Getting Object Views

```cpp
// By label
auto view = obs.find("my_object");

// From object info
for (const auto& obj : obs.objects()) {
    auto view = obs.get(obj);
    // ...
}
```

### Field Access

```cpp
auto view = obs.find("my_object");
if (view) {
    // Read (type inferred from registry)
    int64_t val = view["field_name"];

    // Explicit type
    auto val = view["field_name"].as<int64_t>();

    // Nested fields
    int64_t nested = view["outer.inner"];

    // Check validity
    auto proxy = view["field_name"];
    if (proxy) {
        // Field exists
    }

    // Get field metadata
    const auto* info = proxy.info();
    if (info) {
        std::cout << "Type: " << info->type_id << "\n";
        std::cout << "Atomicity: " << (int)info->atomicity << "\n";
    }
}
```

### Detecting Changes

```cpp
uint64_t last_seq = 0;
while (running) {
    if (obs.sequence() != last_seq) {
        last_seq = obs.sequence();
        obs.refresh();  // Reload types and regions
        // Objects may have been added/removed
    }

    // Read values...
}
```

---

## Memory Management

### How Memory is Organized

```
┌─────────────────────────────────────────────────────────────────┐
│                         SHARED MEMORY                            │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                    HEADER REGION                            │ │
│  │  • Magic, version, session info                            │ │
│  │  • Type registry (schemas)                                 │ │
│  │  • Object directory (instance list)                        │ │
│  │  • Link to first data region                               │ │
│  └────────────────────────────────────────────────────────────┘ │
│                              │                                   │
│                              ▼                                   │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                    DATA REGION 1                            │ │
│  │  • Region descriptor                                       │ │
│  │  • Object data (bump-allocated)                            │ │
│  │  • Link to next region                                     │ │
│  └────────────────────────────────────────────────────────────┘ │
│                              │                                   │
│                              ▼                                   │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                    DATA REGION 2 (if needed)               │ │
│  └────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Shared Memory Naming

```
/dev/shm/memglass_{session}_header
/dev/shm/memglass_{session}_region_0001
/dev/shm/memglass_{session}_region_0002
...
```

### Object Lifecycle

1. **Creation**: `memglass::create<T>("label")` allocates from current region
2. **Usage**: Direct pointer access, fields written normally
3. **Destruction**: `memglass::destroy(ptr)` marks object as destroyed
4. **Cleanup**: Memory reclaimed after grace period (observers may be reading)

### Cleanup on Exit

```cpp
memglass::shutdown();
// - Marks session as shutting down
// - Waits for pending observers (with timeout)
// - Unlinks shared memory files
```

If producer crashes, shared memory files remain. They can be cleaned manually:

```bash
rm /dev/shm/memglass_my_app*
```
