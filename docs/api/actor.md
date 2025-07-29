# Actor 系统 API 文档

Actor 系统是 Shield 框架的核心组件，提供分布式、高并发的消息驱动计算模型。基于 CAF (C++ Actor Framework) 实现，支持跨节点透明通信。

## 📋 模块概览

Actor 系统包含以下主要组件：

- `DistributedActorSystem`: 分布式 Actor 系统管理
- `ActorRegistry`: Actor 注册表和生命周期管理
- `LuaActor`: Lua 脚本驱动的 Actor 实现
- `ActorSystemCoordinator`: 系统协调器

## 🌐 DistributedActorSystem 分布式系统

分布式 Actor 系统提供跨节点 Actor 管理和通信功能。

### 类定义

```cpp
namespace shield::actor {

struct DistributedActorConfig {
    std::string node_id;                    // 节点标识
    std::string cluster_name = "shield";    // 集群名称
    uint16_t port = 0;                     // 通信端口 (0=自动)
    std::chrono::seconds heartbeat_interval{30}; // 心跳间隔
    std::chrono::seconds cleanup_interval{60};   // 清理间隔
};

class DistributedActorSystem {
public:
    DistributedActorSystem(
        caf::actor_system& system,
        std::shared_ptr<discovery::IServiceDiscovery> discovery,
        const DistributedActorConfig& config
    );
    
    ~DistributedActorSystem();
    
    // 系统生命周期
    void initialize();
    void shutdown();
    bool is_initialized() const;
    
    // Actor 管理
    caf::actor create_local_actor(const std::string& behavior_name);
    caf::actor find_actor(const std::string& actor_name);
    bool register_actor(const std::string& name, caf::actor actor);
    void unregister_actor(const std::string& name);
    
    // 节点信息
    const std::string& node_id() const;
    caf::actor_system& system();
    ActorRegistry& registry();
    
    // 集群管理
    std::vector<std::string> get_cluster_nodes() const;
    bool is_node_alive(const std::string& node_id) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::actor
```

### 使用示例

```cpp
// 创建分布式 Actor 系统
caf::actor_system_config cfg;
caf::actor_system system{cfg};

auto discovery = std::make_shared<shield::discovery::LocalDiscovery>();

shield::actor::DistributedActorConfig config;
config.node_id = "game-server-01";
config.cluster_name = "game-cluster";
config.heartbeat_interval = std::chrono::seconds(30);

shield::actor::DistributedActorSystem distributed_system(system, discovery, config);

// 初始化系统
distributed_system.initialize();

// 创建本地 Actor
auto player_actor = distributed_system.create_local_actor("player_behavior");

// 注册 Actor 以便其他节点访问
distributed_system.register_actor("player_001", player_actor);

// 查找其他节点的 Actor
auto remote_actor = distributed_system.find_actor("room_manager");
if (remote_actor) {
    // 向远程 Actor 发送消息
    system.spawn([=](caf::event_based_actor* self) {
        self->request(remote_actor, std::chrono::seconds(5), 
                     std::string("join_room"), std::string("player_001"))
        .then([](const std::string& response) {
            SHIELD_LOG_INFO << "房间加入响应: " << response;
        });
        return caf::behavior{};
    });
}

// 获取集群信息
auto nodes = distributed_system.get_cluster_nodes();
for (const auto& node : nodes) {
    SHIELD_LOG_INFO << "集群节点: " << node;
    if (distributed_system.is_node_alive(node)) {
        SHIELD_LOG_INFO << "  状态: 在线";
    }
}

// 关闭系统
distributed_system.shutdown();
```

## 📋 ActorRegistry Actor 注册表

Actor 注册表管理 Actor 的注册、发现和生命周期。

### 类定义

