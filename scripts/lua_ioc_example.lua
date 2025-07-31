-- Shield Lua IOC 使用示例
-- 展示如何在Lua中使用类似Spring Boot的IOC功能

local shield = require("lua_ioc_container")

-- =====================================
-- 1. 基础服务定义 (类似Spring的@Service)
-- =====================================

-- 数据库服务
local DatabaseService = {
    connection_string = "mongodb://localhost:27017",
    connected = false,
    
    -- 生命周期方法
    on_init = function(self, container)
        print("[DatabaseService] Initializing with connection: " .. self.connection_string)
        self.config = container:resolve("config")
        if self.config then
            self.connection_string = self.config:get("database.url") or self.connection_string
        end
    end,
    
    on_start = function(self)
        print("[DatabaseService] Connecting to database...")
        self.connected = true
        print("[DatabaseService] Connected successfully")
    end,
    
    on_stop = function(self)
        print("[DatabaseService] Disconnecting from database...")
        self.connected = false
    end,
    
    on_config_reloaded = function(self)
        print("[DatabaseService] Configuration reloaded, reconnecting...")
        -- 重新连接逻辑
    end,
    
    -- 业务方法
    find_user = function(self, user_id)
        if not self.connected then
            error("Database not connected")
        end
        return {id = user_id, name = "User_" .. user_id, email = "user" .. user_id .. "@example.com"}
    end,
    
    save_user = function(self, user)
        if not self.connected then
            error("Database not connected")
        end
        print("[DatabaseService] Saving user: " .. user.name)
        return true
    end
}

-- 缓存服务
local CacheService = {
    cache_data = {},
    
    on_init = function(self, container)
        print("[CacheService] Initializing cache service")
    end,
    
    on_start = function(self)
        print("[CacheService] Cache service started")
    end,
    
    get = function(self, key)
        return self.cache_data[key]
    end,
    
    set = function(self, key, value, ttl)
        self.cache_data[key] = {
            value = value,
            expires = os.time() + (ttl or 3600)
        }
    end,
    
    clear = function(self)
        self.cache_data = {}
        print("[CacheService] Cache cleared")
    end
}

-- 用户服务 (依赖注入示例)
local UserService = {
    dependencies = {
        database = "db_service",    -- 注入DatabaseService
        cache = "cache_service"     -- 注入CacheService  
    },
    
    on_init = function(self, container)
        print("[UserService] Initializing with dependencies")
        -- 依赖已经通过IoC容器注入到self.db_service和self.cache_service
    end,
    
    on_start = function(self)
        print("[UserService] User service started")
    end,
    
    get_user = function(self, user_id)
        local cache_key = "user_" .. user_id
        
        -- 先检查缓存
        local cached = self.cache_service:get(cache_key)
        if cached and cached.expires > os.time() then
            print("[UserService] User found in cache: " .. user_id)
            return cached.value
        end
        
        -- 从数据库获取
        print("[UserService] Loading user from database: " .. user_id)
        local user = self.db_service:find_user(user_id)
        
        -- 缓存结果
        self.cache_service:set(cache_key, user, 300) -- 5分钟TTL
        
        return user
    end,
    
    create_user = function(self, user_data)
        print("[UserService] Creating new user: " .. user_data.name)
        local success = self.db_service:save_user(user_data)
        if success then
            -- 清除相关缓存
            self.cache_service:clear()
        end
        return success
    end
}

-- =====================================
-- 2. 配置服务 (类似Spring的@ConfigurationProperties)
-- =====================================

local ConfigService = {
    properties = {
        ["database.url"] = "mongodb://localhost:27017",
        ["database.pool_size"] = 10,
        ["cache.ttl"] = 3600,
        ["features.user_cache.enabled"] = true,
        ["logging.level"] = "INFO"
    },
    
    get = function(self, key)
        return self.properties[key]
    end,
    
    set = function(self, key, value)
        self.properties[key] = value
    end,
    
    reload = function(self)
        print("[ConfigService] Reloading configuration from file...")
        -- 实际实现中会从文件重新加载
        self.properties["database.url"] = "mongodb://prod-server:27017"
    end
}

-- =====================================
-- 3. 工厂函数 (类似Spring的@Bean)
-- =====================================

local function create_logger()
    return {
        log_level = "INFO",
        
        info = function(self, message)
            if self.log_level == "INFO" or self.log_level == "DEBUG" then
                print("[INFO] " .. message)
            end
        end,
        
        error = function(self, message)
            print("[ERROR] " .. message)
        end,
        
        debug = function(self, message)
            if self.log_level == "DEBUG" then
                print("[DEBUG] " .. message)
            end
        end
    }
