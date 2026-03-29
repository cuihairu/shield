# Shield 开发指南

本文档详细介绍如何搭建 Shield 游戏服务器框架的开发环境，包括依赖安装、配置、编译和调试。

## 📋 系统要求

### 操作系统支持
- **Linux**: Ubuntu 20.04+, CentOS 8+, Debian 11+
- **macOS**: macOS 11.0+ (Big Sur)
- **Windows**: Windows 10+ (使用 WSL2 推荐)

### 编译器要求
- **GCC**: 10.0+ (支持 C++20)
- **Clang**: 12.0+ (支持 C++20)
- **MSVC**: Visual Studio 2019 16.8+ (支持 C++20)

### 必需工具
- **CMake**: 3.20+
- **Git**: 2.25+
- **vcpkg**: 最新版本
- **Python**: 3.8+ (用于构建脚本)

## 🛠️ 开发环境搭建

### 1. 安装基础工具

#### Ubuntu/Debian
```bash
# 更新包管理器
sudo apt update

# 安装编译工具
sudo apt install -y build-essential cmake git curl zip unzip tar
sudo apt install -y pkg-config autoconf automake libtool

# 安装 GCC 10+ (如果默认版本不够)
sudo apt install -y gcc-10 g++-10
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100
```

#### CentOS/RHEL
```bash
# 安装开发工具
sudo yum groupinstall -y "Development Tools"
sudo yum install -y cmake git curl zip unzip tar
sudo yum install -y pkgconfig autoconf automake libtool

# 启用 PowerTools 仓库 (CentOS 8)
sudo yum config-manager --set-enabled powertools
```

#### macOS
```bash
# 安装 Xcode 命令行工具
xcode-select --install

# 安装 Homebrew (如果未安装)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装必需工具
brew install cmake git curl zip unzip tar pkg-config autoconf automake libtool
```

### 2. 安装 vcpkg 包管理器

```bash
# 克隆 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg

# 编译 vcpkg
./bootstrap-vcpkg.sh  # Linux/macOS
# ./bootstrap-vcpkg.bat  # Windows

# 设置环境变量 (添加到 ~/.bashrc 或 ~/.zshrc)
export VCPKG_ROOT=/path/to/vcpkg
export PATH=$VCPKG_ROOT:$PATH

# 集成到系统
./vcpkg integrate install
```

### 3. 克隆并构建 Shield

```bash
# 克隆项目
git clone https://github.com/your-repo/shield.git
cd shield

# 创建构建目录
mkdir build && cd build

# 配置项目
cmake -B . -S .. \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug

# 编译项目
cmake --build . --parallel $(nproc)

# 运行测试
ctest -V
```

## 📦 依赖管理

### 核心依赖列表

| 依赖库 | 版本要求 | 用途 | 安装状态 |
|--------|----------|------|----------|
| **Boost.Asio** | 1.82+ | 网络 I/O | 自动安装 |
| **Boost.Beast** | 1.82+ | HTTP/WebSocket | 自动安装 |
| **CAF** | 1.0.2+ | Actor 系统 | 自动安装 |
| **Lua** | 5.4.8+ | 脚本引擎 | 自动安装 |
| **sol2** | 3.5.0+ | Lua 绑定 | 自动安装 |
| **nlohmann-json** | 3.12.0+ | JSON 序列化 | 自动安装 |
| **yaml-cpp** | 0.8.0+ | 配置文件 | 自动安装 |
| **OpenSSL** | 3.0+ | 加密支持 | 自动安装 |

### 可选依赖

| 依赖库 | 版本要求 | 用途 |
|--------|----------|------|
| **protobuf** | 3.21+ | 二进制序列化 |
| **grpc** | 1.50+ | RPC 通信 |
| **etcd-cpp-apiv3** | 0.15+ | etcd 服务发现 |
| **redis-plus-plus** | 1.3+ | Redis 支持 |
| **cpprestsdk** | 2.10+ | REST API |

### 手动安装依赖 (如需要)

