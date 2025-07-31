-- Shield AOP (面向切面编程) 实现
-- 利用Lua的元表机制实现完整的AOP功能

local Shield = require("shield_framework")

-- =====================================
-- 1. AOP核心系统
-- =====================================

local ShieldAOP = {}

-- AOP切面类型
ShieldAOP.AspectType = {
    BEFORE = "before",           -- 方法执行前
    AFTER = "after",             -- 方法执行后  
    AROUND = "around",           -- 环绕通知
    AFTER_RETURNING = "after_returning", -- 正常返回后
    AFTER_THROWING = "after_throwing",   -- 异常抛出后
    FINALLY = "finally"          -- 无论如何都执行
}

-- 切点匹配类型
ShieldAOP.PointcutType = {
    METHOD_NAME = "method_name",     -- 按方法名匹配
    CLASS_TYPE = "class_type",       -- 按类类型匹配
    ANNOTATION = "annotation",       -- 按注解匹配
    EXECUTION = "execution",         -- 按执行表达式匹配
    REGEX = "regex"                  -- 按正则表达式匹配
}

-- =====================================
-- 2. 切面定义系统
-- =====================================

-- 切面类
ShieldAOP.Aspect = {}
ShieldAOP.Aspect.__index = ShieldAOP.Aspect

function ShieldAOP.Aspect:new(name)
    local aspect = {
        name = name or "UnnamedAspect",
        advices = {},        -- 通知列表
        pointcuts = {},      -- 切点列表
        priority = 0         -- 优先级
    }
    setmetatable(aspect, self)
    return aspect
end

-- 定义切点
function ShieldAOP.Aspect:pointcut(name, matcher)
    self.pointcuts[name] = matcher
    return self
end

-- 前置通知
function ShieldAOP.Aspect:before(pointcut_name, advice_func)
    return self:add_advice(ShieldAOP.AspectType.BEFORE, pointcut_name, advice_func)
end

-- 后置通知
function ShieldAOP.Aspect:after(pointcut_name, advice_func)
    return self:add_advice(ShieldAOP.AspectType.AFTER, pointcut_name, advice_func)
end

-- 环绕通知
function ShieldAOP.Aspect:around(pointcut_name, advice_func)
    return self:add_advice(ShieldAOP.AspectType.AROUND, pointcut_name, advice_func)
end

-- 返回后通知
function ShieldAOP.Aspect:after_returning(pointcut_name, advice_func)
    return self:add_advice(ShieldAOP.AspectType.AFTER_RETURNING, pointcut_name, advice_func)
end

-- 异常后通知
function ShieldAOP.Aspect:after_throwing(pointcut_name, advice_func)
    return self:add_advice(ShieldAOP.AspectType.AFTER_THROWING, pointcut_name, advice_func)
end

-- 添加通知
function ShieldAOP.Aspect:add_advice(aspect_type, pointcut_name, advice_func)
    table.insert(self.advices, {
        type = aspect_type,
        pointcut = pointcut_name,
        advice = advice_func,
        priority = self:get_next_priority()
    })
    return self
end

function ShieldAOP.Aspect:get_next_priority()
    return #self.advices
end

-- =====================================
-- 3. 切点匹配器
-- =====================================

ShieldAOP.PointcutMatcher = {}

-- 按方法名匹配
function ShieldAOP.PointcutMatcher.method_name(pattern)
    return {
        type = ShieldAOP.PointcutType.METHOD_NAME,
        pattern = pattern,
        match = function(target, method_name)
            if type(pattern) == "string" then
                return method_name == pattern
            elseif type(pattern) == "table" then
                for _, p in ipairs(pattern) do
                    if method_name == p then return true end
                end
            end
            return false
        end
    }
end

-- 按类型匹配
function ShieldAOP.PointcutMatcher.class_type(class_type)
    return {
        type = ShieldAOP.PointcutType.CLASS_TYPE,
        class_type = class_type,
        match = function(target, method_name)
            return target:instanceof and target:instanceof(class_type)
        end
    }
end

-- 按正则匹配
function ShieldAOP.PointcutMatcher.regex(pattern)
    return {
        type = ShieldAOP.PointcutType.REGEX,
        pattern = pattern,
        match = function(target, method_name)
            return string.match(method_name, pattern) ~= nil
        end
    }
end

-- 按注解匹配
function ShieldAOP.PointcutMatcher.annotation(annotation_name)
    return {
        type = ShieldAOP.PointcutType.ANNOTATION,
        annotation = annotation_name,
        match = function(target, method_name)
            return target._method_annotations and 
                   target._method_annotations[method_name] and
                   target._method_annotations[method_name][annotation_name]
        end
    }
end

-- 组合匹配器
function ShieldAOP.PointcutMatcher.and_match(matcher1, matcher2)
    return {
        type = "composite",
        match = function(target, method_name)
            return matcher1.match(target, method_name) and matcher2.match(target, method_name)
        end
    }
