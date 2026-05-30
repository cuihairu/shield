# Shield 指南

Shield 是一个基于 C++20 的 Skynet 启发的、Actor 模型的、Lua 优先的游戏服务器运行时，使用 CAF 作为 Actor 传输基础。

## 技术栈

- **C++20**: 现代 C++ 标准
- **CAF**: C++ Actor Framework（Actor 传输层）
- **Boost.Asio / Boost.Beast**: 异步网络 I/O + HTTP/WebSocket
- **Lua 5.4+ / sol2**: 业务脚本引擎
- **CMake 3.30+**: 构建系统
- **vcpkg**: 依赖管理
- **平台**: Windows, macOS, Linux

## 核心模块

| 模块 | 说明 |
|------|------|
| 服务层 | Skynet 风格 API（send/call/query/timeout/fork） |
| 网关 | TCP/HTTP/WebSocket/UDP 统一中间件管道分发 |
| 脚本引擎 | Lua VM 池 + shield.* 全局 API |
| 服务发现 | Static/Redis/Nacos/Consul/Etcd 多后端 |
| 配置管理 | YAML 配置，支持热更新 |
| 可观测性 | 健康检查、Prometheus 指标、调试控制台 |

## 下一步

- [安装指南](./installation.md)
- [快速开始](./quickstart.md)
