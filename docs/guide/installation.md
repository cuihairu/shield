# 安装指南

## 系统要求

- **操作系统**: Windows 10+ / macOS 11+ / Linux (Ubuntu 20.04+)
- **编译器**: GCC 11+ / Clang 14+ / MSVC 2019 16.8+（C++20 支持）
- **CMake**: 3.30+
- **vcpkg**: 最新版本

## 依赖安装

### 安装 vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh        # Linux/macOS
# bootstrap-vcpkg.bat       # Windows

export VCPKG_ROOT=/path/to/vcpkg
```

### 核心依赖

所有依赖通过 vcpkg 自动安装（定义在 `vcpkg.json` 中）：

- **CAF**: C++ Actor Framework
- **Boost**: ASIO, Beast, Log, ProgramOptions
- **sol2**: Lua 绑定
- **Lua 5.4+**: 脚本引擎
- **yaml-cpp**: YAML 配置
- **nlohmann/json**: JSON 处理

## 构建

```bash
# 克隆项目
git clone https://github.com/cuihairu/shield.git
cd shield

# 一键构建
./build.sh release           # Linux/macOS
# build.bat release           # Windows
```

或手动 CMake 构建：

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel $(nproc)
```

## 验证

```bash
./build/bin/shield server --config config/app.yaml
```

访问 `http://localhost:8082/health` 确认服务运行。

## 下一步

- [快速开始](./quickstart.md)
- [开发指南](../development-guide.md)
