# 网络运行时语义

本文档包含 Shield 网络、transport 和 gateway 相关的运行时语义决策。

当前 Phase 1 只冻结 TCP session、`SessionHandle` 和 basic transport framing。
UDP、KCP、WebSocket 是目标能力和后续扩展方向，不作为 Phase 1 最小验收阻塞项。
HTTP 业务 gateway 不进入 core；HTTP 管理端点只在 `shield_ops` 显式启用时存在。

## 网络层职责划分

Shield 有两层网络：

| 层 | 职责 | 使用者 |
| --- | --- | --- |
| `shield_net` | 客户端连接管理、Session 生命周期 | Gateway 服务 |
| `shield_transport` | 协议解析（解帧、编解码、加密）、KCP 实现 | shield_net 内部 |
| CAF middleman | 服务间通信、节点间通信 | 内部 Actor |

```
客户端 ──TCP/UDP/KCP/WebSocket──→ shield_net ──shield_transport──→ gateway
                                                              ↓
内部服务 ←────────────────────── CAF ←─────────────────── 业务路由
```

**shield_net 职责：**
- 监听客户端连接（TCP/UDP/KCP/WebSocket）
- 管理连接生命周期（accept、close、reconnect）
- 管理 Session 对象
- 调用 gateway 回调（on_connect、on_disconnect、on_client_message）

**shield_transport 职责：**
- 字节流解帧（framing）
- 协议编解码（codec）
- 数据压缩（compression）
- 数据加密（encryption）
- KCP 协议实现
- 包校验（packet validation）

**职责边界：**
- shield_net 不关心协议细节，只管理连接和 session
- shield_transport 不关心连接管理，只处理字节流
- gateway 收到的是已解码的消息，不直接操作字节流

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

`shield_net` 管理 listener、connection、session。

业务 gateway 是 Lua service：

```lua
local M = {}

function M.on_connect(session)
end

function M.on_disconnect(session, reason)
end

function M.on_client_message(session, payload)
end

return M
```

gateway 负责：

- 登录鉴权。
- session 到 player/service 的映射。
- 包路由。
- 限流和业务校验。

core 不提供 middleware framework。

## SessionHandle

Lua 只看到 opaque `SessionHandle`。

```lua
session:id()
session:send(payload)
session:close(reason)
session:remote_addr()
```

规则：

- Lua 不直接操作 socket。
- session send 是 non-blocking。
- backpressure 超限返回错误。
- session 断开后 handle stale，调用返回 `session_closed`。
- `SessionHandle` 不应跨 service 通过 `shield.send/call` 传递；跨服务只传 `session_id`，由 gateway 维护映射。

## 网络背压与限制

网络和 transport 必须有显式限制：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `max_connections` | 10000 | 最大并发连接数 |
| `max_connections_per_ip` | 100 | 单 IP 最大连接数 |
| `max_frame_size` | 64KB | 单帧最大体积 |
| `max_session_send_queue` | 1000 | 单 session 发送队列上限 |
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
