# 快速上手

本文是重构后的目标体验说明。当前 `shield::run`、CLI 解析、Phase 1 配置验证、Lua service module-table loader、按 `actors` 配置启动服务和最小单节点 Lua API 已经落地；完整 coroutine/mailbox 语义仍在实现中。

## 目标体验

最终希望用户只需要一个 C++ 入口：

```cpp
#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);
}
```

然后在 `app.yaml` 中声明 Lua 服务：

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

- `examples/hello_world/` 已接入统一 C++ 入口并可构建启动；完整 Lua 行为验收仍待补。
- `include/shield/shield.hpp` 和 `shield::run(argc, argv)` 已落地，并有 CLI smoke tests。
- `config/app.yaml` 已收敛为 Phase 1 最小配置；配置错误会在启动期 fail fast。
- Lua service 文件必须返回 table，当前已支持 `on_init(args)`、`on_exit(reason)`、最小 `spawn/send/call/self/sender/names/now/log` 路径。
- 当前源码仍保留旧 CLI、gateway、discovery、metrics 等 legacy 模块。
- 完整 coroutine-aware `shield.*` 语义和 `examples/hello_world/` 业务消息验收仍需要补齐。

## 后续验收标准

当前可验证的最小入口：

```bash
cmake -B build-msvc -S . -G Ninja -DSHIELD_BUILD_TESTS=OFF -DSHIELD_BUILD_EXAMPLES=OFF
cmake --build build-msvc --target shield
ctest --test-dir build-msvc -R "shield_(cli|config)_"
ctest --test-dir build-msvc -R "shield_runtime_lua_smoke"
```

hello world 示例入口：

```bash
cmake -B build-msvc-examples -S . -G Ninja -DSHIELD_BUILD_TESTS=OFF -DSHIELD_BUILD_EXAMPLES=ON
cmake --build build-msvc-examples --target hello_world
./build-msvc-examples/bin/hello_world --config examples/hello_world/config/app.yaml
./build-msvc-examples/bin/hello_world --check-config --config examples/hello_world/config/app.yaml
```

完整业务示例完成前，请以 [架构设计](architecture.md)、[Lua API 契约](lua-api.md) 和 [运行时语义决策稿](runtime-semantics.md) 为准。
