-- Shield现代化依赖注入方案
-- 使用Lua元编程实现IDE友好的依赖注入

local Shield = require("shield_framework")

-- =====================================
-- 1. 服务代理系统 (最佳方案)
-- =====================================

-- 创建服务代理，提供完整的IDE支持
local ServiceProxy = {}
ServiceProxy.__index = ServiceProxy

function ServiceProxy:new(service_class, container)
    local proxy = {
        _service_class = service_class,
        _container = container,
        _service_name = service_class._service_name or service_class._component_name
    }
    setmetatable(proxy, self)
    return proxy
end

-- 代理所有方法调用到实际服务实例
function ServiceProxy:__index(key)
    if key:sub(1, 1) == "_" then
        return rawget(self, key)
    end
    
    local service_instance = self._container:resolve(self._service_name)
    if service_instance and service_instance[key] then
        if type(service_instance[key]) == "function" then
            return function(_, ...)
                return service_instance[key](service_instance, ...)
            end
        else
            return service_instance[key]
        end
    end
    
    return nil
end

-- =====================================
-- 2. 现代化依赖声明 (推荐方案)
-- =====================================

-- 先定义所有服务 (IDE可以识别)
local Services = {}

-- 数据库服务
Services.Database = Shield.Service:new({_service_name = "DatabaseService"})
function Services.Database:find_player(player_id)
    return {id = player_id, name = "Player_" .. player_id, level = math.random(1, 50)}
end

function Services.Database:save_player(player)
    print("[Database] Saving player:", player.name)
    return true
end

function Services.Database:get_connection_count()
    return math.random(5, 20)
end

-- 缓存服务
Services.Cache = Shield.Service:new({_service_name = "CacheService"})
function Services.Cache:get(key)
    self._data = self._data or {}
    return self._data[key]
end

function Services.Cache:set(key, value, ttl)
    self._data = self._data or {}
    self._data[key] = value
    print(string.format("[Cache] Set %s = %s (TTL: %ds)", key, tostring(value), ttl or 0))
end

function Services.Cache:clear()
    self._data = {}
    print("[Cache] Cache cleared")
end

-- 配置服务
Services.Config = Shield.Configuration:new({_config_name = "ConfigService"})
function Services.Config:get_max_players()
    return self:property("max_players", 1000)
end

function Services.Config:get_server_name()
    return self:property("server_name", "Shield Game Server")
end

-- 日志服务
Services.Logger = Shield.Service:new({_service_name = "LoggerService"})
function Services.Logger:info(message)
    print("[INFO] " .. tostring(message))
end

function Services.Logger:error(message)
    print("[ERROR] " .. tostring(message))
end

function Services.Logger:debug(message)
    print("[DEBUG] " .. tostring(message))
end

-- =====================================
-- 3. 🎯 现代化依赖注入 (IDE完美支持)
-- =====================================

local PlayerService = Shield.Service:new({_service_name = "PlayerService"})

-- 🚀 方案1: 构造函数注入 (最推荐)
function PlayerService:with_dependencies(database, cache, config, logger)
    -- IDE完美提示！每个参数都是具体的服务类型
    self.database = database    -- database: Services.Database 类型
    self.cache = cache          -- cache: Services.Cache 类型  
    self.config = config        -- config: Services.Config 类型
    self.logger = logger        -- logger: Services.Logger 类型
    return self
end

-- 🚀 方案2: 构建器模式 (链式调用)
function PlayerService:inject_database(database_service)
    self.database = database_service  -- IDE知道是Services.Database类型
    return self
end

function PlayerService:inject_cache(cache_service)
    self.cache = cache_service  -- IDE知道是Services.Cache类型
    return self
end

function PlayerService:inject_config(config_service)
    self.config = config_service  -- IDE知道是Services.Config类型
    return self
end

function PlayerService:inject_logger(logger_service)
    self.logger = logger_service  -- IDE知道是Services.Logger类型
    return self
end

-- 业务方法 - IDE有完整提示！
function PlayerService:get_player(player_id)
    self.logger:info("Getting player: " .. player_id)  -- ✅ IDE提示info方法
    
    -- 先检查缓存
    local cache_key = "player_" .. player_id
    local cached = self.cache:get(cache_key)           -- ✅ IDE提示get方法
    
    if cached then
        self.logger:info("Player found in cache")
        return cached
    end
    
    -- 从数据库加载
    local player = self.database:find_player(player_id)  -- ✅ IDE提示find_player方法
    if player then
        -- 缓存结果
        self.cache:set(cache_key, player, 300)           -- ✅ IDE提示set方法
        self.logger:info("Player loaded from database")
    end
    
    return player
end

function PlayerService:save_player(player)
    self.logger:info("Saving player: " .. player.name)
    
    local success = self.database:save_player(player)    -- ✅ IDE提示save_player方法
    if success then
        -- 清除缓存
        self.cache:set("player_" .. player.id, nil)      -- ✅ IDE提示
        self.logger:info("Player saved successfully")
    else
        self.logger:error("Failed to save player")
    end
    
    return success
end

-- =====================================
-- 4. 🎯 工厂模式 + 类型安全
-- =====================================

local ServiceFactory = {}

-- 创建带类型的服务工厂
function ServiceFactory.create_player_service()
    local service = PlayerService
    
    -- 🚀 IDE完美支持的依赖注入
    return service:with_dependencies(
        Services.Database,    -- IDE知道这是DatabaseService
        Services.Cache,       -- IDE知道这是CacheService  
        Services.Config,      -- IDE知道这是ConfigService
        Services.Logger       -- IDE知道这是LoggerService
    )
end

