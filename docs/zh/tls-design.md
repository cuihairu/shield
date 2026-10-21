# TLS（network.tls 配置面）

> 状态：**已实现（v1）**。本文是客户端面 TCP 监听 TLS 化的设计依据。
> 范围：`TcpListener`/`TcpSession` 承载的客户端网关连接。ops/console 的
> boost::beast HTTP 面**不在本设计范围内**（见「与 beast HTTP 的关系」）。
> 来源：todo.md「TLS（network.tls 配置面）」候选。

## 问题

客户端网关监听当前只有明文 TCP：`TcpListener` accept 出 `tcp::socket` 直接
交给 `TcpSession`，读写各一处（`async_write` / `async_read_some`）+ close。
需要传输加密的部署（公网接入、跨机房链路）没有开关可开。

设计输入（均已核实）：

1. **配置模型没有全局 `network` 节点**。所有网络配置都挂在 actor 上
   （`actors[].network.*`：tcp / max_connections / rate_limit / blocklist /
   protocol …），由 config.cpp 校验并解析进 `RuntimeActorConfig`。
2. `TcpSession` 的 IO 点收敛且唯一：`do_async_write` 一次 composed
   `async_write`、`do_receive` 一次 `async_read_some`、`close()` 一次
   `socket_.close`。换传输层只需替换这三个触点，会话语义（背压、限流、
   read idle、协议管线）与传输无关。
3. accept 路径已有成熟的拒绝语义：blocklist → 连接上限 → per-IP 上限，
   全部是「关 socket + 计数 + 日志 + **无 session 对象**」。
4. vcpkg 依赖图已有顶层 `openssl`，无需新增依赖条目。
5. 监听器是链式单 socket accept（`async_accept(socket_)` 后 move 出去、
   重新 arm），move 后的 socket 由下一次 `async_accept` 重新打开——该
   模式对「accept 后先握手再建 session」同样成立。

## 决策

### 配置面（per-actor `network.tls`）

```yaml
actors:
  gateway:
    network:
      tcp: "0.0.0.0:7000"
      protocol: { ... }
      tls:
        enabled: true                  # 缺省 false
        cert_file: "certs/server.crt"  # PEM
        key_file: "certs/server.key"   # PEM，未加密私钥
        handshake_timeout_ms: 10000    # 可选，缺省 10000，>= 1
```

- **未配 `tls` 或 `enabled: false` → 行为与现在逐字节一致**（零回归）：
  配置层不做任何额外校验，监听器不持 TLS 状态，accept 路径走原分支。
- `enabled: true` 时配置层 fail-loud：`cert_file` / `key_file` 必须存在且
  为非空字符串；`handshake_timeout_ms` 走 `validate_int_range(1, 600000)`。
- **bootstrap 层二次 fail-loud**：启动时用 `make_tls_server_context()`
  真正加载证书（文件可读、PEM 合法、公私钥匹配），任何失败打印明确错误
  并使启动失败——**绝不带病回落明文**。配置层管类型，bootstrap 管现实。
- `network.tcp` 仍是必填端点，TLS 是叠加不是替代；不支持「enabled 但
  没有 tcp」的组合。

### 流抽象（SessionStream）

`TcpSession` 不再直持 `tcp::socket`，改持 `std::unique_ptr<SessionStream>`
（include/shield/net/session_stream.hpp）：

- `PlainStream`：包 `tcp::socket`，逻辑原样搬移（现路径）；
- `TlsStream`：包 `boost::asio::ssl::stream<tcp::socket>`，接口相同。

两个实现把完成处理器绑定到自己保存的 executor 上（构造时取自底层
socket，session 构造时 `set_executor(strand)` 覆盖为会话 strand）——
会话既有的「一切完成回调落 strand」不变式保持成立，`TcpSession` 内部
代码只是把 `socket_.async_xxx(..., bind_executor(strand_, h))` 换成
`stream_->async_xxx(..., h)`。

- 原 `TcpSession(SessionId, tcp::socket, ...)` 构造签名保留，内部包成
  `PlainStream`——全部既有调用方零改动；
- 新增流式构造：`TcpSession(SessionId, unique_ptr<SessionStream>, ...)`
  供 TLS accept 路径使用；
- TCP_NODELAY 对两种传输统一施加在最低层 socket 上（TLS 记录封包本身
  就小帧友好，Nagle 在其下仍会造成合并延迟）。

每消息一次虚分派相对 TLS 记录加解密成本可忽略；不为它做模板化。

### accept 路径与握手语义

**拒绝闸全部在握手之前**：blocklist → 连接上限 → per-IP 上限照旧先跑
（复用既有计数器），被拒连接不付出握手成本、也永远不会带着完成的握手
被丢弃。TLS 开启时 accept 分支为：

```text
accept → blocklist/limits（不变）
       → tls_context_ 为空：PlainStream → 建 session（原路径）
       → 否则：堆上 TlsHandshake（stream + steady_timer + 共享计数），
         async_handshake(server)，listener 并行 re-arm accept
```

- **握手成功** → 计数 `tls_handshakes_total` → 建 `TcpSession`（TlsStream）
  → 与明文完全相同的后续路径（回调包装、限流、背压、read idle、协议管线）。
- **握手失败**（协议错误、证书无效、**明文客户端连 TLS 口**）→ 计数
  `tls_handshake_failures_total` + WARN 日志（含 `ec.message()` 与对端
  地址）+ 关 socket，**无 session 对象**——与 blocklist 拒绝同语义。
  不做降级、不响应明文。
- **握手超时**（连上不发 ClientHello 的慢连接/恶意连接）→ 同失败语义。
  `handshake_timeout_ms` 缺省 10000。超时与完成竞态用原子
  「先到先得」标志收敛，恰好一侧负责关 socket 与计数。
