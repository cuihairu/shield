-- Shield自动扫描和管理系统
-- 基于继承自动发现和注册Shield服务

local Shield = require("shield_framework")
local lfs = require("lfs")  -- Lua文件系统库

-- =====================================
-- Shield自动管理器
-- =====================================
local ShieldAutoManager = {}

-- 服务注册表
ShieldAutoManager.registered_services = {}
ShieldAutoManager.service_instances = {}
ShieldAutoManager.dependency_graph = {}

-- =====================================
-- 1. 文件系统扫描
-- =====================================

-- 递归扫描目录，查找.lua文件
function ShieldAutoManager.scan_lua_files(directory)
    local lua_files = {}
    
    local function scan_dir(dir)
        for file in lfs.dir(dir) do
            if file ~= "." and file ~= ".." then
                local path = dir .. "/" .. file
                local attr = lfs.attributes(path)
                
                if attr.mode == "directory" then
                    scan_dir(path)  -- 递归扫描子目录
                elseif attr.mode == "file" and file:match("%.lua$") then
                    table.insert(lua_files, path)
                end
            end
        end
    end
    
    scan_dir(directory)
    return lua_files
end

-- 加载和分析Lua文件中的Shield类
function ShieldAutoManager.analyze_lua_file(file_path)
    print(string.format("[ShieldAutoManager] Analyzing file: %s", file_path))
    
    local shield_classes = {}
    
    -- 安全加载文件
    local chunk, error_msg = loadfile(file_path)
    if not chunk then
        print(string.format("[ShieldAutoManager] Failed to load %s: %s", file_path, error_msg))
        return shield_classes
    end
    
    -- 创建沙箱环境
    local env = {}
    setmetatable(env, {__index = _G})
    env.Shield = Shield  -- 注入Shield框架
    
    setfenv(chunk, env)
    
    -- 执行文件
    local success, result = pcall(chunk)
    if not success then
        print(string.format("[ShieldAutoManager] Failed to execute %s: %s", file_path, result))
        return shield_classes
    end
    
    -- 分析返回结果
    if result then
        if type(result) == "table" then
            -- 如果返回的是表，检查每个元素
            for name, obj in pairs(result) do
                if Shield.Scanner.is_shield_managed(obj) then
                    shield_classes[name] = obj
                    print(string.format("[ShieldAutoManager] Found Shield class: %s (%s)", 
                        name, Shield.Scanner.get_service_type(obj)))
                end
            end
        elseif Shield.Scanner.is_shield_managed(result) then
            -- 如果直接返回Shield对象
            local name = result._service_name or result._component_name or "unnamed"
            shield_classes[name] = result
        end
    end
    
    -- 扫描环境中的Shield对象
    for name, obj in pairs(env) do
        if type(obj) == "table" and Shield.Scanner.is_shield_managed(obj) and not shield_classes[name] then
            shield_classes[name] = obj
            print(string.format("[ShieldAutoManager] Found Shield class in env: %s (%s)", 
                name, Shield.Scanner.get_service_type(obj)))
        end
    end
    
    return shield_classes
end

-- =====================================
-- 2. 依赖关系分析
-- =====================================

-- 构建依赖图
function ShieldAutoManager.build_dependency_graph(services)
    local graph = {}
    
    for name, service in pairs(services) do
        graph[name] = {
            service = service,
            dependencies = Shield.Scanner.get_dependencies(service),
            dependents = {}  -- 依赖此服务的其他服务
        }
    end
    
    -- 构建反向依赖关系
    for name, info in pairs(graph) do
        for _, dep_name in ipairs(info.dependencies) do
            if graph[dep_name] then
                table.insert(graph[dep_name].dependents, name)
            end
        end
    end
    
    return graph
end

-- 拓扑排序，确定初始化顺序
function ShieldAutoManager.topological_sort(graph)
    local sorted = {}
    local visited = {}
    local visiting = {}
    
    local function visit(name)
        if visiting[name] then
            error("Circular dependency detected involving: " .. name)
        end
        
        if visited[name] then
            return
        end
        
        visiting[name] = true
        
        -- 先访问所有依赖
        local info = graph[name]
        if info then
            for _, dep_name in ipairs(info.dependencies) do
                if graph[dep_name] then
                    visit(dep_name)
                end
            end
        end
        
        visiting[name] = false
        visited[name] = true
        table.insert(sorted, name)
    end
    
    -- 访问所有节点
    for name in pairs(graph) do
        visit(name)
    end
    
    return sorted
end

-- =====================================
-- 3. 自动注册和初始化
-- =====================================

