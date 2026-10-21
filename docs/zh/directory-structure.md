# Shield 重构后的目录结构设计

本文档描述 Shield 重构后的目标目录结构。

> **状态说明**：本文部分"当前状态"小节拍摄于重构早期，其中提到的 annotations/conditions/di/discovery/events/gateway/data 等目录、`include/shield/api.hpp` 公共头方案与 `test_starter_manager.cpp` 等测试文件已在后续重构中删除或被取代（现行结构见仓库根 `src/`、`include/` 与 `CMakeLists.txt`）。阅读时以目标结构为准，"当前状态"仅作历史参照。
>
> 另：下文「目标目录结构」已与现行仓库布局对齐——源码目录是**扁平**的 `src/{core,config,log,net,lua,bootstrap,plugin,console,cluster,global,player,server,transport,...}`，没有 `shield_` 目录前缀（唯 `src/shield_base/` 例外），也没有 `src/optional/` 层级；`shield_*` 前缀只保留在 CMake target 名上。「迁移计划」小节是历史草案，其中的命令未按原样执行。

## 设计原则

1. **模块边界清晰**：每个模块有独立的 include/ 和 src/ 目录（未采用：现行 `src/` 为扁平目录，模块公共头集中在 `include/shield/<模块>/`，测试集中在根 `tests/`）
2. **依赖关系单向**：基础设施模块在上层，业务模块在下层
3. **便于编译**：CMake 可以按模块独立配置
4. **符合 C++ 惯例**：include/ 放头文件，src/ 放实现，tests/ 放测试

## 当前目录结构（需要重构的部分）

```
shield/
├── include/shield/
│   ├── annotations/      已移除（不需要注解系统）
│   ├── conditions/       已移除（不需要条件装配）
│   ├── di/              已移除（不需要 DI 容器）
│   ├── discovery/       已移除（不需要服务发现）
│   ├── events/          已移除（跨 service 事件由 actor 消息语义表达）
│   ├── health/          已移除（不需要内置健康检查）
│   ├── metrics/         已移除（不需要内置 metrics）
│   ├── gateway/         重新定位（改为 Lua 模板）
│   ├── actor/           保留（内部 CAF 封装）
│   ├── cli/             保留
│   ├── commands/        保留
│   ├── config/          保留
│   ├── core/            保留
│   ├── data/            已移除（数据访问由插件系统 v1 提供）
│   ├── database/        已移除（迁移为 plugins/* provider）
│   ├── extensions/      重新定位
│   ├── fs/              保留
│   ├── http/            合入 net/
│   ├── log/             保留
│   ├── net/             保留
│   ├── protocol/        合入 transport/
│   ├── script/          保留
│   ├── serialization/   仅保留最小消息编码能力
│   └── service/         合入 core/
└── src/
    ├── annotations/     已移除
    ├── conditions/      已移除
    ├── di/             已移除
    ├── discovery/      已移除
    ├── events/         已移除
    ├── health/         已移除
    ├── metrics/        已移除
    ├── database/       已移除（迁移为 plugins/* provider）
    ├── gateway/        改为 Lua 模板
    └── ...（其他同上）
```

## 目标目录结构

### 顶层结构

