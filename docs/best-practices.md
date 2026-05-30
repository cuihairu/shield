# 最佳实践

## 服务拆分

按业务功能拆分 Lua 服务，每个服务一个 `.lua` 文件：

```lua
-- scripts/room.lua — 房间管理服务
function on_message(msg)
    if msg.type == "create" then
        -- 创建房间
    elseif msg.type == "join" then
        -- 加入房间
    end
end
```

避免过度拆分：一个服务处理一类业务，而非一个 API 一个服务。

## Actor 设计

### 单一职责

每个 Lua 服务只处理一类业务。战斗、背包、聊天应分开。

### 状态管理

```lua
local player_state = {
    level = 1,
    experience = 0
}

function update_experience(exp_gain)
    if exp_gain <= 0 then
        return false, "exp must be positive"
    end
    player_state.experience = player_state.experience + exp_gain
    check_level_up()
    return true
end
```

### 异步优先

优先使用 `shield.send()` 异步消息，仅在需要返回值时使用 `shield.call()`：

```lua
-- 异步通知（不阻塞）
shield.send("room_manager", "player_joined", { room_id = "lobby" })

-- 同步调用（需要结果）
local result = shield.call("player_manager", "get_info", { id = "123" }, 3000)
```

## 消息格式

统一使用 `type` + `data` 结构：

```lua
-- 发送
shield.send("service_name", "message_type", {
    key1 = "value1",
    key2 = "value2"
})

-- 接收处理
function on_message(msg)
    if msg.type == "message_type" then
        local key1 = msg.data.key1 or ""
        -- 处理逻辑
    end
end
```

## 输入验证

所有 Lua 服务入口都要验证输入：

```lua
function on_message(msg)
    if not msg.data or type(msg.data) ~= "table" then
        return { success = false, error_message = "Invalid data" }
    end

    local player_id = msg.data.player_id or ""
    if player_id == "" then
        return { success = false, error_message = "Missing player_id" }
    end

    -- 业务逻辑...
end
```

## 网关中间件

利用内置中间件处理横切关注点：

- `logging_middleware()` — 请求日志
- `cors_middleware()` — CORS 头
- `auth_middleware(validator)` — 认证

## 配置管理

- 敏感信息（密钥、密码）通过环境变量传递
- 开发/测试/生产使用不同配置文件
- 参考 `templates/` 目录的配置模板

## 日志规范

```lua
-- 按级别记录
log_info("Player logged in: " .. username)
log_warning("Rate limit exceeded: " .. player_id)
log_error("Database connection failed: " .. err)
```

## 监控

启用 Prometheus 指标和健康检查：

```yaml
metrics:
  enabled: true
  port: 9090
```

定期检查 `GET /health` 和 `GET /status` 端点。
