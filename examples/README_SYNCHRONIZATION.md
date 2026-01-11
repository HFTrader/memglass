# Trading Examples - Original vs Corrected

This directory contains two versions of the trading example to illustrate the synchronization issues and their fix.

## ⚠️ Original Example (INCORRECT)

**Files:**
- `trading_types.hpp` - Struct definitions with plain types
- `trading_producer.cpp` - Producer with direct field access

**Problems:**

### 1. Type Definitions Use Plain Types
```cpp
struct Quote {
    int64_t bid_price;      // @seqlock - COMMENT ONLY!
    int64_t ask_price;
    // ...
};
```

The `@seqlock` annotation is **metadata only** - it doesn't provide synchronization.

### 2. Direct Field Access Without Synchronization
```cpp
sec->quote.bid_price += price_delta(gen);  // ❌ Plain write, no barriers!
sec->quote.ask_price = sec->quote.bid_price + 5;  // ❌ Can tear!
```

**Consequences:**
- **Torn reads**: Observer may see `bid_price` from one update and `ask_price` from another
- **Cache staleness**: Other cores may not see the write immediately
- **No atomicity**: Multi-field update is not atomic
- **Memory layout mismatch**: Observer expects `Guarded<Quote>` (with sequence counter) but producer has plain `Quote`

### 3. Why It Seems to Work

On x86-64, it may appear to work because:
- x86-64 has strong memory ordering (TSO - Total Store Ordering)
- Aligned 8-byte loads/stores are atomic
- Small examples don't stress the system

**But it WILL fail when:**
- Running on multiple cores under load
- Running on ARM/POWER (weak memory model)
- Observer reads during producer writes
- Running for extended periods (cache effects accumulate)

## ✅ Corrected Example (CORRECT)

**Files:**
- `trading_types_corrected.hpp` - Struct definitions with wrapper types
- `trading_producer_corrected.cpp` - Producer with proper synchronization

**Fixes:**

### 1. Type Definitions Use Wrapper Types
```cpp
struct Security {
    memglass::Guarded<Quote> quote;      // @seqlock - Guarded provides sync
    Position position;
};

struct Position {
    std::atomic<int64_t> quantity;       // @atomic - atomic provides sync
    // ...
};
```

The wrapper types (`Guarded<T>`, `std::atomic<T>`) provide actual synchronization.

### 2. Proper Synchronized Access
```cpp
// Read current quote
Quote current_quote = securities[i]->quote.read();  // ✅ Seqlock read

// Modify locally
current_quote.bid_price += price_delta(gen);
current_quote.ask_price = current_quote.bid_price + 5;

// Write back atomically
securities[i]->quote.write(current_quote);  // ✅ Seqlock write, all fields atomic
```

**Benefits:**
- **Atomic updates**: All fields of `Quote` updated together
- **Consistent reads**: Observer always sees consistent snapshot
- **Proper memory barriers**: Cache coherency guaranteed
- **Portable**: Works correctly on all architectures

### 3. Atomic Operations for Independent Fields
```cpp
// Atomic read
int64_t qty = position.quantity.load(std::memory_order_acquire);

// Atomic write  
position.quantity.store(new_qty, std::memory_order_release);

// Atomic increment
counter.fetch_add(1, std::memory_order_relaxed);
```

## Performance Comparison

| Operation | Original (Wrong) | Corrected | Overhead |
|-----------|------------------|-----------|----------|
| Single field write | ~1 ns | ~5-20 ns (atomic) | 5-20x |
| Multi-field write | ~4 ns | ~10-30 ns (seqlock) | 2.5-7.5x |
| Multi-field read | ~4 ns | ~10-50 ns (seqlock) | 2.5-12.5x |

**Notes:**
- Original overhead is misleading - it's fast but **wrong**
- Corrected overhead buys correctness and portability
- Seqlock overhead is negligible in read-heavy workloads
- On ARM, original would be even slower due to cache misses from lack of barriers

## Memory Layout Comparison

### Original (Broken)
```
Quote (sizeof = 32 bytes):
  [bid_price: 8][ask_price: 8][bid_size: 4][ask_size: 4][timestamp: 8]

Security (sizeof = 32 + sizeof(Position)):
  [Quote: 32 bytes][Position: ...]
```

Observer tries to read as `Guarded<Quote>` but there's no sequence counter!

### Corrected
```
Quote (sizeof = 32 bytes):
  [bid_price: 8][ask_price: 8][bid_size: 4][ask_size: 4][timestamp: 8]

Guarded<Quote> (sizeof = 40 bytes):
  [Quote: 32 bytes][sequence: 8 bytes (atomic)]

Security (sizeof = 40 + sizeof(Position)):
  [Guarded<Quote>: 40 bytes][Position: ...]
```

Memory layout matches what observer expects!

## Key Takeaways

1. **Annotations are metadata only** - they don't provide synchronization
2. **Use wrapper types** - `std::atomic<T>`, `Guarded<T>`, `Locked<T>`
3. **Match producer and observer** - both must agree on synchronization level
4. **Test on multiple architectures** - x86-64 hides many problems
5. **Read the docs** - See `docs/MEMORY_MODEL.md` for complete details

## Running the Examples

### Original (to see what NOT to do)
```bash
./build/trading_producer
./build/memglass trading_engine
```

### Corrected (proper synchronization)
```bash
./build/trading_producer_corrected
./build/memglass trading_engine_corrected
```

## Further Reading

- [docs/MEMORY_MODEL.md](../docs/MEMORY_MODEL.md) - Complete memory model guide
- [docs/SYNCHRONIZATION_ANALYSIS.md](../docs/SYNCHRONIZATION_ANALYSIS.md) - Detailed problem analysis
- [docs/advanced.md](../docs/advanced.md) - Advanced synchronization features
