# Shield Framework Lua IoC Integration Guide

## 🚀 概览

Shield框架提供了完整的**C++ ↔ Lua IoC集成**，解决了"Lua中IOC难以实现"的问题。我们的解决方案：

### 核心设计理念

1. **C++为核心，Lua为扩展** - 性能关键组件用C++，业务逻辑用Lua
2. **双向依赖注入** - C++服务可注入到Lua，Lua服务可注入到C++
3. **事件系统统一** - C++和Lua事件无缝互通
4. **热重载支持** - Lua脚本和配置支持热重载

## 🎯 解决方案架构

```
┌─────────────────┐    ┌─────────────────┐
│   C++ Services  │◄──►│   Lua Services  │
│                 │    │                 │
│ • DatabaseSvc   │    │ • GameLogic     │
│ • NetworkSvc    │    │ • PlayerAI      │
│ • CacheSvc      │    │ • EventHandler  │
└─────────────────┘    └─────────────────┘
         ▲                       ▲
         │                       │
         ▼                       ▼
┌─────────────────────────────────────────┐
│         LuaIoCBridge                    │
│  • 双向服务注册                           │
│  • 事件转发                              │
│  • 健康检查聚合                          │
└─────────────────────────────────────────┘
```

## 📋 Lua IoC容器特性

### ✅ 已实现功能

| 功能 | Spring Boot | Shield Lua IoC | 说明 |
|------|-------------|--------------|----|
| 依赖注入 | @Autowired | dependencies表 | 自动解析和注入 |
| 生命周期 | @PostConstruct | on_init/on_start | 完整生命周期钩子 |
| 条件注册 | @ConditionalOnProperty | shield_conditional | 基于配置的条件注册 |
| 事件系统 | ApplicationEvent | shield_event_listener | 异步事件处理 |
| 健康检查 | Actuator | shield_health_check | 组件健康监控 |
| 配置管理 | @ConfigurationProperties | ConfigService | 热重载配置 |

## 🔧 使用方法

### 1. 基础Lua服务定义

```lua
-- 定义一个游戏逻辑服务
local GameLogicService = {
    -- 声明依赖 (类似Spring的@Autowired)
    dependencies = {
        database = "db_service",     -- 注入C++数据库服务
        cache = "cache_service",     -- 注入C++缓存服务
        config = "config"            -- 注入配置服务
    },
    
    -- 生命周期方法
    on_init = function(self, container)
        print("[GameLogic] Initializing with dependencies")
        self.player_data = {}
    end,
    
    on_start = function(self)
        print("[GameLogic] Game logic service started")
    end,
    
    on_config_reloaded = function(self)
        print("[GameLogic] Reloading game configuration")
        -- 重新加载游戏参数
    end,
    
    -- 业务方法
    process_player_action = function(self, player_id, action)
        -- 使用注入的依赖
        local player = self.database:find_player(player_id)
        if not player then
            return {success = false, error = "Player not found"}
        end
        
        -- 处理动作逻辑
        local result = self:handle_action(player, action)
        
        -- 缓存结果
        self.cache:set("player_" .. player_id, result, 300)
        
        return result
    end,
    
    handle_action = function(self, player, action)
        -- 游戏逻辑实现
        return {success = true, result = "Action processed"}
    end
}

-- 注册服务 (类似Spring的@Service)
shield_service("game_logic", GameLogicService)
```

### 2. 条件化服务注册

```lua
-- 只有在开发环境才启用的调试服务
local DebugService = {
    on_init = function(self, container)
        print("[Debug] Debug service enabled for development")
    end,
    
    dump_player_state = function(self, player_id)
        -- 调试功能实现
    end
}

-- 条件注册 (类似Spring的@ConditionalOnProperty)
shield_conditional("debug_service", DebugService, {
    property = "environment",
    value = "development"
})
```

### 3. 事件处理

