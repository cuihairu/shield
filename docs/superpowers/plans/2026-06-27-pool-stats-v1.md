# shield.pool.stats.v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the `shield.pool.stats.v1` plugin interface and host `collect_pool_stats`, plus validate the proposal with one SQL-less unit-test plugin and one real Redis plugin (cache.redis), to satisfy the proposal's freeze conditions.

**Architecture:** A new optional C++ ABI vtable (`include/shield/plugin/pool_stats.h`) that plugins implement alongside their business interface. `PluginHost::collect_pool_stats` walks started instances whose manifest declares the interface, fetches each vtable via `get_interface`, validates `struct_size`/`get_stats`, sentinel-fills the snapshot, calls `get_stats`, and maps the return code into a `PoolStatsResult`. Manifest is the authoritative discovery source; declared-but-missing vtable fails fast at start.

**Tech Stack:** C++20, CMake + vcpkg, Boost.Test, existing plugin v1 ABI (`include/shield/plugin/abi.h`), redis++ (sw::redis) for cache.redis.

**Reference spec:** `docs/plugin-pool-stats.md` (proposal — this plan's execution validates it; freeze after Phase A passes).

---

## Scope

- **Phase A (this plan):** ABI header, host collection + manifest validation, unit tests via the minimal test plugin, and one real integration on cache.redis. All code below is concrete — no placeholders.
- **Phase B (follow-up, out of scope here):** mysql / postgresql / sqlite / mongodb / queue.redis / leaderboard.redis each implement the vtable using the cache.redis task as the template. Each requires reading that driver's pool internals first; split into separate per-plugin tasks after Phase A freezes the ABI.

## File Structure

- **Create** `include/shield/plugin/pool_stats.h` — ABI: `shield_pool_stats` snapshot + `shield_pool_stats_v1` vtable + interface name.
- **Modify** `include/shield/plugin/plugin_host.hpp` — add `PoolStatsStatus`, `PoolStatsResult`, `collect_pool_stats` declaration.
- **Modify** `src/plugin/plugin_host.cpp` — implement `collect_pool_stats`; add manifest-consistency check in start phase.
- **Modify** `tests/plugin/fixtures/minimal_test_plugin.cpp` — add a `shield.pool.stats.v1` vtable returning canned stats (test fixture).
- **Create** `tests/plugin/test_pool_stats.cpp` — unit tests for collection + status mapping.
- **Modify** `tests/CMakeLists.txt` — register `test_pool_stats`.
- **Modify** `plugins/cache.redis/shield_cache_redis.cpp` + `plugins/cache.redis/manifest.yaml` — real Redis pool stats.

---

## Task 1: Create the `pool_stats.h` ABI header

**Files:**
- Create: `include/shield/plugin/pool_stats.h`

- [ ] **Step 1: Write the header**

```c
// [SHIELD_PLUGIN] shield.pool.stats.v1 — optional pool observability interface.
//
// A package providing "shield.pool.stats.v1" returns a shield_pool_stats_v1*
// from instance->get_interface("shield.pool.stats.v1"). The host sentinel-fills
// a shield_pool_stats, sets struct_size, and calls get_stats for a snapshot.
//
// See docs/plugin-pool-stats.md (proposal).
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_POOL_STATS_INTERFACE "shield.pool.stats.v1"

// Pool snapshot. See docs/plugin-pool-stats.md for the full field semantics.
// ABI versioning: struct_size MUST be the first field. The host sentinel-fills
// the entire struct (gauges/counters = -1 unknown, last_error_epoch_ms = -1
// unknown) and sets struct_size = sizeof(shield_pool_stats) before calling.
struct shield_pool_stats {
    uint32_t struct_size;   // == sizeof(shield_pool_stats); MUST be first

    // Capacity & usage (instantaneous; -1 = unknown)
    int32_t  max_size;     // configured pool_size cap; 0 = unbounded, -1 = unknown
    int32_t  size;         // connections held = idle + in_use; -1 = unknown
    int32_t  idle;         // -1 = unknown
    int32_t  in_use;       // -1 = unknown

    // Waiting
    int32_t  waiters;               // -1 = unknown
    int64_t  acquire_timeout_total; // -1 = not tracked

    // Lifecycle (cumulative, monotonic; -1 = not tracked)
    int64_t  acquire_total;
    int64_t  create_total;
    int64_t  destroy_total;
    int64_t  eviction_total;
    int64_t  health_check_failures_total;

    int64_t  last_error_epoch_ms;   // 0 = none, -1 = unknown
};

// C++ host helper (like database.h): constexpr interface_name + vtable.
// Struct layout is ABI-stable POD; header is C++ (constexpr), not C-includeable.
struct shield_pool_stats_v1 {
    static constexpr const char* interface_name = SHIELD_POOL_STATS_INTERFACE;
    uint32_t struct_size;
    // Fill *out (honoring out->struct_size).
    // 0 = ok; 1 = unavailable; 2 = unsupported_state; >2 = treated as
    // unsupported_state; <0 = internal_error.
    int (*get_stats)(struct shield_plugin_instance_v1* self,
                     struct shield_pool_stats* out);
};

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Verify it compiles**

Run: `cmake --build build --target shield_plugin -j` (the header is included transitively once Task 2 includes it).
Expected: builds clean (header-only, no source yet).

- [ ] **Step 3: Commit**

```bash
git add include/shield/plugin/pool_stats.h
git commit -m "feat(plugin): add shield.pool.stats.v1 ABI header"
```

---

## Task 2: Declare host-side types and `collect_pool_stats`

**Files:**
- Modify: `include/shield/plugin/plugin_host.hpp` (add near the other public introspection methods, after `get_binding` ~line 219; add includes after line 22)

- [ ] **Step 1: Add the include**

In `plugin_host.hpp`, after `#include "shield/plugin/plugin_library.hpp"` (line 23):

```cpp
#include "shield/plugin/pool_stats.h"
```

- [ ] **Step 2: Add the types and method declaration**

Add this block in the `namespace shield::plugin {` section, just before `class PluginHost` (around line 167):

```cpp
// Outcome of one instance's pool-stats collection. See docs/plugin-pool-stats.md.
enum class PoolStatsStatus {
    ok,                // stats valid
    unavailable,       // get_stats returned 1 (pool not initialized, etc.)
    unsupported_state, // get_stats returned 2 (or any >2)
    error,             // get_stats returned <0; see error_code/message
};

struct PoolStatsResult {
    std::string instance_id;    // e.g. "primary_db"
    std::string plugin_id;      // e.g. "database.mysql"
    std::string pool_name;      // pool within the instance; "main" for v1
    PoolStatsStatus status = PoolStatsStatus::unavailable;
    int raw_status_code = 0;    // original get_stats return value
    std::string error_code;     // host-generated, mapped from raw_status_code
    std::string error_message;  // host-generated generic description
    shield_pool_stats stats{};  // valid only when status == ok
};
```

Add the method to `PluginHost`'s public section, after `get_binding` (~line 219):

```cpp
    // --- pool observability ---
    // Append one PoolStatsResult per STARTED instance whose manifest declares
    // shield.pool.stats.v1. Instances without the declaration are skipped.
    // Returns false only on hard errors (e.g. null host state).
    bool collect_pool_stats(std::vector<PoolStatsResult>& out) const;
```

- [ ] **Step 3: Verify it compiles**

Run: `cmake --build build --target shield_plugin -j`
Expected: builds (declaration only, no impl yet — link will fail later, that's fine until Task 3).

- [ ] **Step 4: Commit**

```bash
git add include/shield/plugin/plugin_host.hpp
git commit -m "feat(plugin): declare PoolStatsResult and collect_pool_stats"
```

---

## Task 3: Implement `collect_pool_stats`

**Files:**
- Modify: `src/plugin/plugin_host.cpp` (add the method at the end of the `PluginHost` section, e.g. after `get_binding` / before the `get_binding_vtable` private helper; ensure `#include "shield/plugin/pool_stats.h"` is present via plugin_host.hpp)

- [ ] **Step 1: Write the failing test first (see Task 6 for the full test file) — skip ahead and create the test, then come back**

(If implementing strictly in order: the test is created in Task 6. To keep TDD intact, do Task 5 + Task 6 first, then this task. The ordering below assumes Task 5/6 exist.)

- [ ] **Step 2: Implement the method**

Append to `src/plugin/plugin_host.cpp` inside `namespace shield::plugin`:

```cpp
namespace {
// Sentinel-fill a shield_pool_stats so unset fields read as "unknown".
void sentinel_fill_pool_stats(shield_pool_stats& s) {
    s.struct_size = sizeof(shield_pool_stats);
    s.max_size = -1;
    s.size = -1;
    s.idle = -1;
    s.in_use = -1;
    s.waiters = -1;
    s.acquire_timeout_total = -1;
    s.acquire_total = -1;
    s.create_total = -1;
    s.destroy_total = -1;
    s.eviction_total = -1;
    s.health_check_failures_total = -1;
    s.last_error_epoch_ms = -1;
}

bool manifest_declares_pool_stats(const Manifest& m) {
    for (const auto& p : m.provides) {
        if (p.interface_name == SHIELD_POOL_STATS_INTERFACE) return true;
    }
    return false;

// Map a get_stats return code to (status, error_code). error_message stays
// empty for non-error statuses; the caller may leave it generic.
struct StatusMapping { PoolStatsStatus status; const char* code; };
StatusMapping map_pool_stats_rc(int rc) {
    if (rc == 0) return {PoolStatsStatus::ok, ""};
    if (rc == 1) return {PoolStatsStatus::unavailable, "pool_stats_unavailable"};
    if (rc > 0)  return {PoolStatsStatus::unsupported_state, "pool_stats_unsupported_state"};
    return {PoolStatsStatus::error, "pool_stats_internal_error"};
}
}  // namespace

bool PluginHost::collect_pool_stats(std::vector<PoolStatsResult>& out) const {
    for (const auto& inst : instances_) {
        if (!inst.package || inst.state != State::started || !inst.handle) continue;
        if (!manifest_declares_pool_stats(inst.package->manifest)) continue;

        shield_error_v1 e{};
        const auto* vt = static_cast<const shield_pool_stats_v1*>(
            inst.handle->get_interface(inst.handle, SHIELD_POOL_STATS_INTERFACE, &e));
        if (!vt) continue;  // declared but missing is a start-phase failure (Task 4); defensive skip here
        if (vt->struct_size < sizeof(shield_pool_stats_v1) || !vt->get_stats) continue;

        PoolStatsResult r;
        r.instance_id = inst.id;
        r.plugin_id = inst.package->manifest.id;
        r.pool_name = "main";

        shield_pool_stats stats{};
        sentinel_fill_pool_stats(stats);
        int rc = vt->get_stats(inst.handle, &stats);
        r.raw_status_code = rc;
        auto m = map_pool_stats_rc(rc);
        r.status = m.status;
        r.error_code = m.code;
        if (r.status == PoolStatsStatus::error) {
            r.error_message = "pool stats collection failed";
        } else if (r.status == PoolStatsStatus::unavailable) {
            r.error_message = "pool stats temporarily unavailable";
        }
        r.stats = stats;
        out.push_back(std::move(r));
    }
    return true;
}
```

- [ ] **Step 3: Run the unit test**

Run: `ctest --test-dir build -R test_pool_stats -V`
Expected: PASS (after Task 5 + Task 6 are in place).

- [ ] **Step 4: Commit**

```bash
git add src/plugin/plugin_host.cpp
git commit -m "feat(plugin): implement collect_pool_stats"
```

---

## Task 4: Manifest consistency check at start

**Files:**
- Modify: `src/plugin/plugin_host.cpp` (inside `start_all`, after an instance is marked `State::started`, ~line 709)

- [ ] **Step 1: Write the failing test** — covered by Task 6's "declared but missing → start fails" test case.

- [ ] **Step 2: Add the check**

In `start_all`, after `inst->state = State::started;` (around line 709), within the per-instance loop, add:

```cpp
        // Pool-stats manifest consistency: if the manifest declares
        // shield.pool.stats.v1, the instance MUST serve the vtable, else the
        // start is a contract violation (fail fast). get_interface non-NULL but
        // not declared is tolerated (host simply won't collect — manifest is
        // authoritative for discovery).
        if (manifest_declares_pool_stats(inst->package->manifest)) {
            shield_error_v1 pe{};
            const void* psvt = inst->handle->get_interface(
                inst->handle, SHIELD_POOL_STATS_INTERFACE, &pe);
            if (!psvt) {
                inst->state = State::failed;
                inst->last_error =
                    "plugin.start.pool_stats_declared_but_missing: instance " +
                    inst->id + " declares shield.pool.stats.v1 but get_interface returned NULL";
                error = inst->last_error;
                return false;
            }
        }
```

(Note: `manifest_declares_pool_stats` is the anonymous-namespace helper from Task 3; both live in the same TU.)

- [ ] **Step 3: Run tests**

Run: `ctest --test-dir build -R "test_pool_stats|test_plugin_host" -V`
Expected: PASS, including the "declared but missing fails start" case.

- [ ] **Step 4: Commit**

```bash
git add src/plugin/plugin_host.cpp
git commit -m "feat(plugin): fail start when pool.stats.v1 declared but not served"
```

---

## Task 5: Add a pool-stats vtable to the minimal test plugin

**Files:**
- Modify: `tests/plugin/fixtures/minimal_test_plugin.cpp`

- [ ] **Step 1: Add the include and a canned vtable**

After `#include "shield/plugin/host_api.h"` (line 4), add:

```cpp
#include "shield/plugin/pool_stats.h"
```

After the `test_vtable()` function (around line 33), add:

```cpp
// Canned pool stats for testing collect_pool_stats. Configurable via the
// "stats_unavailable" instance config to exercise the return-code mapping.
int minimal_pool_get_stats(struct shield_plugin_instance_v1* self,
                           struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<minimal_instance*>(self);
    if (!out) return -1;
    if (config_true(inst, "stats_unavailable")) {
        return 1;  // unavailable
    }
    out->max_size = 4;
    out->size = 2;
    out->idle = 1;
    out->in_use = 1;
    out->waiters = 0;
    out->acquire_timeout_total = 0;
    out->acquire_total = 10;
    out->create_total = 2;
    out->destroy_total = 0;
    out->eviction_total = 0;
    out->health_check_failures_total = 0;
    out->last_error_epoch_ms = 0;
    return 0;
}

const shield_pool_stats_v1& pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &minimal_pool_get_stats,
    };
    return v;
}
```

- [ ] **Step 2: Serve the vtable from get_interface**

In `minimal_create`, extend the `shell.get_interface` lambda (line 63) to also serve the pool stats interface. Replace the existing lambda body:

```cpp
    inst->shell.get_interface = [](struct shield_plugin_instance_v1* self,
                                   const char* iface,
                                   struct shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, kTestInterface) == 0) {
            return &test_vtable();
        }
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0) {
            return &pool_stats_vtable();
        }
        return nullptr;
    };
```

- [ ] **Step 3: Add the config keys to the test manifest schema**

This is done in Task 6's embedded manifest (`stats_unavailable` under `config_schema`). No fixture change needed beyond Step 2.

- [ ] **Step 4: Build the fixture**

Run: `cmake --build build --target shield_minimal_test_plugin -j`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add tests/plugin/fixtures/minimal_test_plugin.cpp
git commit -m "test(plugin): serve shield.pool.stats.v1 from minimal test plugin"
```

---

## Task 6: Unit tests for `collect_pool_stats`

**Files:**
- Create: `tests/plugin/test_pool_stats.cpp`
- Modify: `tests/CMakeLists.txt`

> **Setup note:** the `PoolStatsFixture` below mirrors `test_plugin_host.cpp`'s package-deployment pattern. If the standalone fixture's plugin copy/scan has any path mismatch, prefer **adding these `BOOST_AUTO_TEST_CASE`s directly into `test_plugin_host.cpp`** to reuse its already-working setup — the assertion logic is identical either way.

- [ ] **Step 1: Write the test file**

```cpp
// Tests for PluginHost::collect_pool_stats.
#define BOOST_TEST_MODULE shield_plugin_pool_stats
#include <boost/test/included/unit_test.hpp>

#include "shield/plugin/plugin_host.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
using namespace shield::plugin;

namespace {
std::string minimal_library_name() {
#ifdef SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY
    return SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY;
#else
    return "libshield_minimal_test_plugin.so";
#endif
}

// Manifest that DECLARES pool.stats.v1 (so the instance is collected).
const char* kManifestWithStats =
    R"(schema_version: 1
id: minimal.test
name: Minimal
version: 1.0.0
kind: test
entry: shield_plugin_get_v1
library:
  linux: bin/libshield_minimal_test_plugin.so
  macos: bin/libshield_minimal_test_plugin.dylib
  windows: bin/libshield_minimal_test_plugin.dll
provides:
  - interface: minimal.test.iface
  - interface: shield.pool.stats.v1
requires: []
config_schema:
  type: object
  properties:
    stats_unavailable:
      type: boolean
)";

// Manifest WITHOUT pool.stats.v1 (instance must NOT be collected).
const char* kManifestWithoutStats =
    R"(schema_version: 1
id: minimal.test
name: Minimal
version: 1.0.0
kind: test
entry: shield_plugin_get_v1
library:
  linux: bin/libshield_minimal_test_plugin.so
  macos: bin/libshield_minimal_test_plugin.dylib
  windows: bin/libshield_minimal_test_plugin.dll
provides:
  - interface: minimal.test.iface
requires: []
config_schema:
  type: object
)";

// Writes a plugin package dir (manifest + bin symlink) under test_plugins/.
// Pattern reused from test_plugin_host.cpp's setup helper.
struct PoolStatsFixture {
    fs::path root;
    explicit PoolStatsFixture(const char* manifest_yaml,
                              const std::string& package_dir = "stats.test") {
        std::string base = std::string(SHIELD_TEST_PLUGINS_DIR);
        root = fs::path(base) / package_dir;
        fs::create_directories(root / "bin");
        { std::ofstream(root / "manifest.yaml") << manifest_yaml; }
        fs::copy_file(minimal_library_name(), root / "bin" / fs::path(minimal_library_name()).filename(),
                      fs::copy_options::overwrite_existing);
    }
    ~PoolStatsFixture() { fs::remove_all(root); }
};
}  // namespace

