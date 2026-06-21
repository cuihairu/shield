---
home: true
title: Shield
titleTemplate: false
heroText: Shield
tagline: Skynet 启发的、Actor 模型的、Lua 优先的单节点优先游戏服务器运行时
actions:
  - text: 架构设计
    link: /shield/architecture
    type: primary
  - text: 运行时语义
    link: /shield/runtime-semantics
    type: secondary
  - text: Lua API
    link: /shield/lua-api
    type: secondary
features:
  - title: 单节点优先
    details: 最小部署路径聚焦单进程/单节点游戏服务；集群能力通过官方可选模块显式扩展。
  - title: Lua 优先
    details: 游戏逻辑通过 Lua 服务编写，C++ 只承载运行时基础设施。
  - title: Skynet 风格语义
    details: 目标 API 聚焦 service、send、call、timer、spawn、exit 等核心能力。
  - title: CAF 内部传输
    details: CAF 是实现细节，不暴露给 Lua 业务代码和用户侧 C++ 扩展。
  - title: 小核心
    details: shield_core 只封装服务、消息、timer 和 coroutine 语义，其他能力由官方模块组合。
  - title: 插件系统 v1
    details: 基于 JSON manifest 的 metadata-first 扩展机制，稳定 C ABI 提供数据库/缓存/队列/排行榜/认证等后端，按需加载、显式依赖注入。
  - title: 设计阶段
    details: 文档描述重构目标；旧实现中的 discovery、metrics、DI 等不再视为 core，扩展能力由独立的插件系统 v1 承载。
  - title: API 契约优先
    details: Lua API 以独立契约和测试矩阵为准，示例只作为用户参考。
  - title: 运维分层
    details: shield_ops 独立承载 metrics、health、console、profile，与 shield_core 解耦。
footer: Apache License 2.0 | Refactor design stage
---

## 关于 Shield

Shield 是一个受 Skynet 启发的游戏服务器运行时，采用 Actor 模型和 Lua 优先的设计理念。本项目目前处于重构设计阶段，文档描述的是目标架构和 API 方向，而非稳定发布的实现。

### 设计目标

- **C++ 运行时基础设施**：Actor 调度、网络通信、Lua 绑定、定时器、配置管理和日志记录
- **Lua 游戏逻辑**：每个服务都是一个使用 `shield.*` API 的 Lua 脚本
- **CAF 作为内部 Actor 传输**：CAF 仅作为实现细节，对 Lua 业务代码和用户 C++ 扩展隐藏
- **单节点最小部署**：核心部署路径专注于单进程/单节点游戏服务器
- **集群能力可选**：集群支持是 shield_core 之外的官方可选扩展

### 设计理念

Shield 遵循以下核心设计原则：

1. **清晰的边界**：`shield_core` 刻意设计得很窄，只包装围绕 CAF 的 actor/service 语义
2. **Lua 优先**：游戏逻辑通过 Lua 服务编写，C++ 不承载业务代码
3. **模块化**：核心功能与可选能力明确分离，用户可以按需组合
4. **简单性优先**：避免过度设计，不提供分布式编排、DI/IoC 容器、ORM 等企业级框架特性

### 适用场景

Shield 适合以下类型的游戏服务器项目：

- **MMOARPG**：大型多人在线角色扮演游戏，需要服务端逻辑和状态管理
- **即时战斗游戏**：MOBA、FPS 等需要低延迟和高并发的游戏
- **社交游戏**：卡牌、棋牌等需要房间管理和实时通信的游戏
- **休闲游戏**：中小型休闲游戏，需要简单易用的服务器框架

### 核心能力

Shield core 仅提供以下核心能力：

| 能力 | 职责 |
|------|------|
| 服务生命周期 | 创建、命名、停止和检查服务 |
| 消息语义 | `send` / `call` 路由和超时行为 |
| 协程调度 | 无阻塞地挂起和恢复 Lua 面向的调用 |
| 定时器语义 | 附加到服务执行的超时原语 |
| CAF 适配器 | 将 CAF 隐藏在 Shield 服务句柄之后 |

### 快速开始

```lua
local M = {}

function M.on_init()
    shield.log.info("echo service started")
end

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

return M
```

### 非目标

Shield core **不提供**以下功能（这些可由用户构建或作为可选扩展评估）：

- 分布式编排或集群管理作为核心路径的一部分
- DI/IoC 容器或基于注解的组装
- ORM 或企业级数据框架
- 与 actor 消息分离的事件总线抽象
- 作为框架策略层的中间件链
- Prometheus、health-check 注册表或插件系统作为核心运行时特性

### 模块结构

官方运行时模块围绕 core 构建，但不属于核心语义的一部分：

| 模块 | 职责 |
|------|------|
| `shield_base` | Result、Error、ByteBuffer、时间、ID 等共享值类型 |
| `shield_lua` | Lua VM 管理和 `shield.*` 绑定 |
| `shield_net` | 客户端连接（Phase 1 仅 TCP；UDP/KCP/WebSocket 延后）、会话管理 |
| `shield_transport` | 可选的字节流适配，如帧或加密 |
| `shield_data` | 原始 DB/Redis 访问，无 ORM 策略 |
| `shield_config` | JSON 配置加载 |
| `shield_log` | 运行时日志记录 |
| `shield_bootstrap` | 将选定模块组合为可运行服务器 |

### 业务模式参考

这些文档不属于 core 契约，但用于指导游戏业务如何在 Shield 之上组织状态：

- [实体与组件草案](/runtime-entity)：Room、Entity、Component 的轻量对象组织方式
- [游戏状态持久化与回档](/runtime-persistence)：传统数据库、内存快照、增量日志、状态进程、回档和事务设计对比

### 状态与路线图

当前项目处于**重构设计阶段**。现有的源代码树仍包含先前较广泛架构的模块。在重构期间：

- 文档应首先描述目标边界
- 实现工作应随后移除、合并或降级不再属于 `shield_core` 或一级运行时模块的模块

查看[路线图](/roadmap)了解分阶段重构计划和当前范围。

### 参与贡献

我们欢迎贡献！请查看[开发指南](/development-guide)了解如何参与项目开发。

### 许可证

Apache License 2.0