```cpp
namespace shield::actor {

struct ActorInfo {
    std::string name;           // Actor 名称
    std::string node_id;        // 所在节点
    caf::actor actor_ref;       // Actor 引用
    std::chrono::system_clock::time_point last_heartbeat; // 最后心跳时间
    std::map<std::string, std::string> metadata; // 元数据
};

class ActorRegistry {
public:
    explicit ActorRegistry(const std::string& node_id);
    ~ActorRegistry();
    
    // Actor 注册
    bool register_actor(const std::string& name, caf::actor actor);
    bool register_actor(const std::string& name, caf::actor actor, 
                       const std::map<std::string, std::string>& metadata);
    void unregister_actor(const std::string& name);
    
    // Actor 查找
    caf::actor find_local_actor(const std::string& name);
    std::optional<ActorInfo> get_actor_info(const std::string& name);
    std::vector<ActorInfo> list_actors();
    std::vector<ActorInfo> list_actors_by_node(const std::string& node_id);
    
    // 生命周期管理
    void start_heartbeat(std::chrono::seconds interval);
    void stop_heartbeat();
    void update_heartbeat(const std::string& actor_name);
    
    // 清理和维护
    void cleanup_expired_actors(std::chrono::seconds timeout);
    void add_node(const std::string& node_id);
    void remove_node(const std::string& node_id);
    
    // 统计信息
    size_t local_actor_count() const;
    size_t total_actor_count() const;
    std::vector<std::string> get_registered_nodes() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::actor
```

### 使用示例

```cpp
// 创建 Actor 注册表
shield::actor::ActorRegistry registry("game-server-01");

// 启动心跳机制
registry.start_heartbeat(std::chrono::seconds(30));

// 注册 Actor
caf::actor player_actor = system.spawn<PlayerActor>();
bool success = registry.register_actor("player_123", player_actor);
if (success) {
    SHIELD_LOG_INFO << "Player Actor 注册成功";
}

// 带元数据注册
std::map<std::string, std::string> metadata = {
    {"type", "player"},
    {"level", "10"},
    {"room", "room_001"}
};
registry.register_actor("player_456", player_actor, metadata);

// 查找 Actor
auto found_actor = registry.find_local_actor("player_123");
if (found_actor) {
    // 发送消息给 Actor
    caf::anon_send(found_actor, std::string("get_status"));
}

// 获取 Actor 详细信息
auto actor_info = registry.get_actor_info("player_456");
if (actor_info) {
    SHIELD_LOG_INFO << "Actor: " << actor_info->name 
                   << ", Node: " << actor_info->node_id;
    for (const auto& [key, value] : actor_info->metadata) {
        SHIELD_LOG_INFO << "  " << key << ": " << value;
    }
}

// 列出所有本地 Actor
auto local_actors = registry.list_actors();
SHIELD_LOG_INFO << "本地 Actor 数量: " << local_actors.size();

// 清理过期 Actor
registry.cleanup_expired_actors(std::chrono::minutes(5));

// 获取统计信息
SHIELD_LOG_INFO << "本地 Actor: " << registry.local_actor_count();
SHIELD_LOG_INFO << "总 Actor: " << registry.total_actor_count();

// 停止心跳并注销 Actor
registry.unregister_actor("player_123");
registry.stop_heartbeat();
```

## 🔧 LuaActor Lua 脚本 Actor

LuaActor 将 Lua 脚本与 Actor 模型结合，提供灵活的业务逻辑实现。

### 类定义

```cpp
namespace shield::actor {

// Lua 消息结构
struct LuaMessage {
    std::string type;                                    // 消息类型
    std::unordered_map<std::string, std::string> data;  // 消息数据
    std::string sender_id;                              // 发送者 ID
    
    LuaMessage() = default;
    LuaMessage(const std::string& msg_type, 
               const std::unordered_map<std::string, std::string>& msg_data = {},
               const std::string& sender = "");
};

// Lua 响应结构
struct LuaResponse {
    bool success = true;                                 // 处理是否成功
    std::unordered_map<std::string, std::string> data;  // 响应数据
    std::string error_message;                          // 错误信息
    
    LuaResponse() = default;
    LuaResponse(bool success_flag, 
                const std::unordered_map<std::string, std::string>& response_data = {},
                const std::string& error = "");
};

class LuaActor : public caf::event_based_actor {
public:
    LuaActor(caf::actor_config& cfg, 
             script::LuaVMPool& lua_vm_pool,
             DistributedActorSystem& actor_system,
             const std::string& script_path,
             const std::string& actor_id = "");
    
    virtual ~LuaActor() = default;
    
    // Actor 行为定义
    caf::behavior make_behavior() override;

protected:
    // 脚本管理
    virtual bool load_script();
    void register_cpp_functions();
    void setup_lua_environment();
    
    // 消息处理
    virtual LuaResponse handle_lua_message(const LuaMessage& msg);
    virtual std::string handle_lua_message_json(const std::string& msg_type, 
                                                const std::string& data_json);
    
    // Lua 绑定函数
    void lua_log_info(const std::string& message);
    void lua_log_error(const std::string& message);
    void lua_send_message(const std::string& target_actor, 
                         const std::string& msg_type,
                         const std::unordered_map<std::string, std::string>& data);

private:
    script::VMHandle m_lua_vm_handle;
    DistributedActorSystem& m_actor_system;
    script::LuaVMPool& m_lua_vm_pool;
    std::string script_path_;
    std::string actor_id_;
    bool script_loaded_;
};

// Actor 创建工厂函数
caf::actor create_lua_actor(caf::actor_system& system,
                           script::LuaVMPool& lua_vm_pool,
                           DistributedActorSystem& actor_system,
                           const std::string& script_path,
                           const std::string& actor_id = "");

} // namespace shield::actor
```

