# 快速上手

本文是重构后的目标体验说明。当前仍处设计阶段，命令和 API 需要等实现完成后再作为稳定用法。

## 目标体验

最终希望用户只需要一个 C++ 入口：

```cpp
#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);
}
```

然后通过 YAML 声明 Lua 服务：

```yaml
actors:
  - name: echo
    script: scripts/echo.lua
    instances: 1
```

Lua 服务只关心业务：

```lua
local M = {}

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

return M
```

## 当前状态

- `examples/hello_world/` 是目标 API 草图。
- `include/shield/shield.hpp` 和 `shield::run(argc, argv)` 尚未稳定。
- 当前源码仍保留旧 CLI、gateway、discovery、metrics 等模块。
- 文档中的命令不应再声称这是已完成的入门路径。

## 后续验收标准

当重构实现完成后，本页应更新为真实可执行步骤：

```bash
cmake -B build -S .
cmake --build build
./build/bin/hello_world --config examples/hello_world/config/app.yaml
```

在此之前，请以 [架构设计](architecture.md)、[API 说明](api.md) 和 [运行时语义决策稿](runtime-semantics.md) 为准。