end

-- =====================================
-- 4. 事件监听器
-- =====================================

local function setup_event_listeners(container)
    -- 应用启动事件
    container:register_event_listener("application_started", function(event_data, container)
        local logger = container:resolve("logger")
        logger:info("🚀 Shield Lua Application Started Successfully!")
        
        -- 执行启动后的健康检查
        local health = container:check_health()
        logger:info("Overall health status: " .. health.status)
    end, 100)
    
    -- 配置重载事件
    container:register_event_listener("config_reloaded", function(event_data, container)
        local logger = container:resolve("logger")
        logger:info("📝 Configuration reloaded")
    end, 50)
    
    -- 应用停止事件
    container:register_event_listener("application_stopped", function(event_data, container)
        local logger = container:resolve("logger")
        logger:info("🛑 Shield Lua Application Stopped")
    end, 100)
end

-- =====================================
-- 5. 健康检查 (类似Spring Boot Actuator)
-- =====================================

local function setup_health_checks(container)
    -- 数据库健康检查
    container:register_health_check("database", function(container)
        local db = container:resolve("db_service")
        if db and db.connected then
            return {
                status = "UP",
                details = {
                    connection_string = db.connection_string,
                    connected = tostring(db.connected)
                }
            }
        else
            return {
                status = "DOWN",
                details = {
                    error = "Database not connected"
                }
            }
        end
    end)
    
    -- 缓存健康检查
    container:register_health_check("cache", function(container)
        local cache = container:resolve("cache_service")
        local cache_size = 0
        for _ in pairs(cache.cache_data) do cache_size = cache_size + 1 end
        
        return {
            status = "UP",
            details = {
                cache_size = tostring(cache_size)
            }
        }
    end)
    
    -- 应用健康检查
    container:register_health_check("application", function(container)
        return {
            status = "UP",
            details = {
                version = "1.0.0",
                uptime = tostring(os.time())
            }
        }
    end)
end

-- =====================================
-- 6. 服务注册和应用启动
-- =====================================

local function main()
    print("=== Shield Lua IoC Framework Demo ===\n")
    
    local container = shield.container
    
    -- 注册基础服务
    container:register_service("config", ConfigService, "singleton")
    container:register_service("db_service", DatabaseService, "singleton")
    container:register_service("cache_service", CacheService, "singleton")
    container:register_service("user_service", UserService, "singleton")
    
    -- 注册工厂函数
    container:register_factory("logger", create_logger, "singleton")
    
    -- 条件化注册 (只有启用缓存功能时才注册)
    container:register_conditional("enhanced_cache", {
        on_init = function(self, container)
            print("[EnhancedCache] Advanced caching enabled!")
        end,
        
        get_with_stats = function(self, key)
            print("[EnhancedCache] Getting " .. key .. " with statistics")
            return "enhanced_" .. key
        end
    }, {
        property = "features.user_cache.enabled",
        value = true
    })
    
    -- 设置事件监听器
    setup_event_listeners(container)
    
    -- 设置健康检查
    setup_health_checks(container)
    
    -- 解析和初始化所有服务
    print("\n--- Resolving Services ---")
    local config = container:resolve("config")
    local logger = container:resolve("logger")
    local user_service = container:resolve("user_service")
    
    -- 启动所有服务
    print("\n--- Starting Application ---")
    container:start_all()
    
    -- 显示调试信息
    container:debug_info()
    
    -- 测试业务逻辑
    print("--- Testing Business Logic ---")
    local user1 = user_service:get_user(123)
    logger:info("Retrieved user: " .. user1.name)
    
    local user2 = user_service:get_user(123) -- 应该从缓存获取
    
    user_service:create_user({name = "New User", email = "new@example.com"})
    
    -- 测试健康检查
    print("\n--- Health Check Results ---")
    local health = container:check_health()
    print("Overall Status: " .. health.status)
    for component, result in pairs(health.components) do
        print(string.format("  %s: %s", component, result.status))
    end
    
    -- 测试配置热重载
    print("\n--- Testing Config Reload ---")
    container:reload_config()
    
    -- 模拟运行一段时间
    print("\n--- Application Running ---")
    print("Application running... (Press any key to stop)")
    -- io.read() -- 在实际环境中取消注释
    
    -- 停止应用
    print("\n--- Stopping Application ---")
    container:stop_all()
end

-- 运行示例
main()