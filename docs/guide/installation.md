# 安装指南

## 系统要求

- **操作系统**: Linux (Ubuntu 20.04+, CentOS 8+) 或 macOS 11+
- **编译器**: GCC 10+ / Clang 12+ / MSVC 2019+
- **CMake**: 3.30 或更高版本
- **vcpkg**: 包管理器

## 依赖安装

### 使用 vcpkg

```bash
# 安装 vcpkg
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh

# 设置环境变量
export VCPKG_ROOT=/path/to/vcpkg
```

### 主要依赖

Shield 依赖以下库：

- **CAF**: C++ Actor Framework
- **Boost**: ASIO, Beast, Serialization, Filesystem, Log, ProgramOptions
- **Sol2**: Lua 绑定
- **yaml-cpp**: YAML 配置解析
- **nlohmann/json**: JSON 处理
- **hiredis/redis++**: Redis 客户端
- **OpenSSL**: TLS 支持

### 可选依赖

- **prometheus-cpp**: Prometheus 指标导出
- **protobuf**: Protocol Buffers 序列化
- **msgpack-cxx**: MessagePack 序列化

## 编译

```bash
# 克隆项目
git clone https://github.com/cuihairu/shield.git
cd shield

# 构建
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
make -j$(nproc)

# 运行测试
ctest --output-on-failure
```

## 安装目录

编译后的文件位于 `build/` 目录：

- `bin/`: 可执行文件
- `lib/`: 库文件
- `include/shield/version.hpp`: 生成的版本头文件