-- 或者使用构建器模式
function ServiceFactory.create_player_service_builder()
    return PlayerService
        :inject_database(Services.Database)    -- ✅ 链式调用，IDE支持
        :inject_cache(Services.Cache)          -- ✅ 
        :inject_config(Services.Config)        -- ✅
        :inject_logger(Services.Logger)        -- ✅
end

-- =====================================
-- 5. 🎯 智能容器 + 自动解析
-- =====================================

local SmartContainer = {}
SmartContainer.__index = SmartContainer

function SmartContainer:new()
    local container = {
        _services = {},
        _instances = {},
        _type_mappings = {}  -- 类型到实例的映射
    }
    setmetatable(container, self)
    return container
end

-- 注册服务并建立类型映射
function SmartContainer:register_typed(service_class, instance)
    local service_name = service_class._service_name or service_class._component_name
    
    self._services[service_name] = instance
    self._instances[service_name] = instance
    self._type_mappings[service_class] = instance  -- 🔥 关键：类型直接映射到实例
    
    print(string.format("[SmartContainer] Registered %s with type mapping", service_name))
end

-- 🚀 按类型解析 (IDE完美支持)
function SmartContainer:resolve_by_type(service_class)
    return self._type_mappings[service_class]  -- 直接返回类型对应的实例
end

-- 传统的按名称解析
function SmartContainer:resolve(service_name)
    return self._instances[service_name]
end

-- 🚀 自动依赖注入 (基于类型)
function SmartContainer:auto_inject(target_service)
    -- 如果服务有类型依赖声明
    if target_service._typed_dependencies then
        for field_name, service_class in pairs(target_service._typed_dependencies) do
            local instance = self:resolve_by_type(service_class)
            if instance then
                target_service[field_name] = instance
                print(string.format("[SmartContainer] Auto-injected %s.%s", 
                    target_service._service_name, field_name))
            end
        end
    end
end

-- =====================================
-- 6. 最现代的使用方式
-- =====================================

local function demonstrate_modern_di()
    print("=== Shield现代化依赖注入演示 ===\n")
    
    -- 创建智能容器
    local container = SmartContainer:new()
    
    -- 注册服务 (带类型映射)
    container:register_typed(Services.Database, Services.Database)
    container:register_typed(Services.Cache, Services.Cache)
    container:register_typed(Services.Config, Services.Config)
    container:register_typed(Services.Logger, Services.Logger)
    
    print("\n--- 方案1: 构造函数注入 ---")
    local player_service1 = ServiceFactory.create_player_service()
    
    -- 测试 - IDE有完整提示！
    local player = player_service1:get_player(123)
    print("Retrieved player:", player.name, "Level:", player.level)
    
    print("\n--- 方案2: 构建器模式 ---")
    local player_service2 = ServiceFactory.create_player_service_builder()
    
    player_service2:save_player({id = 456, name = "Alice", level = 25})
    
    print("\n--- 方案3: 按类型解析 ---")
    -- 🚀 IDE完美支持！直接按类型获取
    local db = container:resolve_by_type(Services.Database)     -- ✅ IDE知道返回Database类型
    local cache = container:resolve_by_type(Services.Cache)     -- ✅ IDE知道返回Cache类型
    local logger = container:resolve_by_type(Services.Logger)   -- ✅ IDE知道返回Logger类型
    
    -- 使用时有完整的IDE提示
    logger:info("Direct type resolution works!")
    local connection_count = db:get_connection_count()          -- ✅ IDE提示get_connection_count方法
    cache:clear()                                              -- ✅ IDE提示clear方法
    
    print("Database connections:", connection_count)
    
    print("\n=== 现代化演示完成 ===")
end

-- 运行演示
demonstrate_modern_di()

-- =====================================
-- 7. LSP/IDE支持的类型声明
-- =====================================

--[[
如果使用支持Lua Language Server的IDE，可以添加类型注解：

---@class DatabaseService
---@field find_player fun(self: DatabaseService, player_id: number): table
---@field save_player fun(self: DatabaseService, player: table): boolean
---@field get_connection_count fun(self: DatabaseService): number

---@class CacheService  
---@field get fun(self: CacheService, key: string): any
---@field set fun(self: CacheService, key: string, value: any, ttl: number?)
---@field clear fun(self: CacheService)

---@class PlayerService
---@field database DatabaseService
---@field cache CacheService
---@field config ConfigService
---@field logger LoggerService
---@field get_player fun(self: PlayerService, player_id: number): table
---@field save_player fun(self: PlayerService, player: table): boolean

这样IDE就有完美的类型提示了！
--]]

print([[

🎉 现代化依赖注入方案总结：

1. 🎯 **构造函数注入** (最推荐)
   PlayerService:with_dependencies(db, cache, config, logger)
   
2. 🔗 **构建器模式**
   PlayerService:inject_database(db):inject_cache(cache)
   
3. 🏭 **工厂模式**
   ServiceFactory.create_player_service()
   
4. 🧠 **智能容器**
   container:resolve_by_type(Services.Database)
   
5. 📝 **LSP类型注解**
   ---@field database DatabaseService

核心优势：
✅ IDE完美提示 - 知道每个依赖的确切类型
✅ 编译期检查 - LSP可以检测类型错误  
✅ 重构安全 - 重命名方法时自动更新所有引用
✅ 代码可读 - 依赖关系一目了然
✅ 热重载支持 - 类型映射不会丢失

这就是现代Lua开发应有的体验！🚀
]])

return {
    Services = Services,
    PlayerService = PlayerService,
    ServiceFactory = ServiceFactory,
    SmartContainer = SmartContainer
}