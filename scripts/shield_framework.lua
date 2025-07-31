-- Shield Lua Framework Base Classes
-- 基于继承的服务管理系统

local Shield = {}

-- =====================================
-- 1. 基础对象类 (所有Shield类的根基类)
-- =====================================
Shield.Object = {}
Shield.Object.__index = Shield.Object

function Shield.Object:new(o)
    o = o or {}
    setmetatable(o, self)
    self.__index = self
    return o
end

function Shield.Object:instanceof(class)
    local mt = getmetatable(self)
    while mt do
        if mt == class then
            return true
        end
        mt = getmetatable(mt)
    end
    return false
end

function Shield.Object:get_class_name()
    return self._class_name or "Unknown"
end

-- =====================================
-- 2. Service基类 (生命周期管理)
-- =====================================
Shield.Service = Shield.Object:new()
Shield.Service._class_name = "Service"
Shield.Service._shield_managed = true
Shield.Service._service_type = "service"

function Shield.Service:new(o)
    o = Shield.Object.new(self, o)
    o._dependencies = o._dependencies or {}
    o._lifecycle_state = "created"
    o._service_name = o._service_name or "UnnamedService"
    return o
end

-- 生命周期钩子 (子类可重写)
function Shield.Service:on_init(container)
    self._lifecycle_state = "initialized"
    print(string.format("[Shield] Service '%s' initialized", self._service_name))
end

function Shield.Service:on_start()
    self._lifecycle_state = "started"
    print(string.format("[Shield] Service '%s' started", self._service_name))
end

function Shield.Service:on_stop()
    self._lifecycle_state = "stopped"
    print(string.format("[Shield] Service '%s' stopped", self._service_name))
end

function Shield.Service:on_config_reloaded()
    print(string.format("[Shield] Service '%s' config reloaded", self._service_name))
end

-- 依赖声明
function Shield.Service:depends_on(...)
    local deps = {...}
    for _, dep in ipairs(deps) do
        table.insert(self._dependencies, dep)
    end
    return self
end

-- 检查是否为Shield管理的服务
function Shield.Service:is_shield_managed()
    return self._shield_managed == true
end

-- =====================================
-- 3. Component基类 (无状态组件)
-- =====================================
Shield.Component = Shield.Object:new()
Shield.Component._class_name = "Component"
Shield.Component._shield_managed = true
Shield.Component._service_type = "component"

function Shield.Component:new(o)
    o = Shield.Object.new(self, o)
    o._dependencies = o._dependencies or {}
    o._component_name = o._component_name or "UnnamedComponent"
    return o
end

function Shield.Component:depends_on(...)
    local deps = {...}
    for _, dep in ipairs(deps) do
        table.insert(self._dependencies, dep)
    end
    return self
end

-- =====================================
-- 4. EventListener基类 (事件处理器)
-- =====================================
Shield.EventListener = Shield.Object:new()
Shield.EventListener._class_name = "EventListener"
Shield.EventListener._shield_managed = true
Shield.EventListener._service_type = "event_listener"

function Shield.EventListener:new(o)
    o = Shield.Object.new(self, o)
    o._event_handlers = o._event_handlers or {}
    o._listener_name = o._listener_name or "UnnamedListener"
    return o
end

-- 注册事件处理方法
function Shield.EventListener:handles(event_type, handler, priority)
    priority = priority or 0
    self._event_handlers[event_type] = {
        handler = handler,
        priority = priority
    }
    return self
end

-- =====================================
-- 5. Configuration基类 (配置类)
-- =====================================
Shield.Configuration = Shield.Object:new()
Shield.Configuration._class_name = "Configuration"
Shield.Configuration._shield_managed = true
Shield.Configuration._service_type = "configuration"

function Shield.Configuration:new(o)
    o = Shield.Object.new(self, o)
    o._config_properties = o._config_properties or {}
    o._config_name = o._config_name or "UnnamedConfig"
    return o
end

function Shield.Configuration:property(key, default_value)
    return self._config_properties[key] or default_value
end

function Shield.Configuration:set_property(key, value)
    self._config_properties[key] = value
end

function Shield.Configuration:load_from_table(config_table)
    for key, value in pairs(config_table) do
        self._config_properties[key] = value
    end
end

-- =====================================
-- 6. Repository基类 (数据访问层)
-- =====================================
Shield.Repository = Shield.Service:new()
Shield.Repository._class_name = "Repository"
Shield.Repository._service_type = "repository"

function Shield.Repository:new(o)
    o = Shield.Service.new(self, o)
    o._entity_type = o._entity_type or "Unknown"
    o._repository_name = o._repository_name or "UnnamedRepository"
    return o
end

-- 标准CRUD方法 (子类应该实现这些)
function Shield.Repository:find_by_id(id)
    error("find_by_id must be implemented by subclass")
end

function Shield.Repository:find_all()
    error("find_all must be implemented by subclass")
end

function Shield.Repository:save(entity)
    error("save must be implemented by subclass")
end

function Shield.Repository:delete(entity)
    error("delete must be implemented by subclass")
end

