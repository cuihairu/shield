# 脚本系统

Shield 集成 Lua 5.4+ 作为脚本引擎，通过 Sol2 提供 C++ 绑定。

## LuaEngine

```cpp
#include <shield/script/lua_engine.hpp>

shield::script::LuaEngine engine("my_engine");
engine.on_init(ctx);
engine.on_start();
```

### 加载脚本

```cpp
engine.load_script("scripts/player.lua");
```

### 调用函数

```cpp
auto result = engine.call_function<int>("get_level");
auto value = engine.get_global<int>("player_level");
```

### 执行代码

```cpp
engine.execute_string("player.level = player.level + 1");
```

## LuaVMPool

虚拟机池管理多个 Lua VM 实例：

```cpp
#include <shield/script/lua_vm_pool.hpp>

shield::script::LuaVMPoolConfig config;
config.min_size = 2;
config.max_size = 10;
config.script_path = "scripts/";

shield::script::LuaVMPool pool(config);
pool.on_init(ctx);
```

### 获取 VM

```cpp
auto vm = pool.acquire();
vm->call_function<void>("process", data);
pool.release(std::move(vm));
```

## LuaActor

Lua 脚本可作为 Actor 运行：

```cpp
#include <shield/actor/lua_actor.hpp>

shield::actor::LuaActor actor("player", "scripts/player.lua");
actor.initialize();
```

## C++ 函数注册

```cpp
engine.register_function("send_message", [](const std::string& target, const std::string& msg) {
    // 发送消息逻辑
});

// Lua 中调用
-- send_message("player_001", "hello")
```
