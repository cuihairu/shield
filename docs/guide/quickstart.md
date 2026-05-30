# 快速开始

## 创建 Lua 服务

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

## 配置

创建 `config/app.yaml`：

```yaml
app:
  name: "My Game"

log:
  global_level: "info"
  console:
    enabled: true

lua:
  script_dir: "scripts/"
  auto_reload: true
  preload_scripts:
    - "init.lua"

gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  http:
    enabled: true
    port: 8082
  websocket:
    enabled: true
    port: 8083
```

## 运行

```bash
./build/bin/shield server --config config/app.yaml
```

## 测试

```bash
# 健康检查
curl http://localhost:8082/health

# 发送游戏动作
curl -X POST http://localhost:8082/api/game/action \
  -H "Content-Type: application/json" \
  -d '{"type":"ping"}'
```

## WebSocket 连接

```javascript
const ws = new WebSocket("ws://localhost:8083/ws");
ws.onopen = () => ws.send(JSON.stringify({ type: "ping" }));
ws.onmessage = (e) => console.log(JSON.parse(e.data));
```

## 下一步

- [游戏后端教程](../tutorial-game-backend.md) — 登录/房间/聊天完整示例
- [架构设计](../architecture.md) — 整体架构
- [API 参考](../api/README.md) — 模块 API