```
shield/
├── CMakeLists.txt              # 根 CMake 配置（全部模块 target 在此定义）
├── build.sh                    # 构建/运行入口脚本
├── README.md                   # 项目说明
├── LICENSE                     # 许可证
├── vcpkg.json                  # 依赖管理
│
├── cmake/                      # （未建：无此目录，CMake 辅助逻辑在根 CMakeLists.txt）
│
├── config/                     # 配置文件示例
│   ├── app.yaml
│   ├── app-dev.yaml
│   ├── app-prod.yaml
│   ├── app-with-redis.yaml
│   └── app-with-sqlite.yaml
│
├── docs/                       # 文档
│   ├── architecture.md         # 架构文档
│   ├── optional-modules.md     # 官方可选模块契约
│   ├── lua-api.md              # Lua API 契约
│   ├── lua-api-tests.md        # Lua API 测试矩阵
│   ├── optional-module-tests.md # 官方可选模块验收矩阵
│   ├── runtime-semantics.md    # 运行时语义索引
│   ├── runtime-*.md            # 各专题运行时文档
│   ├── starter-system.md       # Starter 系统文档
│   ├── cmake-refactor.md       # CMake target 拆分策略
│   └── directory-structure.md  # 本文档
│
├── examples/                   # 示例工程
│   ├── hello_world/            # 最小示例
│   └── kickstart/              # 起步模板工程
│
├── include/                    # 公共头文件（用户可见 API）
│   └── shield/
│       ├── shield.hpp           # 顶层入口（shield::run / request_stop）
│       ├── version.hpp.in       # 版本模板（构建生成 version.hpp）
│       ├── caf_initializer.hpp
│       ├── types.hpp           # 未建（原设计的公共类型头，仓库中不存在）
│       ├── result.hpp          # 未建：Result<T>/Error 实际位于 base/result.hpp
│       ├── api.hpp             # 未建（统一 public API 头方案，见顶部状态说明）
│       ├── base/               # 基础类型（base/result/error/id/byte_buffer/time 头）
│       └── bootstrap/ cluster/ config/ console/ core/ global/ log/
│           lua/ net/ player/ plugin/ server/ transport/   # 各模块公共头
│
├── src/                        # 源代码（扁平按模块组织，无 shield_ 目录前缀）
│   ├── shield_base/            # 基础类型和工具（唯一保留 shield_ 前缀的源码目录）
│   ├── core/                   # 核心 Actor/Service 语义
│   ├── config/                 # 配置管理
│   ├── log/                    # 日志系统
│   ├── lua/                    # Lua VM / 服务 / host 内置 shield.* API 绑定
│   ├── net/                    # 网络层（TCP/UDP/WebSocket/HTTP/console server）
│   ├── plugin/                 # 插件系统 v1 host（manifest/catalog/instance/binding/C ABI）
│   ├── transport/              # 协议适配
│   ├── console/                # 诊断控制台命令与 ops HTTP handler
│   ├── bootstrap/              # 启动器（sink 装配、http/console 启动）
│   ├── cluster/                # 集群通信（可选编译，CLUSTER=ON）
│   ├── global/                 # 全局能力（可选编译，GLOBAL=ON）
│   ├── player/                 # 玩家态（可选编译，PLAYER=ON）
│   ├── server/                 # 服务器状态机（可选编译，SERVER=ON）
│   ├── shield.cpp              # shield::run 实现（编入 shield_bootstrap）
│   ├── caf_initializer.cpp
│   └── main.cpp                # 可执行入口
│
├── plugins/                    # 运行时加载的插件包；目录名用下划线，
│                               # 运行时包 ID 取 manifest 的 id 字段（含点号），两者不同
│   ├── sqlite/                 # 每个插件自包含（C ABI 实现、manifest.yaml、CMakeLists.txt 等）
│   ├── mysql/  postgresql/  mongodb/  redis.driver/
│   ├── cache.redis/  queue.redis/  leaderboard.redis/
│   ├── metric_prometheus/      # 包 ID：metrics.prometheus
│   ├── health_http/            # 包 ID：health.http
│   ├── matchmaking_elo/        # 包 ID：matchmaking.elo
│   ├── auth_jwt/
│   ├── protocol.json/  protocol.msgpack/  protocol.flatbuffers/  protocol.protobuf/
│   └── _shared/
│
├── tests/                      # 测试代码（按模块/域组织，无 unit/integration/e2e 分层）
│   ├── acceptance/  cluster/  global/  lua_api/  net/
│   ├── player/  plugin/  protocol/  server/  transport/
│   ├── coverage/               # 覆盖率辅助
│   └── fixtures/               # 测试夹具（如 config）
│
├── scripts/                    # Lua 脚本目录（业务/示例脚本；不是构建脚本，
│                               # 构建入口是仓库根 build.sh）
│   ├── bootstrap.lua  echo.lua  lib/
│   ├── new_project.sh          # 新工程生成器
│   └── client_demo.py          # Python 演示客户端
│
└── templates/                  # 工程模板（给用户复制）
    └── minimal_game/
```

### 模块内部结构

