-- Shield继承驱动服务示例
-- 只有继承Shield基类的才会被框架自动管理

local Shield = require("shield_framework")

-- =====================================
-- 1. Service示例 - 自动被Shield管理
-- =====================================

-- 玩家服务 (继承Shield.Service)
local PlayerService = Shield.Service:new({
    _service_name = "PlayerService"
})

-- 声明依赖 (链式调用)
PlayerService:depends_on("DatabaseService", "CacheService")

-- 重写生命周期方法
function PlayerService:on_init(container)
    Shield.Service.on_init(self, container)  -- 调用父类方法
    
    -- 依赖注入已自动完成
    self.database = container:resolve("DatabaseService")
    self.cache = container:resolve("CacheService")
    
    self.online_players = {}
    print("[PlayerService] Service initialized with dependencies")
end

function PlayerService:on_start()
    Shield.Service.on_start(self, container)
    print("[PlayerService] Service started, ready to handle players")
end

-- 业务方法
function PlayerService:handle_player_login(player_id, connection_info)
    print(string.format("[PlayerService] Player %d logging in", player_id))
    
    -- 使用注入的依赖
    local player_data = self.database:load_player(player_id)
    self.online_players[player_id] = player_data
    
    -- 缓存玩家数据
    self.cache:set("player_" .. player_id, player_data, 3600)
    
    return player_data
end

function PlayerService:get_online_player_count()
    local count = 0
    for _ in pairs(self.online_players) do count = count + 1 end
    return count
end

-- =====================================
-- 2. Repository示例 - 数据访问层
-- =====================================

-- 玩家仓储 (继承Shield.Repository)  
local PlayerRepository = Shield.Repository:new({
    _repository_name = "PlayerRepository",
    _entity_type = "Player"
})

PlayerRepository:depends_on("DatabaseService")

function PlayerRepository:on_init(container)
    Shield.Repository.on_init(self, container)
    self.database = container:resolve("DatabaseService")
end

-- 实现父类定义的抽象方法
function PlayerRepository:find_by_id(player_id)
    return self.database:query("SELECT * FROM players WHERE id = ?", player_id)
end

function PlayerRepository:find_all()
    return self.database:query("SELECT * FROM players")
end

function PlayerRepository:save(player)
    if player.id then
        return self.database:update("players", player)
    else
        return self.database:insert("players", player)
    end
end

function PlayerRepository:delete(player)
    return self.database:delete("players", {id = player.id})
end

-- 自定义查询方法
function PlayerRepository:find_online_players()
    return self.database:query("SELECT * FROM players WHERE status = 'online'")
end

-- =====================================
-- 3. EventListener示例 - 事件处理
-- =====================================

-- 玩家事件监听器 (继承Shield.EventListener)
local PlayerEventListener = Shield.EventListener:new({
    _listener_name = "PlayerEventListener"
})

PlayerEventListener:depends_on("PlayerService", "LogService")

function PlayerEventListener:on_init(container)
    self.player_service = container:resolve("PlayerService")
    self.log_service = container:resolve("LogService")
    
    -- 注册事件处理器 (链式调用)
    self:handles("player_login", function(event_data)
        self:handle_player_login(event_data)
    end, 100)  -- 高优先级
    
    self:handles("player_logout", function(event_data)
        self:handle_player_logout(event_data)
    end, 50)
    
    self:handles("player_level_up", function(event_data)
        self:handle_player_level_up(event_data)
    end)
end

function PlayerEventListener:handle_player_login(event_data)
    local player_id = event_data.player_id
    print(string.format("[PlayerEventListener] Handling login for player %d", player_id))
    
    -- 记录登录日志
    self.log_service:info(string.format("Player %d logged in", player_id))
    
    -- 更新在线状态
    self.player_service:set_player_online(player_id, true)
end

function PlayerEventListener:handle_player_logout(event_data)
    local player_id = event_data.player_id
    print(string.format("[PlayerEventListener] Handling logout for player %d", player_id))
    
    self.log_service:info(string.format("Player %d logged out", player_id))
    self.player_service:set_player_online(player_id, false)
end

function PlayerEventListener:handle_player_level_up(event_data)
    local player_id = event_data.player_id
    local new_level = event_data.level
    
    print(string.format("[PlayerEventListener] Player %d reached level %d", player_id, new_level))
    
    -- 发送升级奖励
    self.player_service:send_level_up_rewards(player_id, new_level)
end

-- =====================================
-- 4. HealthIndicator示例 - 健康检查
-- =====================================

-- 玩家服务健康检查 (继承Shield.HealthIndicator)
local PlayerHealthIndicator = Shield.HealthIndicator:new({
    _indicator_name = "PlayerServiceHealth"
})

PlayerHealthIndicator:depends_on("PlayerService")

function PlayerHealthIndicator:on_init(container)
    self.player_service = container:resolve("PlayerService")
end

function PlayerHealthIndicator:check()
    local online_count = self.player_service:get_online_player_count()
    local status = "UP"
    local details = {
        online_players = tostring(online_count),
        max_players = "1000"
    }
    
    -- 检查玩家数量是否超限
    if online_count > 1000 then
        status = "DOWN"
        details.error = "Too many online players"
    end
    
    return {
        status = status,
        details = details
    }
