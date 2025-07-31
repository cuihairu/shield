-- Shield统一兼容依赖注入系统
-- 支持多种DI方案，用户可以自由选择和混合使用

local Shield = require("shield_framework")

-- =====================================
-- 统一依赖注入系统 - 兼容所有方案
-- =====================================

-- 扩展Shield.Service，支持多种依赖注入方式
local UnifiedDI = {}

-- 依赖注入方式枚举
UnifiedDI.InjectionStyle = {
    LEGACY_STRING = "legacy_string",           -- 传统字符串方式 (兼容老代码)
    TYPED_REFERENCE = "typed_reference",       -- 类型引用方式 (推荐)
    CONSTRUCTOR_INJECTION = "constructor_injection", -- 构造函数注入 (现代)
    BUILDER_PATTERN = "builder_pattern",       -- 构建器模式 (灵活)
    INTERFACE_BASED = "interface_based",       -- 接口驱动 (企业级)
    ANNOTATION_DRIVEN = "annotation_driven"    -- 注解驱动 (高级)
}

-- 扩展Shield.Service，添加统一DI支持
function Shield.Service:unified_di()
    -- 初始化统一DI状态
    self._unified_di = {
        style_preferences = {},  -- 用户偏好的DI方式
        dependencies = {},       -- 统一的依赖存储  
        injected_fields = {},    -- 已注入的字段
        injection_metadata = {}  -- 注入元数据
    }
    return self
end

-- =====================================
-- 方案1: 传统字符串方式 (向后兼容)
-- =====================================

function Shield.Service:depends_on(...)
    if not self._unified_di then self:unified_di() end
    
    local deps = {...}
    for _, dep in ipairs(deps) do
        table.insert(self._unified_di.dependencies, {
            style = UnifiedDI.InjectionStyle.LEGACY_STRING,
            service_name = dep,
            field_name = dep:gsub("Service$", ""):lower()  -- 自动生成字段名
        })
    end
    
    self._unified_di.style_preferences[UnifiedDI.InjectionStyle.LEGACY_STRING] = true
    return self
end

-- =====================================
-- 方案2: 类型引用方式 (推荐)
-- =====================================

function Shield.Service:inject(service_class, field_name)
    if not self._unified_di then self:unified_di() end
    
    local service_name = service_class._service_name or service_class._component_name
    field_name = field_name or service_name
    
    table.insert(self._unified_di.dependencies, {
        style = UnifiedDI.InjectionStyle.TYPED_REFERENCE,
        service_class = service_class,
        service_name = service_name,
        field_name = field_name
    })
    
    self._unified_di.style_preferences[UnifiedDI.InjectionStyle.TYPED_REFERENCE] = true
    return self
end

function Shield.Service:inject_as(field_name, service_class)
    return self:inject(service_class, field_name)
end

-- =====================================
-- 方案3: 构造函数注入 (现代化)
-- =====================================

function Shield.Service:with_dependencies(...)
    if not self._unified_di then self:unified_di() end
    
    local services = {...}
    for i, service_class in ipairs(services) do
        local service_name = service_class._service_name or service_class._component_name
        local field_name = service_name
        
        table.insert(self._unified_di.dependencies, {
            style = UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION,
            service_class = service_class,
            service_name = service_name,
            field_name = field_name,
            constructor_order = i
        })
    end
    
    self._unified_di.style_preferences[UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION] = true
    return self
end

-- =====================================
-- 方案4: 构建器模式 (灵活)
-- =====================================

function Shield.Service:with_database(database_service)
    return self:inject_as("database", database_service)
end

function Shield.Service:with_cache(cache_service)
    return self:inject_as("cache", cache_service)
end

function Shield.Service:with_logger(logger_service)
    return self:inject_as("logger", logger_service)
end

function Shield.Service:with_config(config_service)
    return self:inject_as("config", config_service)
end

-- 动态构建器方法生成
function Shield.Service:generate_builder_methods(service_types)
    for _, service_type in ipairs(service_types) do
        local method_name = "with_" .. service_type:lower()
        local field_name = service_type:lower()
        
        self[method_name] = function(self, service_class)
            return self:inject_as(field_name, service_class)
        end
    end
    return self
