# Starter 系统

本文档描述 Shield 的 C++ 模块初始化系统（Starter），用于管理运行时模块的启动顺序和生命周期。

## 设计定位

**Starter 是 C++ 模块初始化器**，负责按正确顺序初始化运行时的 C++ 模块。

### 核心职责

- **依赖管理**：确保模块按依赖顺序初始化
- **生命周期钩子**：提供初始化前后的扩展点
- **条件启用**：根据配置决定是否加载模块
- **关闭顺序**：按启动相反顺序清理资源

### 明确非职责

- **不是** DI/IoC 容器（不包含自动装配、依赖注入）
- **不是** 自动配置系统
- **不是** 注解驱动的装配系统
- **不负责** Lua Service 的创建和管理（由 Actor 系统负责）

## 与其他系统的关系

```
┌─────────────────────────────────────────────────────────┐
│                    用户 Lua Service                      │
│                   (由 Actor 系统管理)                     │
├─────────────────────────────────────────────────────────┤
│                   Lua 插件系统                           │
│              shield.plugin.on() 注册监听器                 │
├─────────────────────────────────────────────────────────┤
│                   生命周期事件系统                         │
│           发布框架级事件（C++ + Lua 监听器）               │
├─────────────────────────────────────────────────────────┤
│                   ApplicationContext                       │
│              统一的应用上下文访问入口                     │
├─────────────────────────────────────────────────────────┤
│                   Starter 系统                           │
│           管理 C++ 模块的初始化顺序和生命周期                │
├─────────────────────────────────────────────────────────┤
│                   C++ 运行时模块                           │
│  Env | Config | Log | Core | Script | Data | Net | ...   │
└─────────────────────────────────────────────────────────┘
```

- **Starter 系统**：管理 C++ 模块初始化顺序
- **ApplicationContext**：统一访问入口，提供各种组件的获取
- **生命周期事件系统**：通知插件/扩展框架状态变化
- **Actor 系统**：管理 Lua Service 生命周期

## ApplicationContext

ApplicationContext 是统一的应用上下文访问入口，提供各种组件的获取。

### 核心接口

```cpp
// include/shield/core/application_context.hpp
namespace shield::core {

/**
 * 应用上下文
 * 
 * 职责：
 * - 作为全局访问入口，提供各种组件的访问
 * - 不包含 DI 自动装配能力
 * - 组件要么是单例，要么由 Starter 注册
 */
class ApplicationContext {
public:
    static ApplicationContext& instance();
    
    /**
     * 注册单例组件
     * 
     * @tparam T 组件类型
     * @param instance 组件实例（通常是 shared_ptr）
     * @param name 可选的名称，用于命名访问
     * 
     * 示例：
     * app.register_singleton<Environment>(Environment::instance_ptr());
     * app.register_singleton<Logger>(Logger::instance_ptr());
     */
    template <typename T>
    void register_singleton(std::shared_ptr<T> instance, const std::string& name = "");
    
    /**
     * 注册组件（由 Starter 使用）
     * 
     * @tparam T 组件类型
     * @param instance 组件实例
     * @param name 可选的名称
     * 
     * 示例：
     * app.register_component<LuaVMPool>(lua_pool, "lua_pool");
     * app.register_component<Configuration>(config, "config");
     */
    template <typename T>
    void register_component(std::shared_ptr<T> instance, const std::string& name = "");
    
    /**
     * 获取组件（按类型）
     * 
     * @tparam T 组件类型
     * @return 组件的 shared_ptr，不存在返回 nullptr
     * 
     * 示例：
     * auto env = app.get<Environment>();
     * auto logger = app.get<Logger>();
     * auto config = app.get<Configuration>();
     */
    template <typename T>
    std::shared_ptr<T> get() const;
    
    /**
     * 获取组件（按名称）
     * 
     * @tparam T 组件类型
     * @param name 组件名称
     * @return 组件的 shared_ptr，不存在返回 nullptr
     * 
     * 示例：
     * auto config = app.get_by_name<Configuration>("config");
     * auto lua_pool = app.get_by_name<LuaVMPool>("lua_pool");
     */
    template <typename T>
    std::shared_ptr<T> get_by_name(const std::string& name) const;
    
    /**
     * 检查组件是否存在
     */
    template <typename T>
    bool has() const;
    
    /**
     * 检查名称组件是否存在
     */
    bool has(const std::string& name) const;

private:
    ApplicationContext() = default;
    
    // 按类型索引
    std::unordered_map<std::type_index, std::shared_ptr<void>> type_registry_;
    
    // 按名称索引
    std::unordered_map<std::string, std::shared_ptr<void>> name_registry_;
};

} // namespace shield::core
```

