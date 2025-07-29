# Script 脚本系统 API 文档

脚本系统为 Shield 框架提供 Lua 脚本集成能力，支持热重载、虚拟机池化管理和 C++/Lua 双向绑定。用于实现灵活的游戏业务逻辑。

## 📋 模块概览

脚本系统包含以下主要类：

- `LuaEngine`: Lua 虚拟机引擎封装
- `LuaVMPool`: Lua 虚拟机池管理
- `LuaBinding`: C++/Lua 绑定工具
- `ScriptManager`: 脚本管理器

## 🔧 LuaEngine Lua 引擎

Lua 引擎封装了单个 Lua 虚拟机的操作，提供脚本执行和 C++/Lua 交互功能。

### 类定义

```cpp
namespace shield::script {

enum class LuaType {
    NIL,
    BOOLEAN,
    NUMBER,
    STRING,
    TABLE,
    FUNCTION,
    USERDATA,
    THREAD
};

struct LuaValue {
    LuaType type = LuaType::NIL;
    std::variant<std::monostate, bool, double, std::string, 
                std::map<std::string, LuaValue>> value;
    
    // 便捷构造函数
    LuaValue() = default;
    LuaValue(bool val) : type(LuaType::BOOLEAN), value(val) {}
    LuaValue(double val) : type(LuaType::NUMBER), value(val) {}
    LuaValue(const std::string& val) : type(LuaType::STRING), value(val) {}
    LuaValue(const char* val) : type(LuaType::STRING), value(std::string(val)) {}
    
    // 类型检查和获取
    bool is_nil() const { return type == LuaType::NIL; }
    bool is_boolean() const { return type == LuaType::BOOLEAN; }
    bool is_number() const { return type == LuaType::NUMBER; }
    bool is_string() const { return type == LuaType::STRING; }
    bool is_table() const { return type == LuaType::TABLE; }
    
    bool as_boolean() const;
    double as_number() const;
    std::string as_string() const;
    std::map<std::string, LuaValue> as_table() const;
    
    // JSON 转换
    nlohmann::json to_json() const;
    static LuaValue from_json(const nlohmann::json& json);
};

class LuaEngine {
public:
    explicit LuaEngine(const std::string& vm_id = "");
    virtual ~LuaEngine();
    
    // 虚拟机管理
    bool initialize();
    void shutdown();
    bool is_initialized() const;
    const std::string& get_vm_id() const;
    
    // 脚本执行
    bool load_script(const std::string& script_path);
    bool load_script_from_string(const std::string& script_content);
    bool reload_script(const std::string& script_path);
    
    // 函数调用
    std::optional<LuaValue> call_function(const std::string& function_name,
                                         const std::vector<LuaValue>& args = {});
    
    template<typename... Args>
    std::optional<LuaValue> call_function(const std::string& function_name, Args&&... args);
    
    // 全局变量操作
    void set_global(const std::string& name, const LuaValue& value);
    std::optional<LuaValue> get_global(const std::string& name);
    bool has_global(const std::string& name);
    
    // 栈操作 (高级用法)
    lua_State* get_lua_state();
    int get_stack_size() const;
    void dump_stack() const;
    
    // C++ 函数注册
    template<typename Func>
    void register_function(const std::string& name, Func&& func);
    
    void register_cpp_function(const std::string& name, 
                              std::function<LuaValue(const std::vector<LuaValue>&)> func);
    
    // 错误处理
    bool has_error() const;
    std::string get_last_error() const;
    void clear_error();
    
    // 内存统计
    size_t get_memory_usage() const;
    void garbage_collect();
    void set_memory_limit(size_t limit_kb);
    
    // 调试支持
    void enable_debug(bool enable = true);
    std::vector<std::string> get_call_stack() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::script
```

### 使用示例