### Lua 脚本接口

LuaActor 为 Lua 脚本提供丰富的 C++ 绑定：

```lua
-- Lua Actor 脚本示例: player_actor.lua

-- 玩家状态
local player_state = {
    player_id = "",
    level = 1,
    experience = 0,
    gold = 1000,
    hp = 100,
    mp = 50
}

-- 初始化函数 (可选)
function on_init()
    log_info("Player Actor 初始化完成")
    player_state.player_id = get_actor_id()
    log_info("玩家 ID: " .. player_state.player_id)
end

-- 主要消息处理函数
function on_message(msg)
    log_info("收到消息类型: " .. msg.type)
    
    if msg.type == "get_info" then
        return handle_get_info(msg)
    elseif msg.type == "level_up" then
        return handle_level_up(msg)
    elseif msg.type == "add_experience" then 
        return handle_add_experience(msg)
    elseif msg.type == "buy_item" then
        return handle_buy_item(msg)
    else
        log_error("未知消息类型: " .. msg.type)
        return create_response(false, {}, "未知消息类型")
    end
end

-- 获取玩家信息
function handle_get_info(msg)
    local response_data = {
        player_id = player_state.player_id,
        level = tostring(player_state.level),
        experience = tostring(player_state.experience),
        gold = tostring(player_state.gold),
        hp = tostring(player_state.hp),
        mp = tostring(player_state.mp)
    }
    
    log_info("返回玩家信息: " .. player_state.player_id)
    return create_response(true, response_data)
end

-- 升级处理
function handle_level_up(msg)
    local old_level = player_state.level
    player_state.level = player_state.level + 1
    player_state.hp = player_state.hp + 10  -- 升级增加血量
    player_state.mp = player_state.mp + 5   -- 升级增加魔法值
    
    log_info("玩家升级: " .. old_level .. " -> " .. player_state.level)
    
    -- 通知其他系统
    send_message("achievement_system", "level_up_achieved", {
        player_id = player_state.player_id,
        new_level = tostring(player_state.level)
    })
    
    return create_response(true, {
        old_level = tostring(old_level),
        new_level = tostring(player_state.level),
        hp = tostring(player_state.hp),
        mp = tostring(player_state.mp)
    })
end

-- 增加经验值
function handle_add_experience(msg)
    local exp_gain = tonumber(msg.data.exp) or 0
    if exp_gain <= 0 then
        return create_response(false, {}, "经验值必须大于0")
    end
    
    player_state.experience = player_state.experience + exp_gain
    log_info("获得经验: " .. exp_gain .. ", 总经验: " .. player_state.experience)
    
    -- 检查是否可以升级 (假设每100经验升1级)
    local exp_needed = player_state.level * 100
    if player_state.experience >= exp_needed then
        -- 自动升级
        return handle_level_up(msg)  
    end
    
    return create_response(true, {
        experience = tostring(player_state.experience),
        exp_to_next_level = tostring(exp_needed - player_state.experience)
    })
end

-- 购买物品
function handle_buy_item(msg)
    local item_id = msg.data.item_id or ""
    local price = tonumber(msg.data.price) or 0
    
    if item_id == "" then
        return create_response(false, {}, "物品ID不能为空")
    end
    
    if player_state.gold < price then
        return create_response(false, {}, "金币不足")
    end
    
    -- 扣除金币
    player_state.gold = player_state.gold - price
    
    log_info("购买物品: " .. item_id .. ", 花费: " .. price)
    
    -- 通知背包系统添加物品
    send_message("inventory_system", "add_item", {
        player_id = player_state.player_id,
        item_id = item_id,
        quantity = "1"
    })
    
    return create_response(true, {
        item_id = item_id,
        remaining_gold = tostring(player_state.gold)
    })
end

-- 辅助函数：创建响应
function create_response(success, data, error_msg)
    return {
        success = success,
        data = data or {},
        error_message = error_msg or ""
    }
end

-- 辅助函数：发送消息给其他 Actor
function send_message(target, msg_type, data)
    -- 调用 C++ 绑定的发送函数
    lua_send_message(target, msg_type, data)
end
```

