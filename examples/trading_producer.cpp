// Example producer - trading engine simulation
//
// ⚠️⚠️⚠️ WARNING: THIS EXAMPLE HAS SYNCHRONIZATION ISSUES ⚠️⚠️⚠️
//
// This code demonstrates INCORRECT usage of memglass. The structs use plain
// types (int64_t, uint32_t) but the producer writes to them directly without
// proper synchronization. This can cause:
//
// 1. Torn reads - observer sees inconsistent data
// 2. Cache staleness - observer sees outdated values
// 3. Undefined behavior - memory layout mismatch with observer expectations
// 4. Platform-specific failures - especially on ARM/POWER architectures
//
// For CORRECT usage with proper synchronization, see:
// - trading_producer_corrected.cpp
// - trading_types_corrected.hpp
// - README_SYNCHRONIZATION.md
// - docs/MEMORY_MODEL.md
//
// This example is kept to show what NOT to do and for comparison purposes.
//
// ⚠️⚠️⚠️ DO NOT USE THIS PATTERN IN PRODUCTION CODE ⚠️⚠️⚠️

#include <memglass/memglass.hpp>
#include "trading_types.hpp"
#include "trading_types_generated.hpp"

#include <fmt/format.h>
#include <chrono>
#include <random>
#include <thread>
#include <csignal>
#include <iostream>

static volatile bool g_running = true;

void signal_handler(int) {
    g_running = false;
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize memglass
    if (!memglass::init("trading_engine")) {
        std::cerr << "Failed to initialize memglass\n";
        return 1;
    }

    // Register types
    memglass::generated::register_all_types();

    // Write types to header (trigger refresh in observers)
    memglass::registry::write_to_header(
        memglass::detail::get_context()->header(),
        memglass::detail::get_context()->header_shm().data());

    std::cout << "Trading engine started (PID: " << getpid() << ")\n";
    std::cout << "Press Ctrl+C to stop\n\n";

    // Create securities for tracked symbols
    const char* symbols[] = {"AAPL", "MSFT", "GOOG", "AMZN", "META"};
    std::vector<Security*> securities;

    for (size_t i = 0; i < 5; ++i) {
        auto* sec = memglass::create<Security>(symbols[i]);
        if (!sec) {
            std::cerr << "Failed to create security " << symbols[i] << "\n";
            continue;
        }

        // Initialize quote
        sec->quote.bid_price = 15000 + static_cast<int64_t>(i) * 1000;
        sec->quote.ask_price = sec->quote.bid_price + 5;
        sec->quote.bid_size = 100;
        sec->quote.ask_size = 100;
        sec->quote.timestamp_ns = 0;

        // Initialize position
        sec->position.symbol_id = static_cast<uint32_t>(i);
        sec->position.quantity = 0;
        sec->position.avg_price = 0;
        sec->position.realized_pnl = 0;
        sec->position.unrealized_pnl = 0;

        securities.push_back(sec);
        std::cout << "Created " << symbols[i] << " security\n";
    }

    // Market simulation loop
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> price_delta(-10, 10);
    std::uniform_int_distribution<> size_change(-20, 20);

    uint64_t tick = 0;
    while (g_running) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();

        for (size_t i = 0; i < securities.size(); ++i) {
            auto& quote = securities[i]->quote;
            auto& position = securities[i]->position;

            // Update quotes with price movement
            quote.bid_price += price_delta(gen);
            if (quote.bid_price < 1000) quote.bid_price = 1000;
            quote.ask_price = quote.bid_price + 5;

            int new_bid_size = static_cast<int>(quote.bid_size) + size_change(gen);
            if (new_bid_size < 10) new_bid_size = 10;
            quote.bid_size = static_cast<uint32_t>(new_bid_size);

            int new_ask_size = static_cast<int>(quote.ask_size) + size_change(gen);
            if (new_ask_size < 10) new_ask_size = 10;
            quote.ask_size = static_cast<uint32_t>(new_ask_size);

            quote.timestamp_ns = static_cast<uint64_t>(now);

            // Occasionally change position
            if (tick % 100 == i * 20) {
                int pos_change = (gen() % 3) - 1;  // -1, 0, or 1
                position.quantity += pos_change * 100;

                if (position.quantity != 0 && position.avg_price == 0) {
                    position.avg_price = quote.bid_price;
                }
            }

            // Update P&L
            if (position.quantity != 0) {
                int64_t mark = quote.bid_price;
                position.unrealized_pnl =
                    (mark - position.avg_price) * position.quantity;
            }
        }

        // Print status every second
        if (tick % 100 == 0) {
            std::cout << "\rTick " << tick << ": ";
            for (size_t i = 0; i < securities.size() && i < 3; ++i) {
                std::cout << symbols[i] << "=" << securities[i]->quote.bid_price << " ";
            }
            std::cout << "          " << std::flush;
        }

        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\n\nShutting down...\n";

    // Cleanup
    for (auto* sec : securities) {
        memglass::destroy(sec);
    }

    memglass::shutdown();
    return 0;
}
