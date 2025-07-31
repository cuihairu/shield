-- Shield Lua IOC Container
-- 适合Lua动态特性的依赖注入系统

local LuaIoC = {}
LuaIoC.__index = LuaIoC

-- 创建新的IOC容器
function LuaIoC:new()
    local container = {
        _services = {},           -- 已注册的服务
        _instances = {},          -- 单例实例缓存
        _factories = {},          -- 工厂函数
        _initializers = {},       -- 初始化函数
        _lifecycle_callbacks = {}, -- 生命周期回调
        _event_listeners = {},    -- 事件监听器
        _health_checks = {}       -- 健康检查
    }
    setmetatable(container, self)
    return container
end

-- 服务注册 (类似Spring的@Service)
function LuaIoC:register_service(name, service_table, lifetime)
    lifetime = lifetime or "singleton"
    
    self._services[name] = {
        definition = service_table,
        lifetime = lifetime,
        dependencies = service_table.dependencies or {},
        initialized = false
    }
    
    print(string.format("[Shield-Lua] Registered service: %s (%s)", name, lifetime))
    return self
end

-- 工厂函数注册 (类似Spring的@Bean)
function LuaIoC:register_factory(name, factory_func, lifetime)
    lifetime = lifetime or "singleton"
    
    self._factories[name] = {
        factory = factory_func,
        lifetime = lifetime,
        initialized = false
    }
    
    print(string.format("[Shield-Lua] Registered factory: %s (%s)", name, lifetime))
    return self
end

-- 依赖解析和注入
function LuaIoC:resolve(name)
    -- 检查单例缓存
    if self._instances[name] then
        return self._instances[name]
    end
    
    -- 解析工厂函数
    if self._factories[name] then
        return self:_resolve_factory(name)
    end
    
    -- 解析服务
    if self._services[name] then
        return self:_resolve_service(name)
    end
    
    error(string.format("[Shield-Lua] Service not found: %s", name))
end

-- 解析工厂函数
function LuaIoC:_resolve_factory(name)
    local factory_info = self._factories[name]
    local instance = factory_info.factory(self)
    
    if factory_info.lifetime == "singleton" then
        self._instances[name] = instance
    end
    
    return instance
end

-- 解析服务
function LuaIoC:_resolve_service(name)
    local service_info = self._services[name]
    local service_def = service_info.definition
    
    -- 创建服务实例
    local instance = {}
    
    -- 复制服务定义到实例
    for k, v in pairs(service_def) do
        if type(v) == "function" then
            instance[k] = v
        elseif k ~= "dependencies" then
            instance[k] = v
        end
    end
    
    -- 注入依赖
    if service_info.dependencies then
        for dep_name, dep_key in pairs(service_info.dependencies) do
            instance[dep_key] = self:resolve(dep_name)
        end
    end
    
    -- 调用初始化方法
    if instance.on_init then
        instance:on_init(self)  -- 传入容器引用
        service_info.initialized = true
    end
    
    -- 缓存单例
    if service_info.lifetime == "singleton" then
        self._instances[name] = instance
    end
    
    return instance
end

-- 条件化注册 (类似Spring的@ConditionalOnProperty)
function LuaIoC:register_conditional(name, service_table, condition)
    if self:_evaluate_condition(condition) then
        self:register_service(name, service_table)
        print(string.format("[Shield-Lua] Conditional service registered: %s", name))
    else
        print(string.format("[Shield-Lua] Condition not met, skipping: %s", name))
    end
    return self
end

-- 条件评估
function LuaIoC:_evaluate_condition(condition)
    if type(condition) == "function" then
        return condition(self)
    elseif type(condition) == "table" then
        if condition.property then
            -- 检查配置属性
            local config = self:resolve("config")
            return config and config:get(condition.property) == (condition.value or true)
        elseif condition.missing_service then
            -- 检查服务不存在
            return not self._services[condition.missing_service]
        end
    end
    return true
end

-- 事件系统集成
function LuaIoC:register_event_listener(event_type, listener_func, priority)
    priority = priority or 0
    
    if not self._event_listeners[event_type] then
        self._event_listeners[event_type] = {}
    end
    
    table.insert(self._event_listeners[event_type], {
        listener = listener_func,
        priority = priority
    })
    
    -- 按优先级排序
    table.sort(self._event_listeners[event_type], function(a, b)
        return a.priority > b.priority
    end)
    
    print(string.format("[Shield-Lua] Event listener registered: %s", event_type))
    return self
end