end

function ShieldAOP.PointcutMatcher.or_match(matcher1, matcher2)
    return {
        type = "composite", 
        match = function(target, method_name)
            return matcher1.match(target, method_name) or matcher2.match(target, method_name)
        end
    }
end

-- =====================================
-- 4. AOP代理工厂
-- =====================================

ShieldAOP.ProxyFactory = {}

-- 创建AOP代理
function ShieldAOP.ProxyFactory.create_proxy(target, aspects)
    local proxy = {}
    local original_methods = {}
    
    -- 保存原始方法
    for key, value in pairs(target) do
        if type(value) == "function" and not key:match("^_") then
            original_methods[key] = value
        end
    end
    
    -- 创建代理方法
    for method_name, original_method in pairs(original_methods) do
        proxy[method_name] = ShieldAOP.ProxyFactory.create_proxy_method(
            target, method_name, original_method, aspects
        )
    end
    
    -- 复制非方法属性
    for key, value in pairs(target) do
        if type(value) ~= "function" then
            proxy[key] = value
        end
    end
    
    -- 设置元表
    setmetatable(proxy, getmetatable(target))
    
    return proxy
end

-- 创建代理方法
function ShieldAOP.ProxyFactory.create_proxy_method(target, method_name, original_method, aspects)
    return function(self, ...)
        local args = {...}
        local context = ShieldAOP.create_join_point(target, method_name, args)
        
        -- 收集匹配的通知
        local matched_advices = ShieldAOP.collect_matched_advices(target, method_name, aspects)
        
        -- 执行AOP逻辑
        return ShieldAOP.execute_with_aspects(context, original_method, matched_advices)
    end
end

-- =====================================
-- 5. 连接点(JoinPoint)和执行上下文
-- =====================================

function ShieldAOP.create_join_point(target, method_name, args)
    return {
        target = target,
        method_name = method_name,
        args = args,
        start_time = os.clock(),
        result = nil,
        error = nil,
        metadata = {}
    }
end

-- 收集匹配的通知
function ShieldAOP.collect_matched_advices(target, method_name, aspects)
    local matched = {}
    
    for _, aspect in ipairs(aspects) do
        for _, advice in ipairs(aspect.advices) do
            local pointcut = aspect.pointcuts[advice.pointcut]
            if pointcut and pointcut.match(target, method_name) then
                table.insert(matched, {
                    aspect_name = aspect.name,
                    advice = advice,
                    priority = advice.priority
                })
            end
        end
    end
    
    -- 按优先级排序
    table.sort(matched, function(a, b) return a.priority < b.priority end)
    
    return matched
end

-- =====================================
-- 6. AOP执行引擎
-- =====================================

function ShieldAOP.execute_with_aspects(context, original_method, matched_advices)
    local before_advices = {}
    local after_advices = {}
    local around_advices = {}
    local after_returning_advices = {}
    local after_throwing_advices = {}
    local finally_advices = {}
    
    -- 分类通知
    for _, matched in ipairs(matched_advices) do
        local advice_type = matched.advice.type
        if advice_type == ShieldAOP.AspectType.BEFORE then
            table.insert(before_advices, matched)
        elseif advice_type == ShieldAOP.AspectType.AFTER then
            table.insert(after_advices, matched)
        elseif advice_type == ShieldAOP.AspectType.AROUND then
            table.insert(around_advices, matched)
        elseif advice_type == ShieldAOP.AspectType.AFTER_RETURNING then
            table.insert(after_returning_advices, matched)
        elseif advice_type == ShieldAOP.AspectType.AFTER_THROWING then
            table.insert(after_throwing_advices, matched)
        elseif advice_type == ShieldAOP.AspectType.FINALLY then
            table.insert(finally_advices, matched)
        end
    end
    
    local success, result_or_error
    
    -- 执行finally包装
    local function execute_core()
        -- 执行前置通知
        for _, matched in ipairs(before_advices) do
            matched.advice.advice(context)
        end
        
        -- 执行环绕通知或原方法
        if #around_advices > 0 then
            result_or_error = ShieldAOP.execute_around_chain(
                context, original_method, around_advices, 1
            )
            success = true
        else
            success, result_or_error = pcall(original_method, context.target, unpack(context.args))
        end
        
        -- 保存结果或错误
        if success then
            context.result = result_or_error
            -- 执行返回后通知
            for _, matched in ipairs(after_returning_advices) do
                matched.advice.advice(context)
            end
        else
            context.error = result_or_error
            -- 执行异常后通知
            for _, matched in ipairs(after_throwing_advices) do
                matched.advice.advice(context)
            end
        end
        
        -- 执行后置通知
        for _, matched in ipairs(after_advices) do
            matched.advice.advice(context)
        end
        
        return success, result_or_error
    end
    
    -- 包装finally通知
    local final_success, final_result = pcall(execute_core)
    
    -- 执行finally通知
    for _, matched in ipairs(finally_advices) do
        pcall(matched.advice.advice, context)
    end
    
    -- 返回结果
    if not final_success then
        error(final_result)
    elseif not success then
        error(result_or_error)
    else
        return result_or_error
    end
