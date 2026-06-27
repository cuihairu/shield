# 安全运行时语义

> 状态：设计草案，非当前实现契约。
>
> 本文描述的 sandbox 资源限制、permissions 权限矩阵、`network.tls`、`rate_limit` 等配置项**在当前 `RuntimeActorConfig` 中均未实现**（见 `include/shield/config/config.hpp`），属于 Phase 2+ 安全加固方向。若与 [配置语义](runtime-config.md) 或 [Lua API 契约](lua-api.md) 冲突，以那两份文档为准。

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

连接限制参数见 [网络语义](runtime-network.md#网络背压与限制)。

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

### 速率限制（网关层）

网关层限流是连接级防护，在消息进入业务逻辑前拦截。与业务级分布式限流（`shield.rate_limiter()`，见 [全局数据](runtime-global.md#五限流器)）不同：

| 层级 | 作用 | 存储 | 适用场景 |
|------|------|------|----------|
| 网关层 `rate_limit` | 按客户端 IP/连接限流 | 内存 | 防刷、防 DDoS |
| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | Redis | 全服限流、API 限流 |

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
function M.on_client_message(session, payload)
    -- 网关层限流：按客户端 IP 检查
    if not check_rate_limit(session:remote_addr()) then
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
local pending_auth = {}
local authenticated = {}

function M.on_connect(session)
    pending_auth[session:id()] = true
    session:send({ status = "auth_required" })
end

function M.on_client_message(session, payload)
    local sid = session:id()

    if pending_auth[sid] then
        if payload.type ~= "auth" then
            session:close("auth_required")
            return
        end

        local user = verify_token(payload.token)
        if not user then
            session:close("auth_failed")
            return
        end

        pending_auth[sid] = nil
        authenticated[sid] = user.id
        session:send({ status = "authenticated" })
        return
    end

    dispatch_authenticated_message(session, payload, authenticated[sid])
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
