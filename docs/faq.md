# FAQ

## Shield 当前是什么状态？

处于重构设计阶段。文档描述目标边界和 API 方向，源码仍包含旧架构遗留模块。

## Shield 是分布式框架吗？

`shield_core` 不是分布式框架。重构后的 core 聚焦服务、消息、定时器和 Lua coroutine 语义。

Shield 可以有官方 `shield_cluster` 可选模块，用于多进程/多机器通信、节点心跳、远端路由 cache 和可选服务发现。但它不进入 core，也不是最小运行路径。

## 为什么移出 DI/IoC、插件、Prometheus、健康检查？

这些能力会扩大 core 面积，让项目偏离 Skynet 风格的轻量运行时。当前优先把服务、消息、网络、定时器和 Lua API 做稳定；需要时作为官方可选模块或独立扩展接入。

## 还会支持 HTTP 吗？

当前 core 明确保留 TCP / UDP / WebSocket。HTTP 是否作为 core、transport 扩展或独立 ops 能力，需要后续单独决策，文档不再默认承诺。

## `examples/hello_world/` 可以直接运行吗？

目前应视为目标 API 草图。`include/shield/shield.hpp`、`shield::run(argc, argv)`、完整 `shield.*` Lua 绑定仍需要实现。

## 和 Skynet 的关系是什么？

Shield 参考 Skynet 的服务、消息和 Lua-first 思路，但使用 C++20 和 CAF 作为内部实现基础。Shield 不复制 Skynet harbor、snax、sharedata 等机制。

## 旧文档里的 `/health`、`/status`、Prometheus 怎么办？

这些不属于当前 core。需要时可以作为外部 sidecar 或后续独立扩展重新设计。