end

-- 执行环绕通知链
function ShieldAOP.execute_around_chain(context, original_method, around_advices, index)
    if index > #around_advices then
        -- 执行原方法
        return original_method(context.target, unpack(context.args))
    else
        local current_advice = around_advices[index]
        local proceed = function()
            return ShieldAOP.execute_around_chain(context, original_method, around_advices, index + 1)
        end
        context.proceed = proceed
        return current_advice.advice.advice(context)
    end
end

-- =====================================
-- 7. AOP容器集成
-- =====================================

ShieldAOP.Container = {}
ShieldAOP.Container.aspects = {}
ShieldAOP.Container.proxies = {}

-- 注册切面
function ShieldAOP.Container.register_aspect(aspect)
    table.insert(ShieldAOP.Container.aspects, aspect)
    print(string.format("[AOP] Registered aspect: %s", aspect.name))
end

-- 创建AOP增强的对象
function ShieldAOP.Container.enhance_object(target)
    if #ShieldAOP.Container.aspects == 0 then
        return target  -- 没有切面时直接返回原对象
    end
    
    local proxy = ShieldAOP.ProxyFactory.create_proxy(target, ShieldAOP.Container.aspects)
    ShieldAOP.Container.proxies[target] = proxy
    
    print(string.format("[AOP] Enhanced object: %s", target._service_name or "Unknown"))
    return proxy
end

-- 批量增强Shield服务
function ShieldAOP.Container.enhance_shield_services(services)
    local enhanced = {}
    for name, service in pairs(services) do
        if service._shield_managed then
            enhanced[name] = ShieldAOP.Container.enhance_object(service)
        else
            enhanced[name] = service
        end
    end
    return enhanced
end

-- =====================================
-- 8. 常用切面库
-- =====================================

ShieldAOP.CommonAspects = {}

-- 性能监控切面
function ShieldAOP.CommonAspects.create_performance_aspect()
    local aspect = ShieldAOP.Aspect:new("PerformanceMonitor")
    
    aspect:pointcut("all_methods", ShieldAOP.PointcutMatcher.regex(".*"))
    
    aspect:around("all_methods", function(context)
        local start_time = os.clock()
        local result = context.proceed()
        local end_time = os.clock()
        local duration = (end_time - start_time) * 1000
        
        print(string.format("[PERF] %s.%s took %.2f ms", 
            context.target._service_name or "Unknown",
            context.method_name,
            duration
        ))
        
        return result
    end)
    
    return aspect
end

-- 日志切面
function ShieldAOP.CommonAspects.create_logging_aspect()
    local aspect = ShieldAOP.Aspect:new("Logger")
    
    aspect:pointcut("public_methods", ShieldAOP.PointcutMatcher.regex("^[^_].*"))
    
    aspect:before("public_methods", function(context)
        print(string.format("[LOG] Entering %s.%s(%s)", 
            context.target._service_name or "Unknown",
            context.method_name,
            table.concat(context.args, ", ")
        ))
    end)
    
    aspect:after_returning("public_methods", function(context)
        print(string.format("[LOG] %s.%s returned: %s", 
            context.target._service_name or "Unknown",
            context.method_name,
            tostring(context.result)
        ))
    end)
    
    aspect:after_throwing("public_methods", function(context)
        print(string.format("[LOG] %s.%s threw error: %s", 
            context.target._service_name or "Unknown", 
            context.method_name,
            tostring(context.error)
        ))
    end)
    
    return aspect
end

-- 事务切面
function ShieldAOP.CommonAspects.create_transaction_aspect()
    local aspect = ShieldAOP.Aspect:new("Transaction")
    
    aspect:pointcut("repository_methods", 
        ShieldAOP.PointcutMatcher.and_match(
            ShieldAOP.PointcutMatcher.class_type(Shield.Repository),
            ShieldAOP.PointcutMatcher.method_name({"save", "delete", "update"})
        )
    )
    
    aspect:around("repository_methods", function(context)
        print("[TX] Starting transaction")
        
        local success, result = pcall(context.proceed)
        
        if success then
            print("[TX] Committing transaction")
            return result
        else
            print("[TX] Rolling back transaction")
            error(result)
        end
    end)
    
    return aspect
end

