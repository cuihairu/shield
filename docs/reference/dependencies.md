# 依赖说明

Shield 依赖以下第三方库。

## 核心依赖

| 库 | 版本 | 用途 | 许可证 |
|---|------|------|--------|
| CAF | 0.19+ | Actor 模型 | BSD-3-Clause |
| Boost | 1.80+ | 网络、序列化 | Boost Software License |
| Sol2 | 3.3+ | Lua 绑定 | MIT |
| yaml-cpp | 0.7+ | YAML 解析 | MIT |
| nlohmann/json | 3.11+ | JSON 处理 | MIT |
| Lua | 5.4+ | 脚本引擎 | MIT |
| hiredis | 1.0+ | Redis 客户端 | BSD-3-Clause |
| redis++ | 1.3+ | Redis C++ 客户端 | MIT |
| OpenSSL | 3.0+ | TLS 支持 | Apache-2.0 |

## 可选依赖

| 库 | 版本 | 用途 | 许可证 |
|---|------|------|--------|
| prometheus-cpp | 1.1+ | Prometheus 指标 | MIT |
| protobuf | 3.20+ | Protocol Buffers | BSD-3-Clause |
| msgpack-cxx | 6.0+ | MessagePack 序列化 | BSL-1.0 |

## 安装依赖

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    libboost-all-dev \
    liblua5.4-dev \
    libssl-dev \
    libyaml-cpp-dev \
    libhiredis-dev \
    nlohmann-json3-dev
```

### macOS

```bash
brew install boost lua openssl yaml-cpp hiredis nlohmann-json
```

### vcpkg

```bash
vcpkg install \
    caf boost-asio boost-beast \
    sol2 yaml-cpp nlohmann-json \
    hiredis redis-plus-plus \
    openssl
```

## 依赖版本兼容性

Shield 0.1.0 已测试以下组合：

- GCC 10 + Ubuntu 22.04
- Clang 14 + Ubuntu 22.04
- Clang 15 + macOS 13