### 使用示例

```cpp
// 任何地方都可以通过 ApplicationContext 访问组件
void some_function() {
    auto& app = ApplicationContext::instance();
    
    auto env = app.get<Environment>();
    auto logger = app.get<Logger>();
    auto config = app.get<Configuration>();
    auto lua_pool = app.get<LuaVMPool>();
    
    logger->info("Current environment: " + env->get("RUNTIME_ENV"));
}
```

## Environment（环境变量）

Environment 是第一个初始化的单例组件，负责管理环境变量。

### 核心接口

```cpp
// include/shield/core/environment.hpp
namespace shield::core {

/**
 * 环境变量管理器（单例）
 * 
 * 职责：
 * - 读取进程环境变量
 * - 支持 get/set 操作
 * - 支持环境变量展开（${VAR}）
 * - 提供 Lua API
 */
class Environment {
public:
    static Environment& instance();
    
    /**
     * 返回单例的 shared_ptr（用于注册到 ApplicationContext）
     */
    static std::shared_ptr<Environment> instance_ptr();
    
    /**
     * 从进程环境初始化
     * 在启动最开始调用
     */
    void initialize();
    
    /**
     * 获取环境变量
     * @param key 环境变量名
     * @param default_value 默认值（不存在时返回）
     * @return 环境变量值或默认值
     */
    std::string get(const std::string& key, const std::string& default_value = "") const;
    
    /**
     * 获取整数类型环境变量
     */
    int get_int(const std::string& key, int default_value = 0) const;
    
    /**
     * 获取布尔类型环境变量
     * 支持: true/yes/on/1 (true), false/no/off/0 (false)
     */
    bool get_bool(const std::string& key, bool default_value = false) const;
    
    /**
     * 设置环境变量
     * @param key 环境变量名
     * @param value 要设置的值
     * @note 只影响当前进程，不修改系统环境变量
     */
    void set(const std::string& key, const std::string& value);
    
    /**
     * 检查环境变量是否存在
     */
    bool has(const std::string& key) const;
    
    /**
     * 展开字符串中的环境变量引用
     * 支持格式：${VAR}、${VAR:-default}、${VAR:+replacement}
     * 
     * @param input 输入字符串
     * @return 展开后的字符串
     * 
     * 示例：
     *   "${DB_HOST:-localhost}" → "localhost"（如果 DB_HOST 不存在）
     *   "${DB_PORT:-3306}" → "5432"（如果 DB_PORT=5432）
     *   "${DEBUG:+--debug}" → "--debug"（如果 DEBUG 存在）
     */
    std::string expand(const std::string& input) const;
};

} // namespace shield::core
```

### Lua API

```lua
-- shield.env 模块（由 ScriptStarter 注册）

-- 获取环境变量
local db_host = shield.env.get("DB_HOST", "localhost")
local db_port = shield.env.get_int("DB_PORT", 3306)
local debug = shield.env.get_bool("DEBUG_ENABLED", false)

-- 检查环境变量是否存在
if shield.env.has("PRODUCTION") then
    shield.log.info("Running in production mode")
end

-- 设置环境变量（仅影响当前进程）
shield.env.set("WORKER_ID", worker_id)

-- 环境变量展开
local expanded = shield.env.expand("${DB_HOST:-localhost}:${DB_PORT:-3306}")
```

## IStarter 接口

### 核心接口

