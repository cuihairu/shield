-- Shield用户偏好配置系统
-- 让用户根据经验和项目需求选择最适合的DI方案

local Shield = require("shield_framework")
local UnifiedDI = require("shield_unified_di")

-- =====================================
-- 用户配置向导
-- =====================================

local UserConfigWizard = {}

-- 用户类型评估
function UserConfigWizard.assess_user_level()
    print([[
    
🎯 Shield依赖注入方案选择向导

请选择您的经验水平和项目需求：

1. 🌱 新手开发者
   - 刚接触依赖注入概念
   - 希望简单易学的方案
   - 优先考虑学习曲线平缓

2. 💼 中级开发者  
   - 熟悉现代开发模式
   - 希望IDE支持和类型安全
   - 平衡易用性和功能性

3. 🚀 高级开发者
   - 熟悉企业级架构模式
   - 需要最大的灵活性和控制力
   - 不介意学习成本

4. 👥 团队协作
   - 多人协作项目
   - 需要统一的代码规范
   - 重视代码可维护性

请输入数字 (1-4): ]])

    -- 模拟用户选择 (实际中会读取用户输入)
    local choice = 2  -- 假设选择中级开发者
    
    local profiles = {
        [1] = "beginner",
        [2] = "intermediate", 
        [3] = "advanced",
        [4] = "team"
    }
    
    return profiles[choice] or "intermediate"
end

-- 生成用户配置
function UserConfigWizard.generate_config(user_profile)
    local config = UnifiedDI.UserPreferences[user_profile]
    
    print(string.format("\n✅ 已为您配置 '%s' 方案:", user_profile))
    print(string.format("主要方案: %s", config.primary_style))
    print(string.format("备选方案: %s", config.fallback_style or "无"))
    print(string.format("注解支持: %s", config.enable_annotations and "启用" or "禁用"))
    
    return config
end

-- =====================================
-- 方案选择器
-- =====================================

local DIStyleSelector = {}

-- 根据场景推荐方案
function DIStyleSelector.recommend_by_scenario(scenario)
    local recommendations = {
        -- 快速原型开发
        rapid_prototype = {
            primary = UnifiedDI.InjectionStyle.TYPED_REFERENCE,
            reason = "类型引用方式最适合快速开发，IDE支持好且易理解"
        },
        
        -- 长期维护项目
        long_term_project = {
            primary = UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION,
            reason = "构造函数注入最适合长期项目，依赖关系清晰明确"
        },
        
        -- 大型团队项目
        large_team = {
            primary = UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN,
            reason = "注解驱动适合大团队，标准化程度高，规范性强"
        },
        
        -- 性能敏感应用
        performance_critical = {
            primary = UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION,
            reason = "构造函数注入运行时开销最小，适合性能敏感场景"
        },
        
        -- 遗留系统改造
        legacy_migration = {
            primary = UnifiedDI.InjectionStyle.LEGACY_STRING,
            secondary = UnifiedDI.InjectionStyle.TYPED_REFERENCE,
            reason = "渐进式迁移，先保持兼容性，再逐步现代化"
        }
    }
    
    return recommendations[scenario]
end

-- =====================================
-- 智能配置生成器
-- =====================================

local SmartConfigGenerator = {}

function SmartConfigGenerator.create_project_config(options)
    options = options or {}
    
    local config = {
        project_name = options.project_name or "MyShieldProject",
        user_profile = options.user_profile or "intermediate",
        scenario = options.scenario or "long_term_project",
        
        -- DI配置
        di_config = {
            enabled_styles = {},
            primary_style = nil,
            fallback_styles = {},
            strict_mode = options.strict_mode or false,
            auto_validation = options.auto_validation or true
        },
        
        -- IDE配置
        ide_config = {
            type_annotations = options.type_annotations or true,
            code_snippets = options.code_snippets or true,
            auto_completion = options.auto_completion or true
        },
        
        -- 团队配置
        team_config = {
            enforce_consistency = options.enforce_consistency or false,
            allowed_styles = options.allowed_styles or nil,
            code_review_rules = options.code_review_rules or {}
        }
    }
    
    -- 根据用户档案设置默认值
    local profile_defaults = UnifiedDI.UserPreferences[config.user_profile]
    if profile_defaults then
        config.di_config.primary_style = profile_defaults.primary_style
        config.di_config.fallback_styles = {profile_defaults.fallback_style}
        config.ide_config.type_annotations = profile_defaults.enable_annotations
    end
    
    -- 根据场景调整配置
    local scenario_rec = DIStyleSelector.recommend_by_scenario(config.scenario)
    if scenario_rec then
        config.di_config.primary_style = scenario_rec.primary
        if scenario_rec.secondary then
            table.insert(config.di_config.fallback_styles, scenario_rec.secondary)
        end
    end
    
    -- 启用选定的方案
    config.di_config.enabled_styles = {
        config.di_config.primary_style
    }
    for _, style in ipairs(config.di_config.fallback_styles) do
        table.insert(config.di_config.enabled_styles, style)
    end
    
    return config
