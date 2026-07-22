# Shield 重构后的目录结构设计

本文档描述 Shield 重构后的目标目录结构。

## 设计原则

1. **模块边界清晰**：每个模块有独立的 include/ 和 src/ 目录
2. **依赖关系单向**：基础设施模块在上层，业务模块在下层
3. **便于编译**：CMake 可以按模块独立配置
4. **符合 C++ 惯例**：include/ 放头文件，src/ 放实现，tests/ 放测试

## 当前目录结构（需要重构的部分）

```
shield/
├── include/shield/
│   ├── annotations/      ❌ 移除（不需要注解系统）
│   ├── conditions/       ❌ 移除（不需要条件装配）
│   ├── di/              ❌ 移除（不需要 DI 容器）
│   ├── discovery/       ❌ 移除（不需要服务发现）
│   ├── events/          ❌ 移除（跨 service 事件由 actor 消息语义表达）
│   ├── health/          ❌ 移除（不需要内置健康检查）
│   ├── metrics/         ❌ 移除（不需要内置 metrics）
│   ├── gateway/         ⚠️  重新定位（改为 Lua 模板）
│   ├── actor/           ✅ 保留（内部 CAF 封装）
│   ├── cli/             ✅ 保留
│   ├── commands/        ✅ 保留
│   ├── config/          ✅ 保留
│   ├── core/            ✅ 保留
│   ├── data/            ❌ 移除（数据访问由插件系统 v1 提供）
│   ├── database/        ❌ 移除（迁移为 plugins/* provider）
│   ├── extensions/      ⚠️  重新定位
│   ├── fs/              ✅ 保留
│   ├── http/            ⚠️  合入 net/
│   ├── log/             ✅ 保留
│   ├── net/             ✅ 保留
│   ├── protocol/        ⚠️  合入 transport/
│   ├── script/          ✅ 保留
│   ├── serialization/   ⚠️  仅保留最小消息编码能力
│   └── service/         ⚠️  合入 core/
└── src/
    ├── annotations/     ❌ 移除
    ├── conditions/      ❌ 移除
    ├── di/             ❌ 移除
    ├── discovery/      ❌ 移除
    ├── events/         ❌ 移除
    ├── health/         ❌ 移除
    ├── metrics/        ❌ 移除
    ├── database/       ❌ 移除（迁移为 plugins/* provider）
    ├── gateway/        ⚠️  改为 Lua 模板
    └── ...（其他同上）
```

## 目标目录结构

### 顶层结构

```
shield/
├── CMakeLists.txt              # 根 CMake 配置
├── README.md                   # 项目说明
├── LICENSE                     # 许可证
├── vcpkg.json                  # 依赖管理
├── 
├── cmake/                      # CMake 模块
│   ├── ShieldConfig.cmake
│   └── ...
│
├── config/                     # 配置文件示例
│   ├── app.yaml.example
│   └── logging.yaml.example
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
├── examples/                   # 示例代码
│   ├── hello_world/            # 最小示例
│   ├── echo_server/            # Echo 服务示例
│   ├── chat_room/              # 聊天室示例
│   └── templates/              # Lua 服务模板
│       ├── gateway_service.lua
│       ├── player_service.lua
│       └── ...
│
├── include/                    # 公共头文件（用户可见 API）
│   └── shield/
│       ├── version.hpp         # 版本信息
│       ├── types.hpp           # 公共类型定义
│       ├── result.hpp          # Result<T>, Error
│       └── api.hpp             # 统一的 public API 头文件
│
├── src/                        # 源代码（按模块组织）
│   ├── shield_base/            # 基础类型和工具
│   ├── shield_core/            # 核心 Actor/Service 系统
│   ├── shield_config/          # 配置管理
│   ├── shield_log/             # 日志系统
│   ├── shield_bootstrap/       # 启动器
│   ├── shield_script/          # Lua VM 管理
│   ├── shield_lua/             # host 内置 Lua API 绑定（spawn/send/timer/log/config/plugin introspection）
│   ├── shield_plugin/          # 插件系统 v1 host（manifest/catalog/instance/binding/C ABI）
│   ├── shield_net/             # 网络层
│   ├── shield_transport/       # 协议适配
│   └── optional/               # 官方可选模块（非最小主路径）
│       ├── shield_cluster/     # 集群通信（可选）
│       ├── shield_global/      # 全局能力（可选）
│       └── shield_ops/         # 运维端点（可选）
│
├── plugins/                    # 运行时加载的插件包（每个插件一个目录）
│   ├── sqlite/                 # 每个插件自包含
│   │   ├── shield_db_sqlite.cpp        # C ABI 实现
│   │   ├── lua_bindings.cpp            # register_lua 钩子实现
│   │   ├── lua/                        # 可选：插件自带 Lua 业务封装
│   │   │   └── shield_sqlite.lua
│   │   ├── manifest.yaml               # manifest（含 lua 字段）
│   │   └── CMakeLists.txt
│   ├── mysql/
│   ├── postgresql/
│   ├── mongodb/                # 同构，但实现 shield.document.v1
│   ├── cache.redis/
│   ├── queue.redis/
│   ├── leaderboard.redis/
│   ├── metrics.prometheus/
│   ├── health.http/
│   └── matchmaking.elo/
│
├── tests/                      # 测试代码
│   ├── unit/                   # 单元测试
│   │   ├── base/
│   │   ├── core/
│   │   ├── config/
│   │   └── ...
│   ├── integration/           # 集成测试
│   └── e2e/                   # 端到端测试
│
├── scripts/                    # 构建和部署脚本
│   ├── build.sh
│   ├── build.bat
│   ├── setup.sh
│   └── ...
│
└── templates/                  # Lua 模板（给用户复制）
    ├── services/
    │   ├── gateway.lua
    │   ├── player.lua
    │   └── ...
```