end

-- =====================================
-- 方案5: 接口驱动 (企业级)
-- =====================================

function Shield.Service:inject_interface(field_name, interface_def, implementation)
    if not self._unified_di then self:unified_di() end
    
    local service_name = implementation._service_name or implementation._component_name
    
    table.insert(self._unified_di.dependencies, {
        style = UnifiedDI.InjectionStyle.INTERFACE_BASED,
        interface_def = interface_def,
        service_class = implementation,
        service_name = service_name,
        field_name = field_name
    })
    
    self._unified_di.style_preferences[UnifiedDI.InjectionStyle.INTERFACE_BASED] = true
    return self
end

-- =====================================
-- 方案6: 注解驱动 (高级)
-- =====================================

-- 注解处理器
local AnnotationProcessor = {}

function AnnotationProcessor:process_service(service)
    if not service._annotations then return service end
    
    if not service._unified_di then service:unified_di() end
    
    for annotation_type, annotation_data in pairs(service._annotations) do
        if annotation_type == "Inject" then
            for _, inject_info in ipairs(annotation_data) do
                table.insert(service._unified_di.dependencies, {
                    style = UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN,
                    service_class = inject_info.service_class,
                    service_name = inject_info.service_name,
                    field_name = inject_info.field_name,
                    annotation = inject_info
                })
            end
        end
    end
    
    service._unified_di.style_preferences[UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN] = true
    return service
end

-- 注解宏
function Shield.Service:annotate_inject(field_name, service_class, options)
    self._annotations = self._annotations or {}
    self._annotations.Inject = self._annotations.Inject or {}
    
    table.insert(self._annotations.Inject, {
        field_name = field_name,
        service_class = service_class,
        service_name = service_class._service_name or service_class._component_name,
        required = options and options.required or true,
        lazy = options and options.lazy or false
    })
    
    return self
end

-- =====================================
-- 统一依赖解析器
-- =====================================

local UnifiedResolver = {}