```cpp
// 创建 Lua 引擎
auto lua_engine = std::make_unique<shield::script::LuaEngine>("player_vm_001");

// 初始化引擎
if (!lua_engine->initialize()) {
    SHIELD_LOG_ERROR << "Lua 引擎初始化失败";
    return;
}

// 注册 C++ 函数
lua_engine->register_function("log_info", [](const std::string& message) {
    SHIELD_LOG_INFO << "[Lua]: " << message;
});

lua_engine->register_function("get_current_time", []() -> double {
    return static_cast<double>(std::time(nullptr));
});

lua_engine->register_function("send_message", [](const std::string& target, const std::string& msg) {
    // 发送消息到其他 Actor
    send_actor_message(target, msg);
    return true;
});

// 加载 Lua 脚本
if (!lua_engine->load_script("scripts/player_logic.lua")) {
    SHIELD_LOG_ERROR << "加载脚本失败: " << lua_engine->get_last_error();
    return;
}

// 设置全局变量
lua_engine->set_global("player_id", shield::script::LuaValue("player_001"));
lua_engine->set_global("server_version", shield::script::LuaValue("1.0.0"));

// 调用 Lua 函数
auto result = lua_engine->call_function("on_player_login", 
    shield::script::LuaValue("player_001"),
    shield::script::LuaValue(1001.0)  // 玩家等级
);

if (result) {
    SHIELD_LOG_INFO << "登录处理结果: " << result->as_string();
} else {
    SHIELD_LOG_ERROR << "调用登录函数失败: " << lua_engine->get_last_error();
}

// 处理游戏消息
auto handle_message = [&lua_engine](const std::string& msg_type, const nlohmann::json& data) {
    // 将 JSON 转换为 Lua 值
    auto lua_data = shield::script::LuaValue::from_json(data);
    
    // 调用 Lua 消息处理函数
    auto response = lua_engine->call_function("on_message", 
        shield::script::LuaValue(msg_type), lua_data);
    
    if (response && !response->is_nil()) {
        // 将响应转换回 JSON
        return response->to_json();
    }
    
    return nlohmann::json{{"error", "no response"}};
};

// 示例：处理玩家移动
nlohmann::json move_data = {
    {"x", 100.5},
    {"y", 200.3},
    {"speed", 5.0}
};

auto move_response = handle_message("player_move", move_data);
SHIELD_LOG_INFO << "移动响应: " << move_response.dump();
```

## 🏊 LuaVMPool 虚拟机池

虚拟机池管理多个 Lua 引擎实例，提供池化复用和负载均衡功能。

### 类定义

```cpp
namespace shield::script {

struct LuaVMPoolConfig {
    size_t initial_size = 4;                        // 初始池大小
    size_t max_size = 16;                          // 最大池大小
    std::chrono::seconds idle_timeout{300};        // 空闲超时
    std::chrono::seconds max_lifetime{3600};       // 最大生存时间
    size_t memory_limit_kb = 32 * 1024;           // 内存限制 (32MB)
    bool enable_preload = true;                    // 启用预加载
    std::vector<std::string> preload_scripts;     // 预加载脚本列表
    bool enable_hot_reload = true;                 // 启用热重载
    std::chrono::seconds reload_check_interval{5}; // 重载检查间隔
};

// VM 句柄，用于管理 VM 的生命周期
class VMHandle {
public:
    VMHandle() = default;
    VMHandle(std::shared_ptr<LuaEngine> engine, std::function<void()> releaser);
    VMHandle(VMHandle&& other) noexcept;
    VMHandle& operator=(VMHandle&& other) noexcept;
    ~VMHandle();
    
    // 禁止拷贝
    VMHandle(const VMHandle&) = delete;
    VMHandle& operator=(const VMHandle&) = delete;
    
    // 访问引擎
    LuaEngine* get() const;
    LuaEngine& operator*() const;
    LuaEngine* operator->() const;
    
    // 状态检查
    bool is_valid() const;
    explicit operator bool() const;
    
    // 手动释放
    void release();

private:
    std::shared_ptr<LuaEngine> m_engine;
    std::function<void()> m_releaser;
};

class LuaVMPool : public core::Component {
public:
    explicit LuaVMPool(const std::string& pool_name, const LuaVMPoolConfig& config);
    virtual ~LuaVMPool();
    
    // 组件生命周期
    void on_init() override;
    void on_start() override;
    void on_stop() override;
    
    // VM 获取和释放
    VMHandle acquire_vm();
    VMHandle acquire_vm(const std::string& preferred_script);
    void release_vm(std::shared_ptr<LuaEngine> engine);
    
    // 脚本管理
    void preload_script(const std::string& script_path);
    void reload_script(const std::string& script_path);
    void reload_all_scripts();
    
    // 热重载
    void enable_hot_reload(bool enable);
    void watch_script_directory(const std::string& directory);
    
    // 全局函数注册 (应用到所有 VM)
    template<typename Func>
    void register_global_function(const std::string& name, Func&& func);
    
    void set_global_variable(const std::string& name, const LuaValue& value);
    
    // 池管理
    void resize_pool(size_t new_size);
    void cleanup_idle_vms();
    void warmup_pool();
    
    // 统计信息
    struct Statistics {
        std::atomic<size_t> total_vms{0};
        std::atomic<size_t> available_vms{0};
        std::atomic<size_t> in_use_vms{0};
        std::atomic<uint64_t> total_acquisitions{0};
        std::atomic<uint64_t> total_releases{0};
        std::atomic<uint64_t> acquisition_timeouts{0};
        std::atomic<size_t> total_memory_usage{0};
        std::chrono::system_clock::time_point last_cleanup;
    };
    
    const Statistics& get_statistics() const;
    const std::string& get_pool_name() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace shield::script
```

