# Synchronization Analysis

## Executive Summary

The claim that "there is no synchronization" is **partially correct**. While the codebase includes well-designed synchronization primitives (seqlocks, spinlocks, atomics), they are **not being used on the producer side**. This creates a critical gap where:

1. **Observers read with synchronization** (via `Guarded<T>`, `std::atomic<T>`, etc.)
2. **Producers write without synchronization** (direct memory writes)
3. **Result**: Race conditions, torn reads, and cache coherency issues

## Detailed Analysis

### 1. Synchronization Primitives (Exist and Work)

The codebase provides three synchronization mechanisms:

#### a) Seqlock (`Guarded<T>`) - Lock-free reader-writer
```cpp
template <typename T>
struct Guarded {
    void write(const T &v) noexcept {
        std::size_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);  // Mark write start
        std::atomic_signal_fence(std::memory_order_acq_rel);
        value_ = v;
        std::atomic_signal_fence(std::memory_order_acq_rel);
        seq_.store(s + 2, std::memory_order_release);  // Mark write complete
    }
    
    T read() const noexcept {
        T copy;
        std::size_t s1, s2;
        do {
            s1 = seq_.load(std::memory_order_acquire);
            std::atomic_signal_fence(std::memory_order_acq_rel);
            copy = value_;
            std::atomic_signal_fence(std::memory_order_acq_rel);
            s2 = seq_.load(std::memory_order_acquire);
        } while (s1 != s2 || s1 & 1);
        return copy;
    }
private:
    T value_{};
    std::atomic<std::size_t> seq_;
};
```

**Status**: ✅ Correctly implemented with proper memory barriers

#### b) Spinlock (`Locked<T>`) - Exclusive access
```cpp
template <typename T>
struct Locked {
    void write(const T &v) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            MEMGLASS_PAUSE();
        }
        std::memcpy(&value, &v, sizeof(T));
        lock_.clear(std::memory_order_release);
    }
    
    T read() const {
        while (lock_.test_and_set(std::memory_order_acquire)) {
            MEMGLASS_PAUSE();
        }
        T result;
        std::memcpy(&result, &value, sizeof(T));
        lock_.clear(std::memory_order_release);
        return result;
    }
private:
    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    T value{};
};
```

**Status**: ✅ Correctly implemented with proper acquire-release semantics

#### c) Atomic operations via `std::atomic<T>`
**Status**: ✅ Standard library implementation

### 2. The Critical Problem: Producer-Side Writes

#### Current Implementation (trading_producer.cpp)
```cpp
struct Quote {
    int64_t bid_price;      // @seqlock - COMMENT ONLY!
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t timestamp_ns;
};

// In producer code:
auto* sec = memglass::create<Security>("AAPL");
sec->quote.bid_price += price_delta(gen);  // ❌ DIRECT WRITE - NO SYNCHRONIZATION
sec->quote.ask_price = sec->quote.bid_price + 5;  // ❌ DIRECT WRITE
```

**What happens:**
1. Field is declared as plain `int64_t` (not `Guarded<int64_t>`)
2. Direct assignment compiles to simple memory store
3. **No memory barriers** - compiler/CPU can reorder
4. **No sequence counter update** - observer's seqlock is useless
5. **Cache not flushed** - other cores may see stale values

#### Observer Side (Attempts to Read Safely)
```cpp
template<typename T>
T FieldProxy::read() const {
    switch (field_->atomicity) {
        case Atomicity::Seqlock:
            return reinterpret_cast<Guarded<T>*>(data_)->read();  // ❌ WRONG!
        // ...
    }
}
```

**The mismatch:**
- Observer treats memory as `Guarded<int64_t>` (reads sequence counter)
- Producer wrote plain `int64_t` (no sequence counter exists at that location!)
- **Memory layout doesn't match** - reading garbage data

### 3. Memory Layout Mismatch

#### If using `@seqlock` annotation:
```
Expected by observer (Guarded<int64_t>):
[value: 8 bytes][seq_: 8 bytes (atomic)] = 16 bytes

Actual memory (plain int64_t):
[value: 8 bytes] = 8 bytes
```

#### If using `@atomic` annotation:
```
Expected by observer (std::atomic<int64_t>):
[value: 8 bytes (with atomic operations)]

Actual memory (plain int64_t):
[value: 8 bytes (no atomic guarantees)]
```

### 4. Cache Coherency Issues

Even for fields without annotations:

```cpp
// Producer on Core 0:
sec->quote.bid_size = 100;
sec->quote.ask_size = 200;

// Observer on Core 1:
uint32_t bid = sec->quote.bid_size;  // May still see old value!
uint32_t ask = sec->quote.ask_size;  // May see new value!
```

**Problem**: Without memory barriers, the CPU cache may not be synchronized across cores. The x86-64 architecture provides sequential consistency for aligned loads/stores, but:
- Writes may be buffered in store buffers
- Other cores may read from their cache before seeing the write
- ARM/POWER architectures have weaker memory models (even worse)

### 5. Test Coverage Gap

The existing tests (test_seqlock.cpp) validate the synchronization primitives in isolation:

```cpp
TEST_F(SeqlockTest, GuardedConcurrentAccess) {
    Guarded<SimpleData> guarded;  // ✅ Uses Guarded wrapper
    
    std::thread writer([&]() {
        SimpleData data{i, i};
        guarded.write(data);  // ✅ Proper synchronization
    });
    
    std::thread reader([&]() {
        SimpleData result = guarded.read();  // ✅ Proper synchronization
    });
}
```

**What's NOT tested:**
- Producer writing to plain struct fields
- Observer reading from plain struct fields with atomicity metadata
- Cross-process scenario with actual shared memory

## Root Causes

### 1. Design Ambiguity
The annotations (`@seqlock`, `@atomic`) are intended as **metadata only**:
- Observer uses them to know HOW to read
- But producer must ALSO use them to know HOW to write

Currently, the producer ignores annotations entirely.

### 2. Code Generator Limitation
`memglass-gen` parses annotations but doesn't transform the struct:
- **Current**: `int64_t field; // @seqlock` stays as `int64_t`
- **Needed**: Transform to `Guarded<int64_t> field;` OR provide accessor methods

### 3. Shared Memory Abstraction Leak
The abstraction pretends fields can be accessed like normal C++ members, but:
- Normal members: compiler handles everything
- Shared memory: needs explicit synchronization

## Impact Assessment

### Severity: **HIGH**

1. **Torn Reads**: Multi-field structures (e.g., Quote with bid/ask) can be read inconsistently
2. **Stale Data**: Observer may see outdated values due to cache
3. **Undefined Behavior**: Type-punning between `int64_t` and `Guarded<int64_t>` is UB
4. **Platform Dependent**: Works "by accident" on x86-64 TSO but fails on ARM/POWER
5. **Data Corruption**: If observer writes back (via FieldProxy assignment operator), corruption is certain

### Likelihood: **CERTAIN**

Without barriers:
- Single-core: May work (no cache coherency issues)
- Multi-core: **WILL** have issues under load
- Cross-socket: **WILL** have severe issues
- ARM/POWER: **WILL** fail immediately

## Recommendations

### Option 1: Transform Types (Invasive, Correct)
**Change struct definitions to use wrappers:**
```cpp
struct Quote {
    Guarded<int64_t> bid_price;  // Seqlock-protected
    Guarded<int64_t> ask_price;
    std::atomic<uint32_t> bid_size;  // Atomic
    std::atomic<uint32_t> ask_size;
    uint64_t timestamp_ns;  // Plain (no sync needed)
};

// Producer writes:
sec->quote.bid_price.write(15000);  // Explicit synchronization
```

**Pros:**
- Type-safe at compile time
- Impossible to forget synchronization
- Memory layout matches expectation

**Cons:**
- Changes user-facing API
- More verbose code
- Existing code must be rewritten

### Option 2: Accessor Methods (Less Invasive)
**Keep plain types, provide accessors:**
```cpp
struct Quote {
    int64_t bid_price;  // @seqlock
    int64_t ask_price;
    
    void set_bid_price(int64_t val) {
        auto* g = reinterpret_cast<Guarded<int64_t>*>(&bid_price);
        g->write(val);
    }
    
    int64_t get_bid_price() const {
        auto* g = reinterpret_cast<const Guarded<int64_t>*>(&bid_price);
        return g->read();
    }
};
```

**Pros:**
- Hides synchronization complexity
- Can be generated automatically

**Cons:**
- Still allows direct field access (escape hatch)
- Type-punning is technically UB (though works in practice)
- Memory layout must be carefully managed

### Option 3: Proxy Objects (Current Observer Pattern Extended)
**Use smart proxies for ALL field access:**
```cpp
template<typename T>
class FieldWriter {
    // ...provides transparent write with synchronization
};

// memglass::create returns a proxy instead of T*
auto sec = memglass::create<Security>("AAPL");
sec.quote.bid_price = 15000;  // Looks like assignment but uses proxy
```

**Pros:**
- Looks like normal C++ code
- Type-safe
- Can enforce synchronization

**Cons:**
- Complex implementation
- Performance overhead
- Debugging difficulty

### Option 4: Minimum Fix (Document + Add Barriers)
**Accept current design but add memory barriers:**
```cpp
// In producer writes:
sec->quote.bid_price = 15000;
std::atomic_thread_fence(std::memory_order_release);  // Explicit barrier
```

**Pros:**
- Minimal code change
- Fixes cache coherency

**Cons:**
- Doesn't fix torn reads
- Still wrong memory layout for seqlock/locked fields
- Easy to forget barriers

## Recommended Solution

**Hybrid approach:**

1. **For fields with `@atomic`**: Use `std::atomic<T>` in struct
2. **For fields with `@seqlock`**: Use `Guarded<T>` in struct  
3. **For fields with `@locked`**: Use `Locked<T>` in struct
4. **For plain fields**: Add explicit memory barriers in producer OR document as observer-only

This requires:
- Update code generator to emit transformed types
- Update examples to use new types
- Document the memory model clearly
- Add integration tests with actual shared memory

## Conclusion

The issue reporter is **correct**: the current implementation has critical synchronization gaps. While excellent synchronization primitives exist, they are not being used on the producer side, leading to:

1. **Torn reads** for compound types
2. **Cache coherency issues** across cores
3. **Memory layout mismatches** between producer and observer
4. **Platform-specific failures** on non-x86 architectures

This must be fixed before the library can be considered production-ready.
