# 网络运行时语义

本文档包含 Shield 网络、transport 和 gateway 相关的运行时语义决策。

当前 Phase 1 只冻结 TCP session、gateway handler 桥接、`SessionHandle` 目标语义和 basic transport framing/protocol pipeline。
UDP、KCP、WebSocket 是目标能力和后续扩展方向，不作为 Phase 1 最小验收阻塞项。
HTTP 业务 gateway 不进入 core；HTTP 管理端点只在 `shield_ops` 显式启用时存在。

## 网络层职责划分

Shield 有两层网络：

| 层 | 职责 | 使用者 |
| --- | --- | --- |
| `shield_net` | 客户端连接管理、Session 生命周期 | Gateway 服务 |
| `shield_transport` | 协议解析（Phase 1: 解帧、编解码、加密；后续: KCP/UDP/WebSocket 适配） | shield_net 内部 |
| CAF middleman | 服务间通信、节点间通信 | 内部 Actor |

```
客户端 ──TCP──→ shield_net ──shield_transport──→ gateway
                                                              ↓
内部服务 ←────────────────────── CAF ←─────────────────── 业务路由
```

**shield_net 职责：**
- 监听客户端连接（Phase 1: TCP）
- 管理连接生命周期（accept、close、reconnect）
- 管理 Session 对象
- 调用 gateway 回调（on_connect、on_disconnect、on_client_message）

**shield_transport 职责：**
- 字节流解帧（framing）
- 协议部件执行：`Envelope`、`RouteExtractor`、`RoutePolicy`、`BodyCodec`、`RouteResolver`
- 数据压缩（compression）
- 数据加密（encryption）
- KCP 协议实现（后续）
- 包校验（packet validation）

**职责边界：**
- shield_net 不关心协议细节，只管理连接和 session
- shield_transport 不关心连接管理，只处理字节流
- gateway 的目标边界是 transport 已解码的业务消息；`ForwardRaw`、`Drop` 和协议错误应在 C++ 数据面结束。
- 当前真实 gateway Lua 路径已经收敛为只接收 `on_client_message(session, message)`；protocol 中间态不再透给 Lua。

固定 pipeline 视角下：

- `Envelope` 负责 frame 边界
- `RouteExtractor` 负责进站 route 提取
- `RoutePolicy` 负责决定 decode / forward / drop
- `BodyCodec` 负责业务消息编解码
- `RouteResolver` 负责出站 route 选择

## 传输协议支持

`shield_net` 的长期目标支持以下客户端传输协议：

| 协议 | Phase 1 状态 | 说明 | 适用场景 |
|------|------|------|----------|
| TCP | required | 可靠、有序、流式 | 默认选择，适合大多数场景 |
| UDP | deferred | 不可靠、无序、数据报 | 实时性要求高，允许丢包 |
| KCP | deferred | 快速可靠 ARQ 协议 | 游戏服务器，低延迟需求 |
| WebSocket | deferred | 基于 TCP 的全双工 | Web 客户端、浏览器 |

Phase 1 验收不得要求 UDP、KCP 或 WebSocket listener 可用。相关配置可以保留在文档示例中，但实现未启用时必须给出明确的配置错误或能力不可用错误，不能静默降级成 TCP。

### KCP 支持

KCP 属于 deferred transport extension；以下语义用于后续实现时保持与 TCP session API 一致。

KCP 是一个快速可靠的 ARQ 协议，相比 TCP：

- 更低的延迟（可配置重传间隔）
- 更好的拥塞控制（可选关闭）
- 更适合游戏场景

配置示例：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      kcp: "0.0.0.0:8001"
      kcp_options:
        nodelay: 1        # 启用 nodelay 模式
        interval: 10      # 内部刷新间隔 10ms
        resend: 2         # 快速重传触发次数
        nc: 1             # 关闭流控
```

Lua API：

```lua
-- KCP session 与 TCP session 接口一致
function M.on_client_message(session, payload)
    session:send(response)
    session:close()
end
```

## shield_net 与 gateway

`shield_net` 管理 listener、connection、session。配置了 `actors[].network.tcp` 的单实例 Lua service 会在 bootstrap 中启动 TCP listener，并将 session 事件转发到同名 service 的 gateway 回调；Phase 1 拒绝 `network.tcp` 与 `instances != 1` 的组合。

如果未配置 `actors[].network.protocol`，TCP session 使用 legacy frame path 并调用 `on_client_message(session, payload)`。如果配置了 `network.protocol`，TCP session 使用 `ProtocolPipeline`。这里的 pipeline 不是业务可任意拼接的动态节点链，而是固定骨架上的协议部件组合。真实运行时语义已经收敛为只有 `DecodeLocal` 结果进入 Lua 的 `on_client_message(session, message)`。

更长期的语义里，`network.protocol` 实际上是在为 listener 绑定一条 `ProtocolProfile`。系统内部可以同时存在多种 profile，但单条 session 一旦接受连接，就应固定绑定其中一种；不应允许同一连接在后续业务包中来回切换 `json/protobuf/sproto/...` 协议族。

业务 gateway 是 Lua service：

```lua
local M = {}

function M.on_connect(session)
end

function M.on_disconnect(session, reason)
end

function M.on_client_message(session, message)
end