### 使用示例

```cpp
// 配置 VM 池
shield::script::LuaVMPoolConfig pool_config;
pool_config.initial_size = 8;
pool_config.max_size = 32;
pool_config.idle_timeout = std::chrono::minutes(10);
pool_config.memory_limit_kb = 64 * 1024;  // 64MB
pool_config.enable_preload = true;
pool_config.preload_scripts = {
    "scripts/common_utils.lua",
    "scripts/game_constants.lua",
    "scripts/player_base.lua"
};
pool_config.enable_hot_reload = true;

// 创建 VM 池
auto vm_pool = std::make_unique<shield::script::LuaVMPool>("game_vm_pool", pool_config);

// 注册全局函数 (所有 VM 都会有这些函数)
vm_pool->register_global_function("log_info", [](const std::string& msg) {
    SHIELD_LOG_INFO << "[Lua]: " << msg;
});

vm_pool->register_global_function("get_timestamp", []() -> double {
    return static_cast<double>(std::time(nullptr));
});

vm_pool->register_global_function("json_encode", [](const shield::script::LuaValue& value) -> std::string {
    return value.to_json().dump();
});

vm_pool->register_global_function("json_decode", [](const std::string& json_str) -> shield::script::LuaValue {
    try {
        auto json = nlohmann::json::parse(json_str);
        return shield::script::LuaValue::from_json(json);
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "JSON 解析失败: " << e.what();
        return shield::script::LuaValue();  // nil
    }
});

// 设置全局变量
vm_pool->set_global_variable("SERVER_VERSION", shield::script::LuaValue("1.2.0"));
vm_pool->set_global_variable("MAX_PLAYERS", shield::script::LuaValue(1000.0));

// 启动 VM 池
vm_pool->init();
vm_pool->start();

// 启用热重载监控
vm_pool->watch_script_directory("scripts/");

// 使用 VM 处理请求
auto handle_player_request = [&vm_pool](const std::string& player_id, const std::string& action, 
                                       const nlohmann::json& data) -> nlohmann::json {
    // 获取 VM (自动管理生命周期)
    auto vm_handle = vm_pool->acquire_vm("scripts/player_handler.lua");
    
    if (!vm_handle) {
        SHIELD_LOG_ERROR << "获取 VM 失败";
        return nlohmann::json{{"error", "vm_unavailable"}};
    }
    
    try {
        // 设置玩家上下文
        vm_handle->set_global("current_player_id", shield::script::LuaValue(player_id));
        
        // 调用处理函数
        auto lua_data = shield::script::LuaValue::from_json(data);
        auto result = vm_handle->call_function("handle_player_action", 
            shield::script::LuaValue(action), lua_data);
        
        if (result) {
            return result->to_json();
        } else {
            SHIELD_LOG_ERROR << "脚本执行失败: " << vm_handle->get_last_error();
            return nlohmann::json{{"error", "script_error"}};
        }
        
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "请求处理异常: " << e.what();
        return nlohmann::json{{"error", "exception"}};
    }
    
    // vm_handle 析构时自动释放 VM 回池
};

// 示例：处理多个并发请求
std::vector<std::thread> worker_threads;

for (int i = 0; i < 10; ++i) {
    worker_threads.emplace_back([&handle_player_request, i]() {
        for (int j = 0; j < 100; ++j) {
            std::string player_id = "player_" + std::to_string(i * 100 + j);
            nlohmann::json request_data = {
                {"target", "monster_001"},
                {"skill", "fireball"},
                {"damage", 150}
            };
            
            auto response = handle_player_request(player_id, "attack", request_data);
            
            if (response.contains("error")) {
                SHIELD_LOG_ERROR << "请求失败: " << response["error"];
            } else {
                SHIELD_LOG_INFO << "攻击结果: " << response.dump();
            }
        }
    });
}

// 等待所有线程完成
for (auto& thread : worker_threads) {
    thread.join();
}

// 获取池统计信息
auto stats = vm_pool->get_statistics();
SHIELD_LOG_INFO << "VM 池统计:";
SHIELD_LOG_INFO << "  总 VM 数: " << stats.total_vms.load();
SHIELD_LOG_INFO << "  可用 VM: " << stats.available_vms.load();
SHIELD_LOG_INFO << "  使用中: " << stats.in_use_vms.load();
SHIELD_LOG_INFO << "  总获取次数: " << stats.total_acquisitions.load();
SHIELD_LOG_INFO << "  内存使用: " << stats.total_memory_usage.load() << " KB";
```

