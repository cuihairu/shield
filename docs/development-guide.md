# 开发指南

本文面向重构阶段开发者。当前目标是先把文档、边界和 API 契约统一，再逐步调整源码。

## 开发原则

- 先读 `docs/architecture.md`。
- Lua 用户 API 以 `docs/lua-api.md` 为准。
- Lua API 测试按 `docs/lua-api-tests.md` 设计。
- CMake target 拆分按 `docs/cmake-refactor.md` 推进。
- 不把旧模块继续视为 core 事实。
- 新代码应围绕 `actor`、`net`、`transport`、`script`、`timer`、`data`、`config`、`log` 收敛。
- 不在 `shield_core` 或最小主路径新增 DI/IoC、插件、服务发现、Prometheus、健康检查、中间件链依赖。
- 多进程/多机器能力归入 `shield_cluster` 官方可选模块，不能反向污染 core。
- Lua API 必须有绑定测试。
- 用户侧 API 不暴露 CAF。

## 当前源码状态

源码仍保留旧架构目录，例如：

- `discovery/`
- `metrics/`
- `health/`
- `di/`
- `annotations/`
- `conditions/`
- `events/`
- `plugin` 相关代码
- 独立 `protocol/`
- 部分 `gateway/` 中间件代码

这些目录后续需要删除、合并或移到非核心实验区。

## 目标目录

```text
include/shield/
├── shield.hpp
├── actor/
├── net/
├── transport/
├── script/
├── timer/
├── data/
├── config/
└── log/
```

官方可选模块放在独立路径，例如 `src/optional/shield_cluster/`、`src/optional/shield_global/`、`src/optional/shield_ops/`。

## 构建

构建流程仍以当前 CMake 为准，但 CMake 目标也需要随重构调整。

```bash
cmake -B build -S .
cmake --build build
```

在 `shield::run(argc, argv)` 和最终 CLI 冻结前，不要把当前命令行形态写成稳定用户文档。

## 提交前检查

- 文档是否仍声称旧 Phase 1-7 已完成。
- 是否把非 core 能力描述成默认能力。
- 是否新增了 core 对旧模块的依赖。
- 是否把 cluster/global/ops 误写成最小主路径依赖。
- 示例是否和 `docs/lua-api.md` 的契约冲突。
