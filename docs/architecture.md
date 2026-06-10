# Shield 架构设计

本文描述重构后的目标架构。当前仓库中仍保留旧架构代码，本文不是“已完成实现清单”。

Shield 的目标是一个 **Skynet 启发的、Actor 模型的、Lua 优先的单节点游戏服务器运行时**。`shield_core` 只封装 Actor/Service 语义；Lua、网络、数据、配置、日志通过官方模块组合。CAF 仅作为内部 Actor 传输基础。

运行时细节以 [运行时语义决策稿](./runtime-semantics.md) 为准。本文只描述架构边界。

## 设计边界

`shield_core` 做这些事：

- 服务生命周期与消息路由。
- `send` / `call` / `spawn` / `exit` / timer 等服务语义。
- coroutine pending/resume。
- CAF adapter。

`shield_core` 不做这些事：

- Lua VM 管理和 Lua-C++ 绑定。
- TCP / UDP / WebSocket 连接管理。
- YAML 配置、日志落盘、DB / Redis 访问。
- 分布式编排、集群管理、服务发现框架。
- DI/IoC、注解装配、条件装配。
- 事件总线，Actor 消息就是事件模型。
- 中间件链作为框架级策略。
- Prometheus、HealthCheckRegistry、插件系统。
- ORM 或跨服务分布式事务。

## 分层

```
┌─────────────────────────────────┐
│          用户 Lua 服务           │
│  scripts/*.lua + shield.* API    │
├─────────────────────────────────┤
│          YAML 声明式配置          │
│  actors / network / data / log   │
├─────────────────────────────────┤
│       Shield Runtime Modules     │
│ lua · net · transport · ipc      │
│ data · config · log · bootstrap  │
├─────────────────────────────────┤
│          shield_core             │
│ service · message · timer        │
├─────────────────────────────────┤
│          shield_base             │
│ result · error · buffer · time   │
├─────────────────────────────────┤
│              CAF                │
│  actor transport implementation  │
└─────────────────────────────────┘
```

## 运维与调试层

```
┌─────────────────────────────────┐
│           shield_ops            │  observability / debug / profile
│ metrics · diagnostics · console  │
│ health · profile · exporter     │
├─────────────────────────────────┤
│     Shield Runtime Modules      │  official modules
│ lua · net · transport · ipc      │
│ data · config · log · bootstrap  │
├─────────────────────────────────┤
│          shield_core             │  service semantics
│ service · message · timer        │
├─────────────────────────────────┤
│          shield_base             │  shared value types
│ result · error · buffer · time   │
├─────────────────────────────────┤
│              CAF                │  implementation detail
└─────────────────────────────────┘
```

## shield_core

| 核心能力 | 职责 |
| --- | --- |
| service lifecycle | 服务创建、命名、退出 |
| message routing | `send` / `call` |
| timer semantics | timeout 和调度语义 |
| coroutine state | pending call、resume、timeout |
| CAF adapter | 隐藏 CAF 细节 |

## 官方运行时模块

| 模块 | 职责 | Lua 可见能力 |
| --- | --- | --- |
| `shield_base` | Result、Error、ByteBuffer、时间、基础 ID 类型 | 不直接暴露 |
| `shield_lua` | Lua VM 管理、Lua-C++ 绑定 | `shield.*` API |
| `shield_net` | TCP / UDP / WebSocket I/O、连接管理 | gateway 服务回调、session 发送 |
| `shield_ipc` | 同机多进程通信、进程心跳、远端进程状态 | 未来通过 cluster/IPC API 暴露 |
| `shield_transport` | C++ 字节流扩展点，例如解帧、加密、可靠 UDP | 不直接暴露给 Lua |
| `shield_data` | 原始 DB / Redis 访问 | `shield.db.*`、`shield.redis.*` |
| `shield_config` | YAML 配置加载 | `shield.config` |
| `shield_log` | 日志 | `shield.log.*` |
| `shield_bootstrap` | 读取配置并组合模块 | `shield::run` |

## shield_ops

`shield_ops` 负责 metrics、health、diagnostics、console 和 profile。

它可以读取运行时状态，但不能影响 `shield_core` 的语义边界。业务逻辑不应该直接依赖 `shield_ops`，只能通过管理端点或控制台访问它。

`gateway` 是推荐的 Lua 服务模式，不是独立的框架策略层。网络包进入 `shield_net`，经 `shield_transport` 解码后交给 Lua gateway 服务做业务路由。

## 目标启动流程

```
shield::run(argc, argv)
  → load config/app.yaml
  → initialize log/config
  → initialize shield_core
  → initialize selected modules
  → bind configured Lua/C++ services
  → start network listeners
  → enter event loop
  → stop services in reverse order
```

当前仓库尚未提供稳定的 `include/shield/shield.hpp` 和 `shield::run(argc, argv)` 实现；`examples/hello_world/` 中的写法是目标契约。

## Lua 服务模型

```lua
local M = {}

function M.on_init(args)
    M.session = args.session
end

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

function M.on_exit(reason)
    shield.log.info("player stopped")
end

return M
```

## C++ 扩展点

重构后只保留少量显式扩展点，避免重新滑向通用企业框架。

### Transport

```cpp
class MyTransport : public shield::Transport {
public:
    void on_raw_data(shield::Connection& conn,
                     const char* data,
                     size_t len) override;
};
```

Transport 处理原始字节流，输出运行时可路由的消息。

### C++ Service

```cpp
class CombatService : public shield::CppService {
public:
    void on_init(const shield::Config& cfg) override;
    sol::table on_lua_call(sol::table args) override;
};
```

C++ Service 只用于性能敏感或基础设施能力，不作为普通业务逻辑的默认写法。

## 目录目标

```
include/shield/
├── shield.hpp
├── base/
├── core/
├── lua/
├── net/
├── ipc/
├── transport/
├── data/
├── config/
├── log/
└── bootstrap/
```

旧目录中的 `discovery/`、`metrics/`、`health/`、`di/`、`annotations/`、`conditions/`、`events/`、`plugin/`、独立 `protocol/` 等不属于新 core 边界。后续可以删除、合并到目标模块，或移到明确的非核心实验区。
