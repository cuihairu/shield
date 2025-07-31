-- 完整的Shield继承驱动框架使用示例
-- 展示"只写Lua代码，自动被框架管理"的开发体验

local Shield = require("shield_framework")
local AutoManager = require("shield_auto_manager")
local IoCContainer = require("lua_ioc_container")

-- =====================================
-- 完整的游戏服务示例
-- =====================================

print("=== Shield继承驱动框架演示 ===\n")

-- 创建IoC容器
local container = IoCContainer.LuaIoC:new()

-- 1. 自动扫描和注册所有Shield服务
print("--- 自动扫描服务 ---")
local init_order = AutoManager.auto_discover_and_register("scripts/services", container)

-- 2. 显示统计信息
AutoManager.print_statistics()

-- 3. 模拟启动应用
print("--- 启动应用 ---")
container:start_all()
AutoManager.start_all_services(container)

-- 4. 模拟业务逻辑执行
print("\n--- 执行业务逻辑 ---")

-- 获取服务实例
local player_service = container:resolve("PlayerService")
local player_repo = container:resolve("PlayerRepository") 
local game_config = container:resolve("GameConfiguration")

if player_service then
    print("[Demo] 模拟玩家登录...")
    local player_data = player_service:handle_player_login(123, {ip = "127.0.0.1"})
    print("[Demo] 玩家登录成功")
    
    print("[Demo] 当前在线玩家数:", player_service:get_online_player_count())
end

if game_config then
    print("[Demo] 最大玩家数配置:", game_config:get_max_players())
    print("[Demo] PVP是否启用:", game_config:is_pvp_enabled())
end

-- 5. 发布事件测试
print("\n--- 事件系统测试 ---")
container:publish_event("player_level_up", {
    player_id = 123,
    level = 15,
    timestamp = os.time()
})

-- 6. 健康检查测试
print("\n--- 健康检查测试 ---")
local health = container:check_health()
print("整体健康状态:", health.status)
for component, result in pairs(health.components) do
    print(string.format("  %s: %s", component, result.status))
    if result.details then
        for key, value in pairs(result.details) do
            print(string.format("    %s: %s", key, value))
        end
    end
end

-- 7. 模拟热重载
print("\n--- 热重载测试 ---")
print("[Demo] 模拟配置文件变更...")
container:reload_config()

-- 8. 停止应用
print("\n--- 停止应用 ---")
AutoManager.stop_all_services()
container:stop_all()

print("\n=== 演示完成 ===")

-- =====================================
-- 核心优势总结
-- =====================================

print([[

🎉 Shield继承驱动框架的核心优势：

1. 💻 **纯Lua开发体验**
   • 开发者只需要继承Shield基类
   • 无需写任何C++代码
   • 无需手动注册服务

2. 🔍 **智能自动发现**
   • 自动扫描scripts/目录
   • 只管理继承Shield基类的对象
   • 普通Lua类不受影响

3. 📦 **完整的依赖注入**
   • 声明式依赖: depends_on("ServiceA", "ServiceB")
   • 自动解析和注入
   • 循环依赖检测

4. 🔄 **生命周期管理**
   • 自动调用on_init/on_start/on_stop
   • 按依赖关系排序初始化
   • 支持配置热重载

5. 📊 **企业级功能**
   • 事件驱动架构
   • 健康检查监控
   • 统计和调试信息

6. 🎮 **游戏友好设计**
   • Repository模式用于数据访问
   • Controller模式用于API
   • EventListener用于游戏事件
   • 支持热重载，适合快速迭代

使用方式极其简单：
```lua
-- 1. 继承Shield基类
local MyService = Shield.Service:new({_service_name = "MyService"})

-- 2. 声明依赖
MyService:depends_on("DatabaseService")

-- 3. 实现生命周期
function MyService:on_init(container)
    self.db = container:resolve("DatabaseService")
end

-- 4. 框架自动发现和管理！
```

这就是现代化的Lua开发体验！🚀
]])

return {
    container = container,
    auto_manager = AutoManager
}