```cpp
// include/shield/core/starter.hpp
namespace shield::core {

/**
 * C++ 模块初始化器接口
 * 
 * 每个 Starter 负责一个运行时模块的初始化：
 * - EnvStarter: 环境变量
 * - ConfigStarter: 配置管理
 * - LogStarter: 日志系统
 * - CoreStarter: shield_core
 * - ScriptStarter: Lua VM 管理
 * - DataStarter: 数据库/Redis 连接池
 * - NetStarter: 网络层和连接管理
 * - TransportStarter: 协议适配层
 * - ClusterStarter: 集群通信
 */
class IStarter {
public:
    virtual ~IStarter() = default;
    
    /**
     * 初始化该 Starter 负责的模块
     * 
     * 实现者应该：
     * 1. 通过 ApplicationContext 获取依赖的组件
     * 2. 初始化模块资源
     * 3. 将创建的组件注册到 ApplicationContext
     * 4. 避免抛出异常，使用错误码或日志记录错误
     */
    virtual void initialize() = 0;
    
    /**
     * 返回 Starter 名称
     * 必须唯一，用于依赖解析和日志
     */
    virtual std::string name() const = 0;
    
    /**
     * 返回依赖的 Starter 名称列表
     * 框架会确保依赖先初始化
     * 
     * @return 依赖名称列表，空数组表示无依赖
     */
    virtual std::vector<std::string> depends_on() const { return {}; }
    
    /**
     * 返回是否启用该 Starter
     * 可通过 ApplicationContext 获取配置来决定
     * 
     * @return true 表示启用并初始化，false 表示跳过
     */
    virtual bool is_enabled() const { return true; }
    
    /**
     * 关闭时的清理逻辑
     * 按初始化相反顺序调用
     */
    virtual void shutdown() {}
};

} // namespace shield::core
```

### StarterManager

```cpp
// include/shield/core/starter_manager.hpp
namespace shield::core {

/**
 * Starter 管理器
 * 
 * 负责：
 * - 注册 Starter
 * - 解析依赖顺序（拓扑排序）
 * - 按顺序初始化所有 Starter
 * - 按相反顺序关闭所有 Starter
 */
class StarterManager {
public:
    /**
     * 注册一个 Starter
     * 
     * @param starter 要注册的 Starter，不能为 null
     * @throws std::invalid_argument 如果 starter 为 null
     * @throws std::runtime_error 如果名称已存在
     */
    void register_starter(std::unique_ptr<IStarter> starter);
    
    /**
     * 按依赖顺序初始化所有启用的 Starter
     * 
     * 流程：
     * 1. 过滤出启用的 Starter
     * 2. 解析依赖顺序（拓扑排序）
     * 3. 依次调用每个 Starter 的 initialize()
     * 
     * @throws std::runtime_error 如果检测到循环依赖
     * @throws std::runtime_error 如果依赖的 Starter 不存在
     * @throws 任何 Starter 抛出的异常
     */
    void initialize_all();
    
    /**
     * 按初始化相反顺序关闭所有 Starter
     * 
     * 依次调用每个已初始化 Starter 的 shutdown() 方法
     */
    void shutdown_all();
    
    /**
     * 获取已注册的 Starter 数量
     */
    size_t starter_count() const { return starters_.size(); }
    
    /**
     * 检查是否存在指定名称的 Starter
     */
    bool has_starter(const std::string& name) const;
    
    /**
     * 获取初始化顺序（用于调试）
     * 
     * @return Starter 名称列表，按初始化顺序排列
     */
    std::vector<std::string> initialization_order() const;

private:
    std::vector<std::unique_ptr<IStarter>> starters_;
    std::unordered_map<std::string, size_t> name_to_index_;
    std::vector<std::string> init_order_;
    
    /**
     * 解析初始化顺序（拓扑排序）
     */
    std::vector<size_t> resolve_initialization_order();
    
    /**
     * 拓扑排序的递归实现
     */
    void topological_sort(
        size_t starter_index,
        std::unordered_set<size_t>& visited,
        std::unordered_set<size_t>& visiting,
        std::vector<size_t>& order);
};

} // namespace shield::core
```

## 官方 Starter

### 核心模块 Starter

| Starter | 职责 | 依赖 |
|---------|------|------|
| `EnvStarter` | 初始化环境变量（第一步） | 无 |
| `ConfigStarter` | 加载和管理配置 | EnvStarter |
| `LogStarter` | 初始化日志系统 | ConfigStarter |
| `CoreStarter` | 初始化 shield_core | LogStarter |
| `ScriptStarter` | 创建 Lua VM 池，注册 shield.* API | CoreStarter |
| `DataStarter` | 初始化数据库/Redis 连接池 | ConfigStarter |
| `NetStarter` | 初始化网络层和监听器 | ConfigStarter |
| `TransportStarter` | 初始化协议适配层 | NetStarter |
| `ClusterStarter` | 初始化集群通信 | ConfigStarter |

