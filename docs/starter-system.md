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
| `ConfigStarter` | 加载本地配置文件 | EnvStarter |
| `LogStarter` | 初始化日志系统 | ConfigStarter |
| `ScriptStarter` | 创建 Lua VM，注册基础 API，加载插件 | LogStarter |
| `CoreStarter` | 初始化 shield_core | LogStarter |
| `DataStarter` | 初始化数据库/Redis 连接池 | ConfigStarter |
| `NetStarter` | 初始化网络层和监听器 | ConfigStarter |
| `TransportStarter` | 初始化协议适配层 | NetStarter |
| `ClusterStarter` | 初始化集群通信 | ConfigStarter |

### 启动顺序（Lua 覆盖配置后的所有阶段）

```
EnvStarter（第一步）
    └─ ConfigStarter（加载本地配置文件）
        └─ LogStarter
            ├─ ScriptStarter（创建 Lua VM，发布 ConfigReady）
            │   ├─ DataStarter  ← Lua 可参与
            │   ├─ NetStarter   ← Lua 可参与
            │   ├─ TransportStarter  ← Lua 可参与
            │   └─ ClusterStarter  ← Lua 可参与
            └─ CoreStarter  ← 发布 CoreReady，Lua 可使用完整 API
```

### 设计原则：配置是持续加载的过程

**配置加载不是一次性完成的**，而是一个持续的过程：

1. **ConfigStarter**：加载本地配置文件（YAML）
2. **ScriptStarter**：发布 `ConfigReady` 事件，Lua 代码可以：
   - 从远程配置中心（etcd、Consul）拉取配置
   - 验证和修改配置
   - 合并远程配置到本地配置
3. **后续 Starter**：使用最终确定的配置

### ScriptStarter：Lua VM 生命周期起点

**ScriptStarter 在配置加载完成后立即启动**，只依赖 LogStarter。

职责：
- 创建 Lua VM 池
- 注册**基础 API**（见下文 API 可用性矩阵）
- 加载所有插件脚本
- 发布 `ConfigReady` 事件，触发 Lua 配置处理
- 发布 `ScriptReady` 事件

注意：此时 `shield.call`/`shield.send` 等 Actor API 还不可用（CoreStarter 未完成）。

### Lua API 可用性矩阵

| 阶段 | 可用 API | 说明 |
|-----|---------|------|
| **ConfigReady** | `shield.plugin.on()`<br>`shield.config.*`<br>`shield.env.*`<br>`shield.log.*` | Lua VM 创建完成，可以访问和修改配置 |
| **CoreReady** | + `shield.spawn()`<br>`shield.call()`<br>`shield.send()`<br>`shield.fork()`<br>`shield.sleep()`<br>`shield.timer*()` | Actor 系统就绪，可以创建服务 |
| **PostDataInit** | + `shield.data.*`<br>`shield.redis.*` | 数据层就绪 |
| **PostNetInit** | + `shield.net.*` | 网络层就绪 |
| **PostTransportInit** | + `shield.transport.*` | 协议适配层就绪 |
| **PostClusterInit** | + `shield.cluster.*` | 集群通信就绪 |
| **RuntimeStart** | 所有 API 完全可用 | 进入事件循环 |

### 配置系统设计：支持运行时修改

Configuration 需要支持 Lua 代码的运行时修改：

```cpp
// include/shield/config/configuration.hpp
namespace shield::config {

class Configuration {
public:
    // ... 现有的 get 方法 ...
    
    /**
     * 运行时修改配置（供 Lua 使用）
     * 
     * @param key 配置键（支持点号分隔的路径）
     * @param value 配置值
     * 
     * 示例：
     * config->set("database.host", "remote.db.example.com");
     * config->set("database.port", 5432);
     */
    void set(const std::string& key, const sol::object& value);
    
    /**
     * 合并远程配置
     * 
     * @param remote_config 远程配置表
     * 
     * 示例：
     * local remote = { database = { host = "10.0.0.1", port = 3306 } }
     * config:merge(remote)
     */
    void merge(const sol::table& remote_config);
    
    /**
     * 验证配置完整性
     * 
     * @param schema 验证模式
     * @return 验证结果
     */
    ValidationResult validate(const ValidationSchema& schema) const;
    
private:
    // 配置存储（支持运行时修改）
    std::unordered_map<std::string, ConfigValue> values_;
    mutable std::shared_mutex mutex_;  // 支持并发读写
};

} // namespace shield::config
```

### Lua 配置 API

