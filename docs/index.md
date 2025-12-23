---
home: true
title: Shield
titleTemplate: false
heroImage: /logo.png
actions:
  - text: 快速开始
    link: /guide/quickstart.html
    type: primary
  - text: API 参考
    link: /api/
    type: secondary
features:
  - title: Actor 模型
    details: 基于 CAF 的分布式 Actor 模型，支持并发消息传递
  - title: 多协议网络
    details: 支持 TCP/UDP/HTTP/WebSocket 协议
  - title: Lua 脚本
    details: 集成 Lua 5.4+，支持热重载
  - title: 服务发现
    details: 支持 Nacos/Consul/Etcd/Redis
  - title: 配置管理
    details: YAML 配置，支持热更新
  - title: 健康检查
    details: 多维度指标监控

footer: MIT License | Copyright © 2024
---

# Shield 游戏服务器框架

Shield 是一个基于 C++17 的游戏服务器框架，采用 Actor 模型设计，支持多协议网络通信和 Lua 脚本扩展。

## 技术栈

- **C++17**: 现代C++标准
- **CAF**: C++ Actor Framework
- **Boost.Asio**: 异步网络IO
- **Boost.Beast**: HTTP/WebSocket
- **Lua 5.4+**: 脚本引擎
- **CMake 3.30+**: 构建系统
- **平台**: Linux, macOS

## 快速开始

```bash
# 克隆项目
git clone https://github.com/cuihairu/shield.git
cd shield

# 构建
mkdir build && cd build
cmake ..
make -j$(nproc)

# 运行测试
ctest --output-on-failure
```

