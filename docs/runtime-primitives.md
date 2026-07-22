# 基础组件与运行时适配边界

> 状态：后置草案。本文不属于当前主路径，也不是当前对业务用户承诺的公开 API 契约。当前用户侧最短心智模型仍然是：客户端入站走 gateway / `SessionHandle`，服务间走 `shield.send/call`，对外 HTTP 走 `shield.http`。

本文整理的是更后置的高级能力方向：如果未来要补 Lua runtime primitives、cosocket 风格出站 I/O 和更底层的 crypto/buffer/stream 原语，边界应如何收敛。

本文的目标不是声明这些能力已经全部实现，而是防止后续再把 `auth`、业务协议客户端或 CAF 细节混进 core。

## 草案结论

Shield 应参考 Node.js 与 OpenResty 的方向：**core 提供原语，不提供业务策略**。

也就是说：

- 应提供 `buffer`、`encoding`、`crypto`、`socket`、`tls`、`stream`、`codec`、`url` 这类基础组件。
- 不应把 JWT、OAuth、WeChat、Steam、业务 RPC client 这类上层策略做成 core 或官方插件的中心能力。
- CAF 不直接复用 Lua-facing API；CAF 与 Lua runtime 只复用更底层的纯原语库。

## 分层模型

推荐按四层拆分：

```text
应用层
  -> auth / jwt / oauth / webhook / 自定义 RPC client

Lua 运行时表面
  -> shield.buffer / shield.encoding / shield.crypto
  -> shield.socket / shield.tls / shield.stream
  -> shield.codec / shield.url / shield.http

纯基础原语库
  -> bytes / endian / base64 / digest / hmac / aes-gcm / codec core / tls common

CAF / net / transport 适配器
  -> mailbox / scheduler / node transport / listener / session / gateway bridge
```

规则：

- Lua-facing API 和 CAF adapter 不能互相直接依赖对方的表面模型。
- 两边只允许复用“纯基础原语库”。
- 应用层能力构建在 Lua-facing API 或插件接口之上，不反向进入 primitives。

## 为什么不是 `auth-first`

JWT/Auth 是具体业务能力，而不是基础组件或官方插件能力。

把 `auth` 放在 C++ core 中心位置的问题：

- `auth` 不是原语，而是策略组合。
- `jwt` 依赖的真正基础能力其实是 `json`、`base64url`、`hmac`、`timing_safe_equal`、时钟和可选存储。
- 一旦把 `auth` 做成核心抽象，后续 `oauth`、`steam`、`wechat`、`firebase`、session、ACL 都会沿着同一方向继续膨胀。
- 这会让 core 越来越像“内建业务框架”，而不是小核心运行时。

更干净的方向是：

- core / runtime 提供 `shield.crypto`
- 上层 Lua 或插件基于 `shield.crypto + shield.codec + shield.http` 实现 `jwt/auth/webhook`

## 为什么参考 Node.js 与 OpenResty

### Node.js 的启发

Node.js core 提供的是：

- `Buffer`
- `crypto`
- `net`
- `tls`
- `stream`
- `http` / `https`
- `url`
- `zlib`

但 Node.js core 并不把 JWT/OAuth/Auth 作为内建中心能力。认证能力通常停留在用户态或生态层。

### OpenResty 的启发

OpenResty 提供的关键基础能力是 coroutine-aware 非阻塞 socket 原语：

- `ngx.socket.tcp()`
- `ngx.socket.udp()`
- `sslhandshake`
- `receiveuntil`
- `setkeepalive`

它的价值在于：

- 把 socket I/O 作为 Lua 可直接使用的基础原语
- 与协程深度集成
- 允许上层自己实现 Redis、MySQL、HTTP、自定义协议客户端

Shield 适合借鉴这种“cosocket 风格”，但不应照搬 `ngx.*` 命名、request phase 绑定和 Nginx 语义。

## 基础组件清单

下面的清单表示**目标组件划分**。其中只有少数已经部分存在，很多仍是后续工作。

### P0：若后续推进，应优先冻结的基础组件

#### `shield.buffer`

职责：

- 二进制缓冲区
- 切片/拼接
- 大小端整数读写
- 字节长度计算

建议能力：