### 启动顺序

```
EnvStarter（第一步）
    └─ ConfigStarter
        ├─ LogStarter
        │   └─ CoreStarter
        │       └─ ScriptStarter
        │
        ├─ DataStarter
        ├─ NetStarter
        │   └─ TransportStarter
        └─ ClusterStarter
```

## 具体实现示例

### EnvStarter（第一步）

```cpp
// include/shield/core/env_starter.hpp
namespace shield::core {

class EnvStarter : public IStarter {
public:
    void initialize() override {
        // 初始化环境变量管理器
        Environment::instance().initialize();
        
        // 注册到 ApplicationContext
        ApplicationContext::instance().register_singleton<Environment>(
            Environment::instance_ptr()
        );
    }
    
    std::string name() const override { return "EnvStarter"; }
    
    std::vector<std::string> depends_on() const override { return {}; }
    
    bool is_enabled() const override { return true; }  // 始终启用
};

} // namespace shield::core
```

### ConfigStarter

```cpp
// include/shield/config/config_starter.hpp
namespace shield::config {

class ConfigStarter : public IStarter {
public:
    void initialize() override {
        auto& app = ApplicationContext::instance();
        
        // 通过 ApplicationContext 访问 Environment
        auto env = app.get<Environment>();
        if (!env) {
            throw std::runtime_error("Environment not available");
        }
        
        // 解析配置文件路径（支持环境变量展开）
        std::string config_path = parse_config_path_from_command_line();
        config_path = env->expand(config_path);  // ${CONFIG_DIR}/app.yaml
        
        // 加载配置
        config_ = std::make_shared<Configuration>(config_path, env);
        
        // 注册到 ApplicationContext
        app.register_component<Configuration>(config_, "config");
    }
    
    std::string name() const override { return "ConfigStarter"; }
    
    std::vector<std::string> depends_on() const override {
        return {"EnvStarter"};
    }
    
    bool is_enabled() const override { return true; }
    
    void shutdown() override {
        config_.reset();
    }
    
private:
    std::shared_ptr<Configuration> config_;
};

} // namespace shield::config
```

### LogStarter

```cpp
// include/shield/log/log_starter.hpp
namespace shield::log {

class LogStarter : public IStarter {
public:
    void initialize() override {
        auto& app = ApplicationContext::instance();
        auto config = app.get<Configuration>();
        if (!config) {
            throw std::runtime_error("Configuration not available");
        }
        
        // 创建日志系统
        logger_ = std::make_shared<Logger>(config->get_log_config());
        logger_->initialize();
        
        // 注册到 ApplicationContext
        app.register_singleton<Logger>(logger_);
    }
    
    std::string name() const override { return "LogStarter"; }
    
    std::vector<std::string> depends_on() const override {
        return {"ConfigStarter"};
    }
    
    void shutdown() override {
        logger_->shutdown();
        logger_.reset();
    }
    
private:
    std::shared_ptr<Logger> logger_;
};

} // namespace shield::log
```

### ScriptStarter

```cpp
// include/shield/script/script_starter.hpp
namespace shield::script {

class ScriptStarter : public IStarter {
public:
    void initialize() override {
        auto& app = ApplicationContext::instance();
        
        // 通过 ApplicationContext 访问配置
        auto config = app.get<Configuration>();
        if (!config) {
            throw std::runtime_error("Configuration not available");
        }
        
        // 创建 Lua VM 池
        lua_pool_ = std::make_shared<LuaVMPool>(
            config->get_int("script.vm_pool_size", 10)
        );
        
        // 注册 shield.* API
        register_lua_api(lua_pool_);
        
        // 注册到 ApplicationContext
        app.register_component<LuaVMPool>(lua_pool_, "lua_pool");
        
        // 连接到 LifecyclePublisher（支持 Lua 插件钩子）
        LifecyclePublisher::instance().set_lua_pool(lua_pool_.get());
        
        // 发布生命周期事件：Lua VM 就绪
        LifecyclePublisher::instance().publish(
            LifecyclePhase::ScriptReady,
            R"({"vm_pool_size": )" + std::to_string(lua_pool_->size()) + "}"
        );
    }
    
    std::string name() const override { return "ScriptStarter"; }
    
    std::vector<std::string> depends_on() const override {
        return {"CoreStarter"};
    }
    
    bool is_enabled() const override {
        auto& app = ApplicationContext::instance();
        auto config = app.get<Configuration>();
        return config && config->get_bool("script.enabled", true);
    }
    
    void shutdown() override {
        lua_pool_.reset();
    }
    
private:
    std::shared_ptr<LuaVMPool> lua_pool_;
};

} // namespace shield::script
```

