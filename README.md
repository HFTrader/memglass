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

struct Vec3 { float x, y, z; };
MEMGLASS_REGISTER(Vec3, x, y, z);

struct Player {
    uint32_t id;
    Vec3 position;
    float health;
};
MEMGLASS_REGISTER(Player, id, position, health);

int main() {
    memglass::init("my_game");

    auto* player = memglass::create<Player>("player_1");
    player->id = 1;
    player->health = 100.0f;

    while (running) {
        player->position.x += 0.1f;  // Just write normally
    }

    memglass::shutdown();
}
```

**Observer:**
```cpp
#include <memglass/observer.hpp>

int main() {
    memglass::Observer obs("my_game");
    obs.connect();

    while (true) {
        if (auto p = obs.find("player_1")) {
            float health = p.read<float>("health");
            auto pos = p.field("position");
            printf("Player at (%.1f, %.1f, %.1f) health=%.0f\n",
                   pos.read<float>("x"),
                   pos.read<float>("y"),
                   pos.read<float>("z"),
                   health);
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