### 使用示例

```cpp
// 创建 Lua Actor
auto player_actor = shield::actor::create_lua_actor(
    system,                           // CAF actor system
    lua_vm_pool,                     // Lua VM 池
    distributed_actor_system,        // 分布式系统
    "scripts/player_actor.lua",     // 脚本路径
    "player_001"                     // Actor ID
);

// 注册 Actor
distributed_actor_system.register_actor("player_001", player_actor);

// 向 Actor 发送消息 (JSON 格式)
system.spawn([=](caf::event_based_actor* self) {
    // 获取玩家信息
    self->request(player_actor, std::chrono::seconds(5),
                 std::string("get_info"), std::string("{}"))
    .then([](const std::string& response) {
        auto json_response = nlohmann::json::parse(response);
        if (json_response["success"]) {
            SHIELD_LOG_INFO << "玩家等级: " << json_response["data"]["level"];
            SHIELD_LOG_INFO << "玩家金币: " << json_response["data"]["gold"];
        }
    });
    
    // 添加经验值
    nlohmann::json exp_data = {{"exp", "50"}};
    self->request(player_actor, std::chrono::seconds(5),
                 std::string("add_experience"), exp_data.dump())
    .then([](const std::string& response) {
        auto json_response = nlohmann::json::parse(response);
        SHIELD_LOG_INFO << "经验值响应: " << response;
    });
    
    return caf::behavior{};
});

// 使用传统 LuaMessage 结构发送消息
system.spawn([=](caf::event_based_actor* self) {
    shield::actor::LuaMessage msg;
    msg.type = "buy_item";
    msg.data = {
        {"item_id", "sword_001"},
        {"price", "100"}
    };
    msg.sender_id = "shop_system";
    
    self->request(player_actor, std::chrono::seconds(5), msg)
    .then([](const shield::actor::LuaResponse& response) {
        if (response.success) {
            SHIELD_LOG_INFO << "购买成功，剩余金币: " 
                           << response.data.at("remaining_gold");
        } else {
            SHIELD_LOG_ERROR << "购买失败: " << response.error_message;
        }
    });
    
    return caf::behavior{};
});
```

## 🔗 ActorSystemCoordinator 系统协调器

系统协调器负责 Actor 系统的全局协调和管理。

### 类定义

```cpp
namespace shield::actor {

class ActorSystemCoordinator {
public:
    explicit ActorSystemCoordinator(DistributedActorSystem& system);
    ~ActorSystemCoordinator();
    
    // 协调器生命周期
    void start();
    void stop();
    bool is_running() const;
    
    // 负载均衡
    std::string select_node_for_actor(const std::string& actor_type);
    void rebalance_actors();
    
    // 故障处理
    void handle_node_failure(const std::string& node_id);
    void migrate_actors_from_failed_node(const std::string& failed_node_id);
    
    // 监控和统计
    struct SystemStats {
        size_t total_nodes;
        size_t total_actors;
        std::map<std::string, size_t> actors_per_node;
        std::map<std::string, double> node_loads;
    };
    
    SystemStats get_system_stats() const;
    void log_system_status() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::actor
```

### 使用示例

