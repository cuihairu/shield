-- Shield强类型依赖注入方案
-- 解决IDE提示问题，提供完整的类型安全

local Shield = require("shield_framework")

-- =====================================
-- 1. 强类型依赖声明
-- =====================================

-- 先声明服务类 (获得类型引用)
local DatabaseService = Shield.Service:new({_service_name = "DatabaseService"})
local CacheService = Shield.Service:new({_service_name = "CacheService"})
local ConfigService = Shield.Configuration:new({_config_name = "ConfigService"})

-- 业务方法
function DatabaseService:find_player(player_id)
    return {id = player_id, name = "Player_" .. player_id}
end

function CacheService:get(key)
    return self._cache_data[key]
end

function CacheService:set(key, value, ttl)
    self._cache_data[key] = value
end

-- =====================================
-- 2. 强类型依赖注入 (方案A: 直接类型引用)
-- =====================================

local PlayerService = Shield.Service:new({_service_name = "PlayerService"})

-- 🎯 方案A: 直接引用类型，IDE可以完美提示
PlayerService:inject(DatabaseService)  -- IDE知道这是DatabaseService类型
PlayerService:inject(CacheService)     -- IDE知道这是CacheService类型
PlayerService:inject(ConfigService)    -- IDE知道这是ConfigService类型

function PlayerService:on_init(container)
    -- 自动注入，IDE有完整提示！
    -- self.DatabaseService -> IDE提示DatabaseService的所有方法
    -- self.CacheService -> IDE提示CacheService的所有方法
    -- self.ConfigService -> IDE提示ConfigService的所有方法
    print("[PlayerService] Initialized with typed dependencies")
end

function PlayerService:get_player(player_id)
    -- IDE完美提示！
    local cached = self.CacheService:get("player_" .. player_id)  -- ✅ IDE提示get方法
    if cached then return cached end
    
    local player = self.DatabaseService:find_player(player_id)    -- ✅ IDE提示find_player方法
    self.CacheService:set("player_" .. player_id, player, 300)   -- ✅ IDE提示set方法
    
    return player
end

-- =====================================
-- 3. 自定义注入名称 (方案B: 映射注入)
-- =====================================

local GameLogicService = Shield.Service:new({_service_name = "GameLogicService"})

-- 🎯 方案B: 自定义注入名称，更灵活
GameLogicService:inject_as("db", DatabaseService)      -- 注入为self.db
GameLogicService:inject_as("cache", CacheService)      -- 注入为self.cache
GameLogicService:inject_as("config", ConfigService)    -- 注入为self.config

function GameLogicService:on_init(container)
    -- IDE知道类型！
    -- self.db -> DatabaseService类型，有完整方法提示
    -- self.cache -> CacheService类型，有完整方法提示
    -- self.config -> ConfigService类型，有完整方法提示
    print("[GameLogicService] Initialized with mapped dependencies")
end

function GameLogicService:process_player_action(player_id, action)
    local player = self.db:find_player(player_id)        -- ✅ IDE提示
    local cached_result = self.cache:get("action_" .. action)  -- ✅ IDE提示
    
    -- 游戏逻辑...
    return {success = true, player = player}
end

-- =====================================
-- 4. 接口约束 (方案C: 接口依赖)
-- =====================================

-- 定义接口 (Lua中用表模拟)
local IUserRepository = {
    find_by_id = function(self, id) end,
    save = function(self, user) end,
    delete = function(self, user) end
}

local INotificationService = {
    send_notification = function(self, user_id, message) end,
    send_email = function(self, email, subject, body) end
}

-- 实现接口
local UserRepository = Shield.Repository:new({_repository_name = "UserRepository"})
-- 实现IUserRepository接口
function UserRepository:find_by_id(id)
    return {id = id, name = "User" .. id}
end
function UserRepository:save(user) return true end
function UserRepository:delete(user) return true end

local EmailService = Shield.Service:new({_service_name = "EmailService"})
-- 实现INotificationService接口  
function EmailService:send_notification(user_id, message)
    print(string.format("Notification to %d: %s", user_id, message))
end
function EmailService:send_email(email, subject, body)
    print(string.format("Email to %s: %s", email, subject))
end

-- 🎯 方案C: 基于接口的依赖注入
local UserService = Shield.Service:new({_service_name = "UserService"})

-- 注入接口类型，IDE根据接口提示
UserService:inject_interface("userRepo", IUserRepository, UserRepository)
UserService:inject_interface("notifier", INotificationService, EmailService)

function UserService:on_init(container)
    -- self.userRepo -> 有IUserRepository接口的所有方法提示
    -- self.notifier -> 有INotificationService接口的所有方法提示
    print("[UserService] Initialized with interface dependencies")
end

function UserService:create_user(user_data)
    local user = self.userRepo:save(user_data)           -- ✅ IDE根据接口提示
    self.notifier:send_notification(user.id, "Welcome!") -- ✅ IDE根据接口提示
    return user
end

-- =====================================
-- 5. 类型注解 (方案D: 注释注解)
-- =====================================

local OrderService = Shield.Service:new({_service_name = "OrderService"})

