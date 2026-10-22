# API Overview

Shield's authoritative contracts are spread across the documents below; this page is only a navigation hub and no longer inlines summaries — summaries go stale as contracts evolve, and they once included the deprecated `shield.db:*` / `shield.redis:*` legacy forms (data access now goes uniformly through the plugin namespace `shield.database.*`, see lua-api.md).

## Document Navigation

- **Lua API Contract** — the authoritative entry point for the Lua user API (`shield.spawn/send/call/...`, the plugin namespace `shield.database.*`, etc.).
- **Lua API Tests** — the API acceptance matrix.
- **Architecture Overview** — the refactored architecture overview.
- **[Runtime Semantics Decisions](runtime-semantics.md)** — runtime semantics decisions A1-A35.
- **[Quick Start](quickstart.md)** — the C++ entry point `shield::run` and a minimal example.
- **[Game Backend Tutorial](tutorial-game-backend.md)** — an end-to-end Lua example.

## Implementation Status

- `examples/hello_world/` is the user reference example; it is not the sole acceptance criterion for API correctness.
- `include/`, `src/`, and `tests/` reflect the current implementation state.
- Until the API is stable, no complete per-module API manual is maintained.
- The Lua bindings are implemented per the contract (a few items such as `shield.event` are marked as target contracts; see the per-section implementation snapshots in lua-api.md); the old `shield.service`, colon-style DB/Redis calls, and `on_message(src, type, data)` are deprecated.

## C++ API Goal

The goal is a single entry point:

```cpp
#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);
}
```

This entry point is implemented: both `src/main.cpp` and `examples/hello_world/main.cpp` call `shield::run(argc, argv)` directly.