- 握手状态是每连接堆对象（自持 shared_ptr 直到两侧完成收尾），listener
  只留 weak 引用；`stop()` 顺手关闭未完成握手让 io 尽快排空。
- **不区分线程数**：每连接状态只被自己的完成链触碰，计数与「谁先到」
  判定全部走原子量，多 net 线程下无需加锁。

### 关闭语义（无 close_notify）

服务端关闭直接关底层 fd，不发 TLS close_notify。引入 close_notify 意味着
一条「等对端 close_notify」的挂起路径和新的超时面，而游戏长连接的服务端
主动断开已由帧协议层表达。已知取舍：客户端可能读到 truncation 错误而非
干净 EOF；重连即恢复。

### 明文/TLS 互斥（无 opportunistic TLS）

一个监听端口要么全明文要么全 TLS。不做 STARTTLS / 首字节嗅探降级：
「允许降级」本身就是主动降权攻击面，且帧协议首字节与 TLS ClientHello
无从区分，嗅探只会把判断变成猜。

### 证书加载与热更新

- **v1 = 启动时一次性加载**。`ssl::context` 在 bootstrap 构造（fail-loud），
  listener 以 `shared_ptr` 持有；证书轮换 = 重启服务。
- **不做 v1 热更新**，理由：context 被 in-flight `ssl::stream` 持引用，
  热替换需要 atomic `shared_ptr` swap + 存量连接绑定旧 context 的生命周期
  管理；而证书轮换是低频运维事件，收益撑不起这段复杂度。后续路径已由
  shared_ptr 形态预留：ops console `POST /ops/tls-reload` → make 新
  context → swap 进 listener → 新连接用新 context、旧连接自然消化。
- 约束：最低 TLS 1.2（`SSL_CTX_set_min_proto_version`）、禁压缩；密码
  套件用 OpenSSL 服务器默认，白名单定制留作后续可调项；不加载加密
  私钥（`SSL_CTX_use_PrivateKey_file` 对带口令 key 直接失败，错误如实
  上报）。

### 与 beast HTTP 的关系

ops/console HTTP（http_server.cpp，boost::beast）是运维面，通常本机/内网
可达，TLS 化是独立需求且 beast 侧有现成的 `beast::ssl_stream` 形态。本设计
不触碰它们；v1 覆盖面 = `TcpListener`/`TcpSession` 客户端面。

### 观测

`TcpListener` 新增原子计数，经既有 `ListenerRegistry` snapshot 进入
/ops/metrics gateway 块：

- `shield_gateway_tls_handshakes_total{port=...}`（成功握手）；
- `shield_gateway_tls_handshake_failures_total{port=...}`（失败 + 超时）。

明文监听器这两个值恒为 0；prom_emit_group 对空样本组不输出，纯明文部署
的 /ops/metrics 输出保持逐字节不变。

### 备选方案（否决理由）

- **全局 `network.tls` 节点**：仓库不存在全局 network 节点，为 TLS 单开
  全局面破坏「网络配置挂在 actor」的既有模型。
- **STARTTLS / opportunistic TLS**：见上，主动降权面。
- **BoringSSL / mbedTLS**：引入第二 TLS 栈零收益；OpenSSL 已在依赖图。
- **同步握手**：阻塞 net 线程；慢握手客户端（或恶意慢连）以 O(连接数)
  拖垮 accept 循环——必须 async + deadline。
- **会话模板化（`TcpSession<Stream>`）**：listener 持有
  `shared_ptr<Session>`，模板会把传输类型泄漏进注册表/网关/桥接全部下游；
  每连接一次虚调用不构成成本理由。

## 测试策略

1. **配置解析/校验**（test_cov_config.cpp 增臂）：`tls` 非映射、enabled
   缺 cert/key、空路径、handshake_timeout_ms 越界、enabled 非布尔——各自
   命中明确错误文案；**未配 tls 的 actor 配置逐字段不变**（零回归锚）。
2. **tls_context 单元**（test_cov_tls.cpp 前半）：缺文件 → false + 错误文案；
   坏 PEM → false；cert/key 不匹配 → false；合法自签对 → true。
3. **bootstrap fail-loud**（test_cov_bootstrap 增臂）：enabled 指向不存在的
   证书文件 → 启动失败（对应 bootstrap 错误文案）。
4. **回环握手**（tests/coverage/test_cov_tls.cpp，随全部构建树运行）：自签证书 fixture
   （CN=localhost，SAN IP:127.0.0.1 / DNS:localhost，2040 过期）：
   - TLS 客户端（verify_peer + 信任 fixture CA）→ TLS 监听：握手成功 +
     帧往返（on_packet 收到回显）；
   - 明文客户端 → TLS 监听：连接被关、失败计数 +1、无 session；
   - 握手超时：连上不发 ClientHello + 小超时 → 关闭 + 失败计数；
   - 拒绝优先：blocklist 命中 IP 在握手前被拒，TLS 计数不动；
   - 计数经 ListenerRegistry 在 /ops/metrics 出现（cov 用例）。
5. **明文回归**：既有 test_tcp_listener / test_cov_listener / gateway
   端到端全绿即锚（它们就是零回归的判据）。
6. **门**：build / build-dbg / build-plug / build-net / build-cov 五树
   ctest 全绿 + gcovr（line 98 / branch 100 / function 100，`../src/`）。

## 里程碑

- **M1（本任务，已落地）**：配置面 + context fail-loud + SessionStream +
  accept TLS 分支 + 握手计数 + /ops/metrics 导出 + 上述 1–5 测试。
- **M2（未排期）**：ops console 证书热更新（atomic context swap）、
  beast HTTP 面 TLS、密码套件白名单、客户端证书校验（mTLS）。