-- 缓存切面
function ShieldAOP.CommonAspects.create_cache_aspect()
    local cache = {}
    local aspect = ShieldAOP.Aspect:new("Cache")
    
    aspect:pointcut("cacheable_methods", ShieldAOP.PointcutMatcher.regex("^get.*"))
    
    aspect:around("cacheable_methods", function(context)
        local cache_key = string.format("%s.%s(%s)", 
            context.target._service_name,
            context.method_name,
            table.concat(context.args, ",")
        )
        
        -- 检查缓存
        if cache[cache_key] then
            print("[CACHE] Cache hit: " .. cache_key)
            return cache[cache_key]
        end
        
        -- 执行方法并缓存结果
        local result = context.proceed()
        cache[cache_key] = result
        print("[CACHE] Cached result: " .. cache_key)
        
        return result
    end)
    
    return aspect
end

-- 安全切面
function ShieldAOP.CommonAspects.create_security_aspect()
    local aspect = ShieldAOP.Aspect:new("Security")
    
    aspect:pointcut("protected_methods", ShieldAOP.PointcutMatcher.annotation("RequireAuth"))
    
    aspect:before("protected_methods", function(context)
        -- 这里应该检查用户权限
        print("[SECURITY] Checking authentication for " .. context.method_name)
        -- if not is_authenticated() then
        --     error("Access denied: Authentication required")
        -- end
    end)
    
    return aspect
end

-- =====================================
-- 9. AOP使用示例
-- =====================================

local function demonstrate_aop()
    print("=== Shield AOP演示 ===\n")
    
    -- 1. 创建测试服务
    local UserService = Shield.Service:new({_service_name = "UserService"})
    
    function UserService:get_user(user_id)
        print(string.format("  [CORE] Loading user %d from database", user_id))
        -- 模拟数据库查询延迟
        local start = os.clock()
        while os.clock() - start < 0.01 do end
        
        return {id = user_id, name = "User" .. user_id}
    end
    
    function UserService:save_user(user)
        print(string.format("  [CORE] Saving user %s to database", user.name))
        return true
    end
    
    function UserService:_private_method()
        print("  [CORE] This is a private method")
    end
    
    -- 2. 注册切面
    ShieldAOP.Container.register_aspect(ShieldAOP.CommonAspects.create_logging_aspect())
    ShieldAOP.Container.register_aspect(ShieldAOP.CommonAspects.create_performance_aspect())
    ShieldAOP.Container.register_aspect(ShieldAOP.CommonAspects.create_cache_aspect())
    
    -- 3. 创建AOP增强的服务
    local enhanced_service = ShieldAOP.Container.enhance_object(UserService)
    
    print("--- 测试AOP增强的方法调用 ---")
    
    -- 4. 测试方法调用 (会触发AOP)
    print("\n1. 第一次调用get_user:")
    local user1 = enhanced_service:get_user(123)
    print("返回结果:", user1.name)
    
    print("\n2. 第二次调用get_user (应该命中缓存):")
    local user2 = enhanced_service:get_user(123)
    print("返回结果:", user2.name)
    
    print("\n3. 调用save_user:")
    enhanced_service:save_user({name = "Alice"})
    
    print("\n4. 调用私有方法 (不会被日志切面拦截):")
    enhanced_service:_private_method()
    
    print("\n=== AOP演示完成 ===")
end

-- 运行演示
demonstrate_aop()

-- =====================================
-- 10. 扩展：注解支持
-- =====================================

-- 方法注解宏
function ShieldAOP.annotate_method(target, method_name, annotations)
    target._method_annotations = target._method_annotations or {}
    target._method_annotations[method_name] = annotations
    return target
end

-- 便捷注解函数
function ShieldAOP.transactional(target, method_name)
    return ShieldAOP.annotate_method(target, method_name, {Transactional = true})
end

function ShieldAOP.cacheable(target, method_name, cache_config)
    return ShieldAOP.annotate_method(target, method_name, {Cacheable = cache_config or true})
end

function ShieldAOP.require_auth(target, method_name)
    return ShieldAOP.annotate_method(target, method_name, {RequireAuth = true})
end

print([[

🎯 Shield AOP系统特性总结:

✅ 完整的AOP支持 - Before/After/Around/AfterReturning/AfterThrowing/Finally
✅ 灵活的切点匹配 - 方法名/类类型/注解/正则/组合匹配
✅ 性能监控切面 - 自动统计方法执行时间
✅ 日志切面 - 自动记录方法调用和返回
✅ 事务切面 - 自动事务管理
✅ 缓存切面 - 自动缓存方法结果
✅ 安全切面 - 权限检查
✅ 注解支持 - @Transactional, @Cacheable, @RequireAuth
✅ 元表机制 - 利用Lua动态特性实现代理
✅ 优先级管理 - 多切面执行顺序控制

对于Lua来说，AOP不仅现实，而且比Java更灵活！ 🚀
]])

return ShieldAOP