```lua
-- shield.config 模块（由 ScriptStarter 注册）

-- 获取配置
local db_host = shield.config.get("database.host", "localhost")
local db_port = shield.config.get_int("database.port", 3306)

-- 修改配置（运行时）
shield.config.set("database.host", "remote.db.example.com")
shield.config.set("database.pool_size", 50)

-- 合并远程配置
local remote_config = {
    database = {
        host = "10.0.0.1",
        port = 3306,
        pool_size = 100
    },
    redis = {
        host = "10.0.0.2",
        port = 6379
    }
}
shield.config.merge(remote_config)

-- 验证配置
local ok, err = shield.config.validate(validation_schema)
if not ok then
    error("Configuration validation failed: " .. err)
end
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

### ScriptStarter（配置后的 Lua VM 生命周期起点）

```cpp
// include/shield/script/script_starter.hpp
namespace shield::script {

/**
 * Lua VM 初始化器
 *
 * 职责：
 * - 在 LogStarter 之后创建 Lua VM 池
 * - 注册基础 API（plugin, config, env, log）
 * - 加载所有插件脚本
 * - 发布 ConfigReady 事件，让 Lua 代码可以处理配置
 * - 发布 ScriptReady 事件
 */
class ScriptStarter : public IStarter {
public:
    void initialize() override {
        auto& app = ApplicationContext::instance();
        auto config = app.get<Configuration>();
        auto logger = app.get<Logger>();

        if (!config) {
            throw std::runtime_error("Configuration not available");
        }

        // 创建 Lua VM 池
        lua_pool_ = std::make_shared<LuaVMPool>(
            config->get_int("script.vm_pool_size", 10)
        );

        // 注册基础 API（此时 CoreStarter 可能还未完成）
        register_basic_api(lua_pool_, config, logger);

        // 加载插件脚本
        if (config->get_bool("plugins.enabled", false)) {
            load_plugins(config);
        }

        // 注册到 ApplicationContext
        app.register_component<LuaVMPool>(lua_pool_, "lua_pool");

        // 连接到生命周期发布器
        LifecyclePublisher::instance().set_lua_pool(lua_pool_.get());

        // 发布 ConfigReady 事件，触发 Lua 配置处理
        LifecyclePublisher::instance().publish(
            LifecyclePhase::ConfigReady,
            config->to_json()
        );

        // 注册完整 API 的钩子（等 CoreStarter 完成后）
        LifecyclePublisher::instance().subscribe(LifecyclePhase::CoreReady, [this, &app]() {
            auto core = app.get<ShieldCore>();
            if (core) {
                register_full_api(lua_pool_, core);
            }
        });

        SHIELD_LOG_INFO << "ScriptStarter: Lua VM created, ConfigReady published";
    }

    std::string name() const override { return "ScriptStarter"; }

