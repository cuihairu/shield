# auth.jwt

> HS256 JWT 认证提供者，实现 `shield.auth.v1` 接口，基于 OpenSSL HMAC-SHA256 签名与验证，无状态设计。

## 包信息

- **包 ID**: `auth.jwt`
- **接口**: [`shield.auth.v1`](/plugin-system#interface-model)
- **Capabilities**: `hs256`, `stateless`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_AUTH`
- **源码**: `plugins/auth_jwt/`
- **依赖**: OpenSSL（HMAC-SHA256、`CRYPTO_memcmp` 常量时间比较）

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_AUTH=ON
cmake --build build --config Release
```

构建产物位于 `build/plugins/auth.jwt/bin/` 下，部署到运行环境的 `plugins/auth.jwt/bin/`。

## 配置 Schema

| 字段 | 类型 | 必填 | 默认值 | 范围 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `secret` | string | 是 | — | 非空 | HS256 签名密钥，标记为 `secret`，日志与 introspection 中脱敏 |
| `issuer` | string | 否 | 空 | — | 写入 `iss` claim，验证时若配置非空则必须匹配 |
| `audience` | string | 否 | 空 | — | 写入 `aud` claim，验证时若配置非空则必须匹配 |
| `token_ttl_seconds` | integer | 否 | 3600 | 1–86400 | Token 生存时间，写入 `exp = iat + ttl` |
| `leeway_seconds` | integer | 否 | 60 | 0–3600 | 时钟漂移容忍，应用于 `exp` 与 `iat` 校验 |

`app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: auth.main
      package: auth.jwt
      required: true
      config:
        secret: "change-me-to-a-long-random-string"
        issuer: "shield.example.com"
        audience: "shield-clients"
        token_ttl_seconds: 3600
        leeway_seconds: 60
  bindings:
    auth.default: auth.main
```

## 接口契约

实现 `include/shield/plugin/auth.h` 中的 `shield_auth_v1` vtable。所有方法以 `shield_auth_session*` 为第一个参数；本实现中 session 句柄就是 instance 本身（无状态 JWT，无独立会话资源）。

### connect / disconnect

```c
struct shield_auth_session* (*connect)(const struct shield_auth_config* cfg,
                                       char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_auth_session* session);
```

`auth.jwt` 实现都是 no-op：配置已在 `create()` 阶段解析并存储在 instance 上，`connect` 直接返回 `nullptr`，`disconnect` 不做任何事。Instance 生命周期由 PluginHost 管理。

### authenticate

```c
int (*authenticate)(struct shield_auth_session* session,
                    const struct shield_auth_request* req,
                    struct shield_auth_result* out);
```

主验证入口。流程：

1. 检查 token 是否在 `invalidate` 维护的进程内黑名单中。命中则返回 `token_revoked`。
2. 解析 token 为 `header.payload.signature` 三段；格式非法返回 `invalid_token_format`。
3. 用 `CRYPTO_memcmp` 做常量时间签名比对。不匹配返回 `invalid_signature`。
4. 解码 payload，校验 `exp`（考虑 `leeway_seconds`）、`iat`、`iss`、`aud`，最后提取 `sub` 作为 `user_id`。
5. 成功时 `out->success = 1`、`out->user_id` 填充。失败时 `out->success = 0`，`error_code`/`error_msg` 描述原因，返回 `-1`。

`shield_auth_request` 字段：

| 字段 | 说明 |
| --- | --- |
| `token` | 待验证的 JWT 字符串 |
| `provider` | 预留，本实现忽略 |
| `device_id` | 预留，本实现忽略 |
| `extra_json` | 预留，本实现忽略 |

`shield_auth_result` 字段：

| 字段 | 说明 |
| --- | --- |
| `success` | 1 成功，0 失败 |
| `error_code` / `error_msg` | 失败原因，调用方需用 `free_result` 释放 |
| `user_id` | 成功时填充，调用方负责释放 |
| `token` | 仅 `refresh_token` 路径会填入新 token |
| `token_expiry_ms` | 仅 `refresh_token` 路径填充 |
| `roles_json` / `extra_json` | 预留，本实现不填充 |

### authorize

```c
int (*authorize)(struct shield_auth_session* session,
                 const char* user_id, const char* resource,
                 const char* action, struct shield_authz_result* out);
```

`auth.jwt` 采用开放策略：默认 `allowed = 1`。RBAC/ABAC 应由业务层叠加（基于 `user_id` 查权限表或下发权限策略）。返回 `reason = nullptr`。

### refresh_token

```c
int (*refresh_token)(struct shield_auth_session* session,
                     const char* refresh_token,
                     struct shield_auth_result* out);
```

先用与 `authenticate` 相同的规则校验传入的 `refresh_token`，通过后用当前配置重新签发一个新 token 写入 `out->token`，并按 `token_ttl_seconds` 填充 `token_expiry_ms`（毫秒）。校验失败时填入错误码并返回 `-1`。

注意：HS256 无状态 JWT 没有独立 access token 与 refresh token 的区分；调用方可以给 refresh token 一个更长的 TTL 实例（例如配置第二个 `auth.jwt` 实例）。

### invalidate

```c
int (*invalidate)(struct shield_auth_session* session, const char* token);
```

把 token 加入 instance 进程内黑名单（`std::unordered_set<std::string>`）。后续 `authenticate` 命中黑名单直接失败，返回 `token_revoked`。

重要约束：

- 黑名单是**进程内**的，不跨进程同步。多副本部署下，logout 只能保证在收到请求的那个副本上生效。
- 进程重启后黑名单丢失。生产环境如需持久化登出，应通过 `requires` 注入 Redis/数据库自行实现。
- 黑名单无 TTL，内存随 logout 量线性增长，需要上层周期性清理或换成带 TTL 的实现。

### free_result / free_authz_result

```c
void (*free_result)(struct shield_auth_result* result);
void (*free_authz_result)(struct shield_authz_result* result);
```

释放 `authenticate` / `refresh_token` / `authorize` 写入 result 的所有 `const char*` 字段（内部用 `std::malloc` 分配，这里用 `std::free` 释放）。调用方拿到结果后必须调用一次。

## 使用示例

### C++（通过 binding）

登录验证 + token 刷新的完整场景：

```cpp
#include "shield/plugin/auth.h"

// 假设 host 已注入 auth.default binding 指向的 shield_auth_v1*
const shield_auth_v1* auth = /* ... */;

// 1. 验证客户端传来的 token
shield_auth_request req{};
req.token = client_token.c_str();

shield_auth_result result{};
if (auth->authenticate(session, &req, &result) == 0 && result.success) {
    // 验证通过，拿到用户身份
    std::string user_id = result.user_id ? result.user_id : "";
    auth->free_result(&result);
    // 继续业务...
} else {
    std::string code = result.error_code ? result.error_code : "unknown";
    std::string msg  = result.error_msg  ? result.error_msg  : "";
    auth->free_result(&result);
    // 返回 401
}

// 2. 客户端刷新 token
shield_auth_result refreshed{};
if (auth->refresh_token(session, refresh_token.c_str(), &refreshed) == 0
    && refreshed.success) {
    std::string new_token = refreshed.token;
    int64_t expiry_ms = refreshed.token_expiry_ms;
    auth->free_result(&refreshed);
    // 下发新 token
}

// 3. 用户登出：把 token 加入黑名单
auth->invalidate(session, client_token.c_str());
```

`session` 句柄由 PluginHost 在实例创建时建立；通常通过 host 的 binding 解析机制拿到。

### Lua（规划中）

`auth.jwt` 当前 `register_lua` 是空实现。计划中的 namespace 约定：

```lua
-- 未来将注册到 shield.auth.jwt
local auth = shield.auth.jwt("auth.main")
local ok, result = auth:verify(token)
if ok then
    local user_id = result.user_id
end
local new_token = auth:refresh(refresh_token)
auth:invalidate(token)
```

## 平台特性

### 算法

仅支持 HS256（HMAC-SHA256）。签名用 OpenSSL `HMAC()`，比对用 `CRYPTO_memcmp` 做常量时间比较，避免侧信道泄露。RS256 / ES256 / JWKS 等非对称方案在 v1 不支持；接口契约中保留了 `public_keys_json` 字段为未来扩展预留。

### Token TTL 与时钟漂移

- `token_ttl_seconds` 控制 `exp` claim，默认 1 小时。
- `leeway_seconds` 同时作用于 `exp` 与 `iat`：`exp` 允许超出当前时间最多 `leeway` 秒，`iat` 允许提前当前时间最多 `leeway` 秒，用于容忍多机部署间的时钟漂移。
- 默认 60 秒 leeway 是经验值，对绝大多数 NTP 同步的环境已足够。

### Issuer / Audience 校验

- 仅当配置中 `issuer` / `audience` 非空时才启用对应校验，避免单一签名密钥场景下引入多余字段。
- 当前实现是精确字符串匹配，不支持 `aud` 数组或正则。

### 黑名单

`invalidate` 维护的进程内黑名单用互斥锁保护，支持多线程并发调用 `authenticate` 与 `invalidate`。多副本部署如需一致登出语义，必须外挂共享存储。

### 无状态语义

每个 `authenticate` 调用都是独立的密码学验证，不依赖任何服务端 session 状态，水平扩展无需会话同步。

## 错误处理

`authenticate` / `refresh_token` 失败时返回 `-1`，并通过 `shield_auth_result` 字段携带错误：

| `error_code` | 触发条件 |
| --- | --- |
| `invalid_token_format` | 不是三段式 JWT 字符串 |
| `invalid_signature` | HMAC 不匹配（密钥错误或 token 被篡改） |
| `token_expired` | `now > exp + leeway` |
| `token_not_yet_valid` | `now + leeway < iat`（签发时间在未来） |
| `invalid_issuer` | `iss` 不等于配置值 |
| `invalid_audience` | `aud` 不等于配置值 |
| `missing_subject` | payload 中没有 `sub` claim |
| `token_revoked` | token 命中 `invalidate` 黑名单 |

创建实例时的配置错误：

| `err->code` | 触发条件 |
| --- | --- |
| `plugin.config.invalid` | `secret` 为空 |

所有 `error_code` / `error_msg` / `user_id` / `token` 等 `const char*` 字段均由 `free_result` 统一释放；不要 `free` 单个字段。

## 部署

### 二进制位置

```
plugins/auth.jwt/
├── manifest.yaml
└── bin/
    ├── libshield_auth_jwt.dll        # Windows
    ├── libshield_auth_jwt.so         # Linux
    └── libshield_auth_jwt.dylib      # macOS
```

host 优先扫描 `plugins/auth.jwt/manifest.yaml`，不存在时回退到 `plugin.json`，然后按平台加载 `bin/` 下对应共享库。

### 运行时依赖

- **OpenSSL**：至少 1.1.1，推荐 3.x。`HMAC` 与 `CRYPTO_memcmp` 来自 `libcrypto`。
- Windows 上需要 `libcrypto-3-x64.dll`（或对应版本）在 PATH 中；Linux/macOS 通过 RPATH 或 `LD_LIBRARY_PATH` 解析。

### 密钥管理

- 生产环境的 `secret` 必须通过环境变量、配置中心或 KMS 注入，不要明文写入 `app.yaml`。
- 推荐使用 `shield.config` host API 在 `create()` 阶段从外部源拉取，而非依赖 manifest 静态默认值。
- 轮换密钥需要同时部署新旧两个 `auth.jwt` 实例（不同 binding 名），逐步迁移。

### JWT 生态集成

- HS256 是最广泛兼容的 JWT 算法，前端库（`jsonwebtoken`、`jose`、`pyjwt` 等）原生支持。
- `exp` / `iat` / `iss` / `aud` / `sub` 全部是 RFC 7519 标准 claim，跨语言互通。
- 登出需要配合外挂共享黑名单（如 Redis SET，配合 TTL 等于 token 剩余有效期）。

## 相关链接

- [插件系统](/plugin-system) — 接口模型、ABI 契约、bootstrap pipeline
- [插件参考索引](/plugins/) — 全部官方插件
- [RFC 7519 JSON Web Token (JWT)](https://datatracker.ietf.org/doc/html/rfc7519)
- [RFC 7515 JSON Web Signature (JWS)](https://datatracker.ietf.org/doc/html/rfc7515) — HS256 算法定义
- [OpenSSL HMAC 文档](https://www.openssl.org/docs/man3.0/man3/HMAC.html)