## 🔗 LuaBinding 绑定工具

提供便捷的 C++/Lua 双向绑定功能。

### 类定义

```cpp
namespace shield::script {

// C++ 类到 Lua 的绑定
template<typename T>
class LuaClassBinding {
public:
    explicit LuaClassBinding(LuaEngine& engine, const std::string& class_name);
    
    // 构造函数绑定
    template<typename... Args>
    LuaClassBinding& constructor();
    
    // 成员函数绑定
    template<typename Func>
    LuaClassBinding& method(const std::string& name, Func&& func);
    
    // 属性绑定
    template<typename Type>
    LuaClassBinding& property(const std::string& name, Type T::* member);
    
    // 静态函数绑定
    template<typename Func>
    LuaClassBinding& static_method(const std::string& name, Func&& func);
    
    // 完成绑定
    void finalize();

private:
    LuaEngine& m_engine;
    std::string m_class_name;
};

// 便捷的绑定宏和函数
#define LUA_BIND_CLASS(engine, class_name) \
    shield::script::LuaClassBinding<class_name>(engine, #class_name)

#define LUA_BIND_METHOD(binding, method_name) \
    binding.method(#method_name, &std::decay_t<decltype(binding)>::type::method_name)

#define LUA_BIND_PROPERTY(binding, property_name) \
    binding.property(#property_name, &std::decay_t<decltype(binding)>::type::property_name)

// 枚举绑定
template<typename EnumType>
void bind_enum(LuaEngine& engine, const std::string& enum_name, 
               const std::map<std::string, EnumType>& values);

// 容器绑定
template<typename Container>
void bind_container(LuaEngine& engine, const std::string& container_name);

} // namespace shield::script
```

### 使用示例