> **现状**：现行源码目录是扁平文件布局，没有下列 `include/ + src/ + tests/` 三级子目录——模块公共头在 `include/shield/<模块>/`，模块私有头与实现同目录（如 `src/console/*.hpp`），测试集中在根 `tests/<域>/`，也没有 per-module `CMakeLists.txt`（模块 target 全部在根 `CMakeLists.txt` 定义）。以下各树的**根路径已改为现行目录**；内部子目录为早期目标草案，仅表达结构意图。

每个模块（如 `core`）内部结构（早期目标草案）：

```
src/core/
├── include/                   # 模块私有头文件（未采用）
│   ├── service_registry.hpp
│   ├── service_handle.hpp
│   └── ...
├── src/                       # 实现代码（未采用）
│   ├── service_registry.cpp
│   ├── service_handle.cpp
│   └── ...
├── tests/                     # 模块单元测试（未采用）
│   ├── test_service_registry.cpp
│   └── ...
└── CMakeLists.txt             # 模块 CMake 配置（未采用）
```

### 模块详细说明

#### shield_base

```
src/shield_base/
├── include/
│   ├── result.hpp              # Result<T>, Error
│   ├── byte_buffer.hpp         # ByteBuffer
│   ├── time_point.hpp          # TimePoint
│   └── uuid.hpp                # UUID 生成器
├── src/
│   ├── result.cpp
│   ├── byte_buffer.cpp
│   └── ...
└── CMakeLists.txt
```

职责：提供跨模块的基础类型，不依赖其他 shield 模块。

#### shield_core

```
src/core/
├── include/
│   ├── service_handle.hpp     # ServiceHandle（opaque）
│   ├── service_registry.hpp    # 服务注册表
│   ├── message_envelope.hpp   # MessageEnvelope
│   ├── message_dispatcher.hpp # 消息分发
│   ├── timer_manager.hpp      # 定时器管理
│   ├── coroutine_manager.hpp  # 协程管理
│   └── core.hpp               # ShieldCore 入口
├── src/
│   ├── service_registry.cpp
│   ├── message_dispatcher.cpp
│   └── ...
├── tests/
│   ├── test_service_handle.cpp
│   ├── test_service_registry.cpp
│   └── ...
└── CMakeLists.txt
```

职责：Actor/Service 核心，隐藏 CAF 实现，只暴露 opaque handle。

**禁止依赖**：Lua、网络、数据、配置、日志。

#### shield_config

```
src/config/
├── include/
│   ├── configuration.hpp      # Configuration 主类
│   ├── config_loader.hpp      # YAML 加载
│   └── config_validator.hpp   # 配置验证
├── src/
│   ├── configuration.cpp
│   ├── config_loader.cpp
│   └── ...
├── tests/
│   └── test_configuration.cpp
└── CMakeLists.txt
```

