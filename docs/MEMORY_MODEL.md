# Memory Model and Synchronization Guide

## Overview

memglass uses shared memory for zero-copy data sharing between processes. This requires explicit synchronization to ensure data consistency and cache coherency across CPU cores and processes.

## The Problem

Shared memory presents unique challenges:

1. **Cache Coherency**: Each CPU core has its own cache. Without memory barriers, writes from one core may not be visible to another.
2. **Torn Reads**: Multi-field structures can be read inconsistently if the writer updates them mid-read.
3. **Compiler Reordering**: The compiler may reorder reads/writes for optimization, breaking assumptions about ordering.
4. **CPU Reordering**: Modern CPUs can reorder memory operations for performance.

## Memory Consistency Guarantees

### Platform-Specific Behavior

#### x86-64 (Total Store Ordering - TSO)
- **Reads**: Not reordered with other reads
- **Writes**: Not reordered with other writes (but can be reordered with reads)
- **Aligned loads/stores**: Atomic for data ≤ 8 bytes
- **Unaligned or >8 bytes**: May tear

**Implication**: Simple scalar fields work "by accident" on x86-64, but can still have issues:
- Cache staleness (requires barriers)
- Multi-field structures can tear
- Not portable to ARM/POWER

#### ARM/AArch64 and POWER (Weak Memory Models)
- **All reordering allowed** unless explicit barriers used
- **Plain loads/stores**: Can be reordered freely
- **Cache coherency**: Not guaranteed without barriers

**Implication**: Code that works on x86-64 will **likely fail** on ARM without proper synchronization.

## Required Synchronization

### Rule 1: All Shared Fields Must Use Synchronization

**For Producer (Writer) Side:**

#### Option A: Use Wrapper Types (Recommended)
```cpp
struct [[memglass::observe]] Data {
    std::atomic<int64_t> counter;    // @atomic - atomic reads/writes
    Guarded<Quote> quote;            // @seqlock - seqlock protection
    Locked<char[256]> message;       // @locked - mutex protection
};

// Writing:
data->counter.store(42, std::memory_order_release);
data->quote.write(Quote{100, 200});
data->message.write("hello");
```

**Memory layout**: The wrapper types include metadata (sequence counters, locks) in the struct.

#### Option B: Manual Barriers (Not Recommended)
```cpp
struct [[memglass::observe]] Data {
    int64_t counter;  // @atomic (annotation only!)
    Quote quote;      // @seqlock (annotation only!)
};

// Writing (WRONG - direct access):
data->counter = 42;  // ❌ No atomicity, no cache flush

// Writing (CORRECT - but verbose):
std::atomic_ref<int64_t>(data->counter).store(42, std::memory_order_release);
std::atomic_thread_fence(std::memory_order_release);
```

**Problem**: This is error-prone and doesn't solve torn reads for compound types.

### Rule 2: Annotations Must Match Implementation

The `@atomic`, `@seqlock`, and `@locked` annotations tell the **observer** how to read fields. The **producer** must write using compatible mechanisms.

#### Correct Usage
```cpp
// Type definition
struct Data {
    std::atomic<int64_t> counter;  // @atomic
};

// Producer writes
data->counter.store(100, std::memory_order_release);  // ✅ Matches annotation

// Observer reads
int64_t val = observer["counter"];  // ✅ Uses atomic load
```

#### Incorrect Usage
```cpp
// Type definition
struct Data {
    int64_t counter;  // @atomic (comment only!)
};

// Producer writes
data->counter = 100;  // ❌ Plain write, no atomicity!

// Observer reads
int64_t val = observer["counter"];  // Tries to use atomic load on plain field
                                     // ❌ Undefined behavior (type-punning)
```

## Synchronization Primitives

### 1. Atomic Fields (`std::atomic<T>`)

**When to use:**
- Single scalar values (integers, floating point)
- Size ≤ 8 bytes (or architecture word size)
- Frequent updates
- Lock-free required

