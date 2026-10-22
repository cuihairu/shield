# Security Runtime Semantics

> Status: partially implemented + partially a design draft.
>
> **Implemented**: `lua.sandbox.allow_os` / `lua.sandbox.allow_io` (global-level VM standard library
> switches, see Configuration Semantics; when unset the historical behavior = open is
> kept, and the default config shipped with the repo declares both as false); gateway-layer connection-level
> `rate_limit` (token bucket) and `blocklist.deny` (reject by address/CIDR at accept,
> see "Rate Limiting" and "Address Blocklist" below); client-facing TLS
> (`network.tls`, see "TLS Support" below and the [TLS Design](tls-design.md)).
>
> **Not implemented (Phase 2+ drafts)**: the per-actor sandbox resource limits below
> (max_instructions/allowed_modules etc.), the permissions matrix, and mTLS client certificate
> verification — see `include/shield/config/config.hpp` and the M2 list in the
> [TLS Design](tls-design.md). On conflict with
> Configuration Semantics or the Lua API Contract,
> those two documents prevail.

This document contains the runtime semantics decisions for Shield's security mechanisms.

## Security Principles

- Secure by default: expose no unnecessary capabilities
- Least privilege: services own only the minimum permissions needed to complete their task
- Defense in depth: multiple security layers; rely on no single line of defense

## Lua Sandbox

Each Lua service runs in its own Lua VM — isolation is natural.

### Isolation Capabilities

| Capability | Isolation mechanism |
|------|----------|
| Global variables | not shared between services |
| Memory | separate VM, limit configurable |
| Filesystem | access paths optionally restricted |
| Network | only through the shield.* API |
| System calls | dangerous functions like os.execute not exposed |

### Configurable Limits

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

## Inter-Service Permissions

### Default permissions

Services may call each other freely; no enforcement.

### Optional permission control

For scenarios needing strict control, inter-service call permissions can be configured:

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

### Permission checks

Permission checks happen at `shield.call` and `shield.send`:

```lua
-- 调用时检查权限
local ok, result = shield.call("payment", "charge", amount)
if not ok and result.code == "permission_denied" then
    shield.log.error("no permission to call payment service")
end
```

## Network Security

### Connection limits

