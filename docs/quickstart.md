# Quick Start Guide

This guide will get you up and running with memglass in 5 minutes.

## Prerequisites

- C++20 compiler (GCC 10+, Clang 12+)
- CMake 3.16+
- libclang-dev (for the code generator)

```bash
# Ubuntu/Debian
sudo apt install libclang-dev

# Fedora
sudo dnf install clang-devel

# macOS
brew install llvm
```

## Building

```bash
git clone https://github.com/your-repo/memglass
cd memglass
mkdir build && cd build
cmake ..
make
```

## Step 1: Define Your Types

Create a header with POD structs marked for observation:

```cpp
// my_types.hpp
#pragma once
#include <cstdint>

struct [[memglass::observe]] Counter {
    uint64_t value;      // @atomic
    uint64_t timestamp;
};

struct [[memglass::observe]] Stats {
    int64_t min_value;
    int64_t max_value;
    double average;
    uint32_t count;
};
```

The `[[memglass::observe]]` attribute marks types for automatic reflection. Comments like `// @atomic` specify synchronization behavior.

## Step 2: Generate Registration Code

Run the code generator on your header:

```bash
./build/memglass-gen my_types.hpp -o my_types_generated.hpp
```

This creates type registration functions that memglass uses at runtime.

## Step 3: Write a Producer

```cpp
// producer.cpp
#include <memglass/memglass.hpp>
#include "my_types.hpp"
#include "my_types_generated.hpp"

#include <thread>
#include <chrono>

int main() {
    // Initialize with a session name
    memglass::init("my_app");

    // Register all generated types
    memglass::generated::register_all_types();

    // Create objects in shared memory
    auto* counter = memglass::create<Counter>("main_counter");
    auto* stats = memglass::create<Stats>("global_stats");

    // Initialize
    counter->value = 0;
    counter->timestamp = 0;
    stats->min_value = INT64_MAX;
    stats->max_value = INT64_MIN;
    stats->average = 0.0;
    stats->count = 0;

    // Update loop
    while (true) {
        counter->value++;
        counter->timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();

        // Update stats
        int64_t v = counter->value % 1000;
        if (v < stats->min_value) stats->min_value = v;
        if (v > stats->max_value) stats->max_value = v;
        stats->count++;
        stats->average = (stats->average * (stats->count - 1) + v) / stats->count;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    memglass::shutdown();
    return 0;
}
```

## Step 4: Observe with the TUI

Start your producer in one terminal:

```bash
./producer
```

In another terminal, use the memglass viewer:

```bash
./build/memglass my_app
```

You'll see an interactive tree view:

```
=== Memglass Browser ===
PID: 12345  Objects: 2  Seq: 2
--------------------------------------------------------------------------------
[-] main_counter (Counter)
      value            =         123456 [atomic]
      timestamp        = 1704825432123456789
[+] global_stats (Stats)
--------------------------------------------------------------------------------
h/? for help | q to quit
```

**Controls:**
- `↑/↓` or `j/k` - Navigate
- `Enter` or `Space` - Expand/collapse
- `q` - Quit
- `h` or `?` - Toggle help

## Step 5: Or Use the Web UI

For a browser-based view, start the web server:

```bash
./build/memglass --web my_app
```

Then open http://localhost:8080 in your browser. The web UI provides:
- Auto-refresh with value change highlighting
- Expandable/collapsible tree view
- Atomicity badges (atomic, seqlock, locked)
- Dark theme optimized for monitoring

You can also specify a custom port:

```bash
./build/memglass --web 9000 my_app
```

## Step 6: Write a Custom Observer (Optional)

For programmatic access:

```cpp
// observer.cpp
#include <memglass/observer.hpp>
#include <iostream>
#include <thread>

int main() {
    memglass::Observer obs("my_app");

    if (!obs.connect()) {
        std::cerr << "Failed to connect\n";
        return 1;
    }

    while (true) {
        auto counter = obs.find("main_counter");
        if (counter) {
            uint64_t value = counter["value"];
            std::cout << "Counter: " << value << "\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
```

## Next Steps

- Read the [Advanced Guide](advanced.md) for nested structs, synchronization options, and more
- See the [API Reference](api-reference.md) for complete documentation
- Check the [Architecture](architecture.md) for understanding the internals
