<p align="center">
  <img src="logo.jpg" width="400" alt="memglass logo">
</p>

# memglass

Real-time cross-process observation of C++ POD objects via shared memory.

<p align="center">
  <img src="demo.gif" alt="memglass TUI demo">
</p>

## What it does

A producer application allocates POD structs in shared memory with automatic field registration. An observer process maps the same memory to inspect live object state - no serialization, no IPC overhead, just direct memory reads.

```
Producer Process                    Observer Process
     │                                    │
     ▼                                    ▼
┌─────────┐    ┌───────────────┐    ┌─────────┐
│ Objects │───▶│ Shared Memory │◀───│ Reader  │
└─────────┘    └───────────────┘    └─────────┘
```

## Building

```bash
mkdir build && cd build
cmake ..
make
```

### Dependencies

- C++20 compiler (GCC 10+, Clang 12+)
- libclang-dev (for memglass-gen code generator)
- fmt library (fetched automatically if not found)

## Quick Start

### 1. Define your types

```cpp
// trading_types.hpp
#include <cstdint>

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

### 2. Generate type registration code

```bash
./build/memglass-gen trading_types.hpp -o trading_types_generated.hpp
```

### 3. Create a producer

```cpp
#include <memglass/memglass.hpp>
#include "trading_types.hpp"
#include "trading_types_generated.hpp"

int main() {
    memglass::init("trading_engine");
    memglass::generated::register_all_types();

    auto* aapl = memglass::create<Security>("AAPL");
    aapl->quote.bid_price = 15000;
    aapl->position.quantity = 100;

    while (running) {
        aapl->quote.bid_price += random_delta();
        aapl->quote.timestamp_ns = now();
    }

    memglass::shutdown();
}
```

### 4. Observe with the TUI

```bash
./build/memglass trading_engine
```

The interactive browser shows all objects with expandable nested fields, auto-refreshing every 500ms.

```
=== Memglass Browser ===
PID: 12345  Objects: 5  Seq: 5
--------------------------------------------------------------------------------
[-] AAPL (Security)
  [-] quote
        bid_price        =          15023 [seqlock]
        ask_price        =          15028
        bid_size         =            142
        ask_size         =            98
        timestamp_ns     = 1704825432123456789
  [+] position
[+] MSFT (Security)
[+] GOOG (Security)
```

**Controls:** ↑/↓ or j/k to navigate, Enter/Space to expand/collapse, q to quit

### 5. Or use the Web UI

```bash
./build/memglass --web trading_engine
# Then open http://localhost:8080
```

<p align="center">
  <img src="demo-web.gif" alt="memglass Web UI demo">
</p>

The web interface provides the same tree view with:
- Auto-refresh with value change highlighting
- Expand/collapse all buttons
- Atomicity badges (atomic, seqlock, locked)
- Dark theme optimized for monitoring

### 6. Or write a custom observer

```cpp
#include <memglass/observer.hpp>

int main() {
    memglass::Observer obs("trading_engine");
    obs.connect();

    while (true) {
        for (const auto& obj : obs.objects()) {
            auto view = obs.get(obj);
            if (view) {
                int64_t bid = view["quote.bid_price"];
                int64_t qty = view["position.quantity"];
                printf("%s: bid=%ld qty=%ld\n", obj.label.c_str(), bid, qty);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

## Field Annotations

Use comments to specify synchronization:

```cpp
struct Data {
    int64_t counter;      // @atomic - std::atomic reads/writes
    double price;         // @seqlock - seqlock protection for larger types
    int32_t flags;        // @locked - mutex protection (rare)
    int32_t simple;       // (no annotation) - direct read/write
};
```

## Constraints

- **POD types only** - must be `std::is_trivially_copyable_v<T>`
- No `std::string`, `std::vector`, raw pointers
- Use `char name[N]` for strings, `std::array<T,N>` for fixed arrays
- Nested structs are supported and flattened with dot notation

## Documentation

- [Quick Start Guide](docs/quickstart.md) - Get up and running in 5 minutes
- [Advanced Guide](docs/advanced.md) - Nested structs, synchronization, annotations
- [Architecture](docs/architecture.md) - Internal design and memory layout
- [API Reference](docs/api-reference.md) - Complete API documentation

## Project Structure

```
├── include/memglass/     # Public headers
│   ├── memglass.hpp      # Producer API
│   ├── observer.hpp      # Observer API
│   ├── types.hpp         # Core types
│   └── ...
├── src/                  # Library implementation
├── tools/
│   ├── memglass.cpp      # TUI and Web observer (--web flag)
│   └── memglass-gen/     # Code generator
├── examples/             # Trading example
├── docs/                 # Documentation
└── tests/                # Unit tests
```

## License

MIT
