-- Lua脚本示例：如何使用配置
-- scripts/game_logic.lua

-- 配置已经通过 config.bind_to_lua() 绑定到全局变量 config

print("Game Logic Initialized")
print("Max Players: " .. config.game.max_players)
print("World Size: " .. config.game.world_size)
print("Difficulty: " .. config.game.difficulty)

-- 根据配置设置游戏逻辑
local max_players = config.game.max_players
local spawn_points = config.game.spawn_points

-- 游戏逻辑函数
function initialize_game()
    print("Initializing game with " .. max_players .. " max players")
    
    -- 使用配置中的出生点
    for i, spawn_point in ipairs(spawn_points) do
        print("Spawn point " .. i .. ": (" .. spawn_point[1] .. ", " .. spawn_point[2] .. ", " .. spawn_point[3] .. ")")
    end
end

-- 根据配置调整难度
function get_enemy_strength()
    local difficulty = config.game.difficulty
    if difficulty == "easy" then
        return 0.5
    elseif difficulty == "normal" then
        return 1.0
    elseif difficulty == "hard" then
        return 1.5
    else
        return 1.0
    end
end