function UnifiedResolver:resolve_all_dependencies(service, container)
    if not service._unified_di or not service._unified_di.dependencies then
        return
    end
    
    print(string.format("[UnifiedDI] Resolving dependencies for %s using %d methods", 
        service._service_name, #service._unified_di.dependencies))
    
    -- 按优先级排序依赖
    local sorted_deps = UnifiedResolver:sort_dependencies_by_priority(service._unified_di.dependencies)
    
    for _, dep_info in ipairs(sorted_deps) do
        UnifiedResolver:resolve_single_dependency(service, container, dep_info)
    end
    
    -- 记录注入统计
    UnifiedResolver:log_injection_stats(service)
end

function UnifiedResolver:resolve_single_dependency(service, container, dep_info)
    local style = dep_info.style
    local field_name = dep_info.field_name
    local service_name = dep_info.service_name
    
    -- 根据不同方式解析依赖
    local resolved_service = nil
    
    if style == UnifiedDI.InjectionStyle.LEGACY_STRING then
        resolved_service = container:resolve(service_name)
        
    elseif style == UnifiedDI.InjectionStyle.TYPED_REFERENCE or 
           style == UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION then
        resolved_service = container:resolve(service_name)
        
    elseif style == UnifiedDI.InjectionStyle.INTERFACE_BASED then
        resolved_service = container:resolve(service_name)
        -- 可以添加接口验证逻辑
        
    elseif style == UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN then
        if dep_info.annotation.lazy then
            -- 懒加载：创建代理
            resolved_service = UnifiedResolver:create_lazy_proxy(container, service_name)
        else
            resolved_service = container:resolve(service_name)
        end
    end
    
    -- 注入到服务
    if resolved_service then
        service[field_name] = resolved_service
        service._unified_di.injected_fields[field_name] = {
            service = resolved_service,
            style = style,
            service_name = service_name
        }
        
        print(string.format("[UnifiedDI] Injected %s.%s (%s)", 
            service._service_name, field_name, style))
    else
        print(string.format("[UnifiedDI] Failed to resolve %s for %s.%s", 
            service_name, service._service_name, field_name))
    end
end

function UnifiedResolver:sort_dependencies_by_priority(dependencies)
    -- 按注入方式优先级排序
    local priority_map = {
        [UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION] = 1,
        [UnifiedDI.InjectionStyle.TYPED_REFERENCE] = 2,
        [UnifiedDI.InjectionStyle.INTERFACE_BASED] = 3,
        [UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN] = 4,
        [UnifiedDI.InjectionStyle.BUILDER_PATTERN] = 5,
        [UnifiedDI.InjectionStyle.LEGACY_STRING] = 6
    }
    
    table.sort(dependencies, function(a, b)
        return (priority_map[a.style] or 999) < (priority_map[b.style] or 999)
    end)
    
    return dependencies
end

function UnifiedResolver:create_lazy_proxy(container, service_name)
    local proxy = {}
    local real_service = nil
    
    return setmetatable(proxy, {
        __index = function(t, k)
            if not real_service then
                real_service = container:resolve(service_name)
                print(string.format("[UnifiedDI] Lazy loaded: %s", service_name))
            end
            return real_service and real_service[k]
        end
    })
end

function UnifiedResolver:log_injection_stats(service)
    local stats = {
        total_dependencies = 0,
        successful_injections = 0,
        styles_used = {}
    }
    
    for field_name, injection_info in pairs(service._unified_di.injected_fields) do
        stats.total_dependencies = stats.total_dependencies + 1
        stats.successful_injections = stats.successful_injections + 1
        stats.styles_used[injection_info.style] = (stats.styles_used[injection_info.style] or 0) + 1
    end
    
    print(string.format("[UnifiedDI] %s injection complete: %d/%d successful", 
        service._service_name, stats.successful_injections, stats.total_dependencies))
    
    for style, count in pairs(stats.styles_used) do
        print(string.format("  %s: %d", style, count))
    end
end

-- =====================================
-- 重写Service.on_init，自动处理统一DI
-- =====================================

local original_service_on_init = Shield.Service.on_init

function Shield.Service:on_init(container)
    -- 1. 处理注解
    AnnotationProcessor:process_service(self)
    
    -- 2. 统一依赖解析
    UnifiedResolver:resolve_all_dependencies(self, container)
    
    -- 3. 调用原始on_init
    if original_service_on_init then
        original_service_on_init(self, container)
    end
end

-- =====================================
-- 兼容性测试和使用示例
-- =====================================

local function demonstrate_unified_di()
    print("=== Shield统一兼容依赖注入演示 ===\n")
    
    -- 定义基础服务
    local Services = {}
    Services.Database = Shield.Service:new({_service_name = "DatabaseService"})
    Services.Cache = Shield.Service:new({_service_name = "CacheService"})
    Services.Logger = Shield.Service:new({_service_name = "LoggerService"})
    Services.Config = Shield.Configuration:new({_config_name = "ConfigService"})
    
    -- 添加业务方法
    function Services.Database:find_user(id) return {id = id, name = "User" .. id} end
    function Services.Cache:get(key) return self._data and self._data[key] end
    function Services.Cache:set(key, value) self._data = self._data or {}; self._data[key] = value end
    function Services.Logger:info(msg) print("[INFO] " .. msg) end
    function Services.Config:get_property(key) return self._props and self._props[key] end
    
    print("--- 方案1: 传统字符串方式 (向后兼容) ---")
    local LegacyService = Shield.Service:new({_service_name = "LegacyService"})
    LegacyService:depends_on("DatabaseService", "LoggerService")
    
    print("\n--- 方案2: 类型引用方式 (推荐) ---")
    local TypedService = Shield.Service:new({_service_name = "TypedService"})
    TypedService:inject(Services.Database)
                :inject_as("logger", Services.Logger)
    
    print("\n--- 方案3: 构造函数注入 (现代化) ---")
    local ModernService = Shield.Service:new({_service_name = "ModernService"})
    ModernService:with_dependencies(Services.Database, Services.Cache, Services.Logger)
    
    print("\n--- 方案4: 构建器模式 (灵活) ---")
    local FlexibleService = Shield.Service:new({_service_name = "FlexibleService"})
    FlexibleService:with_database(Services.Database)
                   :with_cache(Services.Cache)
                   :with_logger(Services.Logger)
    
    print("\n--- 方案5: 接口驱动 (企业级) ---")
    local IUserRepository = {find_by_id = function() end, save = function() end}
    local EnterpriseService = Shield.Service:new({_service_name = "EnterpriseService"})
    EnterpriseService:inject_interface("userRepo", IUserRepository, Services.Database)
    
    print("\n--- 方案6: 注解驱动 (高级) ---")
    local AnnotatedService = Shield.Service:new({_service_name = "AnnotatedService"})
    AnnotatedService:annotate_inject("database", Services.Database, {required = true})
                    :annotate_inject("cache", Services.Cache, {lazy = true})
    
    print("\n--- 混合方案: 多种方式并存 ---")
    local HybridService = Shield.Service:new({_service_name = "HybridService"})
    HybridService:depends_on("ConfigService")                    -- 传统方式
                 :inject(Services.Database)                      -- 类型引用
                 :with_cache(Services.Cache)                     -- 构建器模式
                 :annotate_inject("logger", Services.Logger)     -- 注解驱动
    
    -- 测试所有服务
    local container = require("lua_ioc_container").LuaIoC:new()
    
    -- 注册基础服务
    for name, service in pairs(Services) do
        container:register_service(name, service)
    end
    
    -- 注册业务服务
    local test_services = {
        LegacyService, TypedService, ModernService, 
        FlexibleService, EnterpriseService, AnnotatedService, HybridService
    }
    
    for _, service in ipairs(test_services) do
        container:register_service(service._service_name, service)
    end
    
    print("\n--- 统一依赖解析 ---")
    -- 初始化所有服务 (触发统一DI)
    for _, service in ipairs(test_services) do
        service:on_init(container)
    end
    
    print("\n--- 验证注入结果 ---")
    print("LegacyService.databaseservice:", LegacyService.databaseservice and "✅" or "❌")
    print("TypedService.DatabaseService:", TypedService.DatabaseService and "✅" or "❌")
    print("ModernService.DatabaseService:", ModernService.DatabaseService and "✅" or "❌")
    print("FlexibleService.database:", FlexibleService.database and "✅" or "❌")
    print("HybridService.DatabaseService:", HybridService.DatabaseService and "✅" or "❌")
    
    print("\n=== 统一兼容演示完成 ===")
end

-- 运行演示
demonstrate_unified_di()

-- =====================================
-- 用户选择和配置
-- =====================================

-- 用户偏好配置
local UserPreferences = {
    -- 新手用户 - 简单易学
    beginner = {
        primary_style = UnifiedDI.InjectionStyle.TYPED_REFERENCE,
        fallback_style = UnifiedDI.InjectionStyle.LEGACY_STRING,
        enable_annotations = false,
        auto_generate_builders = true
    },
    
    -- 中级用户 - 现代化开发
    intermediate = {
        primary_style = UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION,
        fallback_style = UnifiedDI.InjectionStyle.TYPED_REFERENCE,
        enable_annotations = true,
        auto_generate_builders = true
    },
    
    -- 高级用户 - 企业级功能
    advanced = {
        primary_style = UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN,
        fallback_style = UnifiedDI.InjectionStyle.INTERFACE_BASED,
        enable_annotations = true,
        auto_generate_builders = false,
        strict_interface_checking = true
    },
    
    -- 团队协作 - 统一规范
    team = {
        primary_style = UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION,
        allowed_styles = {
            UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION,
            UnifiedDI.InjectionStyle.TYPED_REFERENCE
        },
        enable_annotations = true,
        enforce_style_consistency = true
    }
}

return {
    UnifiedDI = UnifiedDI,
    UnifiedResolver = UnifiedResolver,
    AnnotationProcessor = AnnotationProcessor,
    UserPreferences = UserPreferences,
    Services = Services
}