-- 🎯 方案D: 使用Lua注释进行类型注解 (LSP可识别)
---@field userRepo UserRepository 用户仓储
---@field paymentService PaymentService 支付服务
---@field notifier EmailService 通知服务

-- 声明注入 (带类型注解)
OrderService:inject_typed(UserRepository, "userRepo")      -- 注入为userRepo，类型为UserRepository
OrderService:inject_typed(EmailService, "notifier")       -- 注入为notifier，类型为EmailService

function OrderService:on_init(container)
    -- IDE通过注解知道确切类型
    print("[OrderService] Initialized with annotated dependencies")
end

---@param order_data table 订单数据
---@return table 创建的订单
function OrderService:create_order(order_data)
    local user = self.userRepo:find_by_id(order_data.user_id)  -- ✅ IDE完美提示
    
    -- 创建订单逻辑...
    local order = {
        id = math.random(1000, 9999),
        user_id = user.id,
        amount = order_data.amount,
        status = "created"
    }
    
    -- 发送通知
    self.notifier:send_notification(user.id, "Order created: " .. order.id)  -- ✅ IDE完美提示
    
    return order
end

-- =====================================
-- 6. 自动类型推导框架扩展
-- =====================================

-- 扩展Shield框架，支持强类型注入
function Shield.Service:inject(service_class)
    -- 自动推导服务名称和类型
    local service_name = service_class._service_name or service_class._component_name
    local field_name = service_class._service_name  -- 使用服务名作为字段名
    
    self._typed_dependencies = self._typed_dependencies or {}
    self._typed_dependencies[field_name] = {
        service_class = service_class,
        service_name = service_name,
        field_name = field_name
    }
    
    -- 传统依赖声明 (用于容器解析)
    table.insert(self._dependencies or {}, service_name)
    
    return self
end

function Shield.Service:inject_as(field_name, service_class)
    local service_name = service_class._service_name or service_class._component_name
    
    self._typed_dependencies = self._typed_dependencies or {}
    self._typed_dependencies[field_name] = {
        service_class = service_class,
        service_name = service_name,
        field_name = field_name
    }
    
    table.insert(self._dependencies or {}, service_name)
    
    return self
end

function Shield.Service:inject_interface(field_name, interface, implementation)
    local service_name = implementation._service_name or implementation._component_name
    
    self._typed_dependencies = self._typed_dependencies or {}
    self._typed_dependencies[field_name] = {
        service_class = implementation,
        interface_class = interface,
        service_name = service_name,
        field_name = field_name
    }
    
    table.insert(self._dependencies or {}, service_name)
    
    return self
end

function Shield.Service:inject_typed(service_class, field_name)
    return self:inject_as(field_name, service_class)
end

-- =====================================
-- 7. 强类型容器解析
-- =====================================

-- 扩展容器，支持强类型注入
function Shield.Service:resolve_typed_dependencies(container)
    if not self._typed_dependencies then return end
    
    for field_name, dep_info in pairs(self._typed_dependencies) do
        local service_instance = container:resolve(dep_info.service_name)
        if service_instance then
            self[field_name] = service_instance
            print(string.format("[Shield] Injected %s as %s.%s", 
                dep_info.service_name, self._service_name, field_name))
        end
    end
end

-- 重写on_init，自动进行强类型注入
local original_on_init = Shield.Service.on_init
function Shield.Service:on_init(container)
    -- 先进行强类型依赖注入
    self:resolve_typed_dependencies(container)
    
    -- 调用原始on_init
    if original_on_init then
        original_on_init(self, container)
    end
end

-- =====================================
-- 8. 演示强类型依赖注入的效果
-- =====================================

local function demonstrate_typed_injection()
    print("=== Shield强类型依赖注入演示 ===\n")
    
    local container = require("lua_ioc_container").LuaIoC:new()
    
    -- 注册服务
    container:register_service("DatabaseService", DatabaseService)
    container:register_service("CacheService", CacheService)
    container:register_service("ConfigService", ConfigService)
    container:register_service("PlayerService", PlayerService)
    container:register_service("GameLogicService", GameLogicService)
    container:register_service("UserRepository", UserRepository)
    container:register_service("EmailService", EmailService)
    container:register_service("UserService", UserService)
    
    -- 初始化服务 (触发强类型注入)
    local services = {PlayerService, GameLogicService, UserService}
    
    for _, service in ipairs(services) do
        service:on_init(container)
    end
    
    print("\n--- 测试强类型依赖 ---")
    
    -- 测试PlayerService
    local player = PlayerService:get_player(123)
    print("Player:", player.name)
    
    -- 测试GameLogicService  
    local result = GameLogicService:process_player_action(123, "attack")
    print("Action result:", result.success)
    
    -- 测试UserService
    local user = UserService:create_user({name = "Alice", email = "alice@example.com"})
    print("Created user:", user.name)
    
    print("\n=== 演示完成 ===")
end

-- 运行演示
demonstrate_typed_injection()

return {
    DatabaseService = DatabaseService,
    CacheService = CacheService,
    PlayerService = PlayerService,
    GameLogicService = GameLogicService,
    UserService = UserService
}