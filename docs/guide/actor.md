# Actor 系统

Shield 基于 CAF (C++ Actor Framework) 实现分布式 Actor 模型。

## Actor 概念

Actor 是并发计算的基本单元，具有以下特性：

- **隔离状态**: 每个 Actor 有独立的状态
- **消息传递**: 通过异步消息通信
- **位置透明**: Actor 可在本地或远程

## ActorSystemConfig

```cpp
#include <shield/actor/actor_system_config.hpp>

shield::actor::ActorSystemConfig config;
config.worker_threads = 4;
config.middleman_network_backend = "asio";
```

## 创建 Actor

### C++ Actor

```cpp
#include <caf/all.hpp>

using my_actor = caf::typed_actor<...>;

my_actor::behavior_type typed_my_actor(my_actor::pointer_selfptr self) {
    return {
        [](int value) {
            // 处理消息
        }
    };
}
```

### Lua Actor

```lua
-- scripts/example_actor.lua

function on_init()
    log_info("Actor initialized")
end

function on_message(msg)
    if msg.type == "ping" then
        return create_response(true, {reply = "pong"})
    end
end
```

## Actor 注册

```cpp
#include <shield/actor/actor_registry.hpp>

auto& registry = shield::actor::ActorRegistry::instance();
registry.register_actor("player", player_actor_behavior);
```

## 分布式 Actor

```cpp
#include <shield/actor/distributed_actor_system.hpp>

shield::actor::DistributedActorSystem system;
system.connect("remote-host", 8080);
system.spawn_remote("player", "remote-actor-id");
```
