#pragma once

#include <memglass/detail/seqlock.hpp>
#include <atomic>
#include <cstdint>

// Trading types for memglass example - WITH PROPER SYNCHRONIZATION
// 
// This demonstrates the CORRECT way to define types for shared memory:
// - Use std::atomic<T> for fields marked @atomic
// - Use Guarded<T> for fields marked @seqlock  
// - Use Locked<T> for fields marked @locked
//
// The annotations (@atomic, @seqlock, @locked) are metadata that tell
// observers HOW to read the fields. They do NOT provide synchronization 
// themselves - you MUST use the wrapper types.

// Quote represents market data that changes frequently
// We wrap the entire Quote in Guarded<> for consistent reads
struct Quote {
    int64_t bid_price;      // Price in ticks
    int64_t ask_price;
    uint32_t bid_size;
    uint32_t ask_size;
    uint64_t timestamp_ns;
};

// Position represents account position data
struct [[memglass::observe]] Position {
    uint32_t symbol_id;
    std::atomic<int64_t> quantity;       // @atomic - frequent updates
    int64_t avg_price;                   // Updated with quantity, so in same sync group
    int64_t realized_pnl;
    int64_t unrealized_pnl;
};

// Order represents an order in the order book
struct [[memglass::observe]] Order {
    uint64_t order_id;                   // @readonly
    uint32_t symbol_id;
    int64_t price;
    std::atomic<uint32_t> quantity;      // @atomic
    std::atomic<uint32_t> filled_qty;    // @atomic - updated independently
    int8_t side;                         // @enum(BUY=1, SELL=-1)
    int8_t status;                       // @enum(PENDING=0, OPEN=1, FILLED=2, CANCELLED=3)
    int8_t padding[2];
};

// Security combines Quote and Position for a single symbol
// Quote changes frequently and needs consistent reads, so use Guarded<>
struct [[memglass::observe]] Security {
    memglass::Guarded<Quote> quote;      // @seqlock - prevents torn reads of multi-field struct
    Position position;
};

// Alternative: If you need each field independently synchronized
// (more overhead but allows partial updates)
struct [[memglass::observe]] QuoteAtomic {
    std::atomic<int64_t> bid_price;      // @atomic
    std::atomic<int64_t> ask_price;      // @atomic
    std::atomic<uint32_t> bid_size;      // @atomic
    std::atomic<uint32_t> ask_size;      // @atomic
    std::atomic<uint64_t> timestamp_ns;  // @atomic
};
