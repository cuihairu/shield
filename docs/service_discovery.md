# 服务发现

服务发现不属于当前重构后的 `shield_core`。多节点场景由 `shield_cluster` 官方可选模块承接。

## 决策

Shield 的 core 目标是单节点优先运行时。core 不负责：

- 节点注册。
- 服务发现后端。
- Redis / Nacos / Consul / Etcd 集成。
- 集群路由或负载均衡。

`shield_core` 只维护本地 `ServiceRegistry`。服务名是本地 alias，不是全局服务发现记录。

## 模块归属

需要多节点时，优先归入 `shield_cluster`。在该模块稳定前，用户仍可以在业务层或外部基础设施中实现：

- 静态配置。
- Kubernetes Service。
- 自定义 Redis / Etcd 注册。
- 独立网关或 sidecar。

这些方案不进入 core 和第一阶段最小启动主路径。

## shield_cluster

多进程或多机器能力应作为独立模块：

| 模块 | 范围 |
| --- | --- |
| `shield_cluster` | 跨进程/跨机器通信、节点心跳、远端路由 cache |

共同约束：

- `NodeId` 在同一部署内必须唯一。
- handshake 必须携带 `node_id` 和 `node_epoch`。
- 重复 `NodeId` 必须拒绝连接。
- heartbeat 驱动 `online/suspect/offline/removed`。
- remote name 是 cache，不进入 `shield_core` 的本地 registry。

完整 ID、heartbeat 和 cluster 预留规则见 [运行时语义决策稿](./runtime-semantics.md)。

## 旧代码状态

仓库中仍可能存在 `include/shield/discovery/` 和 `src/discovery/`。这些属于旧架构遗留代码，后续需要删除、归档或移到明确的非核心实验区。