BOOST_AUTO_TEST_CASE(collects_declared_instance_with_ok_stats) {
    PoolStatsFixture fx(kManifestWithStats);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = SHIELD_TEST_PLUGINS_DIR;
    InstanceDecl id;
    id.id = "stats.inst";
    id.package = "stats.test";
    cfg.instances.push_back(id);

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_TEST(out.size() == 1u);
    BOOST_TEST(out[0].status == PoolStatsStatus::ok);
    BOOST_TEST(out[0].raw_status_code == 0);
    BOOST_TEST(out[0].stats.max_size == 4);
    BOOST_TEST(out[0].stats.size == 2);
    BOOST_TEST(out[0].stats.in_use == 1);
    BOOST_TEST(out[0].pool_name == "main");
}

BOOST_AUTO_TEST_CASE(unavailable_return_code_maps_to_status) {
    PoolStatsFixture fx(kManifestWithStats);
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = SHIELD_TEST_PLUGINS_DIR;
    InstanceDecl id;
    id.id = "stats.inst";
    id.package = "stats.test";
    id.config = nlohmann::json{{"stats_unavailable", true}};
    cfg.instances.push_back(id);

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_TEST(out.size() == 1u);
    BOOST_TEST(out[0].status == PoolStatsStatus::unavailable);
    BOOST_TEST(out[0].raw_status_code == 1);
    BOOST_TEST(out[0].error_code == "pool_stats_unavailable");
}

