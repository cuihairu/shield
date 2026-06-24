# FAQ

## Shield 当前是什么状态？

处于重构设计阶段。文档描述目标边界和 API 方向，源码仍包含旧架构遗留模块。

## Shield 是分布式框架吗？

`shield_core` 不是分布式框架。重构后的 core 聚焦服务、消息、定时器和 Lua coroutine 语义。

Shield 可以有官方 `shield_cluster` 可选模块，用于多进程/多机器通信、节点心跳、远端路由 cache 和可选服务发现。但它不进入 core，也不是最小运行路径。

## 为什么移出 DI/IoC、插件、Prometheus、健康检查？

这些能力会扩大 core 面积，让项目偏离 Skynet 风格的轻量运行时。当前优先把服务、消息、网络、定时器和 Lua API 做稳定；需要时作为官方可选模块或独立扩展接入。

## 还会支持 HTTP 吗？

Phase 1 只冻结 TCP session、`SessionHandle` 和 basic transport framing。UDP、KCP、WebSocket 是后续扩展目标，不作为第一版最小验收阻塞项。

HTTP 不进入 `shield_core` 第一版，也不作为业务 gateway 的默认 transport。第一版只允许 `shield_ops` 在显式启用时提供 HTTP 管理端点，例如 `/ops/health`、`/ops/status`、`/ops/metrics`。业务 HTTP server、REST router、middleware chain 和 Web framework 集成均推迟到独立 transport 扩展或应用层自行实现。

## `examples/hello_world/` 可以直接运行吗？

目前最小路径已可验证：`include/shield/shield.hpp`、`shield::run(argc, argv)`、CLI/config smoke tests、默认 Phase 1 配置、`examples/hello_world` 构建启动路径和 Lua 业务消息验收均已落地。真实多节点、UDP/KCP/WebSocket、官方可选模块和真实后端压力验证仍按路线图推进。

## 和 Skynet 的关系是什么？

Shield 参考 Skynet 的服务、消息和 Lua-first 思路，但使用 C++20 和 CAF 作为内部实现基础。Shield 不复制 Skynet harbor、snax、sharedata 等机制。

## 旧文档里的 `/health`、`/status`、Prometheus 怎么办？

这些不属于当前 core。`/ops/health`、`/ops/status`、`/ops/metrics` 归 `shield_ops` 官方可选模块；Prometheus exporter 也是 `shield_ops` 的可选能力。关闭 `shield_ops` 时，runtime 不暴露任何内置 HTTP 管理端点。