职责：加载和管理配置，支持运行时修改；环境变量展开未实现（见 [配置语义](runtime-config.md#环境变量展开未实现)）。

**依赖**：shield_base, shield_log

#### shield_log

```
src/log/
├── include/
│   ├── logger.hpp             # Logger 主类
│   ├── log_level.hpp          # 日志级别
│   ├── log_sink.hpp           # 日志输出接口
│   └── sinks/
│       ├── console_sink.hpp
│       └── file_sink.hpp
├── src/
│   ├── logger.cpp
│   └── sinks/
│       └── file_sink.cpp
├── tests/
│   └── test_logger.cpp
└── CMakeLists.txt
```

职责：日志系统，支持日志轮转（按大小）；结构化日志为目标态（未实现，见 [日志运行时语义](runtime-log.md)）。

**依赖**：shield_base

#### shield_bootstrap

```
src/bootstrap/
├── include/
│   ├── bootstrap.hpp          # Bootstrap 入口
│   ├── bootstrap_context.hpp  # BootstrapContext 显式上下文
│   ├── starter.hpp            # IStarter 接口
│   ├── starter_manager.hpp    # Starter 管理器
│   └── runtime_options.hpp    # CLI/启动选项
├── src/
│   ├── bootstrap.cpp
│   ├── starter_manager.cpp
│   ├── config_starter.cpp
│   ├── log_starter.cpp
│   ├── core_starter.cpp
│   ├── script_starter.cpp
│   └── ...
├── tests/
│   ├── test_starter_manager.cpp
│   └── ...
└── CMakeLists.txt
```

职责：启动流程、Starter 管理、`shield::run(argc, argv)`，不提供 DI、插件或生命周期事件总线。

**依赖**：shield_base, shield_log, shield_config

#### shield_lua

```
src/lua/
├── include/
│   ├── lua_vm_pool.hpp        # Lua VM 池
│   ├── script_starter.hpp     # ScriptStarter
│   ├── lua_service_loader.hpp # Lua service module loader
│   └── lua_bindings.hpp       # host 内置 API 注册入口
├── src/
│   ├── lua_vm_pool.cpp
│   ├── script_starter.cpp
│   ├── lua_api.cpp            # host 内置 shield.spawn/send/timer/log/config/plugin
│   └── ...
└── CMakeLists.txt
```

职责：Lua VM 管理、ScriptStarter、Lua service loader、**host 内置** `shield.*` API 绑定。

注意：业务 Lua API（`shield.database.*` / `shield.cache.redis` / `shield.queue.redis` 等）由各插件通过 `register_lua` 钩子自行注册，不在 `shield_lua` 里。`shield_lua` 只负责 host 自身能力（service / message / timer / config / log / plugin introspection）。

**依赖**：shield_base, shield_log, shield_config, shield_core, shield_net, shield_plugin

#### shield_plugin

```
src/plugin/
├── include/
│   ├── manifest.hpp           # manifest/catalog model
│   ├── plugin_host.hpp        # PluginHost
│   └── plugin_library.hpp     # dlopen/LoadLibrary wrapper
├── src/
│   ├── manifest.cpp
│   ├── plugin_host.cpp
│   ├── plugin_library.cpp
│   └── ...
├── tests/
│   └── test_plugin_host.cpp
└── CMakeLists.txt
```

职责：插件 manifest/catalog、实例生命周期、binding、C ABI host、`register_lua` 分发和只读 introspection。数据库、Redis、队列、排行榜等后端能力由 `plugins/*` 独立共享库提供，连接池归插件 instance。

**依赖**：shield_base, shield_log, shield_config

#### shield_net

```
src/net/
├── include/
│   ├── tcp_server.hpp         # TCP 服务端
│   ├── tcp_client.hpp         # TCP 客户端
│   ├── udp_socket.hpp         # UDP Socket
│   ├── ws_server.hpp          # WebSocket 服务端
│   ├── connection.hpp         # 连接抽象
│   ├── connection_manager.hpp # 连接管理
│   └── net_starter.hpp       # NetStarter
├── src/
│   ├── tcp_server.cpp
│   ├── tcp_client.cpp
│   └── ...
├── tests/
│   └── test_tcp_server.cpp
└── CMakeLists.txt
```

职责：网络层，TCP/UDP/WebSocket I/O。

**依赖**：shield_base, shield_log, shield_config

#### shield_transport

```
include/shield/transport/
├── protocol.hpp               # ProtocolPipeline / profile / route 表
└── rpc_descriptor.hpp         # 客户端 RPC descriptor 表
src/transport/
├── protocol.cpp
└── rpc_descriptor.cpp
```

职责：协议管线（envelope 定界、body codec、route 校验）与 RPC descriptor 表；在字节流和结构化消息之间转换。

**依赖**：shield_base, shield_log

具体协议实现放在 `shield_transport` 内部或其子目录中。独立 `shield_protocol` 不进入当前目标结构；schema 工具链属于 deferred extension。帧级 fallback codec/encryption 已删除：TCP 必须绑定 `network.protocol`，无管线的入站字节一律拒绝（`protocol_not_configured`）。

具体协议实现放在 `shield_transport` 内部或其子目录中。独立 `shield_protocol` 不进入当前目标结构；schema 工具链属于 deferred extension。

#### shield_cluster（可选）

```
src/cluster/
├── include/
│   ├── node_discovery.hpp     # 节点发现
│   ├── cluster_rpc.hpp        # 集群 RPC
│   └── cluster_starter.hpp    # ClusterStarter
├── src/
│   └── ...
├── tests/
│   └── ...
└── CMakeLists.txt
```

职责：集群通信，node-to-node 消息传递，远端 route cache 和节点心跳。该模块是官方可选模块，不属于最小主路径。

**依赖**：shield_base, shield_log, shield_config, shield_net

#### shield_global（可选）

```
src/global/
├── include/
│   ├── global_data.hpp        # 全局数据
│   ├── distributed_lock.hpp   # 分布式锁
│   └── global_starter.hpp     # GlobalStarter
├── src/
│   └── ...
├── tests/
│   └── ...
└── CMakeLists.txt
```

职责：基于 Redis 等后端提供全局数据、分布式锁、排行榜、队列、限流器。该模块是官方可选模块。

**依赖**：shield_base, shield_log, shield_config, shield_plugin

#### shield_ops（可选）

```
src/optional/shield_ops/        # 未建：无此目录（src/ 下没有 optional/ 层级）
├── include/
│   ├── ops_server.hpp         # 运维 HTTP 服务
│   ├── metrics_collector.hpp  # 指标收集
│   └── health_checker.hpp     # 健康检查
├── src/
│   └── ...
├── tests/
│   └── ...
└── CMakeLists.txt
```

职责：运维端点，提供 HTTP/console 接口查看状态。该模块是官方可选模块，不属于 `shield_core`。

**现状**：`shield_ops` 目录未建；CMake 里的 `shield_ops` target 是空壳（无源文件）。上述能力实际实现于 `src/bootstrap/`（`http:`/`console:` 装配）与 `src/console/`（`ops_http_handler.cpp`、控制台命令），见 [运维运行时语义](runtime-ops.md)。

**依赖**：shield_base, shield_log, shield_net

## 依赖层次

```
┌─────────────────────────────────────────────────────────────┐
│                    shield_lua (用户 API)                      │
├─────────────────────────────────────────────────────────────┤
│ optional: shield_cluster / shield_global / shield_ops          │
├─────────────────────────────────────────────────────────────┤
│                    shield_transport                          │
├─────────────────────────────────────────────────────────────┤
│                    shield_plugin │ shield_net                 │
├─────────────────────────────────────────────────────────────┤
│                    shield_script                             │
├─────────────────────────────────────────────────────────────┤
│                    shield_core                                │
├─────────────────────────────────────────────────────────────┤
│  shield_bootstrap  │  shield_config  │  shield_log            │
├─────────────────────────────────────────────────────────────┤
│                    shield_base                                │
└─────────────────────────────────────────────────────────────┘
```

图中的 `shield_*` 是 **CMake target 名**；对应源码目录是扁平的 `src/`（如 `src/lua/`、`src/cluster/`），没有 `shield_` 目录前缀。`shield_script` 不是独立 target——Lua VM 管理并入 `shield_lua`（`src/lua/`）。

## 迁移计划

> **历史草案，未按原样执行**：现行 `src/` 保持扁平目录（`src/core/`、`src/cluster/` 等），从未创建过 `src/shield_*` 模块目录或 `src/optional/` 层级；下列命令引用的路径均不存在。保留原文仅作决策记录。

### Phase 1: 创建目标结构

```bash
# 1. 创建新的模块目录（未执行）
mkdir -p src/shield_{base,core,config,log,bootstrap,script,lua,data,net,transport}
mkdir -p src/optional/shield_{cluster,global,ops}

# 2. 每个模块创建标准子目录（未执行）
for module in src/shield_* src/optional/shield_*; do
    mkdir -p "$module"/{include,src,tests}
    touch "$module/CMakeLists.txt"
done
```

### Phase 2: 迁移现有代码

| 源目录 | 目标模块 |
|--------|----------|
| `src/core/*` | `shield_core` |
| `src/config/*` | `shield_config` |
| `src/log/*` | `shield_log` |
| `src/script/*` | `shield_script` |
| `src/data/*`, `src/database/*` | 删除或迁移为 `plugins/*` provider |
| `src/net/*`, `src/http/*` | `shield_net` |
| `src/protocol/*` | `shield_transport` |

### Phase 3: 删除废弃模块

```bash
# 删除不需要的模块
rm -rf src/{di,annotations,conditions,events,discovery,health,metrics}
rm -rf include/shield/{di,annotations,conditions,events,discovery,health,metrics}
```

### Phase 4: 重新组织 gateway

```
# gateway 改为 Lua 模板（未执行：src/gateway 不存在；现行 gateway host 侧
# 实现在 src/lua/gateway_actor.cpp 与 src/lua/lua_gateway_bridge.cpp）
mv src/gateway/* examples/templates/services/
rm -rf src/gateway
```

### Phase 5: 更新 CMake

更新根 `CMakeLists.txt`（未执行：现行根 `CMakeLists.txt` 不使用 per-module `add_subdirectory`，而是直接以扁平源码路径定义全部 `shield_*` target）：

```cmake
# 添加子目录
add_subdirectory(src/shield_base)
add_subdirectory(src/shield_log)
add_subdirectory(src/shield_config)
add_subdirectory(src/shield_bootstrap)
add_subdirectory(src/shield_core)
add_subdirectory(src/shield_script)
add_subdirectory(src/shield_plugin)
add_subdirectory(src/shield_net)
add_subdirectory(src/shield_transport)
add_subdirectory(src/shield_lua)

# Optional modules are added only when enabled.
# add_subdirectory(src/optional/shield_cluster)
# add_subdirectory(src/optional/shield_global)
# add_subdirectory(src/optional/shield_ops)
```

## 公共 API 头文件

创建统一的公共 API：

```cpp
// include/shield/api.hpp
#pragma once

// 基础类型
#include "shield/types.hpp"
#include "shield/result.hpp"

// 运行时 API（供 C++ 用户直接使用）
namespace shield {

/**
 * Shield 运行时入口
 * 
 * @param argc 参数计数
 * @param argv 参数向量
 * @return 返回码
 */
int run(int argc, char** argv);

} // namespace shield
```

## 头文件可见性

### 完全公开（用户可见）

- `include/shield/*.hpp`：公共 API（如 `shield.hpp`、`caf_initializer.hpp`）
- `include/shield/<模块>/`：各模块公共头（如 `lua/`、`core/`、`base/`）

### 模块私有（用户不可见）

- `src/<模块>/` 下与源码同目录的 `*.hpp`（如 `src/console/*.hpp`、`src/plugin/schema_validator.hpp`）：只在模块内部使用
- 通过 CMake 的 `target_include_directories()` 控制

## 命名规范

### 目录命名

- 模块源码目录：扁平无前缀（`src/core/`、`src/bootstrap/` 等，唯 `src/shield_base/` 例外）
- 模块 CMake target：`shield_*`（全小写，下划线分隔）
- 测试目录：`tests/<域>/`
- 头文件目录：`include/`, `src/`

### 文件命名

- C++ 头文件：`snake_case.hpp`
- C++ 源文件：`snake_case.cpp`
- 测试文件：`test_*.cpp`

### 类命名

- C++ 类：`PascalCase`
- 接口类：`IPascalCase`
- 异常：`PascalCaseError`

## 总结

| 方面 | 变更 |
|------|------|
| 模块组织 | 保留扁平 `src/` 布局（未采用分层目录结构），模块边界由 CMake target 表达 |
| 目录命名 | `shield_*` 前缀仅用于 CMake target 名；源码目录扁平（唯 `src/shield_base/` 例外） |
| 头文件可见性 | 区分公共 API 和模块私有 |
| 依赖管理 | 通过 CMake target 依赖强制单向依赖 |
| 测试组织 | 集中在根 `tests/<域>/`（未按模块拆分 tests/） |
| 移除模块 | di, annotations, conditions, events, discovery, health, metrics, plugin, middleware |
| 合并模块 | database → 插件系统提供（`plugins/*`，无独立 data 目录）, http → net, protocol → transport |
| 改为模板 | gateway → host 侧实现在 `src/lua/`（`gateway_actor.cpp` 等）；工程模板在 `templates/minimal_game/` |