### DataStarter

```cpp
// include/shield/data/data_starter.hpp
namespace shield::data {

class DataStarter : public IStarter {
public:
    void initialize() override {
        auto& app = ApplicationContext::instance();
        auto config = app.get<Configuration>();
        auto lua_pool = app.get<LuaVMPool>();
        
        if (!config) {
            throw std::runtime_error("Configuration not available");
        }
        
        // 初始化数据库连接池
        if (config->get_bool("database.enabled", false)) {
            db_pool_ = std::make_shared<DBConnectionPool>(
                config->get_database_config()
            );
            app.register_component<DBConnectionPool>(db_pool_, "db_pool");
            
            // 注册 Lua API
            if (lua_pool) {
                register_database_lua_api(lua_pool, db_pool_);
            }
        }
        
        // 初始化 Redis 连接池
        if (config->get_bool("redis.enabled", false)) {
            redis_pool_ = std::make_shared<RedisConnectionPool>(
                config->get_redis_config()
            );
            app.register_component<RedisConnectionPool>(redis_pool_, "redis_pool");
            
            if (lua_pool) {
                register_redis_lua_api(lua_pool, redis_pool_);
            }
        }
    }
    
    std::string name() const override { return "DataStarter"; }
    
    std::vector<std::string> depends_on() const override {
        return {"ConfigStarter", "ScriptStarter"};
    }
    
    bool is_enabled() const override {
        auto& app = ApplicationContext::instance();
        auto config = app.get<Configuration>();
        return config && (config->get_bool("database.enabled", false) ||
                         config->get_bool("redis.enabled", false));
    }
    
    void shutdown() override {
        db_pool_.reset();
        redis_pool_.reset();
    }
    
private:
    std::shared_ptr<DBConnectionPool> db_pool_;
    std::shared_ptr<RedisConnectionPool> redis_pool_;
};

} // namespace shield::data
```

## 在 Bootstrap 中使用

```cpp
// include/shield/bootstrap/shield_bootstrap.hpp
namespace shield::bootstrap {

class Bootstrap {
public:
    void run(int argc, char** argv);
    
private:
    std::unique_ptr<StarterManager> starters_;
    
    void register_starters();
    void initialize_cpp_modules();
    void load_lua_plugins();
    void spawn_lua_services();
    void run_event_loop();
};

} // namespace shield::bootstrap
```