### 模块内部结构

每个模块（如 `shield_core`）内部结构：

```
src/shield_core/
├── include/                   # 模块私有头文件
│   ├── service_registry.hpp
│   ├── service_handle.hpp
│   └── ...
├── src/                       # 实现代码
│   ├── service_registry.cpp
│   ├── service_handle.cpp
│   └── ...
├── tests/                     # 模块单元测试
│   ├── test_service_registry.cpp
│   └── ...
└── CMakeLists.txt             # 模块 CMake 配置
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
src/shield_core/
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
src/shield_config/
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

职责：加载和管理配置，支持环境变量展开、运行时修改。

**依赖**：shield_base, shield_log

#### shield_log

```
src/shield_log/
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

职责：日志系统，支持结构化日志、日志轮转。

**依赖**：shield_base

#### shield_bootstrap

```
src/shield_bootstrap/
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
src/shield_lua/
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
src/shield_plugin/
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
src/shield_net/
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
src/shield_transport/
├── include/
│   ├── packet_reader.hpp      # 数据包读取
│   ├── packet_writer.hpp      # 数据包写入
│   ├── protocol_adapter.hpp   # 协议适配器
│   └── transport_starter.hpp  # TransportStarter
├── src/
│   ├── packet_reader.cpp
│   └── ...
├── tests/
│   └── test_packet_reader.cpp
└── CMakeLists.txt
```

职责：协议适配层，在字节流和消息之间转换。

**依赖**：shield_base, shield_log, shield_net

具体协议实现放在 `shield_transport` 内部或其子目录中。独立 `shield_protocol` 不进入当前目标结构；schema 工具链属于 deferred extension。

#### shield_cluster（可选）

```
src/optional/shield_cluster/
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
src/optional/shield_global/
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
src/optional/shield_ops/
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

## 迁移计划

### Phase 1: 创建目标结构

```bash
# 1. 创建新的模块目录
mkdir -p src/shield_{base,core,config,log,bootstrap,script,lua,data,net,transport}
mkdir -p src/optional/shield_{cluster,global,ops}

# 2. 每个模块创建标准子目录
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
# gateway 改为 Lua 模板
mv src/gateway/* examples/templates/services/
rm -rf src/gateway
```

### Phase 5: 更新 CMake

更新根 `CMakeLists.txt`：

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

- `include/shield/*.hpp`：公共 API
- `include/shield/lua_*.hpp`：Lua 扩展接口

### 模块私有（用户不可见）

- `src/shield_*/include/*.hpp`：只在模块内部使用
- 通过 CMake 的 `target_include_directories()` 控制

## 命名规范

### 目录命名

- 模块目录：`shield_*`（全小写，下划线分隔）
- 测试目录：`tests/`
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
| 模块组织 | 从平面结构改为分层模块结构 |
| 目录命名 | 统一使用 `shield_*` 前缀 |
| 头文件可见性 | 区分公共 API 和模块私有 |
| 依赖管理 | 通过目录层次强制单向依赖 |
| 测试组织 | 每个模块包含自己的 tests/ |
| 移除模块 | di, annotations, conditions, events, discovery, health, metrics, plugin, middleware |
| 合并模块 | database → data, http → net, protocol → transport |
| 改为模板 | gateway → examples/templates/services |