-- 发布事件
function LuaIoC:publish_event(event_type, event_data)
    local listeners = self._event_listeners[event_type]
    if not listeners then return end
    
    print(string.format("[Shield-Lua] Publishing event: %s", event_type))
    
    for _, listener_info in ipairs(listeners) do
        local success, err = pcall(listener_info.listener, event_data, self)
        if not success then
            print(string.format("[Shield-Lua] Event listener error: %s", err))
        end
    end
end

-- 健康检查注册
function LuaIoC:register_health_check(name, check_func)
    self._health_checks[name] = check_func
    print(string.format("[Shield-Lua] Health check registered: %s", name))
    return self
end

-- 执行健康检查
function LuaIoC:check_health()
    local results = {}
    local overall_status = "UP"
    
    for name, check_func in pairs(self._health_checks) do
        local success, result = pcall(check_func, self)
        if success then
            results[name] = result
            if result.status ~= "UP" then
                overall_status = "DOWN"
            end
        else
            results[name] = {
                status = "DOWN",
                error = result
            }
            overall_status = "DOWN"
        end
    end
    
    return {
        status = overall_status,
        components = results
    }
end

-- 生命周期管理
function LuaIoC:start_all()
    print("[Shield-Lua] Starting all services...")
    
    -- 发布启动前事件
    self:publish_event("application_starting", {})
    
    -- 启动所有服务
    for name, instance in pairs(self._instances) do
        if instance.on_start then
            local success, err = pcall(instance.on_start, instance)
            if not success then
                print(string.format("[Shield-Lua] Failed to start %s: %s", name, err))
            end
        end
    end
    
    -- 发布启动完成事件
    self:publish_event("application_started", {})
    print("[Shield-Lua] All services started")
end

function LuaIoC:stop_all()
    print("[Shield-Lua] Stopping all services...")
    
    self:publish_event("application_stopping", {})
    
    -- 反向停止服务
    local service_names = {}
    for name in pairs(self._instances) do
        table.insert(service_names, name)
    end
    
    for i = #service_names, 1, -1 do
        local name = service_names[i]
        local instance = self._instances[name]
        if instance.on_stop then
            local success, err = pcall(instance.on_stop, instance)
            if not success then
                print(string.format("[Shield-Lua] Failed to stop %s: %s", name, err))
            end
        end
    end
    
    self:publish_event("application_stopped", {})
    print("[Shield-Lua] All services stopped")
end

-- 配置热重载支持
function LuaIoC:reload_config()
    print("[Shield-Lua] Reloading configuration...")
    
    -- 重新加载配置
    local config = self:resolve("config")
    if config and config.reload then
        config:reload()
    end
    
    -- 通知可重载的服务
    for name, instance in pairs(self._instances) do
        if instance.on_config_reloaded then
            local success, err = pcall(instance.on_config_reloaded, instance)
            if not success then
                print(string.format("[Shield-Lua] Config reload failed for %s: %s", name, err))
            end
        end
    end
    
    self:publish_event("config_reloaded", {})
end

-- 调试信息
function LuaIoC:debug_info()
    print("\n=== Shield Lua IoC Container Debug Info ===")
    print(string.format("Services: %d", self:_count_table(self._services)))
    print(string.format("Factories: %d", self:_count_table(self._factories)))
    print(string.format("Instances: %d", self:_count_table(self._instances)))
    print(string.format("Event Listeners: %d", self:_count_table(self._event_listeners)))
    print(string.format("Health Checks: %d", self:_count_table(self._health_checks)))
    print("==========================================\n")
end

function LuaIoC:_count_table(t)
    local count = 0
    for _ in pairs(t) do count = count + 1 end
    return count
end

-- 全局容器实例
local global_container = LuaIoC:new()

-- 便捷的全局函数
function shield_service(name, service_def, lifetime)
    return global_container:register_service(name, service_def, lifetime)
end

function shield_factory(name, factory_func, lifetime)
    return global_container:register_factory(name, factory_func, lifetime)
end

function shield_conditional(name, service_def, condition)
    return global_container:register_conditional(name, service_def, condition)
end

function shield_resolve(name)
    return global_container:resolve(name)
end

function shield_event_listener(event_type, listener_func, priority)
    return global_container:register_event_listener(event_type, listener_func, priority)
end

function shield_publish_event(event_type, event_data)
    return global_container:publish_event(event_type, event_data)
end

function shield_health_check(name, check_func)
    return global_container:register_health_check(name, check_func)
end

-- 导出模块
return {
    LuaIoC = LuaIoC,
    container = global_container,
    service = shield_service,
    factory = shield_factory,
    conditional = shield_conditional,
    resolve = shield_resolve,
    event_listener = shield_event_listener,
    publish_event = shield_publish_event,
    health_check = shield_health_check
}