# 安全运行时语义

本文档包含 Shield 安全机制相关的运行时语义决策。

## 安全原则

- 默认安全：不暴露不必要的能力
- 最小权限：服务只拥有完成任务所需的最小权限
- 纵深防御：多层安全机制，不依赖单一防线

## Lua 沙箱

每个 Lua 服务运行在独立的 Lua VM 中，天然隔离。

### 隔离能力

| 能力 | 隔离方式 |
|------|----------|
| 全局变量 | 服务间不共享 |
| 内存 | 独立 VM，可配置限制 |
| 文件系统 | 可选限制访问路径 |
| 网络 | 只能通过 shield.* API |
| 系统调用 | 不暴露 os.execute 等危险函数 |

### 可配置限制

```yaml
actors:
  - name: player
    script: scripts/player.lua
    sandbox:
      max_memory: 100MB          # 最大内存
      max_instructions: 1000000  # 最大指令数（防止死循环）
      allowed_modules:           # 允许加载的模块
        - "json"
        - "string"
      blocked_functions:         # 禁用的函数
        - "os.execute"
        - "io.popen"
        - "loadfile"
        - "dofile"
```

## 服务间权限

### 默认权限

服务间可以自由调用，不强制限制。

### 可选权限控制

对于需要严格控制的场景，可配置服务间调用权限：

```yaml
actors:
  - name: admin
    script: scripts/admin.lua
    permissions:
      allow_call: ["gateway", "player"]  # 只允许调用这些服务
      deny_call: ["payment"]             # 禁止调用这些服务

  - name: payment
    script: scripts/payment.lua
    permissions:
      allow_call_from: ["gateway"]       # 只允许被这些服务调用
```

### 权限检查

权限检查在 `shield.call` 和 `shield.send` 时进行：

```lua
-- 调用时检查权限
local ok, result = shield.call("payment", "charge", amount)
if not ok and result.code == "permission_denied" then
    shield.log.error("no permission to call payment service")
end
```

## 网络安全

### 连接限制

```yaml
network:
  tcp: "0.0.0.0:8001"
  max_connections: 10000           # 最大连接数
  max_connections_per_ip: 100      # 单 IP 最大连接数
  connection_timeout: 30000        # 连接超时
  idle_timeout: 300000             # 空闲超时
```

### TLS/DTLS 支持

```yaml
network:
  tcp: "0.0.0.0:8001"
  tls:
    enabled: true
    cert: "certs/server.crt"
    key: "certs/server.key"
    ca: "certs/ca.crt"             # 客户端证书验证（可选）
```

### 速率限制

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    rate_limit:
      requests_per_second: 1000    # 每秒请求数
      burst_size: 100              # 突发大小
      per_client: true             # 按客户端限制
```

Lua 层实现：

```lua
function M.on_message(session, payload)
    -- 检查速率限制
    if not rate_limiter:check(session:remote_addr()) then
        session:send({ error = "rate_limited" })
        return
    end

    -- 处理消息
    process_message(session, payload)
end
```

## 敏感数据保护

### 配置中的敏感数据

```yaml
# 不推荐：明文密码
database:
  password: "my_password"

# 推荐：环境变量
database:
  password: ${DB_PASSWORD}

# 推荐：密钥管理服务
database:
  password_secret: "vault://secret/data/database"
```

### 日志中的敏感数据

```lua
-- 不推荐：记录敏感信息
shield.log.info("user login: " .. username .. " password: " .. password)

-- 推荐：脱敏处理
shield.log.info("user login: " .. username)
shield.log.debug("password length: " .. #password)  -- 仅调试模式记录长度
```

### 消息中的敏感数据

```lua
-- 业务层处理
function M.get_player(uid)
    local player = find_player(uid)
    if player then
        -- 脱敏后返回
        return {
            id = player.id,
            name = player.name,
            phone = mask_phone(player.phone),  -- 138****1234
            email = mask_email(player.email),  -- t***@example.com
        }
    end
end
```

## 认证与授权

### 客户端认证

Gateway 服务负责客户端认证：

```lua
function M.on_connect(session)
    -- 等待认证消息
    local auth_msg = session:recv(5000)  -- 5秒超时
    if not auth_msg then
        session:close("auth_timeout")
        return
    end

    -- 验证 token
    local user = verify_token(auth_msg.token)
    if not user then
        session:close("auth_failed")
        return
    end

    -- 绑定用户到 session
    session:set_user(user)
    session:send({ status = "authenticated" })
end
```

### 服务间认证

服务间调用默认信任（同一进程内），跨节点时可选认证：

```yaml
cluster:
  auth:
    type: token                    # token | mtls
    token: ${CLUSTER_TOKEN}
```

## 审计日志

重要操作记录审计日志：

```lua
function M.change_password(uid, new_password)
    -- 记录审计日志
    audit_log("password_change", {
        uid = uid,
        ip = session:remote_addr(),
        time = shield.now(),
    })

    -- 执行操作
    update_password(uid, new_password)
end
```

审计日志格式：

```json
{
  "timestamp": 1234567890,
  "event": "password_change",
  "uid": "user123",
  "ip": "192.168.1.100",
  "result": "success"
}
```
