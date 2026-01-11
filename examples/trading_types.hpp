#pragma once

#include <cstdint>

// Trading types for memglass example
//
// ⚠️⚠️⚠️ WARNING: THESE TYPE DEFINITIONS ARE INCORRECT ⚠️⚠️⚠️
//
// This file demonstrates INCORRECT type definitions. The annotations
// (@atomic, @seqlock, etc.) are metadata only - they tell observers
// how to READ fields but do NOT provide synchronization.
//
// Problems with these definitions:
// 1. Fields are plain types (int64_t) not wrappers (std::atomic<int64_t>)
// 2. Producer writes directly to fields without synchronization
// 3. Memory layout doesn't match what annotations suggest
// 4. Will cause torn reads, cache issues, and undefined behavior
//
// For CORRECT type definitions with proper synchronization, see:
// - trading_types_corrected.hpp
// - README_SYNCHRONIZATION.md
// - docs/MEMORY_MODEL.md
//
// ⚠️⚠️⚠️ DO NOT USE THIS PATTERN IN PRODUCTION CODE ⚠️⚠️⚠️

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
};

struct [[memglass::observe]] Order {
    uint64_t order_id;      // @readonly
    uint32_t symbol_id;
    int64_t price;
    uint32_t quantity;
    uint32_t filled_qty;
    int8_t side;            // @enum(BUY=1, SELL=-1)
    int8_t status;          // @enum(PENDING=0, OPEN=1, FILLED=2, CANCELLED=3)
    int8_t padding[2];
};

// Security combines Quote and Position for a single symbol
struct [[memglass::observe]] Security {
    Quote quote;
    Position position;
};
