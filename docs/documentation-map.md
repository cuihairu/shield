# 文档地图

本文说明 Shield 文档的阅读顺序、权威优先级和当前状态口径。遇到不同文档说法不一致时，按本页的优先级判断。

## 文档状态

| 状态 | 含义 |
| --- | --- |
| 权威契约 | 用户和实现都应遵守的稳定边界。代码、测试和示例应向这些文档收敛。 |
| 运行时语义 | 某个 runtime 子系统的细化规则。若与总纲冲突，以总纲为准；若与示例冲突，以语义文档为准。 |
| 参考文档 | 插件、部署、开发、运维等操作性说明。它应反映当前实现，不定义跨模块架构原则。 |
| 草案/归档 | 设计方向、业务模式或未来工具链。不能当作当前 runtime 已实现能力。 |

## 权威优先级

1. [架构总纲](architecture.md)：模块边界、依赖方向、核心非目标和扩展原则。
2. [Lua API 契约](lua-api.md)、[配置运行时语义](runtime-config.md)、[错误码参考](runtime-errors.md)：用户可见 API、配置和错误。
3. [插件系统 v1](plugin-system.md)、[插件参考](plugins/index.md)：插件包、实例、binding、ABI 和官方插件。
4. 各 `runtime-*.md`：服务、消息、定时器、Lua VM、网络、数据、日志、启动、安全等子系统语义。
5. [路线图](roadmap.md) 和 [Decision Log](open-decisions.md)：当前阶段、已关闭决策和后续范围。

## 推荐阅读路径

新用户先读：

- [快速上手](quickstart.md)
- [Lua API 契约](lua-api.md)
- [配置运行时语义](runtime-config.md)
- [插件参考](plugins/index.md)

框架开发者先读：

- [架构总纲](architecture.md)
- [核心设计理念](architecture-core-concepts.md)
- [运行时语义决策](runtime-semantics.md)
- [开发指南](development-guide.md)

插件作者先读：

- [插件系统 v1](plugin-system.md)
- [插件参考](plugins/index.md)
- 目标接口头文件对应的官方插件参考页

运维和部署先读：

- [部署](deployment.md)
- [配置运行时语义](runtime-config.md)
- [日志运行时语义](runtime-log.md)
- [运维语义](runtime-ops.md)

## 当前口径

Shield 当前口径是单节点优先、Lua-first、插件化后端能力。核心 runtime 和插件系统 v1 已进入当前实现路径；`shield_cluster`、`shield_global`、`shield_player`、`shield_server`、`shield_ops` 等官方可选模块按路线图推进。

旧 DI/IoC、annotations、conditions、events、middleware chain、旧插件 v0、旧 discovery/metrics/health core 集成不再作为兼容目标。相关历史文档若仍出现旧口径，以本页、[架构总纲](architecture.md) 和 [路线图](roadmap.md) 为准。

## 文档分区

| 分区 | 用途 |
| --- | --- |
| 入门与状态 | 快速上手、路线图、FAQ、部署和开发入口。 |
| 权威契约 | 总纲、Lua API、配置、错误码、插件系统和测试矩阵。 |
| 核心运行时 | 服务、消息、定时器、Lua VM、网络、数据、日志、启动、安全和 gateway。 |
| 插件参考 | 官方插件和第三方插件开发说明。 |
| 官方可选模块 | cluster/global/player/server/ops 等不属于 core 的扩展语义。 |
| 工程参考 | 目录结构、CMake、CAF 映射、性能、最佳实践和教程。 |
| 草案与归档 | schema 工具链、xmldef toolchain、xmldef descriptor/compiler/runtime spec、Unity generator spec、基础组件/运行时适配边界、实体模式、持久化与回档等未来方向或业务模式参考。 |

当前 `xmldef` 专项草案建议阅读顺序：

1. [Xmldef Toolchain Design](xmldef-toolchain-design.md)
2. [Xmldef Descriptor Spec](xmldef-descriptor-spec.md)
3. [Xmldef Phase 1 Implementation Plan](xmldef-phase1-implementation.md)
4. [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md)
5. [Xmldef Unity Generator Spec](xmldef-unity-generator-spec.md)

协议分层术语、`ProtocolProfile` 模型和当前支持矩阵，以 [Protocol Routing Design](protocol-routing-design.md) 为专项入口。

如果后续需要讨论 Lua primitives、cosocket 风格出站 I/O、CAF adapter 复用边界与 `auth-first` 的否定结论，可参考后置草案 [基础组件与运行时适配边界](runtime-primitives.md)。
