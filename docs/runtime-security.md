# 安全运行时语义

> 状态：部分实现 + 部分设计草案。
>
> **已实现**：`lua.sandbox.allow_os` / `lua.sandbox.allow_io`（全局级
> VM 标准库开关，见 [配置语义](runtime-config.md)；未设置时保持历史
> 行为=开放，随仓库分发的默认配置声明两者为 false）；网关层连接级
> `rate_limit`（令牌桶）与 `blocklist.deny`（accept 时按地址/CIDR 拒绝，
> 见下文「速率限制」与「地址黑名单」）。
>
> **未实现（Phase 2+ 草案）**：下文 per-actor sandbox 资源限制
> （max_instructions/allowed_modules 等）、permissions 权限矩阵、
> `network.tls`——这些在当前 `RuntimeActorConfig` 中均未实现（见
> `include/shield/config/config.hpp`）。若与 [配置语义](runtime-config.md)
> 或 [Lua API 契约](lua-api.md) 冲突，以那两份文档为准。

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
| 网关层 `rate_limit` | 按连接限流（每连接一个令牌桶） | 内存 | 防刷、防 DDoS |
| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | Redis | 全服限流、API 限流 |

```yaml
actors:
  - name: gateway
    script: scripts/auth.lua
    network:
      tcp: "0.0.0.0:8001"
      rate_limit:
        messages_per_second: 1000   # 每秒补充的令牌数（0 = 不限流）
        burst: 100                  # 桶深度（0 = 等于 messages_per_second）
```

语义要点：

- **令牌桶，每连接一个**，随连接创建、由 listener 下发给 `TcpSession`。
  桶初始为满（可立即通过 `burst` 条），之后按 `messages_per_second`
  连续补充；补充是**惰性**的（按经过时间折算），空闲连接不占任何定时器。
- **按解码后的消息计费**，不是按 TCP 读事件。把多帧打包进一个 TCP 段
  不能绕过预算——批量投递与逐条投递消耗同样的令牌。
- **超限帧直接丢弃，不回写错误帧，连接保持存活**：客户端突发超预算不应
  丢失会话。丢弃计数可经 `Session::rate_limited_count()` 观测。
- `messages_per_second` 为 0（未配置）即关闭该闸门，历史行为不变。
- 取值范围：`messages_per_second` 与 `burst` 均为 `0..1000000`，启动期
  校验，越界直接 fail-fast。

业务层若需按玩家/业务 key 的全服配额，仍用 `shield.rate_limiter()`；两者
不互相替代。

### 地址黑名单（网关层）

黑名单在 **accept 时**判定：被拒绝的对端不会创建任何 session 对象，
也不进入连接数/IP 计数，成本只有一次地址比较。

```yaml
actors:
  - name: gateway
    script: scripts/auth.lua
    network:
      tcp: "0.0.0.0:8001"
      blocklist:
        deny:
          - 203.0.113.7        # 精确地址
          - 198.51.100.0/24    # IPv4 CIDR
          - "2001:db8::/32"    # IPv6 CIDR
```

语义要点：

- 条目是**纯地址**或 **CIDR**（`地址/前缀长度`），IPv4/IPv6 都支持。
- **按地址族分表**：v4 地址只与 v4 规则比对，v6 同理，跨族永不误伤。
- **启动期解析并校验**：写错的条目是启动错误（`--check-config` 即可
  发现），而不是在事故当天才发现「规则没生效」。安装失败时**保留原有
  规则集**，不会因为一个错字把正在生效的黑名单清空。
- 拒绝时记录 `last_rejection_reason() == "blocked_ip"`，并打 WARNING 日志。
- 空列表/未配置 = 不启用。

限流与黑名单的分工：**黑名单挡特定来源**（已知的攻击源/刷子），
**限流挡总量**（合法但嘈杂的客户端）。两者都按连接生效，都不替代业务层
的 `shield.rate_limiter()`。

## 敏感数据保护

### 配置中的敏感数据

```yaml
# 不推荐：明文密码
plugins:
  instances:
    - id: db.main
      package: database.mysql
      config:
        password: "my_password"

# 推荐：环境变量
plugins:
  instances:
    - id: db.main
      package: database.mysql
      config:
        password: ${DB_PASSWORD}

# 推荐：密钥管理服务
plugins:
  instances:
    - id: db.main
      package: database.mysql
      config:
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

-- ClientControlMessage::Bound：客户端接入本（auth 入口）服务
function M.on_client_bound(ctx, client)
    pending_auth[client:session_id()] = true
end

-- 编译的 c2s 绑定：登录前所有业务 route 都落在这里
function M.login(ctx, client, request)
    local sid = client:session_id()

    if not pending_auth[sid] then
        return  -- 重复登录等异常帧直接丢弃
    end

    local user = verify_token(request.token)
    if not user then
        shield.client.close(client, "auth_failed")
        return
    end

    pending_auth[sid] = nil
    -- 原子切换单一 target 到 player 服务，返回可用于 s2c 出站的 ClientRef
    local ok, ref = shield.client.bind(client, user.id, "player")
    if ok then
        shield.client_rpc.login_result(ref, { status = "authenticated" })
    end
end

-- ClientControlMessage::Disconnected
function M.on_disconnect(ctx, client, reason)
    pending_auth[client:session_id()] = nil
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
