# API 说明

Shield 的权威契约分布在以下文档，本页仅作导航，不再内联摘要——摘要会随契约演进过时，且曾包含已废弃的 `shield.db:*` / `shield.redis:*` 旧形式（数据访问现统一走插件 namespace `shield.database.*`，见 lua-api.md）。

## 文档导航

- **[Lua API 契约](./lua-api.md)** — Lua 用户 API 的权威入口（`shield.spawn/send/call/...`、插件 namespace `shield.database.*` 等）。
- **[Lua API 测试用例](./lua-api-tests.md)** — API 验收矩阵。
- **[架构总纲](./architecture.md)** — 重构架构总纲。
- **[运行时语义决策稿](./runtime-semantics.md)** — A1-A35 运行时语义决策。
- **[快速开始](./quickstart.md)** — C++ 入口 `shield::run` 与最小示例。
- **[游戏后端教程](./tutorial-game-backend.md)** — 端到端 Lua 示例。

## 实现状态

- `examples/hello_world/` 是用户参考示例，不作为 API 正确性的唯一验收。
- `include/`、`src/`、`tests/` 反映当前实现状态。
- API 稳定前，不维护按模块展开的完整 API 手册。
- Lua 绑定已按契约实现（`shield.event` 等个别项标为目标契约，见 lua-api.md 各节实现快照）；旧的 `shield.service`、冒号式 DB/Redis 调用和 `on_message(src, type, data)` 已废弃。

## C++ API 目标

目标是提供一个单一入口：

```cpp
#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);
}
```

该入口已实现：`src/main.cpp` 与 `examples/hello_world/main.cpp` 均直接调用 `shield::run(argc, argv)`。