end

-- =====================================
-- 5. Controller示例 - 控制器层
-- =====================================

-- 玩家控制器 (继承Shield.Controller)
local PlayerController = Shield.Controller:new({
    _controller_name = "PlayerController"
})

PlayerController:depends_on("PlayerService", "PlayerRepository")

function PlayerController:on_init(container)
    Shield.Controller.on_init(self, container)
    
    self.player_service = container:resolve("PlayerService")
    self.player_repository = container:resolve("PlayerRepository")
    
    -- 注册路由映射
    self:map_route("/api/player/:id", "GET", function(params)
        return self:get_player(params.id)
    end)
    
    self:map_route("/api/player", "POST", function(params, body)
        return self:create_player(body)
    end)
    
    self:map_route("/api/players/online", "GET", function(params)
        return self:get_online_players()
    end)
end

function PlayerController:get_player(player_id)
    local player = self.player_repository:find_by_id(tonumber(player_id))
    if player then
        return {status = 200, data = player}
    else
        return {status = 404, error = "Player not found"}
    end
end

function PlayerController:create_player(player_data)
    local player = self.player_repository:save(player_data)
    return {status = 201, data = player}
end

function PlayerController:get_online_players()
    local online_players = self.player_repository:find_online_players()
    return {status = 200, data = online_players}
end

-- =====================================
-- 6. Configuration示例 - 配置类
-- =====================================

-- 游戏配置 (继承Shield.Configuration)
local GameConfiguration = Shield.Configuration:new({
    _config_name = "GameConfiguration"
})

function GameConfiguration:on_init(container)
    -- 加载默认配置
    self:load_from_table({
        max_players = 1000,
        level_cap = 100,
        xp_multiplier = 1.0,
        pvp_enabled = true,
        maintenance_mode = false
    })
    
    print("[GameConfiguration] Game configuration loaded")
end

function GameConfiguration:get_max_players()
    return self:property("max_players", 1000)
end

function GameConfiguration:is_pvp_enabled()
    return self:property("pvp_enabled", true)
end

function GameConfiguration:is_maintenance_mode()
    return self:property("maintenance_mode", false)
end

-- =====================================
-- 7. 普通Lua类 - 不被Shield管理
-- =====================================

-- 这个类不继承Shield基类，所以不会被自动管理
local RegularLuaClass = {
    name = "RegularClass"
}

function RegularLuaClass:new(o)
    o = o or {}
    setmetatable(o, self)
    self.__index = self
    return o
end

function RegularLuaClass:do_something()
    print("This is a regular Lua class, not managed by Shield")
end

-- =====================================
-- 8. 自动扫描和注册示例
-- =====================================

local function demonstrate_shield_management()
    print("=== Shield继承驱动管理演示 ===\n")
    
    -- 创建Shield IoC容器
    local shield_container = require("lua_ioc_container").LuaIoC:new()
    
    -- 创建服务实例
    local services = {
        PlayerService,
        PlayerRepository, 
        PlayerEventListener,
        PlayerHealthIndicator,
        PlayerController,
        GameConfiguration,
        RegularLuaClass:new()  -- 普通类，不会被管理
    }
    
    print("--- 扫描和注册Shield服务 ---")
    for _, service in ipairs(services) do
        if Shield.Scanner.is_shield_managed(service) then
            local service_type = Shield.Scanner.get_service_type(service)
            local service_name = service._service_name or service._component_name or "unnamed"
            
            print(string.format("✅ Shield管理: %s (%s)", service_name, service_type))
            
            -- 自动注册到容器
            Shield.AutoRegister.register_service(shield_container, service)
        else
            print("❌ 非Shield类，跳过: " .. (service.name or "unknown"))
        end
    end
    
    print("\n--- 服务依赖关系 ---")
    for _, service in ipairs(services) do
        if Shield.Scanner.is_shield_managed(service) then
            local deps = Shield.Scanner.get_dependencies(service)
            local service_name = service._service_name or service._component_name or "unnamed"
            
            if #deps > 0 then
                print(string.format("%s 依赖: %s", service_name, table.concat(deps, ", ")))
            else
                print(string.format("%s 无依赖", service_name))
            end
        end
    end
    
    print("\n--- 类型检查演示 ---")
    print("PlayerService是Service吗？", PlayerService:instanceof(Shield.Service))
    print("PlayerRepository是Repository吗？", PlayerRepository:instanceof(Shield.Repository))
    print("PlayerRepository是Service吗？", PlayerRepository:instanceof(Shield.Service))  -- 应该是true，因为Repository继承自Service
    print("RegularLuaClass是Shield管理的吗？", Shield.Scanner.is_shield_managed(RegularLuaClass:new()))
end

-- 运行演示
demonstrate_shield_management()

-- 导出服务供其他模块使用
return {
    PlayerService = PlayerService,
    PlayerRepository = PlayerRepository,
    PlayerEventListener = PlayerEventListener,
    PlayerHealthIndicator = PlayerHealthIndicator,
    PlayerController = PlayerController,
    GameConfiguration = GameConfiguration
}