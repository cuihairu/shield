# Shield

[![C++23](https://img.shields.io/badge/C++-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Lua 5.4](https://img.shields.io/badge/Lua-5.4-blue.svg)](https://www.lua.org/)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

Shield 是一个**单节点优先、受 Skynet 启发、基于 Actor、Lua 优先的游戏服务器运行时**。

本仓库目前处于**重构设计阶段**。仓库中的文档描述的是目标架构和 API 方向，而非稳定发布的实现。

## 目标定位

- C++ 负责运行时基础设施：Actor 调度、网络、Lua 绑定、定时器、配置、日志和底层数据访问。
- Lua 负责游戏逻辑：每个服务都是一个使用 `shield.*` API 的 Lua 脚本。
- CAF 仍作为内部的 Actor 传输基础。
- Shield 暴露游戏服务器语义，而非 CAF 细节。
- 最小部署路径是单节点运行时。
- 集群支持是 `shield_core` 边界之外的官方可选扩展。

## 非目标

Shield 核心不提供：

- 核心路径中的分布式编排或集群管理。
- DI/IoC 容器或基于注解的装配。
- ORM 或企业数据框架。
- 独立于 Actor 消息的事件总线抽象。
- 作为框架策略层的中间件链。
- Prometheus、健康检查注册或插件系统作为核心运行时特性。

这些能力可能由用户构建或稍后作为可选扩展进行评估。它们不是当前核心契约的一部分。

## 核心边界

`shield_core` 故意保持狭窄。它只拥有围绕 CAF 的 actor/service 语义：

| 核心组件 | 职责 |
| --- | --- |
| 服务生命周期 | 创建、命名、停止和检查服务 |
| 消息语义 | `send` / `call` 路由和超时行为 |
| 协程调度 | 挂起和恢复面向 Lua 的调用，不阻塞运行时线程 |
| 定时器语义 | 附加到服务执行的超时原语 |
| CAF 适配器 | 将 CAF 隐藏在 Shield 服务句柄后面 |

第一方运行时模块围绕该核心构建，但不是核心语义的一部分：

| 模块 | 职责 |
| --- | --- |
| `shield_base` | 共享值类型，如 Result、Error、ByteBuffer、时间和 ID |
| `shield_lua` | Lua VM 管理和 `shield.*` 绑定 |
| `shield_net` | 客户端连接（Phase 1 仅 TCP；UDP/KCP/WebSocket 延后）、会话管理 |
| `shield_transport` | 可选的字节流适配，如帧或加密 |
| `shield_data` | 原始 DB/Redis 访问，无 ORM 策略 |
| `shield_config` | YAML 配置加载 |
| `shield_log` | 运行时日志 |
| `shield_bootstrap` | 将选定的模块组合成可运行的服务器 |

可选的官方扩展位于最小的主路径之外：

| 模块 | 职责 |
| --- | --- |
| `shield_cluster` | 跨进程/机器通信、服务发现、节点心跳 |
| `shield_global` | 分布式数据、锁、队列/排名/限流 |
| `shield_ops` | 诊断、指标、控制台、性能分析 |

## 目标 Lua 形态

```lua
local M = {}

function M.on_init()
    shield.log.info("echo started")
end

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

return M
```

当前的源代码树仍然包含来自先前更广泛架构的模块。在重构期间，文档应首先描述目标边界；实现工作应随后删除、合并或降级不再属于 `shield_core` 或第一方运行时模块的模块。

## 文档

权威设计契约：

- [架构](docs/architecture.md)：模块边界、依赖方向、对象所有权和已删除的遗留方向。
- [Lua API 契约](docs/lua-api.md)：目标 `shield.*` 用户 API。
- [运行时语义](docs/runtime-semantics.md)：主题索引和实现顺序。
- [配置语义](docs/runtime-config.md)：核心配置模式。
- [可选模块契约](docs/optional-modules.md)：官方可选模块边界。
- [路线图](docs/roadmap.md)：分阶段重构计划和当前范围。
- [决策日志](docs/open-decisions.md)：已关闭的设计决定和任何新发现的开放问题，必须与权威文档同步。

补充文档，如 CMake 重构笔记、运维设计、模式草稿和历史重构摘要位于 `docs/` 下。如果它们与上述权威契约冲突，权威契约优先。

## 当前状态

设计尚未最终确定。在实现相应的运行时入口点和 Lua 绑定之前，应将 `examples/hello_world/` 下的示例视为目标 API 草图。

## Docker 构建

仓库提供了一个用于生产镜像构建的多阶段 [Dockerfile](Dockerfile)。从
git 工作区构建时，建议显式传入当前 commit hash，这样镜像内的
`shield --version` 仍然能显示源码修订号：

```bash
docker build \
  --build-arg SHIELD_GIT_COMMIT_HASH="$(git rev-parse HEAD)" \
  -t shield:latest .
```

如果构建发生在不包含 git 元数据的上下文里，镜像会回退为 `Unknown`
作为嵌入的 commit hash。