**Example:**
```cpp
struct Counter {
    std::atomic<uint64_t> value;  // @atomic
    std::atomic<uint64_t> timestamp;  // @atomic
};

// Producer
counter->value.fetch_add(1, std::memory_order_relaxed);
counter->timestamp.store(now(), std::memory_order_release);

// Observer
uint64_t v = observer["value"];  // Uses atomic load
```

**Memory ordering:**
- `memory_order_relaxed`: No ordering guarantees, fastest
- `memory_order_release` (write) + `memory_order_acquire` (read): Ensures writes before store are visible after load
- `memory_order_seq_cst`: Full sequential consistency (slowest, usually overkill)

**Guidelines:**
- Use `relaxed` for independent counters
- Use `release/acquire` when fields depend on each other
- Use `seq_cst` only when absolute ordering is critical

### 2. Seqlock Fields (`Guarded<T>`)

**When to use:**
- Compound types (structs with multiple fields)
- Read-heavy workloads (many observers, few writers)
- Single writer (seqlock is optimized for this)
- Lock-free reads required

**Example:**
```cpp
struct Quote {
    int64_t bid_price;
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
};

struct MarketData {
    Guarded<Quote> quote;  // @seqlock
};

// Producer (single writer)
Quote q{15000, 15005, 100, 200};
data->quote.write(q);  // Seqlock ensures consistency

// Observer (multiple readers)
Quote q = observer["quote"];  // Spins if write in progress

// Or non-blocking:
auto maybe_q = observer["quote"].try_get<Quote>();
if (maybe_q) {
    // Got consistent snapshot
}
```

**How it works:**
1. Writer increments sequence number (marks write in progress)
2. Writer updates value
3. Writer increments sequence number again (marks complete)
4. Reader checks sequence before and after read; retries if mismatch

**Performance:**
- **Reads**: ~10-50ns (if no contention), may spin on contention
- **Writes**: ~10-30ns
- **Overhead**: 8 bytes (sequence counter) per field

### 3. Locked Fields (`Locked<T>`)

**When to use:**
- Complex read-modify-write operations
- Multiple writers
- String buffers or variable-length data
- When correctness matters more than performance

**Example:**
```cpp
struct Status {
    Locked<char[256]> message;  // @locked
};

// Producer
status->message.write("Starting...");

// Or read-modify-write:
status->message.update([](char* buf) {
    std::strcat(buf, " [OK]");
});

// Observer
char msg[256];
std::strcpy(msg, observer["message"].as<const char*>());
```

**How it works:**
- Uses `std::atomic_flag` as spinlock
- Acquires lock on both read and write

**Performance:**
- **Reads**: ~20-100ns
- **Writes**: ~20-100ns
- **Overhead**: 1 byte (atomic flag) per field

### 4. Plain Fields (No Synchronization)

**When to use:**
- Debug/diagnostic data where consistency doesn't matter
- Read-only data set at initialization
- Documentation/metadata fields

**Example:**
```cpp
struct Debug {
    uint32_t version;        // Set once at startup
    uint32_t debug_flags;    // Consistency not critical
};

// Producer
debug->version = 1;
debug->debug_flags = 0x0F;  // May tear, but acceptable

// Observer
uint32_t v = observer["version"];  // Direct read, may be stale
```

**Caveats:**
- **No guarantees**: May see torn reads, stale data
- **Platform dependent**: May work on x86-64 but fail on ARM
- **Should be rare**: Only use when consistency truly doesn't matter

## Best Practices

### 1. Always Wrap Shared Fields

❌ **BAD:**
```cpp
struct Data {
    int64_t price;  // @atomic (comment doesn't help!)
};

data->price = 100;  // Plain write
```

✅ **GOOD:**
```cpp
struct Data {
    std::atomic<int64_t> price;  // @atomic (type enforces)
};

data->price.store(100, std::memory_order_release);  // Synchronized write
```