```cpp
// 定义要绑定的 C++ 类
class Player {
public:
    Player(const std::string& name, int level) : m_name(name), m_level(level) {}
    
    const std::string& get_name() const { return m_name; }
    void set_name(const std::string& name) { m_name = name; }
    
    int get_level() const { return m_level; }
    void set_level(int level) { m_level = level; }
    
    void level_up() { m_level++; }
    bool can_use_skill(const std::string& skill) const {
        return m_level >= get_skill_requirement(skill);
    }
    
    static int get_skill_requirement(const std::string& skill) {
        if (skill == "fireball") return 5;
        if (skill == "heal") return 3;
        return 1;
    }

private:
    std::string m_name;
    int m_level;
};

enum class PlayerClass {
    WARRIOR = 1,
    MAGE = 2,
    ROGUE = 3
};

// 绑定到 Lua
void bind_game_classes(shield::script::LuaEngine& engine) {
    // 绑定 Player 类
    LUA_BIND_CLASS(engine, Player)
        .constructor<const std::string&, int>()
        .method("get_name", &Player::get_name)
        .method("set_name", &Player::set_name)
        .method("get_level", &Player::get_level)
        .method("set_level", &Player::set_level)
        .method("level_up", &Player::level_up)
        .method("can_use_skill", &Player::can_use_skill)
        .static_method("get_skill_requirement", &Player::get_skill_requirement)
        .finalize();
    
    // 绑定枚举
    shield::script::bind_enum<PlayerClass>(engine, "PlayerClass", {
        {"WARRIOR", PlayerClass::WARRIOR},
        {"MAGE", PlayerClass::MAGE},
        {"ROGUE", PlayerClass::ROGUE}
    });
    
    // 绑定容器类型
    shield::script::bind_container<std::vector<std::string>>(engine, "StringArray");
    shield::script::bind_container<std::map<std::string, std::string>>(engine, "StringMap");
}

// 在 Lua 中使用绑定的类
const char* lua_script = R"(
-- 创建玩家对象
local player = Player.new("张三", 10)
log_info("创建玩家: " .. player:get_name() .. ", 等级: " .. player:get_level())

-- 使用方法
player:level_up()
log_info("升级后等级: " .. player:get_level())

-- 检查技能需求
local skill = "fireball"
local requirement = Player.get_skill_requirement(skill)
log_info(skill .. " 技能需求等级: " .. requirement)

if player:can_use_skill(skill) then
    log_info(player:get_name() .. " 可以使用 " .. skill)
else
    log_info(player:get_name() .. " 不能使用 " .. skill)
end

-- 使用枚举
local player_class = PlayerClass.MAGE
log_info("玩家职业: " .. player_class)

-- 返回玩家信息
function get_player_info()
    return {
        name = player:get_name(),
        level = player:get_level(),
        class = PlayerClass.MAGE
    }
end
)";

// 完整使用示例
void test_lua_binding() {
    auto engine = std::make_unique<shield::script::LuaEngine>();
    engine->initialize();
    
    // 绑定类和函数
    bind_game_classes(*engine);
    
    // 加载脚本
    engine->load_script_from_string(lua_script);
    
    // 调用 Lua 函数
    auto player_info = engine->call_function("get_player_info");
    if (player_info) {
        auto json_info = player_info->to_json();
        SHIELD_LOG_INFO << "玩家信息: " << json_info.dump(2);
    }
}
```

## 📚 最佳实践

### 1. 脚本架构设计

```cpp
// ✅ 良好的脚本架构
class GameScriptSystem {
public:
    void setup_script_architecture() {
        // 1. 分层脚本设计
        vm_pool_->preload_script("scripts/core/constants.lua");      // 常量定义
        vm_pool_->preload_script("scripts/core/utils.lua");         // 工具函数
        vm_pool_->preload_script("scripts/core/base_actor.lua");    // Actor 基类
        
        // 2. 模块化业务逻辑
        vm_pool_->preload_script("scripts/modules/player.lua");     // 玩家模块
        vm_pool_->preload_script("scripts/modules/combat.lua");     // 战斗模块
        vm_pool_->preload_script("scripts/modules/items.lua");      // 物品模块
        
        // 3. 注册核心绑定
        register_core_bindings();
        register_game_api();
        register_database_api();
    }

private:
    void register_core_bindings() {
        // 日志功能
        vm_pool_->register_global_function("log", [](const std::string& level, const std::string& msg) {
            if (level == "info") SHIELD_LOG_INFO << "[Lua]: " << msg;
            else if (level == "warn") SHIELD_LOG_WARN << "[Lua]: " << msg;
            else if (level == "error") SHIELD_LOG_ERROR << "[Lua]: " << msg;
            else SHIELD_LOG_DEBUG << "[Lua]: " << msg;
        });
        
        // 时间工具
        vm_pool_->register_global_function("now", []() -> double {
            return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        });
        
        // JSON 工具
        vm_pool_->register_global_function("json_encode", [](const LuaValue& value) {
            return value.to_json().dump();
        });
        
        vm_pool_->register_global_function("json_decode", [](const std::string& json_str) {
            try {
                return LuaValue::from_json(nlohmann::json::parse(json_str));
            } catch (...) {
                return LuaValue();  // nil on error
            }
        });
    }
    
    void register_game_api() {
        // 玩家 API
        vm_pool_->register_global_function("get_player", [this](const std::string& player_id) {
            return get_player_data(player_id);
        });
        
        vm_pool_->register_global_function("update_player", [this](const std::string& player_id, const LuaValue& data) {
            return update_player_data(player_id, data);
        });
        
        // 游戏事件
        vm_pool_->register_global_function("emit_event", [this](const std::string& event_name, const LuaValue& data) {
            emit_game_event(event_name, data);
        });
    }
};
```

### 2. 错误处理和调试