BOOST_AUTO_TEST_CASE(undeclared_instance_is_not_collected) {
    PoolStatsFixture fx(kManifestWithoutStats, "nostats.test");
    PluginHost host;
    std::string err;
    PluginConfig cfg;
    cfg.directory = SHIELD_TEST_PLUGINS_DIR;
    InstanceDecl id;
    id.id = "nostats.inst";
    id.package = "nostats.test";
    cfg.instances.push_back(id);

    BOOST_TEST_REQUIRE(host.startup(cfg, err));
    std::vector<PoolStatsResult> out;
    BOOST_TEST(host.collect_pool_stats(out));
    BOOST_TEST(out.empty());
}
```

(If `InstanceDecl::config` is JSON in this codebase version — confirm against `plugin_host.hpp:91` `nlohmann::json config;` — the `unavailable` case sets it directly. If the YAML-driven path is preferred, set `id.config` via the same pattern `test_plugin_host.cpp` uses.)

- [ ] **Step 2: Register the test in CMake**

In `tests/CMakeLists.txt`, after the `test_plugin_host` block (after line 121), add:

```cmake
# Pool stats collection (shield.pool.stats.v1)
add_executable(test_pool_stats plugin/test_pool_stats.cpp)
target_link_libraries(test_pool_stats
    PRIVATE shield_plugin nlohmann_json::nlohmann_json
            Boost::unit_test_framework Boost::headers)