```lua
-- 注册事件监听器 (类似Spring的@EventListener)
shield_event_listener("player_level_up", function(event_data, container)
    local game_logic = container:resolve("game_logic")
    local player_id = event_data.player_id
    local new_level = event_data.level
    
    print(string.format("[Event] Player %d leveled up to %d", player_id, new_level))
    
    -- 触发相关游戏逻辑
    game_logic:handle_level_up(player_id, new_level)
end, 100) -- 高优先级
```

### 4. 健康检查

```lua
-- 注册健康检查 (类似Spring Boot Actuator)
shield_health_check("game_logic", function(container)
    local game_logic = container:resolve("game_logic")
    
    if game_logic and game_logic.player_data then
        local player_count = 0
        for _ in pairs(game_logic.player_data) do 
            player_count = player_count + 1 
        end
        
        return {
            status = "UP",
            details = {
                active_players = tostring(player_count),
                memory_usage = collectgarbage("count") .. " KB"
            }
        }
    else
        return {
            status = "DOWN",
            details = {error = "Game logic not initialized"}
        }
    end
end)
```

## 🌉 C++ ↔ Lua 集成

### C++端集成代码

```cpp
#include "shield/script/lua_ioc_bridge.hpp"

void setup_lua_integration() {
    auto& context = shield::core::ApplicationContext::instance();
    auto& lua_engine = context.get_service<shield::script::LuaEngine>();
    
    // 创建C++ ↔ Lua桥接
    shield::lua::LuaIoCBridge bridge(context, *lua_engine);
    bridge.initialize();
    
    // 导出C++服务到Lua
    SHIELD_EXPORT_TO_LUA(DatabaseService, db_service);
    SHIELD_EXPORT_TO_LUA(CacheService, cache_service);
    SHIELD_EXPORT_TO_LUA(NetworkService, network_service);
    
    // 导入Lua服务到C++
    SHIELD_IMPORT_FROM_LUA(game_logic_service, game_logic);
    SHIELD_IMPORT_FROM_LUA(player_ai_service, player_ai);
    
    // 设置事件转发
    bridge.setup_event_forwarding();
    
    // 加载Lua IoC脚本
    bridge.load_lua_ioc_script("scripts/game_services.lua");
    
    // 启动Lua容器
    bridge.start_lua_container();
}

// 在C++中使用Lua服务
void use_lua_service() {
    auto& context = shield::core::ApplicationContext::instance();
    
    // 通过桥接调用Lua服务
    auto lua_service = context.get_service<shield::lua::CppLuaServiceWrapper>("game_logic_service");
    
    if (lua_service) {
        // 调用Lua服务的方法
        auto result = lua_service->call_lua_method("process_player_action", 123, "move_north");
        
        // 处理返回结果
        if (result.is<sol::table>()) {
            sol::table result_table = result.as<sol::table>();
            bool success = result_table["success"];
            SHIELD_LOG_INFO << "Lua service call result: " << (success ? "SUCCESS" : "FAILED");
        }
    }
}
```

### 事件双向转发

```cpp
// C++事件自动转发到Lua
context.get_event_publisher().publish_event(
    shield::events::lifecycle::ApplicationStartedEvent());
// ↓ 自动转发到Lua
// shield_publish_event("application_started", {...})

// Lua事件转发到C++
// shield_publish_event("player_died", {player_id = 123})
// ↓ 自动转发到C++
// context.get_event_publisher().publish_event(CustomPlayerEvent{...})
```

## 🎮 游戏开发示例

### 完整的游戏服务架构