```cpp
// 创建系统协调器
shield::actor::ActorSystemCoordinator coordinator(distributed_actor_system);

// 启动协调器
coordinator.start();

// 选择最佳节点创建 Actor
std::string best_node = coordinator.select_node_for_actor("player_actor");
SHIELD_LOG_INFO << "为 player_actor 选择节点: " << best_node;

// 获取系统统计信息
auto stats = coordinator.get_system_stats();
SHIELD_LOG_INFO << "集群节点数: " << stats.total_nodes;
SHIELD_LOG_INFO << "总 Actor 数: " << stats.total_actors;

for (const auto& [node, count] : stats.actors_per_node) {
    SHIELD_LOG_INFO << "节点 " << node << ": " << count << " actors";
}

// 手动触发负载均衡
coordinator.rebalance_actors();

// 处理节点故障 (通常由服务发现系统触发)
coordinator.handle_node_failure("failed-node-01");

// 输出系统状态
coordinator.log_system_status();

// 停止协调器
coordinator.stop();
```

## 🧪 测试示例

### 单元测试

```cpp
#define BOOST_TEST_MODULE ActorSystemTest
#include <boost/test/unit_test.hpp>
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/discovery/local_discovery.hpp"

BOOST_AUTO_TEST_SUITE(DistributedActorSystemTest)

BOOST_AUTO_TEST_CASE(test_actor_registration) {
    // 设置测试环境
    caf::actor_system_config cfg;
    caf::actor_system system{cfg};
    
    auto discovery = std::make_shared<shield::discovery::LocalDiscovery>();
    
    shield::actor::DistributedActorConfig config;
    config.node_id = "test-node";
    
    shield::actor::DistributedActorSystem distributed_system(system, discovery, config);
    distributed_system.initialize();
    
    // 创建测试 Actor
    auto test_actor = system.spawn([](caf::event_based_actor* self) {
        return caf::behavior{
            [](const std::string& message) -> std::string {
                return "echo: " + message;
            }
        };
    });
    
    // 测试 Actor 注册
    bool registered = distributed_system.register_actor("test_actor", test_actor);
    BOOST_CHECK(registered);
    
    // 测试 Actor 查找
    auto found_actor = distributed_system.find_actor("test_actor");
    BOOST_CHECK(found_actor != nullptr);
    
    // 测试消息发送
    bool message_sent = false;
    system.spawn([&](caf::event_based_actor* self) {
        self->request(found_actor, std::chrono::seconds(1), std::string("hello"))
        .then([&](const std::string& response) {
            BOOST_CHECK_EQUAL(response, "echo: hello");
            message_sent = true;
        });
        return caf::behavior{};
    });
    
    // 等待消息处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_CHECK(message_sent);
    
    // 清理
    distributed_system.unregister_actor("test_actor");
    distributed_system.shutdown();
}

BOOST_AUTO_TEST_SUITE_END()
```

### 集成测试

```cpp
BOOST_AUTO_TEST_SUITE(ActorSystemIntegrationTest)

BOOST_AUTO_TEST_CASE(test_lua_actor_integration) {
    // 创建临时 Lua 脚本
    std::string lua_script = R"(
function on_message(msg)
    if msg.type == "test" then
        return create_response(true, {result = "test_passed"})
    else
        return create_response(false, {}, "unknown message type")
    end
end

function create_response(success, data, error_msg)
    return {
        success = success,
        data = data or {},
        error_message = error_msg or ""
    }
end
)";
    
    std::string script_file = "test_actor.lua";
    std::ofstream file(script_file);
    file << lua_script;
    file.close();
    
    // 设置测试环境
    caf::actor_system_config cfg;
    caf::actor_system system{cfg};
    
    auto discovery = std::make_shared<shield::discovery::LocalDiscovery>();
    
    shield::actor::DistributedActorConfig config;
    config.node_id = "test-node";
    
    shield::actor::DistributedActorSystem distributed_system(system, discovery, config);
    distributed_system.initialize();
    
    // 创建 Lua VM 池
    shield::script::LuaVMPoolConfig vm_config;
    shield::script::LuaVMPool vm_pool("test_pool", vm_config);
    vm_pool.init();
    vm_pool.start();
    
    // 创建 Lua Actor
    auto lua_actor = shield::actor::create_lua_actor(
        system, vm_pool, distributed_system, script_file, "test_lua_actor"
    );
    
    // 测试消息处理
    bool test_passed = false;
    system.spawn([&](caf::event_based_actor* self) {
        self->request(lua_actor, std::chrono::seconds(5),
                     std::string("test"), std::string("{}"))
        .then([&](const std::string& response) {
            auto json_response = nlohmann::json::parse(response);
            if (json_response["success"] && 
                json_response["data"]["result"] == "test_passed") {
                test_passed = true;
            }
        });
        return caf::behavior{};
    });
    
    // 等待测试完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_CHECK(test_passed);
    
    // 清理
    vm_pool.stop();
    distributed_system.shutdown();
    std::remove(script_file.c_str());
}

BOOST_AUTO_TEST_SUITE_END()
```