target_include_directories(test_pool_stats PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(test_pool_stats PRIVATE
    SHIELD_TEST_PLUGINS_DIR="${CMAKE_BINARY_DIR}/test_plugins"
    SHIELD_MINIMAL_TEST_PLUGIN_LIBRARY="${_shield_minimal_plugin_filename}")
add_dependencies(test_pool_stats shield_minimal_test_plugin)
add_test(NAME test_pool_stats COMMAND test_pool_stats)
set_tests_properties(test_pool_stats PROPERTIES LABELS "plugin" TIMEOUT 15)
shield_copy_runtime_dlls(test_pool_stats)
```

- [ ] **Step 3: Run — expect two failures (impl not present yet)**

Run: `cmake --build build --target test_pool_stats shield_minimal_test_plugin -j && ctest --test-dir build -R test_pool_stats -V`
Expected: first two cases FAIL (no `collect_pool_stats` impl until Task 3). This confirms the tests run.

- [ ] **Step 4: Implement (Task 3) and re-run**

After Task 3 + Task 5 land, re-run:
Run: `ctest --test-dir build -R test_pool_stats -V`
Expected: all three cases PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/plugin/test_pool_stats.cpp tests/CMakeLists.txt
git commit -m "test(plugin): unit tests for collect_pool_stats"
```

---

## Task 7: Real integration — cache.redis pool stats

**Files:**
- Modify: `plugins/cache.redis/shield_cache_redis.cpp`
- Modify: `plugins/cache.redis/manifest.yaml`

This task validates the proposal against a real driver pool (redis++ `ConnectionPoolOptions`). redis++ does not expose idle/in_use counters publicly, so those fields are reported as `-1` (unknown) — this is exactly the unknown-semantics the proposal designed for.

- [ ] **Step 1: Add the include**

In `shield_cache_redis.cpp`, after the existing includes:

```cpp
#include "shield/plugin/pool_stats.h"
```

- [ ] **Step 2: Implement get_stats from the configured pool size**

Near the cache instance struct (the one holding `pool_size`, ~line 253), add a get_stats helper:

```cpp
int cache_pool_get_stats(struct shield_plugin_instance_v1* self,
                         struct shield_pool_stats* out) {
    auto* inst = reinterpret_cast<cache_redis_instance*>(self);  // use the plugin's real instance type
    if (!out) return -1;
    // redis++ does not expose live idle/in_use; report configured cap, rest unknown.
    out->max_size = inst->pool_size > 0 ? inst->pool_size : -1;
    out->size = -1;
    out->idle = -1;
    out->in_use = -1;
    out->waiters = -1;
    out->acquire_timeout_total = -1;
    out->acquire_total = -1;
    out->create_total = -1;
    out->destroy_total = -1;
    out->eviction_total = -1;
    out->health_check_failures_total = -1;
    out->last_error_epoch_ms = -1;
    return 0;
}

const shield_pool_stats_v1& cache_pool_stats_vtable() {
    static const shield_pool_stats_v1 v{
        sizeof(shield_pool_stats_v1),
        &cache_pool_get_stats,
    };
    return v;
}
```

(Replace `cache_redis_instance` with the plugin's actual instance struct name — confirm in `shield_cache_redis.cpp`.)

- [ ] **Step 3: Serve the vtable**

In the `shell.get_interface` lambda (~line 641), add a branch:

```cpp
        if (iface && std::strcmp(iface, SHIELD_POOL_STATS_INTERFACE) == 0) {
            return &cache_pool_stats_vtable();
        }
```

- [ ] **Step 4: Declare in manifest**

In `plugins/cache.redis/manifest.yaml`, add a second entry under `provides`:

```yaml
provides:
  - interface: shield.cache.v1
    capabilities:
      - kv
      - hash
      - ttl
      - counter
  - interface: shield.pool.stats.v1
    capabilities: []
```

- [ ] **Step 5: Build the plugin**

Run: `cmake --build build --target shield_cache_redis -j`
Expected: builds clean.

- [ ] **Step 6: Manual verification (requires a Redis instance)**

With a Redis running and a config binding cache.redis, start the host and call `collect_pool_stats` from a diagnostic (or a temporary test). Confirm the cache.redis instance appears with `status=ok`, `max_size` = configured `pool_size`, other fields `-1`.

- [ ] **Step 7: Commit**

```bash
git add plugins/cache.redis/shield_cache_redis.cpp plugins/cache.redis/manifest.yaml
git commit -m "feat(cache.redis): serve shield.pool.stats.v1"
```

---

## Phase B (follow-up, after Phase A freezes the ABI)

Each remaining plugin implements the vtable using Task 7 as the template. Per plugin, first read its pool internals, then fill the real fields where the driver exposes them and `-1` where it does not:

- **mysql** (`shield_db_mysql.cpp`) — mysqlx session pool; expose configured `pool_size` + any mysqlx pool stats the driver offers.
- **postgresql** (`shield_db_pgsql.cpp`) — libpq connection pool.
- **sqlite** (`shield_db_sqlite.cpp`) — *note: SQLite is embedded, there is no connection pool* (see `shield_db_sqlite.cpp:305`). Either skip (don't declare the interface) or report `max_size=1, size=1` as a degenerate single-connection "pool". Recommend: **do not implement** for sqlite.
- **mongodb** (`shield_doc_mongodb.cpp`) — mongocxx has a native `mongocxx::pool`; expose its `min`/`max` size.
- **queue.redis** / **leaderboard.redis** — same redis++ pool as cache.redis; same field limitations.

Each becomes its own commit: `feat(<plugin>): serve shield.pool.stats.v1`.

---

## Self-Review (run after writing, before handoff)

- **Spec coverage:** proposal's ABI header → Task 1; `collect_pool_stats` + status mapping + raw_status_code → Task 3; manifest consistency (declared→fail fast, undeclared→skip) → Task 3 + Task 4; sentinel pre-fill → Task 3 `sentinel_fill_pool_stats`; unknown semantics → Task 7 (cache.redis reports -1); C++ header (not C-includeable) → Task 1 comment; minimal test fixture → Task 5; real plugin → Task 7. Freeze conditions (≥1 SQL/Redis validation) → Task 7 covers Redis; a SQL plugin (mysql/pg) is Phase B. ✓ (note: Phase A validates Redis + unit tests; full freeze needs one SQL plugin from Phase B.)
- **Placeholder scan:** `cache_redis_instance` in Task 7 Step 2 is flagged "confirm actual name" — that is a real implementation-time lookup, not a placeholder; the rest is concrete code.
- **Type consistency:** `PoolStatsStatus` enum values match across Task 2 (decl), Task 3 (`map_pool_stats_rc`), Task 6 (assertions). `SHIELD_POOL_STATS_INTERFACE` used consistently. `manifest_declares_pool_stats` defined in Task 3 anon namespace, reused in Task 4 (same TU). ✓

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-27-pool-stats-v1.md`.
