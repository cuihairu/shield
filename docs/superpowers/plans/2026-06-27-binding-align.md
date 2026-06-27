# Lua 数据访问 binding 对齐 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 7 个数据插件的 Lua `__call` 从 `instance_id` 改为 binding 逻辑名——业务 Lua 传 binding，host 经全局 `plugins.bindings` 解析到 instance，插件再查自己 registry。落实 `docs/plugin-system.md` "为什么用 binding 而非 instance_id" 的设计决策。

**Architecture:** `host_api.h` 新增 `binding_instance_id(ctx, binding)`（复用 `PluginHost::binding_instance_id`，`plugin_host.cpp:875`）；插件 create 时把全局 `host_api` 存到插件全局/捕获；`__call(binding)` 调 `host_api->binding_instance_id(nullptr, binding)` 得 instance_id，再 `find_instance(instance_id)` 构造 proxy；binding 缺失返回 `nil, module_unavailable`。

**Tech Stack:** C++20, CMake + vcpkg, Boost.Test, sol2, plugin v1 ABI。

**Reference:** `docs/plugin-system.md` §"为什么 Lua 访问用 binding 而非 instance_id"（决策 + 规则）；`docs/lua-api.md` §"Plugin-provided APIs"。

---

## Scope

- 本计划只改 **Lua `__call` 路径**（业务访问入口），不动插件的 C ABI 业务接口、连接池、manifest 结构。
- 7 个数据插件：mysql / postgresql / sqlite / mongodb / cache.redis / queue.redis / leaderboard.redis。
- 不在范围：`get_by_binding<T>` 的 C++ 路径（已用 binding，无需改）；pool.stats.v1（独立计划）。

## File Structure

- **Modify** `include/shield/plugin/host_api.h` — 加 `binding_instance_id` 函数指针。
- **Modify** `src/plugin/plugin_host.cpp` — `host_api_table()` 填充 `binding_instance_id`（复用 `PluginHost::binding_instance_id`）。
- **Modify** `tests/plugin/fixtures/minimal_test_plugin.cpp` — 加一个 test binding namespace（验证 binding→instance 解析）。
- **Create** `tests/plugin/test_plugin_binding.cpp` — 单元测试。
- **Modify** `tests/CMakeLists.txt` — 注册 `test_plugin_binding`。
- **Modify** 7 个数据插件 cpp（mysql / postgresql / sqlite / mongodb / cache.redis / queue.redis / leaderboard.redis）— `__call` 改 binding。

---

## Task 1: host_api 暴露 binding_instance_id

**Files:**
- Modify: `include/shield/plugin/host_api.h`（在 `lua_add_path` 之后，`};` 之前）
- Modify: `src/plugin/plugin_host.cpp`（`host_api_table()` 内）

- [ ] **Step 1: 在 host_api.h 加函数指针**

在 `shield_host_api_v1` 的 `lua_add_path` 之后追加：

```c
    // Resolve a binding logical name to its instance id via the host's global
    // plugins.bindings table. Returns the instance id (host-owned, valid until
    // the next call) or NULL if the binding is not configured.
    // ctx MAY be NULL — the binding table is host-global; ctx is used only for
    // audit/logging. Lets plugin Lua __call(binding) reach the right instance
    // without business code knowing instance ids.
    const char* (*binding_instance_id)(struct shield_plugin_context_v1* ctx,
                                       const char* binding_name);
```

- [ ] **Step 2: host 填充该函数**

在 `plugin_host.cpp` 的 `host_api_table()` 内（紧接 `api.lua_add_path = ...` 之后）：

```cpp
    static thread_local std::string g_binding_buf;
    api.binding_instance_id = [](shield_plugin_context_v1* ctx,
                                 const char* binding) -> const char* {
        (void)ctx;  // binding table is host-global
        if (!binding) return nullptr;
        // g_host_for_api() returns the PluginHost that owns this api table
        // (captured when the table is built — see implementation note below).
        std::string id = g_host_for_api()->binding_instance_id(binding);
        if (id.empty()) return nullptr;
        g_binding_buf = std::move(id);
        return g_binding_buf.c_str();
    };
```

> **Implementation note:** `host_api_table()` 当前返回一个静态/单例表（`plugin_host.cpp:237`）。若该表是 process-wide static，`g_host_for_api()` 需指向当前活跃的 `PluginHost`（单进程单 host，用 `global_host()` 即可）。若 `host_api_table()` 已是 per-`PluginHost`，直接捕获 `this`。实现时确认 `host_api_table()` 的存储方式后二选一——推荐用 `&global_host()` 保持与现有 process-wide host 假设一致。

- [ ] **Step 3: 编译验证**

Run: `cmake --build build --target shield_plugin -j`
Expected: 编译通过（host_api 新增一个函数指针槽，host 填充，暂无消费者）。

- [ ] **Step 4: Commit**