## 📚 最佳实践

### 1. Actor 设计原则

```cpp
// ✅ 好的 Actor 设计
class GoodPlayerActor {
private:
    // 状态封装在 Actor 内部
    PlayerState m_state;
    
    // 使用消息传递通信
    caf::behavior make_behavior() {
        return {
            [this](const GetPlayerInfo& msg) -> PlayerInfo {
                return m_state.get_info();
            },
            [this](const UpdateExperience& msg) -> bool {
                return m_state.add_experience(msg.amount);
            }
        };
    }
};

// ❌ 不good的 Actor 设计  
class BadPlayerActor {
public:
    // 不要暴露内部状态！
    PlayerState public_state;  // 错误！
    
    // 不要使用共享内存通信！
    static std::map<std::string, PlayerState> shared_players; // 错误！
    
    void direct_method_call() {  // 错误！不要直接方法调用
        // ...
    }
};
```

### 2. 消息设计

```cpp
// ✅ 好的消息设计
struct PlayerAction {
    std::string action_type;
    std::string player_id;
    std::map<std::string, std::string> parameters;
    std::chrono::system_clock::time_point timestamp;
};

struct PlayerActionResult {
    bool success;
    std::string result_data;
    std::string error_message;
};

// ❌ 不好的消息设计
struct BadMessage {
    void* raw_pointer;     // 危险！不要传递指针
    std::thread worker;    // 错误！不可序列化
    // 缺少版本信息、时间戳等重要字段
};
```

### 3. 错误处理

```cpp
// ✅ 好的错误处理
system.spawn([](caf::event_based_actor* self) {
    self->request(remote_actor, std::chrono::seconds(5), message)
    .then([](const Response& response) {
        // 处理成功响应
        handle_success(response);
    },
    [](caf::error& err) {
        // 处理错误
        SHIELD_LOG_ERROR << "请求失败: " << caf::to_string(err);
        handle_failure(err);
    });
    
    return caf::behavior{};
});

// ❌ 不好的错误处理
system.spawn([](caf::event_based_actor* self) {
    // 忽略错误处理！
    self->request(remote_actor, std::chrono::seconds(5), message)
    .then([](const Response& response) {
        handle_success(response);
    });
    // 没有错误处理分支！
    
    return caf::behavior{};
});
```

### 4. 性能优化

```cpp
// ✅ 性能优化技巧

// 1. 批量消息处理
caf::behavior batch_behavior() {
    return {
        [this](const BatchMessages& batch) {
            // 批量处理消息，减少上下文切换
            for (const auto& msg : batch.messages) {
                process_message(msg);
            }
        }
    };
}

// 2. 消息池化
class MessagePool {
public:
    std::unique_ptr<PlayerAction> acquire() {
        if (!m_pool.empty()) {
            auto msg = std::move(m_pool.back());
            m_pool.pop_back();
            return msg;
        }
        return std::make_unique<PlayerAction>();
    }
    
    void release(std::unique_ptr<PlayerAction> msg) {
        msg->reset();  // 重置消息内容
        m_pool.push_back(std::move(msg));
    }
    
private:
    std::vector<std::unique_ptr<PlayerAction>> m_pool;
};

// 3. 避免频繁字符串操作
class EfficientActor {
private:
    // 预分配字符串缓冲区
    std::string m_string_buffer;
    
    void process_message(const std::string& data) {
        // 重用缓冲区而不是创建新字符串
        m_string_buffer.clear();
        m_string_buffer.reserve(data.size() + 100);
        m_string_buffer = data;
        // 处理...
    }
};
```

---

Actor 系统是 Shield 框架的核心，掌握 Actor 模型的设计原则和最佳实践对于构建高性能分布式游戏服务器至关重要。