-- =====================================
-- 7. Controller基类 (控制器层)
-- =====================================
Shield.Controller = Shield.Service:new()
Shield.Controller._class_name = "Controller"
Shield.Controller._service_type = "controller"

function Shield.Controller:new(o)
    o = Shield.Service.new(self, o)
    o._route_mappings = o._route_mappings or {}
    o._controller_name = o._controller_name or "UnnamedController"
    return o
end

-- 路由映射 (类似Spring的@RequestMapping)
function Shield.Controller:map_route(path, method, handler)
    self._route_mappings[path] = {
        method = method,
        handler = handler
    }
    return self
end

-- =====================================
-- 8. HealthIndicator基类 (健康检查)
-- =====================================
Shield.HealthIndicator = Shield.Component:new()
Shield.HealthIndicator._class_name = "HealthIndicator"
Shield.HealthIndicator._service_type = "health_indicator"

function Shield.HealthIndicator:new(o)
    o = Shield.Component.new(self, o)
    o._indicator_name = o._indicator_name or "UnnamedHealthIndicator"
    return o
end

function Shield.HealthIndicator:check()
    return {
        status = "UP",
        details = {}
    }
end

-- =====================================
-- 9. 框架扫描器 (自动发现Shield管理的类)
-- =====================================
Shield.Scanner = {}

-- 扫描对象是否为Shield管理
function Shield.Scanner.is_shield_managed(obj)
    return obj._shield_managed == true
end

-- 获取服务类型
function Shield.Scanner.get_service_type(obj)
    return obj._service_type or "unknown"
end

-- 获取依赖列表
function Shield.Scanner.get_dependencies(obj)
    return obj._dependencies or {}
end

-- 扫描文件中的Shield类
function Shield.Scanner.scan_file(file_path)
    local chunk = loadfile(file_path)
    if not chunk then
        return {}
    end
    
    local env = {}
    setmetatable(env, {__index = _G})
    setfenv(chunk, env)
    
    local result = chunk()
    local shield_objects = {}
    
    -- 如果返回的是Shield对象
    if result and Shield.Scanner.is_shield_managed(result) then
        table.insert(shield_objects, result)
    end
    
    -- 扫描环境中的Shield对象
    for name, obj in pairs(env) do
        if type(obj) == "table" and Shield.Scanner.is_shield_managed(obj) then
            table.insert(shield_objects, obj)
        end
    end
    
    return shield_objects
end

-- 扫描目录中的所有Shield服务
function Shield.Scanner.scan_directory(directory)
    local shield_services = {}
    -- 这里需要实现目录扫描逻辑
    -- 在实际实现中会使用文件系统API
    return shield_services
end

-- =====================================
-- 10. 自动注册器
-- =====================================
Shield.AutoRegister = {}

-- 自动注册Shield服务到容器
function Shield.AutoRegister.register_service(container, service_obj)
    if not Shield.Scanner.is_shield_managed(service_obj) then
        return false
    end
    
    local service_type = Shield.Scanner.get_service_type(service_obj)
    local service_name = service_obj._service_name or service_obj._component_name or "unnamed"
    
    print(string.format("[Shield] Auto-registering %s: %s", service_type, service_name))
    
    -- 根据类型注册到不同的容器中
    if service_type == "service" then
        container:register_service(service_name, service_obj)
    elseif service_type == "component" then
        container:register_component(service_name, service_obj)
    elseif service_type == "event_listener" then
        Shield.AutoRegister.register_event_handlers(container, service_obj)
    elseif service_type == "health_indicator" then
        container:register_health_check(service_name, function(c)
            return service_obj:check()
        end)
    end
    
    return true
end

-- 注册事件处理器
function Shield.AutoRegister.register_event_handlers(container, listener_obj)
    for event_type, handler_info in pairs(listener_obj._event_handlers) do
        container:register_event_listener(event_type, handler_info.handler, handler_info.priority)
    end
end

-- 批量自动注册
function Shield.AutoRegister.register_all_from_directory(container, directory)
    local services = Shield.Scanner.scan_directory(directory)
    local registered_count = 0
    
    for _, service in ipairs(services) do
        if Shield.AutoRegister.register_service(container, service) then
            registered_count = registered_count + 1
        end
    end
    
    print(string.format("[Shield] Auto-registered %d services from %s", registered_count, directory))
    return registered_count
end

-- =====================================
-- 11. 便捷宏/函数
-- =====================================

-- 创建Shield服务的便捷函数
function Shield.service(name)
    local service = Shield.Service:new()
    service._service_name = name
    return service
end

function Shield.component(name)
    local component = Shield.Component:new()
    component._component_name = name
    return component
end

function Shield.event_listener(name)
    local listener = Shield.EventListener:new()
    listener._listener_name = name
    return listener
end

function Shield.health_indicator(name)
    local indicator = Shield.HealthIndicator:new()
    indicator._indicator_name = name
    return indicator
end

function Shield.repository(name, entity_type)
    local repo = Shield.Repository:new()
    repo._repository_name = name
    repo._entity_type = entity_type
    return repo
end

function Shield.controller(name)
    local controller = Shield.Controller:new()
    controller._controller_name = name
    return controller
end

-- 导出Shield框架
return Shield