```lua
-- ========================================
-- 游戏核心服务
-- ========================================

-- 玩家管理服务
local PlayerService = {
    dependencies = {
        database = "db_service",
        cache = "cache_service",
        events = "event_publisher"
    },
    
    online_players = {},
    
    on_init = function(self, container)
        print("[PlayerService] Initializing player management")
    end,
    
    on_player_connect = function(self, player_id, connection_info)
        -- 加载玩家数据
        local player_data = self.database:load_player(player_id)
        self.online_players[player_id] = player_data
        
        -- 发布事件
        shield_publish_event("player_connected", {
            player_id = player_id,
            timestamp = os.time()
        })
        
        return player_data
    end,
    
    on_player_disconnect = function(self, player_id)
        -- 保存玩家数据
        local player_data = self.online_players[player_id]
        if player_data then
            self.database:save_player(player_data)
            self.online_players[player_id] = nil
            
            shield_publish_event("player_disconnected", {
                player_id = player_id,
                timestamp = os.time()
            })
        end
    end
}

-- 战斗系统服务
local CombatService = {
    dependencies = {
        player_service = "player_service",
        config = "config"
    },
    
    on_init = function(self, container)
        self.damage_formulas = self.config:get("combat.damage_formulas")
    end,
    
    process_attack = function(self, attacker_id, target_id, skill_id)
        local attacker = self.player_service.online_players[attacker_id] 
        local target = self.player_service.online_players[target_id]
        
        if not attacker or not target then
            return {success = false, error = "Player not found"}
        end
        
        -- 计算伤害
        local damage = self:calculate_damage(attacker, target, skill_id)
        
        -- 应用伤害
        target.hp = math.max(0, target.hp - damage)
        
        -- 检查死亡
        if target.hp <= 0 then
            shield_publish_event("player_died", {
                player_id = target_id,
                killer_id = attacker_id,
                damage = damage
            })
        end
        
        return {
            success = true,
            damage = damage,
            target_hp = target.hp
        }
    end,
    
    calculate_damage = function(self, attacker, target, skill_id)
        -- 复杂的伤害公式
        local base_damage = attacker.attack * 1.2
        local defense_reduction = target.defense * 0.8
        return math.max(1, base_damage - defense_reduction)
    end
}

-- 注册服务
shield_service("player_service", PlayerService)
shield_service("combat_service", CombatService)

-- 战斗事件监听
shield_event_listener("player_died", function(event_data, container)
    local player_service = container:resolve("player_service")
    
    print(string.format("Player %d was killed by %d", 
        event_data.player_id, event_data.killer_id))
    
    -- 处理死亡逻辑
    player_service:handle_player_death(event_data.player_id)
end)
```

## 🔥 性能优化策略

### 1. 混合架构性能分配

```cpp
// 高频性能关键：C++实现
class NetworkService : public Service {
    // 消息序列化/反序列化 - 编译期优化
    template<typename MessageType>
    void send_message(int connection_id, const MessageType& msg) {
        // 零开销抽象，编译期特化
    }
};

// 业务逻辑：Lua实现  
// -- 灵活性优先，性能要求不高
// local PlayerLogic = {
//     process_quest_completion = function(self, player_id, quest_id)
//         -- 复杂的任务逻辑，支持热重载
//     end
// }
```

### 2. 缓存和预编译

```cpp
class LuaServiceCache {
    // 缓存已解析的Lua服务
    std::unordered_map<std::string, sol::object> cached_services_;
    
    // 预编译热点Lua函数
    std::unordered_map<std::string, sol::protected_function> compiled_functions_;
};
```

## 📊 监控和调试

### 统一健康检查端点

```http
GET /health
{
    "status": "UP",
    "components": {
        "cpp_services": {
            "database": {"status": "UP", "connections": 10},
            "network": {"status": "UP", "active_connections": 150}
        },
        "lua_services": {
            "game_logic": {"status": "UP", "active_players": 42},
            "combat_system": {"status": "UP", "battles_in_progress": 5}
        }
    }
}
```

## 🚀 总结

Shield的Lua IoC解决方案完美解决了"Lua中IOC难实现"的问题：

1. **保留C++性能优势** - 核心系统用C++
2. **发挥Lua灵活性** - 业务逻辑用Lua，支持热重载
3. **无缝集成** - 双向依赖注入和事件通信
4. **生产就绪** - 完整的监控、健康检查、错误处理

这套方案让游戏开发既有C++的性能，又有Lua的灵活性！🎮✨