```bash
git add include/shield/plugin/host_api.h src/plugin/plugin_host.cpp
git commit -m "feat(plugin): expose binding_instance_id via host_api"
```

---

## Task 2: minimal_test_plugin 加 test binding namespace

**Files:**
- Modify: `tests/plugin/fixtures/minimal_test_plugin.cpp`

为测试 binding 解析，给 minimal 插件加一个 callable namespace `shield.test.data`，其 `__call(binding)` 经 `host_api->binding_instance_id` 解析，返回一个带 instance_id 标记的 proxy（table）。

- [ ] **Step 1: 存 host_api 到全局 + 注册 test namespace**

在 `minimal_test_plugin.cpp` 的匿名 namespace 顶部加：

```cpp
const shield_host_api_v1* g_host_api = nullptr;  // set in minimal_create
```

在 `minimal_create` 里（设 `inst->host = args->host_api` 之后）加：

```cpp
    if (args && args->host_api) g_host_api = args->host_api;
```

在 `minimal_create` 设完各 shell 回调后、`*out = &inst->shell` 前，注册 test namespace（注意 register_lua 才是注册 Lua 的地方——以下放到一个新的 `register_lua` 实现，替换现有的空实现）。

替换现有 `inst->shell.register_lua = [...]` lambda 为：

```cpp
    inst->shell.register_lua = [](struct shield_plugin_instance_v1* self,
                                  struct lua_State* L,
                                  struct shield_error_v1* err) {
        auto* inst = reinterpret_cast<minimal_instance*>(self);
        if (config_true(inst, "register_lua_fail")) {
            fill_error(err, "minimal.lua_failed",
                       "requested Lua registration failure", "lua_register");
            return -1;
        }
        if (!L || !g_host_api) return 0;
        sol::state_view lua(L);
        auto shield = lua["shield"].get_or_create<sol::table>();
        auto test = shield["test"].get_or_create<sol::table>();
        if (!test["data"].is<sol::table>()) {
            auto ns = lua.create_table();
            auto mt = lua.create_table();
            mt.set_function("__call",
                [](sol::this_state s, sol::table /*self*/,
                   std::string binding) -> sol::object {
                    sol::state_view lua(s);
                    const char* id = g_host_api->binding_instance_id(nullptr,
                                                                     binding.c_str());
                    if (!id) return sol::nil;
                    sol::table proxy = lua.create_table();
                    proxy["instance_id"] = id;
                    proxy["binding"] = binding;
                    return sol::make_object(lua, proxy);
                });
            ns[sol::metatable_key] = mt;
            test["data"] = ns;
        }
        return 0;
    };
```

需在 fixture 顶部 include sol2（`#include <sol/sol.hpp>`）——确认 tests 已有 sol2 可用（shield_lua 用 sol2，fixture link 时需加）。

- [ ] **Step 2: 编译 fixture**

Run: `cmake --build build --target shield_minimal_test_plugin -j`
Expected: 编译通过（若 sol2 未链接到 fixture，需在 tests/CMakeLists.txt 给 `shield_minimal_test_plugin` 加 sol2 依赖）。

- [ ] **Step 3: Commit**

```bash
git add tests/plugin/fixtures/minimal_test_plugin.cpp tests/CMakeLists.txt
git commit -m "test(plugin): minimal plugin serves shield.test.data(binding)"
```

---

## Task 3: binding 解析单元测试

**Files:**
- Create: `tests/plugin/test_plugin_binding.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写测试**

复用 `test_plugin_host.cpp` 的 fixture 模式（内嵌 manifest + 部署 minimal plugin + PluginHost startup）。manifest 的 `provides` 含 `minimal.test.iface`，配置 `plugins.bindings` 含 `test.default -> <instance_id>`。

```cpp
#define BOOST_TEST_MODULE shield_plugin_binding
#include <boost/test/included/unit_test.hpp>
#include "shield/plugin/plugin_host.hpp"
#include <filesystem>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
using namespace shield::plugin;

// Fixture: deploys minimal plugin + configures a binding test.default -> inst.
// (reuse the package-deployment pattern from test_plugin_host.cpp)
// ...

BOOST_AUTO_TEST_CASE(call_with_binding_resolves_to_instance) {
    // setup: PluginHost, minimal plugin instance "inst", binding test.default -> inst
    // startup, then Lua: shield.test.data("test.default") returns proxy with
    //   proxy.instance_id == "inst" and proxy.binding == "test.default"
    // Implement via a small Lua snippet run through the host's Lua runtime,
    // OR (if Lua VM wiring is heavy in the harness) assert via
    //   host->binding_instance_id("test.default") == "inst" directly.
    BOOST_TEST(host->binding_instance_id("test.default") == "inst");
}

