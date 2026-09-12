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
  - name: auth
    script: scripts/auth.lua
    instances: 1
```

Lua 服务只关心业务：

```lua
local M = {}

function M.ping(ctx)
    shield.send(ctx.sender, "pong", { time = shield.now() })
end

return M
```

## 当前状态

- `examples/hello_world/` 是可构建、可启动的完整示例：auth（TCP 监听 +
  预登录 login）→ player（认证后单一 target）→ room（私有转发 + s2c
  回包）的客户端 RPC 闭环，并配有两个 acceptance 测试（脚本/配置存在性、
  真实 TCP e2e：`tests/acceptance/test_client_rpc_e2e.cpp`）。
- `include/shield/shield.hpp` 和 `shield::run(argc, argv)` 已落地，并有 CLI smoke tests。
- 配置错误在启动期 fail fast；`--check-config` 可离线校验。
- Lua service 文件必须返回 table；`on_init(args)`、`on_exit(reason)` 与
  `send/call/self/sender/names/now/log` 已可用。
- `shield.call` 与 handler 内的 `shield.sleep` 走 coroutine-aware 路径；
  timer callback、fork task 与客户端 RPC handler 同样以协程方式执行，可
  在其中 `sleep`/`call` 而不阻塞 actor。
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
