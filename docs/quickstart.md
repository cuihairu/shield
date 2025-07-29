# 快速上手

本指南将帮助您快速开始使用 Shield 游戏服务器框架。

## 📋 前置要求

在开始之前，请确保您的系统满足以下要求：

- **操作系统**: Linux/macOS/Windows (推荐 Linux)
- **编译器**: 支持 C++20 的编译器 (GCC 10+, Clang 12+, MSVC 2019+)
- **CMake**: 3.20 或更高版本
- **vcpkg**: 包管理器

## 🚀 5 分钟快速体验

### 1. 获取源码

```bash
git clone https://github.com/your-repo/shield.git
cd shield
```

### 2. 安装依赖

```bash
# 安装 vcpkg (如果尚未安装)
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)
cd ..
```

### 3. 编译项目

```bash
# 配置和编译
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel $(nproc)
```

### 4. 运行服务器

```bash
# 启动服务器
./bin/shield --config config/shield.yaml
```

如果看到类似输出，说明服务器启动成功：

```
[INFO] Shield 游戏服务器框架 v1.0.0
[INFO] 网关服务启动: 0.0.0.0:8080 (TCP)
[INFO] HTTP 服务启动: 0.0.0.0:8081 (HTTP)  
[INFO] WebSocket 服务启动: 0.0.0.0:8082 (WebSocket)
[INFO] 服务器启动完成，等待连接...
```

### 5. 测试连接

**HTTP 测试:**
```bash
curl http://localhost:8081/api/health
# 应该返回: {"status":"ok","timestamp":1234567890}
```

**WebSocket 测试:**
```bash
# 使用 wscat 或其他 WebSocket 客户端
wscat -c ws://localhost:8082
> {"type":"ping"}
< {"type":"pong","timestamp":1234567890}
```

## 🎮 第一个游戏逻辑

创建一个简单的玩家 Actor 来处理游戏逻辑：

### 1. 创建 Lua 脚本

创建文件 `scripts/hello_player.lua`:

```lua
-- 玩家状态
local player_state = {
    name = "新玩家",
    level = 1,
    exp = 0,
    gold = 100
}

-- 初始化函数
function on_init()
    log_info("玩家 Actor 初始化完成")
end

-- 消息处理函数
function on_message(msg)
    log_info("收到消息: " .. msg.type)
    
    if msg.type == "get_info" then
        return {
            success = true,
            data = {
                name = player_state.name,
                level = tostring(player_state.level),
                exp = tostring(player_state.exp),
                gold = tostring(player_state.gold)
            }
        }
    elseif msg.type == "add_exp" then
        local exp_gain = tonumber(msg.data.exp) or 0
        player_state.exp = player_state.exp + exp_gain
        
        -- 检查升级
        if player_state.exp >= player_state.level * 100 then
            player_state.level = player_state.level + 1
            player_state.exp = 0
            log_info("玩家升级到 " .. player_state.level .. " 级！")
        end
        
        return {
            success = true,
            data = {
                level = tostring(player_state.level),
                exp = tostring(player_state.exp)
            }
        }
    end
    
    return {success = false, error = "未知消息类型"}
end
```

### 2. 测试脚本

重启服务器后，使用 HTTP API 测试：

```bash
# 获取玩家信息
curl -X POST http://localhost:8081/api/actor \
  -H "Content-Type: application/json" \
  -d '{
    "actor_id": "player_001",
    "script": "hello_player.lua",
    "message": {
      "type": "get_info"
    }
  }'

# 添加经验值
curl -X POST http://localhost:8081/api/actor \
  -H "Content-Type: application/json" \
  -d '{
    "actor_id": "player_001", 
    "message": {
      "type": "add_exp",
      "data": {"exp": "150"}
    }
  }'
```

## 🛠️ 常用配置

### 修改端口

编辑 `config/shield.yaml`:

```yaml
gateway:
  listener:
    tcp_port: 9090    # 修改 TCP 端口
    http_port: 9091   # 修改 HTTP 端口  
    ws_port: 9092     # 修改 WebSocket 端口
```

### 调整线程数

```yaml
gateway:
  threading:
    io_threads: 8     # I/O 线程数 (建议设为 CPU 核心数)
```

### 启用调试日志

```yaml
logger:
  level: debug        # 设置为 debug 级别
  console: true       # 启用控制台输出
```

## 🔍 故障排除

### 编译失败

**问题**: 找不到依赖库
```bash
# 解决方案：确保 vcpkg 路径正确
export VCPKG_ROOT=/path/to/vcpkg
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

**问题**: C++20 支持问题
```bash
# 解决方案：检查编译器版本
gcc --version  # 确保 GCC 10+
clang --version  # 确保 Clang 12+
```

### 运行时问题

**问题**: 端口被占用
```bash
# 检查端口占用
netstat -tlnp | grep :8080

# 修改配置文件中的端口号
```

**问题**: 配置文件找不到
```bash
# 确保从项目根目录运行
cd /path/to/shield
./bin/shield --config config/shield.yaml
```

## 📚 下一步

现在您已经成功运行了 Shield 服务器，可以继续探索：

- **[架构设计](architecture.md)** - 了解框架的整体设计
- **[API 参考](api/core.md)** - 深入学习各个模块
- **[配置指南](configuration.md)** - 详细的配置选项
- **[开发指南](development-guide.md)** - 完整的开发环境搭建

开始构建您的游戏服务器吧！🎉