# 快速开始

## Hello World

创建一个简单的 Shield 服务器：

```cpp
#include <shield/core/application_context.hpp>
#include <shield/log/logger.hpp>

int main() {
    // 初始化日志
    shield::log::LogConfig log_config;
    shield::log::Logger::init(log_config);

    // 获取应用上下文
    auto& ctx = shield::core::ApplicationContext::instance();

    SHIELD_LOG_INFO << "Shield server started";

    // 启动服务
    ctx.start();

    return 0;
}
```

## 配置文件

创建 `config/shield.yaml`:

```yaml
log:
  level: info
  file: logs/shield.log

actor:
  worker_threads: 4

network:
  gateways:
    - port: 8080
      protocol: tcp
```

## Lua Actor

创建 `scripts/player.lua`:

```lua
local player = {
    id = nil,
    name = "",
    level = 1,
    x = 0,
    y = 0
}

function on_init()
    player.id = actor_id
    log_info("Player initialized: " .. player.id)
end

function on_message(msg)
    if msg.type == "login" then
        player.name = msg.data.player_name
        player.level = tonumber(msg.data.level) or 1
        return create_response(true, {
            player_id = player.id,
            player_name = player.name,
            level = player.level
        })
    elseif msg.type == "move" then
        player.x = tonumber(msg.data.x) or player.x
        player.y = tonumber(msg.data.y) or player.y
        return create_response(true, {
            x = player.x,
            y = player.y
        })
    end
end
```

## 运行

```bash
# 构建项目
cd build
make -j$(nproc)

# 启动服务器
./bin/shield --config ../config/shield.yaml
```

## 测试

```bash
# 运行所有测试
ctest --output-on-failure

# 运行特定测试
./tests/unit/test_logger
./tests/actor/test_lua_actor
```