```cpp
// ✅ 健壮的错误处理
class RobustScriptExecutor {
public:
    nlohmann::json execute_script_safely(const std::string& script_name, 
                                        const std::string& function_name,
                                        const std::vector<LuaValue>& args) {
        auto vm_handle = vm_pool_->acquire_vm(script_name);
        if (!vm_handle) {
            return create_error_response("vm_unavailable", "无法获取虚拟机");
        }
        
        try {
            // 设置错误处理
            vm_handle->enable_debug(true);
            
            // 设置执行超时
            auto start_time = std::chrono::steady_clock::now();
            const auto timeout = std::chrono::seconds(30);
            
            // 调用函数
            auto result = vm_handle->call_function(function_name, args);
            
            // 检查超时
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed > timeout) {
                SHIELD_LOG_WARN << "脚本执行超时: " << script_name << "::" << function_name;
                return create_error_response("timeout", "脚本执行超时");
            }
            
            // 检查结果
            if (!result) {
                std::string error = vm_handle->get_last_error();
                SHIELD_LOG_ERROR << "脚本执行失败: " << error;
                
                // 记录调用栈
                auto call_stack = vm_handle->get_call_stack();
                for (const auto& frame : call_stack) {
                    SHIELD_LOG_ERROR << "  " << frame;
                }
                
                return create_error_response("script_error", error);
            }
            
            // 检查内存使用
            size_t memory_usage = vm_handle->get_memory_usage();
            if (memory_usage > memory_limit_) {
                SHIELD_LOG_WARN << "脚本内存使用过高: " << memory_usage << " KB";
                vm_handle->garbage_collect();
            }
            
            return create_success_response(result->to_json());
            
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "脚本执行异常: " << e.what();
            return create_error_response("exception", e.what());
        }
    }

private:
    nlohmann::json create_error_response(const std::string& error_code, const std::string& message) {
        return {
            {"success", false},
            {"error", {
                {"code", error_code},
                {"message", message},
                {"timestamp", std::time(nullptr)}
            }}
        };
    }
    
    nlohmann::json create_success_response(const nlohmann::json& data) {
        return {
            {"success", true},
            {"data", data},
            {"timestamp", std::time(nullptr)}
        };
    }
    
    size_t memory_limit_ = 128 * 1024;  // 128MB
};
```

### 3. 性能优化

```cpp
// ✅ 脚本性能优化
class OptimizedScriptSystem {
public:
    void optimize_performance() {
        // 1. 预编译热点脚本
        precompile_hot_scripts();
        
        // 2. 实现脚本缓存
        setup_script_cache();
        
        // 3. 批量执行优化
        setup_batch_execution();
        
        // 4. 内存池优化
        setup_memory_pools();
    }

private:
    void precompile_hot_scripts() {
        std::vector<std::string> hot_scripts = {
            "scripts/player/combat.lua",
            "scripts/player/movement.lua",
            "scripts/items/consume.lua"
        };
        
        for (const auto& script : hot_scripts) {
            precompile_script(script);
        }
    }
    
    void setup_script_cache() {
        // 使用 LRU 缓存编译后的脚本
        script_cache_ = std::make_unique<LRUCache<std::string, CompiledScript>>(100);
    }
    
    void setup_batch_execution() {
        // 批量处理相同类型的脚本调用
        batch_processor_ = std::make_unique<BatchScriptProcessor>(
            [this](const std::vector<ScriptCall>& calls) {
                process_script_batch(calls);
            }
        );
    }
    
    void process_script_batch(const std::vector<ScriptCall>& calls) {
        // 按脚本类型分组
        std::map<std::string, std::vector<ScriptCall>> grouped_calls;
        for (const auto& call : calls) {
            grouped_calls[call.script_name].push_back(call);
        }
        
        // 并行处理每组
        std::vector<std::future<void>> futures;
        for (const auto& [script_name, script_calls] : grouped_calls) {
            futures.push_back(std::async(std::launch::async, [this, script_name, script_calls]() {
                auto vm_handle = vm_pool_->acquire_vm(script_name);
                for (const auto& call : script_calls) {
                    execute_single_call(vm_handle.get(), call);
                }
            }));
        }
        
        // 等待所有任务完成
        for (auto& future : futures) {
            future.wait();
        }
    }
};
```

---

脚本系统是 Shield 框架的核心特性之一，提供了强大而灵活的 Lua 集成能力。通过合理的架构设计和性能优化，可以实现高效的游戏业务逻辑执行和热更新功能。