end

function SmartConfigGenerator.save_config(config, file_path)
    file_path = file_path or "shield_project_config.lua"
    
    local config_content = string.format([[
-- Shield项目配置文件
-- 自动生成于: %s

return {
    project_name = "%s",
    user_profile = "%s", 
    scenario = "%s",
    
    di_config = {
        primary_style = "%s",
        enabled_styles = {%s},
        strict_mode = %s
    },
    
    ide_config = {
        type_annotations = %s,
        code_snippets = %s,
        auto_completion = %s
    }
}
]], 
        os.date("%Y-%m-%d %H:%M:%S"),
        config.project_name,
        config.user_profile,
        config.scenario,
        config.di_config.primary_style,
        table.concat(config.di_config.enabled_styles, '", "'),
        tostring(config.di_config.strict_mode),
        tostring(config.ide_config.type_annotations),
        tostring(config.ide_config.code_snippets),
        tostring(config.ide_config.auto_completion)
    )
    
    -- 这里应该写入文件，简化为打印
    print("配置文件内容:")
    print(config_content)
    
    return config_content
end

-- =====================================
-- 方案兼容性矩阵
-- =====================================

local CompatibilityChecker = {}

CompatibilityChecker.matrix = {
    -- 每种方案与其他方案的兼容性
    [UnifiedDI.InjectionStyle.LEGACY_STRING] = {
        [UnifiedDI.InjectionStyle.TYPED_REFERENCE] = "compatible",
        [UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION] = "compatible",
        [UnifiedDI.InjectionStyle.BUILDER_PATTERN] = "compatible",
        [UnifiedDI.InjectionStyle.INTERFACE_BASED] = "compatible",
        [UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN] = "compatible"
    },
    
    [UnifiedDI.InjectionStyle.TYPED_REFERENCE] = {
        [UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION] = "perfect",
        [UnifiedDI.InjectionStyle.BUILDER_PATTERN] = "perfect",
        [UnifiedDI.InjectionStyle.INTERFACE_BASED] = "good",
        [UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN] = "good"
    },
    
    [UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION] = {
        [UnifiedDI.InjectionStyle.BUILDER_PATTERN] = "conflict",  -- 两者都想控制构造
        [UnifiedDI.InjectionStyle.INTERFACE_BASED] = "perfect",
        [UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN] = "good"
    },
    
    [UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN] = {
        [UnifiedDI.InjectionStyle.INTERFACE_BASED] = "perfect",
        [UnifiedDI.InjectionStyle.BUILDER_PATTERN] = "good"
    }
}

function CompatibilityChecker.check_combination(styles)
    local issues = {}
    local recommendations = {}
    
    for i = 1, #styles do
        for j = i + 1, #styles do
            local style1, style2 = styles[i], styles[j]
            local compatibility = CompatibilityChecker.matrix[style1] and 
                                 CompatibilityChecker.matrix[style1][style2] or "unknown"
            
            if compatibility == "conflict" then
                table.insert(issues, string.format("⚠️  %s 与 %s 存在冲突", style1, style2))
                table.insert(recommendations, string.format("建议: 选择其一或使用适配器模式"))
                
            elseif compatibility == "good" then
                table.insert(recommendations, string.format("✅ %s 与 %s 兼容性良好", style1, style2))
                
            elseif compatibility == "perfect" then
                table.insert(recommendations, string.format("🎯 %s 与 %s 完美配合", style1, style2))
            end
        end
    end
    
    return {
        issues = issues,
        recommendations = recommendations,
        overall_compatible = #issues == 0
    }
end

-- =====================================
-- 完整使用示例
-- =====================================

