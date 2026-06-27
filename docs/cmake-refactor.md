# CMake 重构策略

> 状态：**历史记录——重构已完成，不再是待执行计划。**
>
> 本文原描述的是 target 拆分方向。该重构已 100% 落地：当前 `CMakeLists.txt` 已按本文拆分为 `shield_base`/`shield_log`/`shield_config`/`shield_core`/`shield_data`/`shield_transport`/`shield_net`/`shield_lua`/`shield_bootstrap`/`shield` 等 target，旧 discovery/metrics/health/DI 等模块已删除（见 [路线图](roadmap.md)）。本文保留作为 target 结构与依赖约束的参考。

本文定义重构后的 CMake target 拆分方向。

## 目标

- `shield_core` 只编译 service、message、timer、coroutine 和 CAF adapter。
- Lua、net、data、config、log、bootstrap 都是独立 target。
- discovery、metrics、health、DI、annotations、conditions、plugin、middleware 旧模块直接删除，不保留兼容 target。
- optional 官方模块单独 target，默认不链接进最小 runtime。

## Target 列表

| Target | 内容 | 允许依赖 |
| --- | --- | --- |
| `shield_base` | Result、Error、ByteBuffer、time、id 类型 | 标准库 |
| `shield_log` | 日志 facade 和 sink | `shield_base` |
| `shield_config` | YAML 加载、schema 验证 | `shield_base`, `shield_log`, `yaml-cpp` |
| `shield_core` | service registry、message router、timer、CAF adapter | `shield_base`, CAF |
| `shield_data` | raw DB/Redis access | `shield_base`, `shield_log`, `shield_config`, DB/Redis libs |
| `shield_transport` | frame/codec/encryption adapter | `shield_base`, `shield_log` |
| `shield_net` | listener、session、connection 管理 | `shield_base`, `shield_log`, `shield_config`, `shield_transport`, Asio/Beast |
| `shield_lua` | Lua VM、Lua API binding、Lua service loader | `shield_base`, `shield_log`, `shield_config`, `shield_core`, `shield_data`, `shield_net`, Lua/sol2 |
| `shield_bootstrap` | Starter、`shield::run` | selected runtime modules |
| `shield` | CLI/runtime executable | `shield_bootstrap` |

Optional:

| Target | 默认 | 说明 |
| --- | --- | --- |
| `shield_cluster` | OFF | 跨进程/多机器通信 |
| `shield_global` | OFF | Redis-based global helpers |
| `shield_ops` | OFF | diagnostics、metrics、health、console、profile |

## 禁止依赖

`shield_core` 禁止依赖：

- Lua / sol2。
- Boost.Asio / Beast。
- Redis / MySQL / PostgreSQL。
- yaml-cpp。
- log implementation。
- HTTP、gateway、ops、cluster、global。
- discovery、metrics、health、DI、annotations、conditions、plugin。

`shield_lua` 可以依赖 `shield_core`，但 `shield_core` 不能反向依赖 `shield_lua`。

`shield_bootstrap` 可以组合模块，但模块不能反向依赖 `shield_bootstrap`。

## 迁移步骤

1. 新建 `src/shield_base` 并迁移基础类型。
2. 新建 `src/shield_core`，只迁移 service handle、registry、message envelope、timer 和 CAF adapter。
3. 从当前 `shield_core` target 中移除 log/config/CLI/net/data/script/gateway/protocol/serialization。
4. 新建 `shield_lua`，迁移 Lua VM、Lua service loader、Lua API binding。
5. 新建 `shield_net` 和 `shield_transport`，合并旧 `protocol/http/gateway` 中仍需要的字节流和 session 代码。
6. 新建 `shield_data`，只保留 raw DB/Redis 能力，删除 ORM/mapper/cache policy 代码。
7. 新建 `shield_bootstrap`，迁移 Starter 和 `shield::run(argc, argv)`。
8. 删除旧模块 target 和源码：DI、annotations、conditions、events、discovery、metrics、health、plugin、middleware。
9. 添加 CMake dependency check，禁止 `shield_core` 链接上层库。
10. 添加 include boundary check，禁止 public API 暴露 CAF。

## CMake 目标形态

```cmake
add_library(shield_base STATIC ...)
add_library(shield_log STATIC ...)
add_library(shield_config STATIC ...)
add_library(shield_core STATIC ...)
add_library(shield_data STATIC ...)
add_library(shield_transport STATIC ...)
add_library(shield_net STATIC ...)
add_library(shield_lua STATIC ...)
add_library(shield_bootstrap STATIC ...)

target_link_libraries(shield_core
  PUBLIC shield_base
  PRIVATE CAF::core
)

target_link_libraries(shield_lua
  PUBLIC shield_base shield_core
  PRIVATE shield_log shield_config shield_data shield_net ${LUA_LIBRARIES}
)

target_link_libraries(shield_bootstrap
  PUBLIC shield_base
  PRIVATE shield_log shield_config shield_core shield_lua shield_data shield_net
)
```

## 检查项

CI 应至少检查：

- `shield_core` link libraries 不包含 Lua、Redis、Beast、yaml-cpp、Boost.Log。
- `include/shield/core/**` 不 include `sol/sol.hpp`、`lua.hpp`、`boost/asio`、`yaml-cpp`。
- public API 不出现 `caf::actor`。
- 被删除旧模块不能再被任何 target 引用。
- `shield` executable 不直接链接旧 `shield_extensions` 总包。
