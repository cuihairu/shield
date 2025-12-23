# Shield 指南

Shield 是一个基于 C++17 的游戏服务器框架，采用 Actor 模型设计，支持多协议网络通信和 Lua 脚本扩展。

## 技术栈

- **C++17**: 现代C++标准
- **CAF**: C++ Actor Framework
- **Boost.Asio**: 异步网络IO
- **Boost.Beast**: HTTP/WebSocket
- **Lua 5.4+**: 脚本引擎
- **CMake 3.30+**: 构建系统
- **平台**: Linux, macOS

## 核心模块

| 模块 | 说明 |
|------|------|
| Actor 系统 | 基于 CAF 的分布式 Actor 模型 |
| 网络层 | TCP/UDP/HTTP/WebSocket 支持 |
| 脚本引擎 | Lua 集成，支持热重载 |
| 服务发现 | Nacos/Consul/Etcd/Redis 支持 |
| 配置管理 | YAML 配置，支持热更新 |
| 健康检查 | 多维度指标监控 |
| 依赖注入 | IoC 容器支持 |

## 下一步

- [安装指南](./installation.md)
- [快速开始](./quickstart.md)