return M
```

目标语义里，`message` 是业务消息而不是未解码 transport payload。若 `body.codec = raw`，则 `message` 可以是字节串，但它仍是显式 decode 结果。`ForwardRaw` 和 `Drop` 不应触发 Lua 回调。

实现快照：当前 protocol path 中，`json` 和 `msgpack` decode-local 都会把结构化业务消息作为 Lua table 送入 `on_client_message`；`raw` decode-local 会把字节串送入 `on_client_message`；`protobuf` 需要通过 `body.provider` 引用 `shield.protocol.codec.v1` 插件后才能进入 `DecodeLocal`；`ForwardRaw` 和 `Drop` 不触发 Lua 回调。尚未落地真实 provider 的 codec 当前不能进入 `DecodeLocal`。

如果未来需要同一 listener 支持多种协议族，推荐也只在连接建立首阶段协商一次 profile，并把协商结果固化到 session。正常业务包头不应重复携带“我是 protobuf/sproto/json”这类协议家族标识；包头只应包含当前 profile 自身执行所需的 route/type/session/flags 等字段。

gateway 负责：

- 登录鉴权。
- session 到 player/service 的映射。
- 包路由。
- 限流和业务校验。

core 不提供 middleware framework。

## SessionHandle

Lua 只看到 opaque `SessionHandle`。当前真实 TCP bridge 已直接把连接事件桥接成可用的 `SessionHandle` userdata；handle 内部通过 session id 回查 live `shield_net::Session`，因此不会把原始 transport payload 或 C++ 指针直接暴露成业务对象。

```lua
session:id()
session:send(payload)
session:close(reason)
session:remote_addr()
```

规则：

- Lua 不直接操作 gateway listener 对应的底层客户端 socket；业务侧通过 `SessionHandle` 与被接入的客户端连接交互。
- session send 是 non-blocking。
- backpressure 超限返回错误。
- session 断开后 handle stale，调用返回 `session_closed`。
- `SessionHandle` 不应跨 service 通过 `shield.send/call` 传递；跨服务只传 `session_id`，由 gateway 维护映射。
- 对绑定了 `network.protocol` 的 session，`session:send(payload)` 走固定出站 pipeline：先 resolve route，再做 body/envelope encode；其中 `raw` codec 发送字节串，`json/msgpack` 这类 structured codec 发送业务消息对象。对未绑定 protocol 的 session，仍按原始字节发送。

这不排斥未来在后置阶段提供独立的 Lua 出站 socket 原语。若后续引入 `shield.socket`，它的定位也应是“Lua 主动发起的出站连接”，而不是取代 listener/session/gateway 体系。相关方向目前只保留为后置草案，见 [基础组件与运行时适配边界](runtime-primitives.md)。

## 网络背压与限制

网络和 transport 必须有显式限制：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `max_connections` | 10000 | 最大并发连接数 |
| `max_connections_per_ip` | 100 | 单 IP 最大连接数 |
| `max_frame_size` | 64KB | 单帧最大体积 |
| `max_session_send_queue` | 1000 | 单 session 待发送消息条数上限（非字节；每条消息体积受 `max_frame_size` 约束） |
| `max_decode_errors` | 10 | 连续解码错误次数上限（超过断开） |
| `read_idle_timeout` | 60s | 读空闲超时 |
| `write_idle_timeout` | 30s | 写空闲超时 |
| `handshake_timeout` | 10s | 握手超时 |

配置示例：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      max_connections: 50000
      max_connections_per_ip: 200
      max_frame_size: 131072       # 128KB
      read_idle_timeout: 120000    # 2 分钟
```

超过限制时优先返回错误或主动断开，不允许无界队列。

### 背压行为

```
发送队列满时：
  - 返回 session_send_queue_full 错误
  - 业务层决定丢弃或等待

连接数达上限时：
  - 新连接直接拒绝（TCP RST）
  - 记录拒绝日志

解码错误达上限时：
  - 主动断开连接
  - 记录 IP 和错误详情
```

## I/O 线程模型

网络层采用「多线程 I/O + 单线程业务逻辑」模型，与主流游戏引擎在业务侧的单线程简化保持一致：

- **I/O 线程池**：由顶层 `net.threads` 控制并发执行 `io_context::run()` 的线程数。这些线程只做 socket 读写、frame/protocol 解码，以及 per-session strand 上串行化的回调派发；**不执行任何 Lua 业务代码**。
- **per-session strand**：每条 session 的读、写、idle timer 完成事件绑定到独立 strand，即使跨 N 个 I/O 线程，单条 session 的事件依然严格串行、有序。
- **业务逻辑单线程**：所有 Lua handler 经 `LuaGatewayBridge` 投递到 `LuaServiceManager` 邮箱，由唯一的 worker 线程排干。net 线程永远不内联跑 Lua。
- **默认单线程**：`net.threads = 0`（默认）退回 legacy 单 I/O 线程路径；多线程是显式 opt-in，仅在连接数高、单线程 syscall/解码成为瓶颈时开启。

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `net.threads` | 0 | I/O 线程数；0 = 单 I/O 线程（legacy），范围 0..64 |

`net.threads` 是顶层配置，不属于 `actors[].network`：

```yaml
net:
  threads: 4
actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
```

解码（frame/protocol pipeline）当前在 I/O 线程的 strand 上执行而非业务线程——解码是 CPU 密集且无共享状态的工作，并行化通常是有益的。若需要「一切计算都进单业务线程」，可后续将解码也挪进 worker，但当前设计选择前者。
