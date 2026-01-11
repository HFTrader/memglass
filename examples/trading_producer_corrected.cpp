// Example producer demonstrating CORRECT synchronization usage
//
// This example shows how to properly use memglass with synchronized types.
// Compare with trading_producer.cpp to see the differences.

#include <memglass/memglass.hpp>
#include "trading_types_corrected.hpp"

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
    if (!memglass::init("trading_engine_corrected")) {
        std::cerr << "Failed to initialize memglass\n";
        return 1;
    }

    std::cout << "Trading engine started with CORRECT synchronization (PID: " << getpid() << ")\n";
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

        // Initialize quote using Guarded<T>::write()
        Quote initial_quote{
            .bid_price = 15000 + static_cast<int64_t>(i) * 1000,
            .ask_price = 15005 + static_cast<int64_t>(i) * 1000,
            .bid_size = 100,
            .ask_size = 100,
            .timestamp_ns = 0
        };
        sec->quote.write(initial_quote);  // ✅ Proper seqlock write

        // Initialize position using std::atomic
        sec->position.symbol_id = static_cast<uint32_t>(i);
        sec->position.quantity.store(0, std::memory_order_release);  // ✅ Proper atomic write
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
            // Read current quote (consistent read via seqlock)
            Quote current_quote = securities[i]->quote.read();

            // Update quote with price movement
            current_quote.bid_price += price_delta(gen);
            if (current_quote.bid_price < 1000) current_quote.bid_price = 1000;
            current_quote.ask_price = current_quote.bid_price + 5;

            int new_bid_size = static_cast<int>(current_quote.bid_size) + size_change(gen);
            if (new_bid_size < 10) new_bid_size = 10;
            current_quote.bid_size = static_cast<uint32_t>(new_bid_size);

            int new_ask_size = static_cast<int>(current_quote.ask_size) + size_change(gen);
            if (new_ask_size < 10) new_ask_size = 10;
            current_quote.ask_size = static_cast<uint32_t>(new_ask_size);

            current_quote.timestamp_ns = static_cast<uint64_t>(now);

            // Write updated quote atomically (all fields updated together)
            securities[i]->quote.write(current_quote);  // ✅ Proper seqlock write

            // Occasionally change position
            if (tick % 100 == i * 20) {
                int pos_change = (gen() % 3) - 1;  // -1, 0, or 1
                
                // Update quantity atomically
                int64_t old_qty = securities[i]->position.quantity.load(std::memory_order_acquire);
                int64_t new_qty = old_qty + pos_change * 100;
                securities[i]->position.quantity.store(new_qty, std::memory_order_release);  // ✅ Proper atomic write

                if (new_qty != 0 && securities[i]->position.avg_price == 0) {
                    securities[i]->position.avg_price = current_quote.bid_price;
                }
            }

            // Update P&L
            int64_t quantity = securities[i]->position.quantity.load(std::memory_order_acquire);  // ✅ Proper atomic read
            if (quantity != 0) {
                int64_t mark = current_quote.bid_price;
                securities[i]->position.unrealized_pnl =
                    (mark - securities[i]->position.avg_price) * quantity;
            }
        }

        // Print status every second
        if (tick % 100 == 0) {
            std::cout << "\rTick " << tick << ": ";
            for (size_t i = 0; i < securities.size() && i < 3; ++i) {
                // Read current bid price using seqlock
                Quote q = securities[i]->quote.read();
                std::cout << symbols[i] << "=" << q.bid_price << " ";
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