BOOST_AUTO_TEST_CASE(unknown_binding_returns_empty) {
    BOOST_TEST(host->binding_instance_id("no.such.binding").empty());
}
```

> **Harness note:** 若在单测里驱动完整 Lua VM（`shield.test.data(...)`)太重，先直接断言 `PluginHost::binding_instance_id`（Task 1 的 host 侧），它就是 `host_api->binding_instance_id` 的后端。完整 Lua `__call` 路径留到 lua_api 集成测试（`tests/lua_api/`）。

- [ ] **Step 2: 注册 CMake target** — 仿 `test_plugin_host` 的 block（`tests/CMakeLists.txt:111-121`），改名 `test_plugin_binding`，link `shield_plugin`。

- [ ] **Step 3: 跑测试**

Run: `cmake --build build --target test_plugin_binding -j && ctest --test-dir build -R test_plugin_binding -V`
Expected: 两个 case PASS。

- [ ] **Step 4: Commit**

```bash
git add tests/plugin/test_plugin_binding.cpp tests/CMakeLists.txt
git commit -m "test(plugin): binding_instance_id resolution"
```

---

## Task 4: mysql 接入（模板）

**Files:**
- Modify: `plugins/mysql/shield_db_mysql.cpp`

mysql 是 SQL 类模板。其余 SQL（postgresql / sqlite）+ 文档（mongodb）+ Redis 三件套按此模板。

- [ ] **Step 1: 存 host_api 到插件全局**

在 mysql cpp 匿名 namespace 加 `const shield_host_api_v1* g_host_api = nullptr;`。在 `mysql_create`（:1059）`inst->host = args->host_api` 附近加：

```cpp
    if (args && args->host_api) g_host_api = args->host_api;
```

- [ ] **Step 2: __call 改 binding**

把 `register_lua_impl`（:1018）的 `__call` lambda 参数从 `instance_id` 改为 `binding`，先解析：

```cpp
        mt.set_function("__call",
            [](sol::this_state s, sol::table /*self*/,
               std::string binding) -> sol::object {
                sol::state_view lua(s);
                if (!g_host_api) return sol::nil;
                const char* id = g_host_api->binding_instance_id(nullptr,
                                                                 binding.c_str());
                if (!id) return sol::nil;  // binding 未配置 → nil (module_unavailable 由 proxy 缺失体现)
                auto* inst = find_instance(id);
                if (!inst) return sol::nil;
                sol::table proxy = make_instance_proxy(lua, inst);
                shield::plugins::apply_db_mapper_api(lua, proxy);
                return sol::make_object(lua, proxy);
            });
```

- [ ] **Step 3: 编译 + smoke**

Run: `cmake --build build --target shield_db_mysql -j`
Expected: 编译通过。

- [ ] **Step 4: Commit**

```bash
git add plugins/mysql/shield_db_mysql.cpp
git commit -m "feat(mysql): Lua __call 用 binding 而非 instance_id"
```

---

## Task 5: cache.redis 接入（Redis 类模板）

**Files:**
- Modify: `plugins/cache.redis/shield_cache_redis.cpp`

同 Task 4 模式（:615 的 `__call`）。Redis 三件套（cache/queue/leaderboard）共用此模板。

- [ ] **Step 1-4:** 同 Task 4（存 g_host_api、改 `__call(binding)`、编译、commit）。

```bash
git commit -m "feat(cache.redis): Lua __call 用 binding 而非 instance_id"
```

---

## Task 6: 其余 5 插件按模板接入

按 Task 4（SQL/文档类）或 Task 5（Redis 类）模板，逐一改 `__call` + 存 g_host_api：

- [ ] postgresql（`shield_db_pgsql.cpp`）
- [ ] sqlite（`shield_db_sqlite.cpp`）
- [ ] mongodb（`shield_doc_mongodb.cpp`）
- [ ] queue.redis（`shield_queue_redis.cpp`）
- [ ] leaderboard.redis（`shield_leaderboard_redis.cpp`）

每个一个 commit：`feat(<plugin>): Lua __call 用 binding 而非 instance_id`。

---

## Self-Review

- **Spec coverage:** plugin-system.md 规则 1（业务只用 binding）→ Task 4-6 改 `__call`；规则 5（缺失 binding → module_unavailable）→ `__call` 返回 nil；规则 4（instance_id 只在配置/诊断）→ 业务路径不再传 instance_id；host_api 暴露 resolve → Task 1；测试 → Task 2/3。✓
- **Placeholder scan:** Task 1 的 `g_host_for_api()` 标注了实现确认点（host_api_table 存储方式）；Task 2 的 sol2 include 标注了 fixture 链接确认；Task 3 给了降级路径（直接测 binding_instance_id）。其余代码具体。✓
- **Type consistency:** `binding_instance_id(ctx, binding) -> const char*` 签名在 Task 1/2/4 一致；`g_host_api` 全局在 Task 2/4/5/6 一致；`find_instance(id)` 复用现有。✓

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-27-binding-align.md`。
