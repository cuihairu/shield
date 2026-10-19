# 安全运行时语义

> 状态：部分实现 + 部分设计草案。
>
> **已实现**：`lua.sandbox.allow_os` / `lua.sandbox.allow_io`（全局级
> VM 标准库开关，见 [配置语义](runtime-config.md)；未设置时保持历史
> 行为=开放，随仓库分发的默认配置声明两者为 false）；网关层连接级
> `rate_limit`（令牌桶）与 `blocklist.deny`（accept 时按地址/CIDR 拒绝，
> 见下文「速率限制」与「地址黑名单」）；客户端面 TLS
> （`network.tls`，见下文「TLS 支持」与 [TLS 设计](tls-design.md)）。
>
> **未实现（Phase 2+ 草案）**：下文 per-actor sandbox 资源限制
> （max_instructions/allowed_modules 等）、permissions 权限矩阵、
> mTLS 客户端证书校验——见 `include/shield/config/config.hpp` 与
> [TLS 设计](tls-design.md) 的 M2 清单。若与
> [配置语义](runtime-config.md) 或 [Lua API 契约](lua-api.md) 冲突，
> 以那两份文档为准。

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

连接限制参数见 [网络语义](runtime-network.md#背压与限制)。

### TLS 支持

客户端面 listener 的 TLS 已实现（v1），配置挂在 actor 的 `network` 下：

```yaml
actors:
  - name: gateway
    script: scripts/auth.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol: { ... }
      tls:
        enabled: true
        cert_file: "certs/server.crt"
        key_file: "certs/server.key"
        handshake_timeout_ms: 10000   # 可选，缺省 10000
```

fail-loud 双层校验：config 层校验字段形态，bootstrap 层真正加载证书
（缺文件、坏 PEM、公私钥不匹配 = 启动失败），绝不带病回落明文。一个
端口要么全 TLS 要么全明文；握手失败/超时的连接被关闭且不产生 session；
`blocklist` 与连接数限制先于握手生效。DTLS（UDP）与 mTLS 客户端证书
校验（v1 不做 `ca` 加载）属后续里程碑，见 [TLS 设计](tls-design.md)。

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
  丢失会话。丢弃计数可经 `Session::rate_limited_count()` 观测，listener 级
  累计（跨会话单调）进 `/ops/metrics` 的
  `shield_gateway_rate_limited_messages_total`（口径见
  [运维运行时语义](runtime-ops.md)）。
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
- 拒绝与放行的观测走 `/ops/metrics`：`shield_gateway_rejections_total{
  port,reason}`（blocked_ip / connection_limit / ip_limit）与
  `shield_gateway_connections_total`、`shield_gateway_active_sessions`。

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

服务间调用默认信任（同一进程内）。跨节点已实现 **PSK + HMAC-SHA256 双向挑战认证**（需编译启用 `shield_cluster`）：

```yaml
cluster:
  node_id: node-a
  listen: 127.0.0.1:9000
  auth:
    type: token
    token: ${CLUSTER_TOKEN}
    time_window_ms: 30000
```

- `type: token` 消费现有 `cluster.auth.token`，PSK 只能写成完整的 `${VAR}` 环境变量引用（变量名为字母/下划线开头的字母、数字、下划线）；不支持默认值、嵌入展开或内联密钥。变量未设置、为空、长度不在 32–4096 字节，配置非法或 `type: mtls` 都拒绝启动。生产 PSK 应由密码学随机源生成，所有节点通过部署环境注入；不要提交、打印或复用测试值。解析后的 PSK 只保存在集群内存配置中，不写回可供 Lua/诊断读取的全局配置。其他配置键仍无通用环境变量展开，见 [配置语义](runtime-config.md#环境变量展开未实现)。
- **未配置 `cluster.auth`，或显式 `auth: {type: none}`，关闭集群认证并在 transport 启动时打警告**。保留这一行为是为了兼容既有本地/可信网段部署；这两种配置只绑定自报身份，不能阻止接入端口者冒充节点，不能作为不可信网络的安全部署方式。`none` 不允许同时携带 `token`。
- 协议版本为 **2**。`Hello(A)` 携带随机 nonce、UTC 毫秒时间戳、node_id/epoch 和初始 HMAC；接收方返回 `HelloAck(B, HMAC(A))`；发起方验证后返回 `Hello(HMAC(B))`。HMAC 使用有长度前缀且区分协议/阶段的完整双方 transcript，涵盖版本、身份、epoch、两个 nonce 和时间戳。nonce 为 OpenSSL `RAND_bytes` 生成的 256 位随机数，摘要以常量时间比较。接收方验证其发出的 B 后才记录该 CAF sender 的认证身份；发起方验证 Ack 后记录身份，并保证最终证明先于其数据消息发送。
- 时间窗默认 30 秒，可通过 `time_window_ms` 设置为 1–300000 毫秒。跨机器新鲜度校验使用 `shield::base::now_ms()`（真实 UTC wall clock，允许窗口内时钟偏差）；本地挑战有效期及重放缓存使用 `shield::base::now()`（真实 steady clock），不受 Lua MockClock 影响。部署需同步系统时钟。每个 B 只允许一次验证尝试；初始 nonce 在两倍时间窗内也拒绝复用。过期/失败/无待验证挑战的证明全部拒绝；进程重启后旧证明不能回答新生成的随机 B。待验证挑战最多 4096 个、重放缓存最多 8192 项，满时拒绝新握手而不驱逐仍有效的 nonce。
- Envelope 在服务派发前，将 `source_node` 与已完成握手的 CAF sender 绑定。未握手或身份已被撤销返回 `peer_not_authenticated`，自报身份不匹配返回 `peer_identity_mismatch`；call 返回该错误，send 丢弃并记录警告，不调用服务桥。握手拒绝通过 CAF runtime error 携带 `auth_proof_invalid`、`auth_replay`、`auth_timestamp_expired`、`auth_challenge_expired`、`auth_capacity_exceeded` 等代码；拒绝会撤销该 sender 的认证状态。启用 PSK 时，心跳、路由同步和调用回复也要求已认证 sender。
- **默认且始终不自动降级**。认证模式不匹配、版本不匹配直接失败；版本 1 的旧二进制无法反序列化版本 2 的扩展消息，保持未认证并超时（日志 `auth_handshake_timeout`），需要协调升级。没有旧协议降级开关。显式 `type: none` 只关闭版本 2 的认证，风险如上，不能与 `token` 对端互通。

**边界与升级路径**：PSK 验证的是同一信任组的密钥持有者；持有 PSK 的节点能声明组内任意 node_id，没有逐节点证书授权。BASP 后续业务流量没有加密或逐消息 MAC，主动中间人仍可转发握手并篡改后续明文，不能将本实现视作传输完整性/保密性保护。仅适用于受控主机/私网，必要时经受保护的隧道；不应直接暴露到公网。当前轮换方式是注入新环境变量值并协调重启所有节点，没有双密钥热轮换。mTLS 仍推迟：未来为集群 transport 引入证书链校验、节点证书身份授权和独立的证书轮换策略，再用于不可信网络。

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
