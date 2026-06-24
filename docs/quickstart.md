# 快速上手

本文说明当前推荐的最小启动路径。`shield::run`、CLI 解析、Phase 1 配置验证、Lua service module-table loader、按 `actors` 配置启动服务、基础消息/定时器/coroutine 语义和插件系统 v1 已进入当前实现路径。

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
- `shield.call` / `shield.call_timeout` 和 handler 内的 `shield.sleep` 已走 coroutine-aware 路径；timer callback 和 fork task 当前通过受保护的非协程调用执行。
- 后端能力通过插件系统 v1 提供；没有声明实例或 binding 时，业务代码必须按不可用能力处理。

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

进一步的 API 细节以 [架构设计](architecture.md)、[Lua API 契约](lua-api.md)、[配置运行时语义](runtime-config.md) 和 [插件参考](plugins/index.md) 为准。
