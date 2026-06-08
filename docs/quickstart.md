# 快速上手

5 分钟启动一个 Shield 游戏服务器。

## 前置要求

- C++20 编译器（MSVC / GCC 11+ / Clang 14+）
- CMake 3.30+
- [vcpkg](https://vcpkg.io/)（依赖管理）
- Lua 5.4+（通过 vcpkg 安装）

## 1. 获取并构建

```bash
git clone https://github.com/cuihairu/shield.git
cd shield

# 设置 vcpkg 路径
export VCPKG_ROOT=/path/to/vcpkg

# 一键构建
./build.sh release
```

Windows：

```cmd
set VCPKG_ROOT=C:\path\to\vcpkg
build.bat release
```

## 2. 启动服务器

```bash
./build/bin/shield server --config config/app.yaml
```

启动日志：

```
[INFO] Shield Server starting...
[INFO] Script Starter initialized successfully
[INFO] Actor Starter initialized successfully
[INFO] Service Starter initialized successfully
[INFO] Gateway Starter initialized successfully
[INFO] HTTP server started on port 8082
[INFO] WebSocket server started on port 8083
[INFO] Application running. Press Ctrl+C to exit.
```

## 3. 验证

```bash
# 健康检查
curl http://localhost:8082/health

# 运行时状态
curl http://localhost:8082/status

# 游戏动作
curl -X POST http://localhost:8082/api/game/action \
  -H "Content-Type: application/json" \
  -d '{"action":"ping"}'
```

## 4. 编写 Lua 服务

创建 `scripts/my_service.lua`：

```lua
local state = { count = 0 }

function on_init()
    log_info("My service initialized")
end

function on_message(msg)
    if msg.type == "ping" then
        state.count = state.count + 1
        return {
            success = true,
            data = {
                message = "pong",
                count = tostring(state.count)
            }
        }
    end

    -- 调用其他服务
    if msg.type == "get_player" then
        local result = shield.call("player_manager", "get_info", {
            player_id = msg.data.player_id or ""
        })
        return result
    end

    return { success = false, error_message = "Unknown: " .. msg.type }
end
```

## 5. WebSocket 连接

```javascript
const ws = new WebSocket("ws://localhost:8083/ws");
ws.onopen = () => ws.send(JSON.stringify({ type: "ping" }));
ws.onmessage = (e) => console.log(JSON.parse(e.data));
```

## 6. 调试控制台

```bash
telnet localhost 13000
# list          — 列出服务
# info <name>   — 服务详情
# call <name> <json>  — 同步调用
```

## 下一步

- [架构设计](architecture.md) — 整体架构和核心概念
- [Skynet 对比](skynet-comparison.md) — 与 Skynet 的区别
- [CAF 映射](caf-mapping.md) — CAF 提供什么，Shield 添加什么
- [游戏后端教程](tutorial-game-backend.md) — 登录/房间/聊天完整示例
- [API 说明](api.md) — 当前 API 文档维护策略
