# 框架对比分析

## Shield vs Skynet

| 特性 | Shield | Skynet |
|------|--------|--------|
| **语言** | C++20 + Lua | C + Lua |
| **Actor 传输** | CAF | 自研轻量 Actor |
| **网络协议** | TCP + HTTP + WebSocket + UDP | TCP |
| **服务发现** | 多后端（Static/Redis/Nacos/Consul/Etcd） | 无内置 |
| **HTTP 支持** | 内置 Boost.Beast | 需自建或第三方 |
| **WebSocket** | 内置 RFC 6455 实现 | 无内置 |
| **指标/监控** | Prometheus + RuntimeDiagnostics | 无内置 |
| **配置** | YAML | .config 文件 |
| **跨平台** | Windows / macOS / Linux | Linux only |
| **热重载** | Lua 脚本热重载 | Lua 热重载 |
| **资源占用** | 中等 | 极低 |

Shield 以 Skynet 为设计参考，提供 Skynet 风格的服务 API（send/call/query/uniqueservice），但在传输层使用 CAF，在网络层支持更多协议，在运维层内置服务发现和可观测性。

Shield 不覆盖 Skynet 的以下能力：harbor 集群、snax 框架、sharedata 机制。详见 [Skynet 对比](skynet-comparison.md)。

## Shield vs KBEngine

| 特性 | Shield | KBEngine |
|------|--------|----------|
| **定位** | 游戏服务器运行时 | 游戏引擎服务器 |
| **脚本语言** | Lua | Python |
| **Actor 模型** | CAF | 自研 Cell/Space |
| **客户端 SDK** | 无（仅服务端） | 多平台 SDK |
| **实体系统** | 无内置 | 内置 Entity/Avatar |
| **数据库集成** | 可选扩展 | 内置 |

## Shield vs Pomelo

| 特性 | Shield | Pomelo |
|------|--------|--------|
| **语言** | C++20 + Lua | Node.js |
| **并发模型** | Actor (CAF) | 事件循环 |
| **性能** | 高（原生代码） | 中等（V8 JIT） |
| **协议** | TCP/HTTP/WS/UDP | TCP/HTTP/WS |
| **适用场景** | MMO/竞技 | 中小型游戏 |
