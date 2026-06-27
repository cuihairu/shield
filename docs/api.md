# API 说明

Shield 的 API 仍处于重构设计阶段。权威契约分布在以下文档，本页仅作导航，不再内联摘要——摘要会随契约演进过时，且曾包含已废弃的 `shield.db:*` / `shield.redis:*` 旧形式（数据访问现统一走插件 namespace `shield.database.*`，见 lua-api.md）。

## 文档导航

- **[Lua API 契约](./lua-api.md)** — Lua 用户 API 的权威入口（`shield.spawn/send/call/...`、插件 namespace `shield.database.*` 等）。
- **[Lua API 测试用例](./lua-api-tests.md)** — API 验收矩阵。
- **[架构总纲](./architecture.md)** — 重构架构总纲。
- **[运行时语义决策稿](./runtime-semantics.md)** — A1-A35 运行时语义决策。
- **[快速开始](./quickstart.md)** — C++ 入口 `shield::run` 与最小示例。
- **[游戏后端教程](./tutorial-game-backend.md)** — 端到端 Lua 示例。

## 实现状态

- `examples/hello_world/` 是用户参考示例，不作为 API 正确性的唯一验收。
- `include/`、`src/`、`tests/` 反映当前实现状态，但仍含旧架构遗留模块。
- API 稳定前，不维护按模块展开的完整 API 手册。
- 当前源码中的 Lua 绑定与目标契约仍有差距，以代码为准；旧的 `shield.service`、冒号式 DB/Redis 调用和 `on_message(src, type, data)` 不再进入重构目标。

## C++ API 目标

目标是提供一个单一入口：

```cpp
#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);
}
```

该入口尚未稳定。当前源码仍使用 CLI command 入口。