```cpp
// src/bootstrap/shield_bootstrap.cpp
namespace shield::bootstrap {

void Bootstrap::run(int argc, char** argv) {
    // 保存命令行参数（供 ConfigStarter 使用）
    command_args_ = {argc, argv};
    
    // ========== 1. 创建并注册 Starters ==========
    starters_ = std::make_unique<StarterManager>();
    register_starters();
    
    // ========== 2. 初始化 C++ 模块 ==========
    initialize_cpp_modules();
    
    // ========== 3. 加载 Lua 插件 ==========
    load_lua_plugins();
    
    // ========== 4. 启动 Lua Services ==========
    spawn_lua_services();
    
    // ========== 5. 进入事件循环 ==========
    run_event_loop();
}

void Bootstrap::register_starters() {
    // 按任意顺序注册，StarterManager 会自动解析依赖顺序
    starters_->register_starter(std::make_unique<EnvStarter>());
    starters_->register_starter(std::make_unique<ConfigStarter>());
    starters_->register_starter(std::make_unique<LogStarter>());
    starters_->register_starter(std::make_unique<CoreStarter>());
    starters_->register_starter(std::make_unique<ScriptStarter>());
    starters_->register_starter(std::make_unique<DataStarter>());
    starters_->register_starter(std::make_unique<NetStarter>());
    starters_->register_starter(std::make_unique<TransportStarter>());
    starters_->register_starter(std::make_unique<ClusterStarter>());
}

void Bootstrap::initialize_cpp_modules() {
    // 发布生命周期事件：模块初始化前
    LifecyclePublisher::instance().publish(
        LifecyclePhase::PreModuleInit,
        "Starting C++ module initialization"
    );
    
    // 初始化所有 Starters
    // StarterManager 会自动按依赖顺序初始化
    starters_->initialize_all();
    
    // 发布生命周期事件：模块初始化完成
    LifecyclePublisher::instance().publish(
        LifecyclePhase::PostModuleInit,
        "C++ module initialization completed"
    );
}

void Bootstrap::load_lua_plugins() {
    auto& app = ApplicationContext::instance();
    auto config = app.get<Configuration>();
    auto lua_pool = app.get<LuaVMPool>();
    
    if (!config || !lua_pool) {
        SHIELD_LOG_WARN << "Cannot load plugins: Configuration or Lua VM not available";
        return;
    }
    
    if (!config->get_bool("plugins.enabled", false)) {
        SHIELD_LOG_INFO << "Plugins disabled";
        return;
    }
    
    std::string plugin_dir = config->get_string("plugins.directory", "plugins");
    auto plugin_names = config->get_string_list("plugins.load", {});
    
    for (const auto& plugin_name : plugin_names) {
        std::string plugin_path = plugin_dir + "/" + plugin_name + ".lua";
        
        try {
            auto plugin = lua_pool->load_script(plugin_path);
            
            // 获取插件配置
            auto plugin_config = config->get_subconfig("plugins.config." + plugin_name);
            
            // 调用插件的 on_load 函数
            sol::function on_load = plugin["on_load"];
            if (on_load.valid()) {
                on_load(plugin_config.to_lua_table());
            }
            
            SHIELD_LOG_INFO << "Loaded plugin: " << plugin_name;
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to load plugin '" << plugin_name 
                             << "': " << e.what();
        }
    }
}

void Bootstrap::spawn_lua_services() {
    auto& app = ApplicationContext::instance();
    auto config = app.get<Configuration>();
    
    if (!config) {
        throw std::runtime_error("Configuration not available");
    }
    
    LifecyclePublisher::instance().publish(LifecyclePhase::PreServiceSpawn);
    
    // 从配置读取服务列表并启动
    auto services = config->get_list("actors", {});
    for (const auto& service_config : services) {
        std::string name = service_config["name"];
        std::string script = service_config["script"];
        
        spawn_lua_service(name, script, service_config);
    }
    
    LifecyclePublisher::instance().publish(LifecyclePhase::PostServiceSpawn);
}

void Bootstrap::run_event_loop() {
    LifecyclePublisher::instance().publish(LifecyclePhase::RuntimeStart, "Entering event loop");
    
    auto& app = ApplicationContext::instance();
    auto core = app.get<ShieldCore>();
    if (!core) {
        throw std::runtime_error("Shield core not available");
    }
    
    // 进入事件循环（阻塞直到收到停止信号）
    core->run_event_loop();
    
    // 关闭流程
    LifecyclePublisher::instance().publish(LifecyclePhase::RuntimeStop);
    starters_->shutdown_all();
    LifecyclePublisher::instance().publish(LifecyclePhase::PostShutdown);
}

} // namespace shield::bootstrap
```

## 生命周期事件系统

生命周期事件系统支持 C++ 和 Lua 两种监听器，用于框架扩展和插件开发。

### 事件阶段

```cpp
// include/shield/core/lifecycle.hpp
namespace shield::core {

enum class LifecyclePhase {
    // C++ 层（Lua VM 不存在时）
    PreConfigLoad,
    PostConfigLoad,
    
    // Lua 层（Lua VM 存在后可用）
    PreDataInit,
    PostDataInit,
    PreNetInit,
    PostNetInit,
    PreTransportInit,
    PostTransportInit,
    PreClusterInit,
    PostClusterInit,
    
    ScriptReady,        // Lua VM 就绪
    
    // 通用
    PreModuleInit,
    PostModuleInit,
    PreServiceSpawn,
    PostServiceSpawn,
    RuntimeStart,
    RuntimeStop,
    PreShutdown,
    PostShutdown,
};

struct LifecycleEvent {
    LifecyclePhase phase;
    std::string context;  // JSON 格式的上下文数据
};

} // namespace shield::core
```