```bash
# 安装所有依赖
vcpkg install \
  boost-asio boost-beast boost-log boost-program-options boost-test boost-url \
  caf lua sol2 nlohmann-json yaml-cpp openssl \
  protobuf grpc etcd-cpp-apiv3[async] redis-plus-plus cpprestsdk[compression]

# 查看已安装包
vcpkg list
```

## 🔧 配置选项

### CMake 构建选项

```bash
# 调试构建 (默认)
cmake -DCMAKE_BUILD_TYPE=Debug

# 发布构建
cmake -DCMAKE_BUILD_TYPE=Release

# 带调试信息的发布构建
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo

# 启用编译命令导出 (IDE 支持)
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 静态链接
cmake -DBUILD_SHARED_LIBS=OFF

# 启用详细输出
cmake -DCMAKE_VERBOSE_MAKEFILE=ON
```

### Shield 特定选项

```bash
# 禁用测试构建
cmake -DSHIELD_BUILD_TESTS=OFF

# 启用示例构建
cmake -DSHIELD_BUILD_EXAMPLES=ON

# 启用覆盖率 (GCC/Clang)
cmake -DSHIELD_ENABLE_COVERAGE=ON

# 启用性能分析
cmake -DSHIELD_ENABLE_PROFILING=ON

# 启用内存检查
cmake -DSHIELD_ENABLE_SANITIZERS=ON
```

## 🗃️ 项目结构

```
shield/
├── README.md                 # 项目介绍
├── LICENSE                   # 许可证
├── CMakeLists.txt            # 主构建文件
├── vcpkg.json               # 依赖配置
├── config/                  # 配置文件
│   └── app.yaml           # 默认配置
├── docs/                    # 文档目录
│   ├── api/                # API 文档
│   ├── architecture.md     # 架构设计
│   ├── development-guide.md # 开发指南
│   └── roadmap.md          # 路线图
├── include/                 # 头文件
│   └── shield/
│       ├── core/           # 核心模块
│       ├── actor/          # Actor 系统
│       ├── net/            # 网络模块
│       ├── protocol/       # 协议处理
│       ├── script/         # Lua 集成
│       ├── discovery/      # 服务发现
│       └── serialization/ # 序列化
├── src/                     # 源代码
│   ├── main.cpp            # 主入口
│   ├── core/               # 核心实现
│   ├── actor/              # Actor 实现
│   ├── net/                # 网络实现
│   ├── protocol/           # 协议实现
│   ├── script/             # Lua 实现
│   ├── discovery/          # 服务发现实现
│   └── serialization/      # 序列化实现
├── scripts/                 # Lua 脚本
│   ├── player_actor.lua    # 玩家 Actor
│   ├── http_actor.lua      # HTTP Actor
│   └── websocket_actor.lua # WebSocket Actor
├── tests/                   # 测试代码
│   ├── unit/               # 单元测试
│   ├── integration/        # 集成测试
│   └── performance/        # 性能测试
├── examples/                # 示例项目
│   ├── simple_game/        # 简单游戏示例
│   └── chat_server/        # 聊天服务器示例
└── tools/                   # 开发工具
    ├── deploy/             # 部署脚本
    └── monitoring/         # 监控工具
```

## 🎯 开发工作流

### 1. 代码开发流程

```bash
# 1. 创建功能分支
git checkout -b feature/your-feature-name

# 2. 进行开发
# 编辑代码...

# 3. 编译和测试
cmake --build build
cd build && ctest -V

# 4. 代码格式化
clang-format -i src/**/*.cpp include/**/*.hpp

# 5. 提交代码
git add .
git commit -m "feat: add your feature description"

# 6. 推送分支
git push origin feature/your-feature-name

# 7. 创建 Pull Request
```

### 2. 调试环境配置

#### VS Code 配置

创建 `.vscode/launch.json`:
```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug Shield",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/bin/shield",
            "args": ["--config", "config/app.yaml"],
            "stopAtEntry": false,
            "cwd": "${workspaceFolder}",
            "environment": [],
            "externalConsole": false,
            "MIMode": "gdb",
            "setupCommands": [
                {
                    "description": "启用 gdb 的整齐打印",
                    "text": "-enable-pretty-printing",
                    "ignoreFailures": true
                }
            ],
            "preLaunchTask": "build"
        }
    ]
}
```