-- 自动发现和注册所有Shield服务
function ShieldAutoManager.auto_discover_and_register(base_directory, container)
    print(string.format("[ShieldAutoManager] Starting auto-discovery in: %s", base_directory))
    
    -- 1. 扫描所有Lua文件
    local lua_files = ShieldAutoManager.scan_lua_files(base_directory)
    print(string.format("[ShieldAutoManager] Found %d Lua files", #lua_files))
    
    -- 2. 分析每个文件，提取Shield类
    local all_services = {}
    for _, file_path in ipairs(lua_files) do
        local services = ShieldAutoManager.analyze_lua_file(file_path)
        for name, service in pairs(services) do
            if all_services[name] then
                print(string.format("[ShieldAutoManager] Warning: Duplicate service name: %s", name))
            end
            all_services[name] = service
        end
    end
    
    print(string.format("[ShieldAutoManager] Discovered %d Shield services", 
        ShieldAutoManager.count_table(all_services)))
    
    -- 3. 构建依赖图
    local dependency_graph = ShieldAutoManager.build_dependency_graph(all_services)
    ShieldAutoManager.dependency_graph = dependency_graph
    
    -- 4. 拓扑排序，确定初始化顺序
    local init_order = ShieldAutoManager.topological_sort(dependency_graph)
    print("[ShieldAutoManager] Service initialization order:")
    for i, name in ipairs(init_order) do
        print(string.format("  %d. %s", i, name))
    end
    
    -- 5. 按顺序注册和初始化服务
    for _, name in ipairs(init_order) do
        local service = all_services[name]
        if service then
            ShieldAutoManager.register_single_service(container, name, service)
        end
    end
    
    -- 6. 存储注册信息
    ShieldAutoManager.registered_services = all_services
    
    print(string.format("[ShieldAutoManager] Auto-registration completed. %d services registered.", 
        #init_order))
    
    return init_order
end

-- 注册单个服务
function ShieldAutoManager.register_single_service(container, name, service)
    local service_type = Shield.Scanner.get_service_type(service)
    
    print(string.format("[ShieldAutoManager] Registering %s: %s", service_type, name))
    
    if service_type == "service" or service_type == "repository" or service_type == "controller" then
        container:register_service(name, service, "singleton")
    elseif service_type == "component" then
        container:register_service(name, service, "transient")
    elseif service_type == "configuration" then
        container:register_service(name, service, "singleton")
    elseif service_type == "event_listener" then
        container:register_service(name, service, "singleton")
        -- 注册事件处理器
        for event_type, handler_info in pairs(service._event_handlers or {}) do
            container:register_event_listener(event_type, handler_info.handler, handler_info.priority)
        end
    elseif service_type == "health_indicator" then
        container:register_health_check(name, function(c)
            return service:check()
        end)
    end
    
    ShieldAutoManager.service_instances[name] = service
end

-- =====================================
-- 4. 运行时管理
-- =====================================

-- 启动所有服务
function ShieldAutoManager.start_all_services(container)
    print("[ShieldAutoManager] Starting all services...")
    
    local init_order = ShieldAutoManager.get_initialization_order()
    
    for _, name in ipairs(init_order) do
        local service = ShieldAutoManager.service_instances[name]
        if service and service.on_start then
            print(string.format("[ShieldAutoManager] Starting service: %s", name))
            local success, error_msg = pcall(service.on_start, service)
            if not success then
                print(string.format("[ShieldAutoManager] Failed to start %s: %s", name, error_msg))
            end
        end
    end
    
    print("[ShieldAutoManager] All services started")
end

-- 停止所有服务
function ShieldAutoManager.stop_all_services()
    print("[ShieldAutoManager] Stopping all services...")
    
    local init_order = ShieldAutoManager.get_initialization_order()
    
    -- 反向停止
    for i = #init_order, 1, -1 do
        local name = init_order[i]
        local service = ShieldAutoManager.service_instances[name]
        if service and service.on_stop then
            print(string.format("[ShieldAutoManager] Stopping service: %s", name))
            local success, error_msg = pcall(service.on_stop, service)
            if not success then
                print(string.format("[ShieldAutoManager] Failed to stop %s: %s", name, error_msg))
            end
        end
    end
    
    print("[ShieldAutoManager] All services stopped")
end

-- 重新加载服务 (热重载)
function ShieldAutoManager.reload_service(name, file_path)
    print(string.format("[ShieldAutoManager] Reloading service: %s", name))
    
    -- 分析新的服务定义
    local new_services = ShieldAutoManager.analyze_lua_file(file_path)
    local new_service = new_services[name]
    
    if not new_service then
        print(string.format("[ShieldAutoManager] Service %s not found in reloaded file", name))
        return false
    end
    
    -- 停止旧服务
    local old_service = ShieldAutoManager.service_instances[name]
    if old_service and old_service.on_stop then
        old_service:on_stop()
    end
    
    -- 更新服务实例
    ShieldAutoManager.service_instances[name] = new_service
    ShieldAutoManager.registered_services[name] = new_service
    
    -- 启动新服务
    if new_service.on_start then
        new_service:on_start()
    end
    
    print(string.format("[ShieldAutoManager] Service %s reloaded successfully", name))
    return true
end

-- =====================================
-- 5. 工具函数
-- =====================================

function ShieldAutoManager.count_table(t)
    local count = 0
    for _ in pairs(t) do count = count + 1 end
    return count
end

function ShieldAutoManager.get_initialization_order()
    local graph = ShieldAutoManager.dependency_graph
    if not graph or not next(graph) then
        return {}
    end
    return ShieldAutoManager.topological_sort(graph)
end

-- 获取服务统计信息
function ShieldAutoManager.get_statistics()
    local stats = {
        total_services = ShieldAutoManager.count_table(ShieldAutoManager.registered_services),
        service_types = {},
        dependency_count = 0
    }
    
    for name, service in pairs(ShieldAutoManager.registered_services) do
        local service_type = Shield.Scanner.get_service_type(service)
        stats.service_types[service_type] = (stats.service_types[service_type] or 0) + 1
        
        local deps = Shield.Scanner.get_dependencies(service)
        stats.dependency_count = stats.dependency_count + #deps
    end
    
    return stats
end

-- 打印服务统计信息
function ShieldAutoManager.print_statistics()
    local stats = ShieldAutoManager.get_statistics()
    
    print("\n=== Shield服务统计 ===")
    print(string.format("总服务数: %d", stats.total_services))
    print(string.format("总依赖数: %d", stats.dependency_count))
    print("\n服务类型分布:")
    for service_type, count in pairs(stats.service_types) do
        print(string.format("  %s: %d", service_type, count))
    end
    print("========================\n")
end

return ShieldAutoManager