### 2. Match Annotations to Implementation

The annotation tells observers how to read. Producer must write compatibly.

| Annotation | Producer Type | Producer Write | Observer Read |
|------------|---------------|----------------|---------------|
| `@atomic` | `std::atomic<T>` | `.store()` / `.fetch_add()` | Atomic load |
| `@seqlock` | `Guarded<T>` | `.write()` | Seqlock read |
| `@locked` | `Locked<T>` | `.write()` / `.update()` | Locked read |
| (none) | `T` (plain) | Direct assignment | Direct read |

### 3. Use Coarse-Grained Synchronization

Instead of:
```cpp
struct Data {
    std::atomic<int64_t> field1;  // 8 bytes overhead
    std::atomic<int64_t> field2;  // 8 bytes overhead
    std::atomic<int64_t> field3;  // 8 bytes overhead
};
```

Consider:
```cpp
struct DataFields {
    int64_t field1;
    int64_t field2;
    int64_t field3;
};

struct Data {
    Guarded<DataFields> fields;  // Single 8-byte overhead for all
};
```

### 4. Minimize Seqlock Contention

Seqlock readers spin if writer is active. Avoid:
```cpp
// BAD: Long-running write holds lock
data->large_struct.write(compute_expensive_value());  // Blocks readers!
```

Instead:
```cpp
// GOOD: Compute first, then write quickly
LargeStruct result = compute_expensive_value();
data->large_struct.write(result);  // Minimal lock time
```

### 5. Test on Multiple Architectures

Code that works on x86-64 may fail on ARM:
```bash
# Test on ARM if available
docker run --rm -it arm64v8/ubuntu:latest
# Build and run your tests
```

Use ThreadSanitizer to detect races:
```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" ..
make
./your_test  # Will detect data races
```

## FAQ

### Q: Can I mix plain and synchronized fields?

Yes, but be careful:
```cpp
struct Data {
    uint32_t version;                    // Plain (set once)
    std::atomic<uint64_t> counter;       // Atomic (frequent updates)
    Guarded<ComplexData> data;           // Seqlock (compound type)
};
```

Just ensure each field's usage matches its synchronization level.

### Q: What about arrays?

For arrays of scalars, use `std::atomic` array:
```cpp
struct Data {
    std::atomic<int32_t> values[100];  // @atomic
};
```

For arrays of structs, wrap the struct:
```cpp
struct Data {
    Guarded<Quote> quotes[10];  // @seqlock
};
```

### Q: How do I handle strings?

Fixed-size strings use `Locked`:
```cpp
struct Data {
    Locked<char[256]> message;  // @locked
};

data->message.write("Hello");
```

### Q: What's the performance impact?

| Type | Read Overhead | Write Overhead | Memory Overhead |
|------|---------------|----------------|-----------------|
| Plain | 0% | 0% | 0% |
| Atomic | +20-50% | +20-50% | 0% |
| Seqlock | +50-200% (if contention) | +30-80% | +8 bytes |
| Locked | +100-400% | +100-400% | +1 byte |

Use profiling to measure actual impact in your application.

### Q: Can observers write back to fields?

Yes, via `FieldProxy`:
```cpp
auto view = observer.find("obj");
view["counter"] = 42;  // Writes through proxy with synchronization
```

But be careful:
- Multiple observers writing = race conditions
- Consider making fields `@readonly` if observers shouldn't modify

## Summary

**Golden Rules:**
1. ✅ Use `std::atomic<T>` for scalar values
2. ✅ Use `Guarded<T>` for compound types
3. ✅ Use `Locked<T>` for strings or RMW operations
4. ✅ Match annotations to implementation
5. ✅ Test on multiple architectures
6. ❌ Never use plain types for data that must be consistent
7. ❌ Never rely on "it works on my machine" without barriers

Following these rules ensures your shared memory is consistent, portable, and correct across all platforms and CPU architectures.