Connection limit parameters in [Network Semantics](runtime-network.md#背压与限制).

### TLS Support

TLS for client-facing listeners is implemented (v1), configured under the actor's `network`:

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

Fail-loud two-layer validation: the config layer validates field shapes, and the bootstrap layer actually
loads the certificates (missing file, bad PEM, public/private mismatch = startup failure), never silently
falling back to plaintext. A port is either all TLS or all plaintext; connections failing/timing out the
handshake are closed and produce no session; `blocklist` and connection limits take effect before the
handshake. DTLS (UDP) and mTLS client certificate verification (v1 does no `ca` loading) are later
milestones, see the [TLS Design](tls-design.md).

### Rate Limiting (gateway layer)

Gateway-layer rate limiting is connection-level protection, intercepting messages before they enter business
logic. Unlike business-level distributed rate limiting (`shield.rate_limiter()`, see
Global Data):

| Layer | Purpose | Storage | Scenarios |
|------|------|------|----------|
| Gateway-layer `rate_limit` | per-connection rate limit (one token bucket per connection) | memory | anti-abuse, anti-DDoS |
| Business-layer `shield.rate_limiter()` | per-business-key rate limit | Redis | server-wide limits, API limits |

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

Semantic essentials:

- **A token bucket, one per connection**, created with the connection and handed by the listener to the
  `TcpSession`. The bucket starts full (an immediate `burst` may pass), then refills continuously at
  `messages_per_second`; refilling is **lazy** (converted from elapsed time), so idle connections occupy no
  timers.
- **Billed per decoded message**, not per TCP read event. Packing multiple frames into one TCP segment does
  not bypass the budget — batched delivery and per-item delivery consume the same tokens.
- **Over-limit frames are dropped directly, no error frame written back, the connection stays alive**: a
  client burst exceeding budget should not lose the session. Drop counts are observable via
  `Session::rate_limited_count()`, with listener-level cumulative totals (monotonic across sessions) entering
  `/ops/metrics` as `shield_gateway_rate_limited_messages_total` (conventions in
  Ops Runtime Semantics).
- `messages_per_second` of 0 (unconfigured) disables the gate; historical behavior unchanged.
- Value ranges: both `messages_per_second` and `burst` are `0..1000000`, validated at startup; out of range
  fails fast.

For per-player/business-key server-wide quotas, the business layer still uses `shield.rate_limiter()`; the
two do not replace each other.

### Address Blocklist (gateway layer)

The blocklist is judged **at accept**: a rejected peer creates no session object at all, does not enter the
connection/IP counters, and costs one address comparison.

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

Semantic essentials:

- Entries are **plain addresses** or **CIDR** (`address/prefix length`); both IPv4/IPv6 supported.
- **Tables split by address family**: v4 addresses compare only against v4 rules, likewise v6 — cross-family
  false positives never happen.
- **Parsed and validated at startup**: a malformed entry is a startup error (findable with
  `--check-config`), not a "rule never took effect" discovered on incident day. On installation failure the
  **original rule set is kept** — one typo never wipes an active blocklist.
- On rejection, `last_rejection_reason() == "blocked_ip"` is recorded and a WARNING logged.
- Empty list / unconfigured = disabled.
- Rejection and admission observability via `/ops/metrics`: `shield_gateway_rejections_total{
  port,reason}` (blocked_ip / connection_limit / ip_limit) plus
  `shield_gateway_connections_total` and `shield_gateway_active_sessions`.

The division of labor: the **blocklist blocks specific sources** (known attackers/abusers);
**rate limiting blocks volume** (legitimate but noisy clients). Both act per connection, and neither replaces
the business layer's `shield.rate_limiter()`.

## Sensitive Data Protection

### Sensitive data in configuration

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

### Sensitive data in logs

```lua
-- 不推荐：记录敏感信息
shield.log.info("user login: " .. username .. " password: " .. password)

-- 推荐：脱敏处理
shield.log.info("user login: " .. username)
shield.log.debug("password length: " .. #password)  -- 仅调试模式记录长度
```

### Sensitive data in messages

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

## Authentication and Authorization

### Client authentication

The Gateway service is responsible for client authentication:

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

### Inter-service authentication

Inter-service calls trust by default (same process). Cross-node has implemented **PSK + HMAC-SHA256 mutual
challenge authentication** (requires building with `shield_cluster` enabled):

```yaml
cluster:
  node_id: node-a
  listen: 127.0.0.1:9000
  auth:
    type: token
    token: ${CLUSTER_TOKEN}
    time_window_ms: 30000
```

- `type: token` consumes the existing `cluster.auth.token`; the PSK can only be written as a complete
  `${VAR}` environment variable reference (variable names starting with a letter/underscore, then letters,
  digits, underscores); defaults, nested expansion, or inline secrets are not supported. An unset or empty
  variable, a length outside 32–4096 bytes, invalid config, or `type: mtls` all refuse startup. Production
  PSKs should come from a cryptographic random source, injected on all nodes via the deployment environment;
  never commit, print, or reuse test values. The resolved PSK is kept only in the cluster's in-memory config,
  not written back to the global config readable by Lua/diagnostics. Other config keys still have no generic
  environment variable expansion, see
  Configuration Semantics.
- **No `cluster.auth` configured, or explicit `auth: {type: none}`, turns off cluster authentication with a
  warning at transport startup**. This behavior is kept for compatibility with existing local/trusted-network
  deployments; both bind only the self-reported identity, cannot stop whoever reaches the port from
  impersonating a node, and must not be used as a security deployment on untrusted networks. `none` may not
  carry `token` at the same time.
- The protocol version is **2**. `Hello(A)` carries a random nonce, a UTC millisecond timestamp,
  node_id/epoch, and the initial HMAC; the receiver answers `HelloAck(B, HMAC(A))`; after verifying, the
  initiator returns `Hello(HMAC(B))`. The HMAC covers the full two-sided transcript — length-prefixed and
  distinguishing protocol/phase — including version, identities, epochs, both nonces, and timestamps. Nonces
  are 256-bit OpenSSL `RAND_bytes` values; digests compare in constant time. The receiver records the CAF
  sender's authenticated identity only after verifying the B it issued; the initiator records identity after
  verifying the Ack and guarantees the final proof precedes its data messages.
- The time window defaults to 30 seconds, configurable via `time_window_ms` to 1–300000 ms. Cross-machine
  freshness checks use `shield::base::now_ms()` (real UTC wall clock, tolerating in-window clock skew); local
  challenge validity and the replay cache use `shield::base::now()` (real steady clock), unaffected by the
  Lua MockClock. Deployments must keep system clocks in sync. Each B allows exactly one verification attempt;
  the initial nonce is also refused for reuse within twice the window. Expired/failed/awaiting-no-challenge
  proofs are all rejected; after a process restart, old proofs cannot answer freshly generated random Bs.
  Pending challenges cap at 4096 and the replay cache at 8192 entries; when full, new handshakes are refused
  rather than evicting still-valid nonces.
- The Envelope binds `source_node` to the handshake-completed CAF sender before service dispatch.
  Unhandshaked or revoked identity returns `peer_not_authenticated`; a self-reported identity mismatch
  returns `peer_identity_mismatch`; call returns the error, send drops with a warning and does not invoke the
  service bridge. Handshake rejections carry codes like `auth_proof_invalid`, `auth_replay`,
  `auth_timestamp_expired`, `auth_challenge_expired`, `auth_capacity_exceeded` via CAF runtime errors; a
  rejection revokes that sender's authenticated state. With PSK enabled, heartbeats, route sync, and call
  replies also require an authenticated sender.
- **By default and always, no automatic downgrade**. Auth-mode mismatch or version mismatch fails outright;
  version-1 old binaries cannot deserialize version-2 extended messages, stay unauthenticated, and time out
  (log `auth_handshake_timeout`) — a coordinated upgrade is required. There is no old-protocol downgrade
  switch. Explicit `type: none` only turns off version-2 authentication, with the risks above, and cannot
  interoperate with `token` peers.

**Boundaries and upgrade path**: PSK proves membership of the same trust group; a node holding the PSK can
claim any node_id in the group — there is no per-node certificate authorization. Subsequent BASP business
traffic has neither encryption nor per-message MAC, so an active man-in-the-middle can still forward the
handshake and tamper with later plaintext; this implementation must not be treated as transport
integrity/confidentiality protection. It suits controlled hosts/private networks only, behind protected
tunnels when necessary; it should not be exposed directly to the public internet. Rotation today means
injecting a new environment variable value and coordinating a restart of all nodes; there is no dual-key hot
rotation. mTLS remains deferred: certificate chain verification, node certificate identity authorization,
and an independent certificate rotation policy would come to the cluster transport before use on untrusted
networks.

## Audit Logs

Important operations record audit logs:

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

Audit log format:

```json
{
  "timestamp": 1234567890,
  "event": "password_change",
  "uid": "user123",
  "ip": "192.168.1.100",
  "result": "success"
}
```