    std::vector<std::string> depends_on() const override {
        return {"LogStarter"};  // 只依赖日志，不依赖 CoreStarter
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

    /**
     * 注册基础 API（ConfigStarter 完成后可用）
     */
    void register_basic_api(std::shared_ptr<LuaVMPool> pool,
                            std::shared_ptr<Configuration> config,
                            std::shared_ptr<Logger> logger) {
        // shield.plugin.on() - 事件订阅
        // shield.config.* - 配置访问（支持运行时修改）
        // shield.env.* - 环境变量
        // shield.log.* - 日志
    }

    /**
     * 注册完整 API（CoreStarter 完成后调用）
     */
    void register_full_api(std::shared_ptr<LuaVMPool> pool,
                           std::shared_ptr<ShieldCore> core) {
        // shield.spawn() - 创建服务
        // shield.call() - 同步调用
        // shield.send() - 异步发送
        // shield.fork() - 创建协程
        // shield.sleep() - 挂起协程
        // shield.timer*() - 定时器
        // ...

        // 发布 ScriptReady 事件
        LifecyclePublisher::instance().publish(LifecyclePhase::ScriptReady);
    }

    void load_plugins(std::shared_ptr<Configuration> config);
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
    // 注意：Lua 插件已在 ScriptStarter 阶段加载

    // ========== 3. 启动 Lua Services ==========
    spawn_lua_services();

    // ========== 4. 进入事件循环 ==========
    run_event_loop();
}

void Bootstrap::register_starters() {
    // 按任意顺序注册，StarterManager 会自动解析依赖顺序
    starters_->register_starter(std::make_unique<EnvStarter>());
    starters_->register_starter(std::make_unique<ConfigStarter>());
    starters_->register_starter(std::make_unique<LogStarter>());
    starters_->register_starter(std::make_unique<ScriptStarter>());  // 创建 Lua VM
    starters_->register_starter(std::make_unique<CoreStarter>());
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
    // 注意：Lua 插件已在 ScriptStarter 阶段加载
    // 此函数保留仅为兼容性，实际执行为空操作
    SHIELD_LOG_INFO << "Plugins already loaded in ScriptStarter phase";
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

### Lua 参与配置加载示例

```lua
-- plugins/remote_config.lua
local M = {}

-- HTTP 客户端（用于从远程配置中心拉取配置）
local http = require("shield.http")

function M.on_load(config)
    -- 订阅 ConfigReady 事件，从远程配置中心拉取配置
    shield.plugin.on("ConfigReady", function(local_config)
        shield.log.info("Fetching remote configuration...")
        
        -- 从 etcd/Consul 拉取远程配置
        local remote_config = M:fetch_from_etcd()
        
        if remote_config then
            -- 合并远程配置到本地配置
            shield.config.merge(remote_config)
            shield.log.info("Remote configuration merged successfully")
        else
            shield.log.warn("Failed to fetch remote configuration, using local config")
        end
        
        -- 验证最终配置
        M:validate_config()
    end)
    
    -- 订阅 DataInit 之前的事件，动态调整数据库连接池配置
    shield.plugin.on("PreDataInit", function()
        local pool_size = shield.config.get_int("database.pool_size", 10)
        
        -- 根据服务器规格动态调整连接池大小
        local cpu_count = os.getenv("CPU_COUNT") or 4
        pool_size = math.max(pool_size, cpu_count * 2)
        
        shield.config.set("database.pool_size", pool_size)
        shield.log.info("Database pool size adjusted to " .. pool_size)
    end)
end

function M:fetch_from_etcd()
    -- 连接到 etcd
    local etcd_host = shield.env.get("ETCD_HOST", "localhost")
    local etcd_port = shield.env.get_int("ETCD_PORT", 2379)
    
    local url = string.format("http://%s:%d/v2/keys/game/config", etcd_host, etcd_port)
    
    -- 发起 HTTP GET 请求
    local response, err = http.get(url)
    if err then
        shield.log.error("Failed to fetch from etcd: " .. err)
        return nil
    end
    
    -- 解析响应
    local data = json.decode(response.body)
    
    -- etcd v2 返回格式: { node: { value: "..." } }
    if data.node and data.node.value then
        return json.decode(data.node.value)
    end
    
    return nil
end

function M:validate_config()
    -- 验证必需的配置项
    local required_keys = {
        "database.host",
        "database.port",
        "database.name",
        "redis.host",
        "redis.port"
    }
    
    for _, key in ipairs(required_keys) do
        local value = shield.config.get(key)
        if not value then
            error("Missing required configuration: " .. key)
        end
    end
    
    shield.log.info("Configuration validation passed")
end

return M
```

### 多环境配置示例

```lua
-- plugins/environment_config.lua
local M = {}

function M.on_load(config)
    -- 根据环境变量加载不同环境的配置
    shield.plugin.on("ConfigReady", function()
        local env = shield.env.get("RUNTIME_ENV", "development")
        
        -- 从不同路径加载环境特定配置
        local env_config_file = string.format("config/%s.yaml", env)
        
        -- 合并环境特定配置
        local env_config = load_yaml_file(env_config_file)
        if env_config then
            shield.config.merge(env_config)
            shield.log.info(string.format("Loaded %s environment config", env))
        end
        
        -- 覆盖特定配置项
        if env == "production" then
            shield.config.set("log.level", "warn")
            shield.config.set("database.pool_size", 100)
        elseif env == "development" then
            shield.config.set("log.level", "debug")
            shield.config.set("database.pool_size", 10)
        end
    end)
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

### Phase 4: 添加 ScriptStarter

1. 实现 `ScriptStarter`：在 LogStarter 之后创建 Lua VM
2. 注册基础 API（plugin, config, env, log）
3. 加载所有插件脚本
4. 发布 `ConfigReady` 事件，让 Lua 代码可以处理配置
5. 订阅 `CoreReady` 事件，补全完整 API

### Phase 5: 添加生命周期事件系统

1. 实现 `LifecyclePublisher`
2. 在 Bootstrap 各阶段发布事件
3. 支持 C++ 和 Lua 监听器

## 总结

| 特性 | 状态 |
|------|------|
| 依赖管理（拓扑排序） | ✅ 保留 |
| 生命周期钩子 | ✅ 保留 |
| 条件启用 | ✅ 保留 |
| ApplicationContext 统一访问 | ✅ 新设计 |
| Environment 环境变量 | ✅ 新增 |
| Lua 覆盖配置后的所有阶段 | ✅ 新设计 |
| 配置系统支持运行时修改 | ✅ 新增 |
| 生命周期事件系统 | ✅ 新增 |
| Lua 插件全生命周期参与 | ✅ 新增 |
| DI 能力 | ❌ 移除 |
| Service 注册/获取 | ❌ 移除 |

**新定位：**
- **ApplicationContext**：统一的应用上下文访问入口
- **Starter**：C++ 模块初始化器，负责运行时模块的启动顺序和生命周期管理
- **Environment**：环境变量管理器，支持 get/set 和变量展开
- **ScriptStarter**：Lua VM 生命周期起点，只依赖 LogStarter
- **配置是持续的过程**：本地配置 → ConfigReady → Lua 拉取远程配置 → 最终配置
- **生命周期事件**：框架级事件通知，支持 Lua 代码参与整个启动流程
- **插件系统**：通过 `shield.plugin.on()` 订阅生命周期事件，只是 Lua 能力的一部分