创建 `.vscode/tasks.json`:
```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build",
            "type": "shell",
            "command": "cmake",
            "args": ["--build", "build", "--parallel"],
            "group": {
                "kind": "build",
                "isDefault": true
            },
            "presentation": {
                "echo": true,
                "reveal": "always",
                "focus": false,
                "panel": "shared"
            },
            "problemMatcher": "$gcc"
        }
    ]
}
```

#### CLion 配置

1. 打开项目根目录
2. CLion 会自动检测 CMakeLists.txt
3. 配置 CMake 参数：
   ```
   -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
   -DCMAKE_BUILD_TYPE=Debug
   -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   ```



## 🐛 常见问题

### 编译问题

**Q: vcpkg 依赖安装失败**
```bash
# 清理 vcpkg 缓存
vcpkg remove --outdated
vcpkg install --reconfigure

# 更新 vcpkg
cd $VCPKG_ROOT
git pull
./bootstrap-vcpkg.sh
```

**Q: CMake 找不到依赖**
```bash
# 确保设置了 VCPKG_ROOT
export VCPKG_ROOT=/path/to/vcpkg

# 使用正确的工具链文件
cmake -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

**Q: 链接错误**
```bash
# 检查依赖是否正确安装
vcpkg list | grep boost
vcpkg list | grep caf

# 清理并重新构建
rm -rf build
mkdir build && cd build
cmake -B . -S .. [your-options]
cmake --build .
```

### 运行时问题

**Q: 配置文件找不到**
```bash
# 确保在正确目录运行
cd /path/to/shield
./bin/shield --config config/app.yaml

# 或使用绝对路径
./bin/shield --config /absolute/path/to/config/app.yaml
```

**Q: Lua 脚本加载失败**
```bash
# 检查脚本路径
ls scripts/
ls scripts/player_actor.lua

# 检查脚本语法
lua scripts/player_actor.lua
```

## 🔧 性能调优

### 编译优化
```bash
# 发布构建优化
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-O3 -march=native -DNDEBUG"

# 启用 LTO (链接时优化)
cmake -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
```

### 运行时调优
```yaml
# config/app.yaml 性能配置
gateway:
  threading:
    io_threads: 8  # 根据 CPU 核心数调整

lua_vm_pool:
  initial_size: 16   # 增加 VM 池大小
  max_size: 64
  idle_timeout_ms: 60000

actor_system:
  scheduler:
    max_threads: 16  # 根据 CPU 核心数调整
```

### 内存优化
```bash
# 启用内存检查 (开发时)
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=leak"

# 使用 jemalloc (生产环境)
sudo apt install libjemalloc-dev
export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
```

## 📚 推荐资源

### 学习资料
- [C++20 特性指南](https://en.cppreference.com/w/cpp/20)
- [Boost.Asio 文档](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html)
- [CAF 用户手册](https://actor-framework.readthedocs.io/)
- [Lua 5.4 参考手册](https://www.lua.org/manual/5.4/)

### 开发工具
- [Valgrind](https://valgrind.org/) - 内存检查
- [perf](https://perf.wiki.kernel.org/) - 性能分析
- [gdb](https://www.gnu.org/software/gdb/) - 调试器
- [clang-format](https://clang.llvm.org/docs/ClangFormat.html) - 代码格式化

### 监控工具
- [htop](https://htop.dev/) - 系统监控
- [iotop](http://guichaz.free.fr/iotop/) - I/O 监控
- [netstat](https://net-tools.sourceforge.io/) - 网络监控
- [tcpdump](https://www.tcpdump.org/) - 网络抓包

---

## 🤝 获取帮助

- **技术文档**: [docs/](../docs/)
- **问题反馈**: [GitHub Issues](https://github.com/your-repo/shield/issues)
- **技术讨论**: [GitHub Discussions](https://github.com/your-repo/shield/discussions)
- **邮件联系**: shield-dev@example.com

**祝您开发愉快！** 🚀