- `alloc(size)`
- `from_string(s)`
- `from_hex(s)`
- `slice(offset, length)`
- `concat(list)`
- `byte_length(value)`
- `read_u16be/le`
- `read_u32be/le`
- `write_u16be/le`
- `write_u32be/le`

说明：

- 它是 `socket`、`stream`、`crypto`、协议编解码的共同底座。
- 当前 `raw` codec、gateway 原始字节发送路径未来都应可映射到这一层。

#### `shield.encoding`

职责：

- 文本与字节编码转换

建议能力：

- `hex_encode/decode`
- `base64_encode/decode`
- `base64url_encode/decode`
- `url_encode/decode`
- `utf8_validate`

说明：

- JWT、webhook、签名串、HTTP query、调试日志都要用到。

#### `shield.crypto`

职责：

- 提供稳定的密码学原语封装，而不是暴露 OpenSSL 对象模型

建议能力：

- `random_bytes`
- `random_uuid`
- `digest`
- `hmac`
- `timing_safe_equal`
- `hkdf`
- `pbkdf2`
- `aes_gcm_encrypt`
- `aes_gcm_decrypt`

可后置能力：

- 非对称签名验签
- 证书解析
- JWKS

说明：

- Lua API 不应命名成 `shield.openssl.*`
- 底层是否用 OpenSSL 属于实现细节

#### `shield.socket`

职责：

- Lua coroutine-aware 非阻塞出站 socket 原语

建议能力：

- `tcp()`
- `udp()`
- `unix()`
- `connect()`
- `send()`
- `receive()`
- `receiveuntil()`
- `shutdown()`
- `close()`
- `settimeout()/settimeouts()`
- `setkeepalive()`
- `getreusedtimes()`

规则：

- socket 句柄只属于当前 Lua VM / service
- 不允许跨 service 传递 socket
- 不允许多个 coroutine 同时作为同一 socket 的 reader

说明：

- 这层应主要先服务“出站客户端”场景，不应一开始就让 Lua 动态接管通用 listener/accept 模型。

#### `shield.tls`

职责：

- TLS 握手与证书校验能力

建议形式：

- 优先作为 `shield.socket` 上的方法存在，例如 `sock:tls_handshake(opts)`

建议能力：

- SNI
- 证书校验开关
- CA 路径/证书配置
- session reuse

说明：

- 不建议做出一套完全独立于 socket 的 Lua 抽象。

#### `shield.stream`

职责：

- 面向 socket 的读写流语义

建议能力：

- `read_exact(n)`
- `read_until(delim)`
- `read_frame(decoder)`
- `write(data)`
- `write_frame(encoder)`
- backpressure / buffered write

说明：

- socket 解决“连接”，stream 解决“半包、拆包、缓冲、读写语义”。
- 很多高层客户端会直接建在 `stream` 上，而不是裸 `socket` 上。

#### `shield.codec`

职责：

- 通用数据编解码原语

建议最小能力：

- `json.encode/decode`
- `msgpack.encode/decode`

说明：

- `protobuf` / `sproto` / `xmldef` / `fbs` 更适合作为协议族或 descriptor/profile 系统的一部分，不是最小 runtime core 必须硬编码进去的一组顶层脚本 API。
- 但底层纯编解码能力如果需要，仍可通过可选模块方式接入 `shield.codec.protobuf` / `shield.codec.sproto`。

#### `shield.url`

职责：

- URL 解析与 query 处理

建议能力：

- `parse(url)`
- `format(parts)`
- `encode_query(tbl)`
- `decode_query(str)`

说明：

- `http`、OAuth、webhook 签名与回调都依赖它。

### P1：高价值但可后置

#### `shield.http`

定位：

- 高层 HTTP client

说明：

- 当前仓库已经存在 `shield.http`
- 长期应明确它建立在 `socket + tls + stream + url + codec` 之上
- 不应成为替代这些基础原语的唯一网络入口

#### `shield.compress`

建议能力：

- `gzip`
- `deflate`
- `brotli`

#### `shield.dns`

建议能力：

- `lookup`
- `resolve`

#### `shield.websocket`

建议范围：

- 先做 client
- server 仍走固定 listener/gateway 体系

## 哪些能力不应进入基础组件层

