# memglass

Real-time cross-process observation of C++ POD objects via shared memory.

## What it does

A producer application allocates POD structs in shared memory with automatic field registration. An observer process maps the same memory to inspect live object state - no serialization, no IPC overhead, just direct memory reads.

```
Producer Process                    Observer Process
     │                                    │
     ▼                                    ▼
┌─────────┐    ┌──────────────┐    ┌─────────┐
│ Objects │───▶│ Shared Memory │◀───│ Reader  │
└─────────┘    └──────────────┘    └─────────┘
```

## Quick example

**Producer:**
```cpp
#include <memglass/memglass.hpp>

struct [[memglass::observe]] Quote {
    int64_t bid_price;
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t timestamp_ns;
};

struct [[memglass::observe]] Position {
    uint32_t symbol_id;
    int64_t quantity;       // @atomic
    int64_t avg_price;
    Quote last_quote;       // @seqlock
};

int main() {
    memglass::init("trading_engine");

    auto* pos = memglass::create<Position>("AAPL_position");
    pos->symbol_id = 1;
    pos->quantity = 0;

    while (running) {
        pos->last_quote.bid_price = get_market_bid();  // Just write normally
    }

    memglass::shutdown();
}
```

**Observer:**
```cpp
#include <memglass/observer.hpp>

int main() {
    memglass::Observer obs("trading_engine");
    obs.connect();

    while (true) {
        if (auto p = obs.find("AAPL_position")) {
            int64_t qty = p["quantity"];
            int64_t bid = p["last_quote"]["bid_price"];
            int64_t ask = p["last_quote"]["ask_price"];
            printf("AAPL: qty=%ld bid=%ld ask=%ld\n", qty, bid, ask);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
```

## Constraints

- **POD types only** - must be `std::is_trivially_copyable_v<T>`
- No `std::string`, `std::vector`, raw pointers
- Use `char name[N]` for strings, `std::array<T,N>` for fixed arrays

## Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 19.29+)
- Linux/macOS (POSIX shm) or Windows (CreateFileMapping)

## Status

Design phase. See [spec-proposal.md](spec-proposal.md) for full specification.

## License

MIT
