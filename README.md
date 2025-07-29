# Shield 游戏服务器框架

Shield 是一个现代化的 C++ 游戏服务器框架，融合了 Pitaya 的分布式架构和 Skynet 的高性能并发模型，专为构建大型多人在线游戏而设计。

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

## 📚 详细文档

- **[开发指南](docs/development-guide.md)** - 环境搭建、依赖安装、开发配置
- **[架构设计](docs/architecture.md)** - 系统架构、框架对比、设计理念
- **[API 文档](docs/api/)** - 各模块详细接口文档
- **[配置指南](docs/configuration.md)** - 配置文件详解和最佳实践
- **[开发路线图](docs/roadmap.md)** - 功能规划和版本计划

### 📖 在本地构建文档

本项目使用 [mdBook](https://rust-lang.github.io/mdBook/) 构建文档，您可以按照以下步骤在本地构建和查看文档：

#### 安装 mdBook

**方法一：使用 Cargo 安装（推荐）**
```bash
# 确保已安装 Rust 和 Cargo
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# 安装 mdBook
cargo install mdbook
```

**方法二：从预编译的二进制文件安装**
```bash
# macOS (使用 Homebrew)
brew install mdbook

# 或下载预编译的二进制文件
# 访问 https://github.com/rust-lang/mdBook/releases
# 下载适合您系统的版本并解压到 PATH 路径中
```

#### 构建和查看文档

```bash
# 在项目根目录下构建文档
mdbook build

# 启动本地服务器并自动打开浏览器查看文档
mdbook serve --open

# 或者手动打开浏览器访问 http://localhost:3000
```

构建完成后，静态文档将生成在 `book/` 目录中，您也可以直接打开 `book/index.html` 文件查看。

## 📄 许可证

本项目采用 [Apache License 2.0 许可证](LICENSE)。