以下能力不应作为 runtime primitives 或 core 的固定组成：

- JWT / OAuth provider
- Steam / WeChat / Firebase 登录
- Redis / MySQL / MongoDB 客户端
- 业务 RPC client
- ACL / RBAC / session store
- 框架级 middleware chain

它们属于：

- Lua 应用层
- 插件层
- 官方可选模块

## 与 CAF 的关系

CAF 不能直接复用 Lua-facing `socket/stream/http` API。

原因：

- Lua socket 模型是 coroutine I/O 原语
- CAF 模型是 actor mailbox / scheduler / transport
- 两者的并发模型、对象所有权和生命周期语义不同

### CAF 可以复用什么

CAF / cluster / transport 适配器应复用的，是纯基础原语：

- bytes / buffer helper
- encoding
- crypto
- codec core
- tls common

### CAF 不应复用什么

CAF 不应直接复用以下 Lua 表面 API：

- `shield.socket`
- `shield.stream`
- `shield.http`
- coroutine-aware timeout / keepalive / receiveuntil 语义

正确做法是：

- Lua runtime 做一套 adapter
- CAF transport 做一套 adapter
- 两边共享同一批 primitives

## 对当前网络/gateway 语义的影响

这套 primitives 设计不会推翻当前 gateway 模型。

边界应保持：

- 入站 listener / session / gateway 继续由 `shield_net + shield_transport` 管理
- `SessionHandle` 继续是固定网络入口的业务句柄
- `shield.socket` 主要用于 Lua 发起的出站连接
- 不建议第一阶段让 Lua 动态创建通用服务端 listener 来取代现有 gateway/session 模型

也就是说：

- `SessionHandle` 面向“被接入的客户端连接”
- `shield.socket` 面向“Lua 主动发起的出站连接”

两者不是同一个对象模型。

## 对插件系统的影响

插件系统仍然负责：

- 数据库
- 缓存
- 队列
- 排行榜
- 特定认证 provider（业务层自建）

业务层 provider 设计应优先复用基础组件，而不是重复发明原语。

例如：

- JWT provider 的密码学原语应建立在 `shield.crypto` 之上
- 它不应继续被视为 Lua runtime 必备核心能力

## 当前实现状态

截至当前文档版本：

- `shield.http` 已存在
- `SessionHandle` 与 gateway bridge 已存在
- transport 中已有 OpenSSL 加密实现片段
- JWT/Auth 不作为 Shield 官方插件发布

但以下能力尚未作为稳定基础组件正式冻结并实现：

- `shield.buffer`
- `shield.encoding`
- `shield.crypto`
- `shield.socket`
- `shield.tls`
- `shield.stream`
- `shield.codec` 统一 Lua 表面

因此本文是**后置分层草案**，不是当前实现完成声明，也不是当前公开 API 承诺。

## 若未来推进，推荐顺序

建议按以下顺序推进：

1. 冻结 `shield.buffer` / `shield.encoding` / `shield.crypto`
2. 冻结 `shield.socket` / `shield.tls` / `shield.stream`
3. 把 `shield.http` 明确重构为建立在上述 primitives 之上的高层 API
4. 明确 JWT/Auth 由业务层基于 primitives 或外部服务实现
5. 后续再视需要补 `codec`、`compress`、`websocket`、`dns`

## 参考

- [Node.js API index](https://nodejs.org/api/index.html)
- [Node.js Buffer](https://nodejs.org/api/buffer.html)
- [Node.js Crypto](https://nodejs.org/api/crypto.html)
- [Node.js Net](https://nodejs.org/api/net.html)
- [Node.js TLS](https://nodejs.org/api/tls.html)
- [Node.js Stream](https://nodejs.org/api/stream.html)
- [Node.js URL](https://nodejs.org/api/url.html)
- [Node.js HTTP](https://nodejs.org/api/http.html)
- [Node.js Zlib](https://nodejs.org/api/zlib.html)
- [OpenResty `ngx.socket.tcp`](https://openresty-reference.readthedocs.io/en/latest/Lua_Nginx_API/#ngxsockettcp)
- [OpenResty `ngx.socket.udp`](https://openresty-reference.readthedocs.io/en/latest/Lua_Nginx_API/#ngxsocketudp)
