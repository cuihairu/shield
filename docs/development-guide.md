# 开发指南

本文档只保留当前仓库仍然需要长期维护的开发信息：环境要求、构建方式、目录结构、调试入口。

## 系统要求

### 操作系统支持

- **Linux**: Ubuntu 20.04+, CentOS 8+, Debian 11+
- **macOS**: macOS 11.0+ (Big Sur)
- **Windows**: Windows 10+

### 编译器要求

- **GCC**: 11+ (支持 C++20)
- **Clang**: 14+ (支持 C++20)
- **MSVC**: Visual Studio 2019 16.8+ (支持 C++20)

### 必需工具

- **CMake**: 3.30+
- **Git**: 2.25+
- **vcpkg**: 最新版本

## 开发环境搭建

### 1. 安装基础工具

#### Ubuntu/Debian

```bash
sudo apt update
sudo apt install -y build-essential cmake git curl zip unzip tar pkg-config
```

#### macOS

```bash
xcode-select --install
brew install cmake git curl zip unzip tar pkg-config
```

#### Windows

安装 Visual Studio 2019+（含 C++ CMake 工具），或使用 MinGW。

### 2. 安装 vcpkg 包管理器

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh        # Linux/macOS
# bootstrap-vcpkg.bat       # Windows

export VCPKG_ROOT=/path/to/vcpkg
```

### 3. 克隆并构建 Shield

```bash
git clone https://github.com/cuihairu/shield.git
cd shield

# 一键构建
./build.sh debug            # Linux/macOS
# build.bat debug            # Windows
```

或手动 CMake 构建：

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build --parallel $(nproc)
```

## 依赖管理

### 核心依赖

| 依赖库 | 用途 |
|--------|------|
| **CAF** | Actor 系统 |
| **Boost.Asio** | 网络 I/O |
| **Boost.Beast** | HTTP/WebSocket |
| **Lua 5.4+** | 脚本引擎 |
| **sol2** | Lua 绑定 |
| **nlohmann-json** | JSON 序列化 |
| **yaml-cpp** | 配置文件 |
| **Boost.Log** | 日志系统 |
| **Boost.ProgramOptions** | 命令行解析 |

所有依赖通过 vcpkg 自动安装，无需手动操作。

## CMake 构建选项

```bash
# 发布构建
cmake -DCMAKE_BUILD_TYPE=Release

# 启用编译命令导出 (IDE 支持)
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 启用指标
cmake -DSHIELD_ENABLE_METRICS=ON
```

## 项目结构

```
shield/
├── CMakeLists.txt            # 主构建文件（shield_core + shield_extensions）
├── vcpkg.json                # 依赖配置
├── build.sh / build.bat      # 一键构建脚本
├── Dockerfile                # 多阶段生产构建
├── config/
│   └── app.yaml              # 默认配置
├── scripts/
│   ├── shield_service.lua    # Lua 服务基类
│   └── *.lua                 # 业务脚本
├── include/shield/
│   ├── core/                 # 生命周期、ApplicationContext
│   ├── actor/                # CAF Actor、LuaActor、DistributedActorSystem
│   ├── script/               # LuaVMPool、LuaServiceApi
│   ├── service/              # send/call/query/timeout/fork
│   ├── gateway/              # GatewayRequest、MiddlewareChain、GameGateway
│   ├── protocol/             # TCP/HTTP/WS/UDP 协议适配
│   ├── discovery/            # IServiceDiscovery 多后端
│   ├── config/               # ConfigManager
│   ├── log/                  # Logger
│   └── serialization/        # JSON
├── src/                      # 对应实现
├── templates/
│   ├── single-node/          # 单节点模板
│   └── multi-node/           # 多节点模板
├── docs/                     # 文档
└── tests/                    # 测试
```

## 调试环境

### VS Code

创建 `.vscode/launch.json`：

```json
{
    "version": "0.2.0",
    "configurations": [{
        "name": "Debug Shield",
        "type": "cppdbg",
        "request": "launch",
        "program": "${workspaceFolder}/build/bin/shield",
        "args": ["server", "--config", "config/app.yaml"],
        "cwd": "${workspaceFolder}"
    }]
}
```

### 调试控制台

Shield 内置 TCP 调试控制台（端口 13000），运行时可直接交互：

```bash
telnet localhost 13000
# list, info <name>, send <name> <json>, call <name> <json>, nodes
```

## 常见问题

### 编译：找不到依赖

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

### 运行：端口被占用

```bash
# Linux/macOS
lsof -i :8082

# Windows
netstat -ano | findstr :8082
```

修改 `config/app.yaml` 中的端口号即可。

## 测试

```bash
cmake --build build --target test
```

按模块运行测试时，直接使用 `build/bin/` 下的测试可执行文件。

## 推荐资源

- [CAF 用户手册](https://actor-framework.readthedocs.io/)
- [Lua 5.4 参考手册](https://www.lua.org/manual/5.4/)
- [Boost.Beast 文档](https://www.boost.org/doc/libs/release/libs/beast/)
