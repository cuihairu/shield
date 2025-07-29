# Shield 游戏服务器框架

欢迎使用 Shield 游戏服务器框架！这是一个现代化的 C++ 游戏服务器框架，融合了 Pitaya 的分布式架构和 Skynet 的高性能并发模型，专为构建大型多人在线游戏而设计。

## ✨ 核心特性

- **🌐 分布式架构**: 微服务架构，支持水平扩展
- **🚀 高性能并发**: 基于 Actor 模型的多线程处理
- **🔧 Lua 脚本集成**: C++ 引擎 + Lua 业务逻辑，支持热重载
- **🌍 多协议支持**: TCP/HTTP/WebSocket 统一处理
- **🔄 服务发现**: 支持 etcd/consul/nacos/redis 多种注册中心

## 🚀 快速开始

```bash
# 克隆项目
git clone https://github.com/your-repo/shield.git
cd shield

# 构建运行
cmake -B build -S .
cmake --build build
./bin/shield --config config/shield.yaml
```

## 📚 文档导航

本文档包含以下主要部分：

- **[安装与配置](development-guide.md)** - 环境搭建和依赖安装指南
- **[架构设计](architecture.md)** - 了解 Shield 的整体架构和设计理念  
- **[API 参考](api/core.md)** - 各模块详细的 API 文档和使用示例
- **[配置指南](configuration.md)** - 配置文件和参数详解
- **[开发路线图](roadmap.md)** - 功能规划和版本计划

## 🎯 适用场景

Shield 特别适合以下类型的项目：

- **大型 MMO 游戏**: 需要支持大量并发用户
- **实时竞技游戏**: 对延迟和性能要求极高
- **社交游戏**: 需要复杂的业务逻辑和频繁更新
- **企业级应用**: 对稳定性和可维护性要求高

## 🤝 社区与支持

- **GitHub 仓库**: [https://github.com/your-repo/shield](https://github.com/your-repo/shield)
- **问题反馈**: [Issues](https://github.com/your-repo/shield/issues)
- **技术讨论**: [Discussions](https://github.com/your-repo/shield/discussions)

开始探索 Shield 框架，构建您的下一代游戏服务器！