### C++ 监听器

```cpp
// C++ 监听器接口
class ILifecycleListener {
public:
    virtual void on_event(const LifecycleEvent& event) = 0;
    virtual ~ILifecycleListener() = default;
};

// 使用示例
class MyListener : public ILifecycleListener {
public:
    void on_event(const LifecycleEvent& event) override {
        if (event.phase == LifecyclePhase::RuntimeStart) {
            std::cout << "Runtime started!" << std::endl;
        }
    }
};

// 注册
auto& publisher = LifecyclePublisher::instance();
publisher.subscribe(LifecyclePhase::RuntimeStart, new MyListener());
```

### Lua 监听器（插件钩子）

```lua
-- Lua 插件通过 shield.plugin.on() 订阅事件

-- 订阅数据初始化完成事件
shield.plugin.on("PostDataInit", function(context)
    local config = json.decode(context)
    shield.log.info("Data initialized: " .. config.database.host)
end)

-- 订阅服务启动前事件
shield.plugin.on("PreServiceSpawn", function(context)
    shield.log.info("About to spawn services...")
    preload_common_resources()
end)

-- 订阅运行时开始事件
shield.plugin.on("RuntimeStart", function(context)
    shield.log.info("Runtime started, initializing background tasks...")
    start_background_tasks()
end)

-- 订阅关闭事件
shield.plugin.on("PreShutdown", function(context)
    shield.log.info("Shutdown requested, cleaning up...")
    cleanup_resources()
end)
```

### 插件示例

```lua
-- plugins/my_metrics.lua
local M = {}

function M.on_load(config)
    if config.enabled then
        shield.log.info("MyMetrics plugin enabled, export on port " .. config.export_port)
    end
    
    -- 订阅生命周期事件
    shield.plugin.on("RuntimeStart", function()
        M:start_exporter(config.export_port)
    end)
    
    shield.plugin.on("PreShutdown", function()
        M:stop_exporter()
    end)
end

function M:start_exporter(port)
    -- 启动指标导出服务
    self.exporter = http.createServer(port, function(req, res)
        res:write(M:collect_metrics())
    end)
    
    shield.log.info("Metrics exporter started on port " .. port)
end

function M:stop_exporter()
    if self.exporter then
        self.exporter:close()
        shield.log.info("Metrics exporter stopped")
    end
end

function M:collect_metrics()
    return {
        services = shield.ops.services(),
        memory = collectgc("count"),
    }
end

return M
```

## 扩展 Starter

用户可以自定义 Starter 来扩展框架功能：

```cpp
// 用户自定义 Starter
class MyMetricsStarter : public core::IStarter {
public:
    void initialize() override {
        auto& app = ApplicationContext::instance();
        
        auto config = app.get<Configuration>();
        auto lua_pool = app.get<LuaVMPool>();
        
        // 初始化自定义指标收集
        metrics_ = std::make_shared<MyMetricsCollector>(config);
        
        // 注册到 ApplicationContext
        app.register_component<MyMetricsCollector>(metrics_, "my_metrics");
        
        // 注册 Lua API
        if (lua_pool) {
            lua_pool->register_function("my_metrics", metrics_);
        }
    }
    
    std::string name() const override { return "MyMetricsStarter"; }
    
    std::vector<std::string> depends_on() const override {
        return {"ScriptStarter"};
    }
    
    bool is_enabled() const override {
        auto& app = ApplicationContext::instance();
        auto config = app.get<Configuration>();
        return config && config->get_bool("my_metrics.enabled", false);
    }
    
    void shutdown() override {
        metrics_.reset();
    }
    
private:
    std::shared_ptr<MyMetricsCollector> metrics_;
};

// 在 Bootstrap 中注册
starters_->register_starter(std::make_unique<MyMetricsStarter>());
```

## 配置

### 环境变量

```bash
# 开发环境
export RUNTIME_ENV=development
export DB_HOST=localhost
export DEBUG_ENABLED=true
./shield

# 生产环境
export RUNTIME_ENV=production
export DB_HOST=prod.db.example.com
export DB_PASSWORD=${SECRET_PASSWORD}
./shield
```

### 配置文件（支持环境变量展开）