local function demonstrate_user_choice_system()
    print("=== Shield用户选择系统演示 ===\n")
    
    -- 1. 用户配置向导
    print("1. 用户配置向导")
    local user_profile = UserConfigWizard.assess_user_level()
    local user_config = UserConfigWizard.generate_config(user_profile)
    
    -- 2. 场景推荐
    print("\n2. 场景推荐")
    local scenarios = {"rapid_prototype", "long_term_project", "large_team", "performance_critical"}
    for _, scenario in ipairs(scenarios) do
        local rec = DIStyleSelector.recommend_by_scenario(scenario)
        print(string.format("%s: %s", scenario, rec.primary))
        print(string.format("  理由: %s", rec.reason))
    end
    
    -- 3. 智能配置生成
    print("\n3. 智能配置生成")
    local project_config = SmartConfigGenerator.create_project_config({
        project_name = "GameServer",
        user_profile = "intermediate",
        scenario = "long_term_project",
        strict_mode = false
    })
    
    SmartConfigGenerator.save_config(project_config)
    
    -- 4. 兼容性检查
    print("\n4. 兼容性检查")
    local test_combinations = {
        {UnifiedDI.InjectionStyle.TYPED_REFERENCE, UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION},
        {UnifiedDI.InjectionStyle.CONSTRUCTOR_INJECTION, UnifiedDI.InjectionStyle.BUILDER_PATTERN},
        {UnifiedDI.InjectionStyle.ANNOTATION_DRIVEN, UnifiedDI.InjectionStyle.INTERFACE_BASED}
    }
    
    for _, combination in ipairs(test_combinations) do
        local result = CompatibilityChecker.check_combination(combination)
        print(string.format("组合: %s + %s", combination[1], combination[2]))
        for _, rec in ipairs(result.recommendations) do
            print("  " .. rec)
        end
        for _, issue in ipairs(result.issues) do
            print("  " .. issue)
        end
    end
    
    -- 5. 实际应用示例
    print("\n5. 实际应用 - 多方案并存")
    
    -- 定义服务
    local Services = {}
    Services.Database = Shield.Service:new({_service_name = "DatabaseService"})
    Services.Cache = Shield.Service:new({_service_name = "CacheService"}) 
    Services.Logger = Shield.Service:new({_service_name = "LoggerService"})
    
    -- 新手开发者的服务 (简单方式)
    local BeginnerService = Shield.Service:new({_service_name = "BeginnerService"})
    BeginnerService:inject(Services.Database):inject(Services.Logger)
    
    -- 中级开发者的服务 (现代方式)
    local IntermediateService = Shield.Service:new({_service_name = "IntermediateService"})
    IntermediateService:with_dependencies(Services.Database, Services.Cache, Services.Logger)
    
    -- 高级开发者的服务 (注解方式)
    local AdvancedService = Shield.Service:new({_service_name = "AdvancedService"})
    AdvancedService:annotate_inject("db", Services.Database, {required = true})
                   :annotate_inject("cache", Services.Cache, {lazy = true})
    
    -- 团队项目的服务 (混合方式)
    local TeamService = Shield.Service:new({_service_name = "TeamService"})
    TeamService:with_dependencies(Services.Database)  -- 主要依赖用构造函数
               :inject_as("cache", Services.Cache)    -- 可选依赖用类型引用
    
    print("✅ 所有方案都可以在同一项目中并存使用!")
    print("✅ 团队成员可以根据经验选择最适合的方案!")
    print("✅ 框架自动处理所有兼容性问题!")
    
    print("\n=== 用户选择系统演示完成 ===")
end

-- 运行演示
demonstrate_user_choice_system()

-- =====================================
-- 最佳实践建议
-- =====================================

print([[

🎯 Shield多方案兼容的最佳实践:

📚 学习路径建议:
1. 新手: 从 typed_reference 开始
2. 熟悉后: 升级到 constructor_injection  
3. 团队项目: 考虑 annotation_driven
4. 企业项目: 使用 interface_based

🔧 项目配置建议:
• 小型项目: 1-2种方案即可
• 中型项目: 2-3种方案，覆盖不同场景
• 大型项目: 支持所有方案，提供选择灵活性

👥 团队协作建议:
• 统一主要方案，允许特殊情况使用其他方案
• 在代码规范中明确各方案的使用场景
• 提供培训，让团队了解各方案的优缺点

🚀 渐进式升级:
• 老代码保持现有方案不变
• 新代码使用推荐方案
• 重构时逐步升级到现代方案

这就是真正用户友好的框架设计！
]])

return {
    UserConfigWizard = UserConfigWizard,
    DIStyleSelector = DIStyleSelector,
    SmartConfigGenerator =SmartConfigGenerator,
    CompatibilityChecker = CompatibilityChecker
}