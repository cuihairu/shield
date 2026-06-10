# Shield 架构定义

本文是当前重构设计稿。它描述目标架构，不表示当前源码已经完成对应调整。

具体运行时语义以 `docs/runtime-semantics.md` 为准。

## 定位

Shield 是 **单节点游戏服务运行时**。

- C++ 负责运行时基础设施；其中 `shield_core` 只封装 Actor/Service 语义，其他能力通过官方模块组合。
- Lua 负责游戏逻辑：每个服务是一个 Lua 脚本，通过 `shield.*` API 与运行时交互。
- CAF 是内部 Actor 传输基础，不暴露给用户业务代码。
- 设计参考 Skynet 的极简哲学：服务、消息、网络、定时器。

## 不做

`shield_core` 不做：

- 分布式编排、集群管理、服务发现框架。
- DI/IoC、注解装配、条件装配。
- ORM、跨服务分布式事务。
- 独立事件总线。
- 框架级中间件链。
- Prometheus、HealthCheckRegistry、插件系统。

这些能力可以由用户在业务层实现，或以后作为明确的非核心扩展重新评估。

## 架构分层

```
┌─────────────────────────────────┐
│       用户 Lua 服务              │  game logic
│  scripts/*.lua + shield.* API    │
├─────────────────────────────────┤
│       YAML 声明式配置            │  glue layer
│  actors / network / data / log   │
├─────────────────────────────────┤
│       Shield Runtime Modules     │  infrastructure
│ lua · net · transport · ipc      │
│ data · config · log · bootstrap  │
├─────────────────────────────────┤
│       shield_core                │  service semantics
│ service · message · timer        │
├─────────────────────────────────┤
│       shield_base                │  shared value types
│ result · error · buffer · time   │
├─────────────────────────────────┤
│       CAF Actor System           │  implementation detail
└─────────────────────────────────┘
```

## shield_core 边界

`shield_core` 只放 Actor 封装语义：

| 核心能力 | 职责 |
| --- | --- |
| `ServiceHandle` | 隐藏 CAF actor handle |
| `ServiceRegistry` | 服务命名、查找、注册 |
| `send` / `call` | 异步消息和请求-响应语义 |
| `spawn` / `exit` | 服务生命周期语义 |
| `timer` / `timeout` | 和服务执行上下文绑定的时间语义 |
| coroutine pending/resume | `call` 和异步 I/O 的协程挂起/恢复 |
| CAF adapter | 只在 core 内部接触 CAF |

`shield_core` 不知道 Lua、网络、数据库、YAML、日志落盘策略，也不直接依赖这些模块。

## 官方运行时模块

| 模块 | 职责 | Lua 可见能力 |
| --- | --- | --- |
| `shield_base` | Result、Error、ByteBuffer、时间、基础 ID 类型 | 不直接暴露 |
| `shield_lua` | Lua VM 管理、Lua-C++ 绑定 | `shield.*` |
| `shield_net` | TCP / UDP / WebSocket 监听、连接管理、原始字节处理 | gateway 服务回调、session 发送 |
| `shield_ipc` | 同机多进程通信、进程心跳、远端进程状态 | 未来通过 IPC/cluster API 暴露 |
| `shield_transport` | 自定义传输层，例如私有帧、加密、可靠 UDP | C++ 扩展点，Lua 不直接调用 |
| `shield_data` | 原始 DB / Redis 访问，不提供 ORM | `shield.db.*`、`shield.redis.*` |
| `shield_config` | YAML 解析和只读配置访问 | `shield.config` |
| `shield_log` | 日志 | `shield.log.*` |
| `shield_bootstrap` | 读取配置并组合 core + modules | `shield::run` |

`gateway` 是基于 `shield_core` + `shield_net` + `shield_lua` 的 Lua 服务模式，不是独立框架策略层。

## 目标 Lua API

```lua
local M = {}

function M.on_init(args) end
function M.ping(value) return value end
function M.on_exit(reason) end

return M
```

### 服务生命周期

```lua
local h, err = shield.spawn("gateway", {
  name = "gateway.main",
  args = { port = 8888 },
})

shield.exit()
shield.self()
shield.names()
```

### 消息通信

```lua
local ok, err = shield.send(target, "kick", uid)
local ok, value = shield.call(target, "get_profile", uid)
local ok, value = shield.call_timeout(30000, target, "get_profile", uid)
```

### 定时器

```lua
local timer = shield.timer(interval_ms, callback)
local timer = shield.timer_once(delay_ms, callback)
shield.cancel_timer(timer)
shield.sleep(delay_ms)
shield.fork(function() ... end)
shield.now()
```

### 数据访问

```lua
local ok, rows = shield.db.query(sql, params)
local ok, result = shield.db.execute(sql, params)

shield.redis.get(key)
shield.redis.set(key, value, ttl)
shield.redis.publish(channel, data)
shield.redis.subscribe(channel, callback)
```

### 配置和日志

```lua
shield.config("database.host")
shield.log.info("message")
shield.log.warn("message")
shield.log.error("message")
shield.log.debug("message")
```

## C++ 扩展点

### Transport

处理原始字节流：私有二进制帧、加密握手、UDP 自定义可靠层。解码后的消息交给 Lua gateway 服务。

```cpp
class MyTransport : public shield::Transport {
public:
    void on_raw_data(shield::Connection& conn,
                     const char* data,
                     size_t len) override;
};
```

数据流：

```
网络字节流 → C++ Transport → Lua gateway → Lua service
```

### CppService

仅用于性能敏感或基础设施扩展，例如物理模拟、寻路、编解码。

```cpp
class CombatService : public shield::CppService {
public:
    void on_init(const shield::Config& cfg) override;
    sol::table on_lua_call(sol::table args) override;
};
```

## 配置驱动注册

目标配置示例：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    transport: MyTransport
    network:
      tcp: "0.0.0.0:8001"

  - name: echo
    script: scripts/echo.lua
    instances: 1

  - name: player
    script: scripts/player.lua
    instances: 0
```

## 目标目录结构

```
include/shield/
├── shield.hpp
├── base/
├── core/          # service/message/timer semantics
├── lua/
├── net/
├── ipc/
├── transport/
├── data/
├── config/
├── log/
└── bootstrap/
```

## 需要移出或合并的旧模块

| 目录 | 决策 |
| --- | --- |
| `di/` | 不进入 core |
| `annotations/` | 不进入 core |
| `conditions/` | 不进入 core |
| `events/` | 由 Actor 消息替代 |
| `discovery/` | 单节点 core 不做服务发现 |
| `metrics/` | 不进入 core |
| `health/` | 不进入 core |
| `plugin/` | 不进入 core |
| `protocol/` | 合并到 `net/` 或 `transport/` |
| `gateway/` | 收敛为 Lua 服务模式或薄适配 |
| `serialization/` | 只保留运行时实际需要的最小编码能力 |
| `fs/` | 不进入启动主路径 |
| `business/` | 用户业务，不属于框架 |

## CI 边界目标

- 用户代码禁止 include CAF 头文件。
- `shield_core` 禁止依赖 Lua、net、data、config、log 等上层模块。
- `shield.*` Lua API 必须有绑定测试。
- `examples/hello_world/` 必须成为最终验收示例。
