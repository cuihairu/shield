# 运行时语义决策稿

本文记录 Shield 重构后的运行时语义决策。当前仍处于设计阶段，本文是编码实现的目标契约，不代表源码已经全部完成。

目标是让后续实现者不需要重新讨论基础语义，可以按本文拆分任务、补测试、替换旧实现。

## 设计原则

- `shield_core` 只承载 Actor/Service 核心语义。
- CAF 是内部机制，不出现在 Lua API 和 public C++ API。
- Lua 是默认业务语言，C++ 负责运行时和少量性能敏感扩展。
- 本地、IPC、cluster 的消息语义必须尽量一致。
- 第一版优先单节点稳定，cluster 和 hot reload 先定边界，不强行实现。

## 语义文档索引

运行时语义已按专题拆分为独立文档：

| 文档 | 内容 |
| --- | --- |
| [服务语义](runtime-service.md) | ServiceHandle、ServiceId、Service Registry、spawn、self、Lua service module、handler 上下文、exit |
| [消息语义](runtime-messaging.md) | MessageEnvelope、MessagePayload、send、call、背压、QoS、超时、nested call、coroutine 调度、错误处理 |
| [玩家生命周期](runtime-player.md) | PlayerSession、PlayerManager、认证、断线重连、离线消息缓存、多设备策略 |
| [服务器状态](runtime-server.md) | ServerManager、服务器状态、运行时信息、状态变更通知 |
| [全局数据](runtime-global.md) | GlobalData、跨进程共享数据、分布式锁（重入、续期）、排行榜、限流器、Pub/Sub |
| [定时器语义](runtime-timer.md) | timer API、时间语义、错误语义、sleep、fork、TaskHandle |
| [Lua VM 语义](runtime-lua-vm.md) | Lua VM 模型、热更新策略、Blue-Green 替换 |
| [网络语义](runtime-network.md) | shield_net、shield_transport、gateway、SessionHandle、KCP 支持 |
| [数据语义](runtime-data.md) | shield_data、连接池、DB/Redis API、事务、错误处理 |
| [日志语义](runtime-log.md) | 日志级别、结构化格式、上下文注入、轮转、审计日志 |
| [配置语义](runtime-config.md) | YAML 配置 schema、配置验证、环境差异 |
| [启动流程](runtime-bootstrap.md) | 启动顺序、关闭顺序、超时、信号处理、优雅重启 |
| [运维语义](runtime-ops.md) | shield_ops、运维端点、metrics、健康检查 |
| [安全语义](runtime-security.md) | Lua 沙箱、服务间权限、网络安全、敏感数据保护 |
| [集群语义](runtime-cluster.md) | shield_cluster、节点发现、负载均衡、旧代码处理 |

## 实现顺序建议

后续编码按以下顺序推进，避免循环依赖。

### M1. 基础类型与 target

- 建立 `shield_base`。
- 定义 `Result<T>`、`Error`、`ByteBuffer`、`TimePoint`。
- 定义 `ServiceId`、`NodeId`、`ServiceAddress`。
- 增加 public header forbidden CAF 检查。

### M2. Service identity 与 registry

- 实现 `ServiceHandle`。
- 实现 `ServiceRegistry`。
- 支持 name reserve/publish/unregister/query。
- 支持 owner exit 自动清理。
- 增加 name conflict、invalid name、stale handle 测试。

### M3. Envelope 与 payload

- 实现 `MessageEnvelope`。
- 实现 `MessagePayload`。
- 实现 LuaPack 编码接口。
- 支持 method、argc、args。
- 增加 nil、false、多返回值、ServiceHandle 编码测试。

### M4. spawn/send/call

- 实现 coroutine-aware `spawn`。
- 实现 non-blocking `send`。
- 实现 `call` pending registry。
- 实现 timeout 和 late response 丢弃。
- 增加 self-send、stale handle、mailbox full 测试。

### M5. Lua service lifecycle

- 实现 module table loader。
- 实现 `on_init` / method dispatch / `on_exit`。
- 实现 `shield.self`、`shield.sender`、`shield.names`。
- 增加 init failure rollback、method_not_found、handler_error 测试。

### M6. timer、sleep、fork

- 实现 `TimerId`。
- 实现 `timer_once`、`timer`、`cancel_timer`。
- 实现 `sleep`。
- 实现 `fork` 和 `TaskHandle`。
- 增加 fixed-delay、timer error stop、service exit auto-cancel 测试。

### M7. net/gateway/data/ops

- 整理 `shield_transport` 和 `shield_net` 边界。
- 实现 gateway Lua callback 草图。
- 保留 data 原始 DB/Redis API。
- 实现 ops snapshot 和本地 diagnostics。

### M8. IPC/cluster 预留

- 不把 cluster 放进 core。
- 为 route key 保留 `node_epoch`。
- 为 ops 增加 node heartbeat 状态模型。
- 后续独立实现 `shield_cluster`。