```yaml
# config/app.yaml
app:
  name: ${APP_NAME:-MyGame}
  env: ${RUNTIME_ENV:-development}

database:
  host: ${DB_HOST:-localhost}
  port: ${DB_PORT:-3306}
  username: ${DB_USER:-root}
  password: ${DB_PASSWORD}
  database: ${DB_NAME:-game}

redis:
  host: ${REDIS_HOST:-localhost}
  port: ${REDIS_PORT:-6379}

# 开关示例
debug:
  enabled: ${DEBUG_ENABLED:-false}
  log_level: ${DEBUG_LOG_LEVEL:-info}

# 条件替换示例
profiler:
  enabled: ${PERF_PROFILE:+true}
  output_dir: ${PERF_PROFILE:-/tmp/profile}
```

### Starter 配置

```yaml
# config/app.yaml
# Script 模块配置
script:
  enabled: true
  vm_pool_size: 10

# Data 模块配置
database:
  enabled: true
  # ...

redis:
  enabled: true
  # ...

# 插件配置
plugins:
  enabled: true
  directory: "plugins"
  load:
    - my_metrics
    - custom_auth
  config:
    my_metrics:
      enabled: true
      export_port: 9091
    custom_auth:
      secret_key: "${AUTH_SECRET}"
```

## 错误处理

### 依赖解析错误

```cpp
// 循环依赖检测
void StarterManager::topological_sort(/* ... */) {
    if (visiting.find(starter_index) != visiting.end()) {
        throw std::runtime_error(
            "Circular dependency detected involving Starter: " +
            starters_[starter_index]->name()
        );
    }
}

// 依赖不存在检测
for (const std::string& dep_name : dependencies) {
    auto it = name_to_index_.find(dep_name);
    if (it == name_to_index_.end()) {
        throw std::runtime_error(
            "Starter '" + starters_[starter_index]->name() +
            "' depends on unknown Starter: " + dep_name
        );
    }
}
```

### 初始化失败处理

```cpp
void StarterManager::initialize_all() {
    for (size_t index : enabled_order) {
        auto& starter = starters_[index];
        
        try {
            starter->initialize();
            
            SHIELD_LOG_INFO << "Successfully initialized Starter: " 
                            << starter->name();
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to initialize Starter '"
                             << starter->name() << "': " << e.what();
            
            // 清理已初始化的 Starter
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                if (*it == index) break;
                starters_[*it]->shutdown();
            }
            
            throw;  // 重新抛出，终止启动
        }
    }
}
```

## 实施建议

### Phase 1: 简化 ApplicationContext

1. 移除原有的 `register_service`/`get_service` DI 能力
2. 简化为 `register_singleton`/`register_component`/`get`/`get_by_name`
3. 移除 `StarterContext`

### Phase 2: 添加 Environment

1. 实现 `Environment` 单例
2. 添加 `EnvStarter` 作为第一个初始化的 Starter
3. 支持环境变量展开 `${VAR:-default}`

### Phase 3: 重构现有 Starter

1. 修改所有 Starter 使用 `ApplicationContext::instance().get<T>()`
2. 移除对 `StarterContext` 的依赖
3. 组件通过 `register_component` 注册到 ApplicationContext

### Phase 4: 添加生命周期事件系统

1. 实现 `LifecyclePublisher`
2. 在 Bootstrap 各阶段发布事件
3. 支持 C++ 和 Lua 监听器

### Phase 5: 添加插件支持

1. 实现插件加载器
2. 注册 Lua API `shield.plugin.on()`
3. 支持插件配置

## 总结

| 特性 | 状态 |
|------|------|
| 依赖管理（拓扑排序） | ✅ 保留 |
| 生命周期钩子 | ✅ 保留 |
| 条件启用 | ✅ 保留 |
| ApplicationContext 统一访问 | ✅ 新设计 |
| Environment 环境变量 | ✅ 新增 |
| 生命周期事件系统 | ✅ 新增 |
| Lua 插件钩子 | ✅ 新增 |
| DI 能力 | ❌ 移除 |
| Service 注册/获取 | ❌ 移除 |

**新定位：**
- **ApplicationContext**：统一的应用上下文访问入口
- **Starter**：C++ 模块初始化器，负责运行时模块的启动顺序和生命周期管理
- **Environment**：环境变量管理器，支持 get/set 和变量展开
- **生命周期事件**：框架级事件通知，支持 C++ 和 Lua 监听器
