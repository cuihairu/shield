# 实体与组件化运行时语义

本文档包含 Shield 游戏实体（怪物、NPC、场景对象等）的抽象方式和组件化设计决策。

## 设计原则

- Actor/Service 是重量级执行单元（独立 Lua VM），仅用于需要独立生命周期和网络通信的实体。
- 游戏实体（怪物、NPC、掉落物等）是轻量级数据（Lua 表），由 Room Service 统一管理。
- 组件化设计避免继承爆炸，支持按需组合能力。

## 实体分层

```
┌─────────────────────────────────────────────────────────────┐
│                    抽象层级                                  │
├─────────────────────────────────────────────────────────────┤
│  Actor/Service    │ 独立 Lua VM、独立生命周期               │
│  (Player等)       │ 适合: 有网络连接、需要跨节点通信        │
├─────────────────────────────────────────────────────────────┤
│  Entity           │ Lua 表、Room 内管理                     │
│  (Monster/NPC)    │ 适合: 无独立网络、生命周期由 Room 管理  │
├─────────────────────────────────────────────────────────────┤
│  Component        │ Entity 的能力模块                       │
│  (AI/Movement等)  │ 适合: 可复用的行为逻辑                 │
└─────────────────────────────────────────────────────────────┘
```

### 选择标准

| 场景 | 推荐方式 | 原因 |
|------|----------|------|
| 玩家 Avatar | Actor/Service | 独立网络、跨节点通信 |
| 怪物 | Entity + Components | 数据驱动、内存高效 |
| NPC | Entity + Components | 通常不需要独立网络 |
| Room/场景 | Actor/Service | 管理多个 Entity、独立生命周期 |
| 公会/队伍 | Actor/Service | 需要跨玩家协调 |
| 掉落物 | Entity (简化版) | 短生命周期、纯数据 |

## 为什么用组件化

### 继承的问题

```lua
-- 继承方式：类爆炸
local Monster = {}    -- 战斗 + 掉落
local NPC = {}        -- 对话
local Boss = {}       -- 战斗 + 对话 + 掉落？继承谁？
local MerchantNPC = {} -- 对话 + 交易
local CombatPet = {}   -- 战斗 + 跟随
-- 每种组合都要写一个类，爆炸式增长
```

### 组件化解决

```lua
-- 组件化：能力是独立的，按需组合
local boss = Entity.new("boss_001")
boss:add_component("Attributes", {hp=10000, attack=500})
boss:add_component("AIBrain", {ai_type="boss"})
boss:add_component("Movement", {speed=80})
boss:add_component("Combat", {})
boss:add_component("Dialogue", {})  -- Boss 也能说话

local merchant = Entity.new("merchant_001")
merchant:add_component("Attributes", {hp=999999})
merchant:add_component("Dialogue", {})
merchant:add_component("Shop", {})   -- 多了商店组件

local slime = Entity.new("slime_001")
slime:add_component("Attributes", {hp=100, attack=10})
slime:add_component("AIBrain", {ai_type="aggressive"})
slime:add_component("Movement", {speed=50})
slime:add_component("Loot", {})      -- 会掉东西
```

**一句话总结：组件化 = 乐高积木，想拼什么就拼什么。**

## Room Service

Room Service 是场景容器，管理该场景内所有 Entity。

```lua
-- room_service.lua
local M = {}

function M.on_init(args)
    M.room_id = args.room_id
    M.entities = {}           -- 所有实体
    M.entity_counter = 0      -- 本地实体 ID 计数器
    M.tick_interval = 50      -- 20fps
end

-- 实体注册
function M.spawn_entity(template_id, position)
    M.entity_counter = M.entity_counter + 1
    local eid = M.entity_counter

    local entity, err = EntityFactory.create(template_id, {
        eid = eid,
        room = M.room_id,
        position = position,
    })

    if not entity then
        return nil, err
    end

    M.entities[eid] = entity
    return eid
end

-- 实体销毁
function M.destroy_entity(eid)
    local entity = M.entities[eid]
    if entity then
        entity:on_destroy()
        M.entities[eid] = nil
    end
end

-- 每帧更新
function M.on_timer()
    for eid, entity in pairs(M.entities) do
        entity:update(M.tick_interval)

        if entity:is_dead() then
            M:destroy_entity(eid)
        end
    end
end

-- 查询
function M.get_entity(eid)
    return M.entities[eid]
end

function M.get_entities_in_range(pos, radius)
    local result = {}
    for eid, entity in pairs(M.entities) do
        if distance(pos, entity.position) <= radius then
            result[#result + 1] = entity
        end
    end
    return result
end

return M
```

## Entity 基类

```lua
-- entity.lua
local Entity = {}
Entity.__index = Entity

function Entity.new(template_id, opts)
    local self = setmetatable({}, Entity)

    -- 基础属性
    self.eid = opts.eid
    self.template_id = template_id
    self.room = opts.room
    self.position = opts.position or {x=0, y=0, z=0}

    -- 组件系统
    self.components = {}

    -- 模板数据（由工厂注入）
    self.template = nil

    return self
end

-- 组件注册
function Entity:add_component(name, config)
    local component = ComponentRegistry.create(name, config)
    self.components[name] = component
    component.entity = self

    if component.on_attach then
        component:on_attach()
    end
end

function Entity:get_component(name)
    return self.components[name]
end

function Entity:has_component(name)
    return self.components[name] ~= nil
end

-- 统一更新
function Entity:update(dt)
    for _, component in pairs(self.components) do
        if component.update then
            component:update(dt)
        end
    end
end

-- 销毁
function Entity:on_destroy()
    for _, component in pairs(self.components) do
        if component.on_detach then
            component:on_detach()
        end
    end
end

-- 死亡判断（可由组件覆写）
function Entity:is_dead()
    local attrs = self:get_component("Attributes")
    return attrs and attrs.hp <= 0
end

return Entity
```

## 组件系统

### 组件基类

```lua
-- component.lua
local Component = {}
Component.__index = Component

function Component.new(config)
    local self = setmetatable({}, Component)
    self.config = config or {}
    self.entity = nil  -- 由 Entity:add_component 设置
    return self
end

-- 生命周期钩子
function Component:on_attach() end   -- 挂载到 Entity 时
function Component:on_detach() end   -- 从 Entity 卸载时
function Component:update(dt) end    -- 每帧更新

return Component
```

### 组件注册表

```lua
-- component_registry.lua
local ComponentRegistry = {}

local registered = {}

function ComponentRegistry.register(name, class)
    registered[name] = class
end

function ComponentRegistry.create(name, config)
    local class = registered[name]
    if not class then
        error("component not found: " .. name)
    end
    return class.new(config)
end

return ComponentRegistry
```

### 内置组件

#### Attributes（属性）

```lua
-- components/attributes.lua
local Attributes = setmetatable({}, {__index = Component})
Attributes.__index = Attributes

function Attributes.new(config)
    local self = Component.new(config)
    setmetatable(self, Attributes)
    return self
end

function Attributes:on_attach()
    local template = self.entity.template
    self.max_hp = self.config.max_hp or template.max_hp or 100
    self.hp = self.config.hp or self.max_hp
    self.max_mp = self.config.max_mp or template.max_mp or 0
    self.mp = self.config.mp or self.max_mp
    self.attack = self.config.attack or template.attack or 10
    self.defense = self.config.defense or template.defense or 0
    self.speed = self.config.speed or template.speed or 100
end

function Attributes:take_damage(amount)
    local actual = math.max(1, amount - self.defense)
    self.hp = math.max(0, self.hp - actual)
    return actual, self.hp <= 0
end

function Attributes:heal(amount)
    local actual = math.min(amount, self.max_hp - self.hp)
    self.hp = self.hp + actual
    return actual
end

ComponentRegistry.register("Attributes", Attributes)
```

#### AIBrain（AI 行为）

```lua
-- components/ai_brain.lua
local AIBrain = setmetatable({}, {__index = Component})
AIBrain.__index = AIBrain

function AIBrain.new(config)
    local self = Component.new(config)
    setmetatable(self, AIBrain)
    return self
end

function AIBrain:on_attach()
    self.state = "idle"
    self.target = nil
    self.state_timer = 0
    self.ai_type = self.config.ai_type or "aggressive"
end

function AIBrain:update(dt)
    self.state_timer = self.state_timer + dt

    if self.state == "idle" then
        self:idle_behavior(dt)
    elseif self.state == "patrol" then
        self:patrol_behavior(dt)
    elseif self.state == "chase" then
        self:chase_behavior(dt)
    elseif self.state == "attack" then
        self:attack_behavior(dt)
    end
end

function AIBrain:idle_behavior(dt)
    -- 主动型：寻找敌人
    if self.ai_type == "aggressive" then
        local enemy = self:find_nearest_enemy()
        if enemy then
            self.target = enemy
            self.state = "chase"
            return
        end
    end

    -- 空闲超时后巡逻
    if self.state_timer > 3000 then
        self.state = "patrol"
        self.state_timer = 0
    end
end

function AIBrain:chase_behavior(dt)
    local movement = self.entity:get_component("Movement")
    if not movement or not self.target then
        self.state = "idle"
        return
    end

    local dist = distance(self.entity.position, self.target.position)
    if dist < 50 then
        self.state = "attack"
        self.state_timer = 0
    else
        movement:move_to(self.target.position)
    end
end

function AIBrain:attack_behavior(dt)
    if not self.target or self.target:is_dead() then
        self.target = nil
        self.state = "idle"
        return
    end

    local combat = self.entity:get_component("Combat")
    if combat and self.state_timer > 1000 then
        combat:attack(self.target)
        self.state_timer = 0
    end
end

function AIBrain:find_nearest_enemy()
    -- 通过 Room 查询附近玩家
    -- 实现略
    return nil
end

ComponentRegistry.register("AIBrain", AIBrain)
```

#### Movement（移动）

```lua
-- components/movement.lua
local Movement = setmetatable({}, {__index = Component})
Movement.__index = Movement

function Movement.new(config)
    local self = Component.new(config)
    setmetatable(self, Movement)
    return self
end

function Movement:on_attach()
    self.speed = self.config.speed or self.entity.template.speed or 100
    self.path = {}
    self.target_pos = nil
end

function Movement:move_to(target_pos)
    self.target_pos = target_pos
    -- 简化：直线移动，实际项目用寻路算法
    self.path = {target_pos}
end

function Movement:update(dt)
    if #self.path == 0 then
        return
    end

    local next_pos = self.path[1]
    local pos = self.entity.position
    local dx = next_pos.x - pos.x
    local dz = next_pos.z - pos.z
    local dist = math.sqrt(dx*dx + dz*dz)

    if dist < 1 then
        table.remove(self.path, 1)
        return
    end

    local step = self.speed * dt / 1000
    local ratio = math.min(step / dist, 1)
    self.entity.position = {
        x = pos.x + dx * ratio,
        y = pos.y,
        z = pos.z + dz * ratio,
    }
end

ComponentRegistry.register("Movement", Movement)
```

#### Combat（战斗）

```lua
-- components/combat.lua
local Combat = setmetatable({}, {__index = Component})
Combat.__index = Combat

function Combat.new(config)
    local self = Component.new(config)
    setmetatable(self, Combat)
    return self
end

function Combat:on_attach()
    self.attack_range = self.config.attack_range or 50
    self.attack_cooldown = 0
end

function Combat:update(dt)
    if self.attack_cooldown > 0 then
        self.attack_cooldown = math.max(0, self.attack_cooldown - dt)
    end
end

function Combat:attack(target)
    if self.attack_cooldown > 0 then
        return false, "cooldown"
    end

    local attrs = self.entity:get_component("Attributes")
    if not attrs then
        return false, "no_attributes"
    end

    local target_attrs = target:get_component("Attributes")
    if not target_attrs then
        return false, "target_no_attributes"
    end

    local damage, killed = target_attrs:take_damage(attrs.attack)
    self.attack_cooldown = 1000  -- 1 秒冷却

    return true, {damage = damage, killed = killed}
end

ComponentRegistry.register("Combat", Combat)
```

#### Loot（掉落）

```lua
-- components/loot.lua
local Loot = setmetatable({}, {__index = Component})
Loot.__index = Loot

function Loot.new(config)
    local self = Component.new(config)
    setmetatable(self, Loot)
    return self
end

function Loot:on_attach()
    self.loot_table = self.config.loot_table or self.entity.template.loot_table or {}
end

function Loot:generate_drops()
    local drops = {}

    for _, entry in ipairs(self.loot_table) do
        if math.random() < (entry.chance or 1.0) then
            local count = math.random(entry.min or 1, entry.max or 1)
            drops[#drops + 1] = {
                item = entry.item,
                count = count,
            }
        end
    end

    return drops
end

ComponentRegistry.register("Loot", Loot)
```

#### Dialogue（对话）

```lua
-- components/dialogue.lua
local Dialogue = setmetatable({}, {__index = Component})
Dialogue.__index = Dialogue

function Dialogue.new(config)
    local self = Component.new(config)
    setmetatable(self, Dialogue)
    return self
end

function Dialogue:on_attach()
    self.dialogue_id = self.config.dialogue_id or self.entity.template.dialogue_id
end

function Dialogue:get_dialogue(player)
    -- 从配置或数据库加载对话树
    return DialogueManager.get(self.dialogue_id, player)
end

ComponentRegistry.register("Dialogue", Dialogue)
```

## 模板配置

### 模板数据结构

```yaml
# templates/monsters.yaml
slime:
  type: monster
  max_hp: 100
  attack: 10
  defense: 2
  speed: 50
  components:
    - Attributes
    - AIBrain
    - Movement
    - Combat
    - Loot
  ai_type: aggressive
  attack_range: 30
  loot_table:
    - item: gold
      min: 1
      max: 10
      chance: 1.0
    - item: potion
      min: 1
      max: 1
      chance: 0.3

boss_dragon:
  type: monster
  max_hp: 100000
  attack: 500
  defense: 100
  speed: 80
  components:
    - Attributes
    - AIBrain
    - Movement
    - Combat
    - Dialogue
    - Loot
  ai_type: boss
  attack_range: 100
  dialogue_id: dragon_boss
  loot_table:
    - item: dragon_scale
      min: 1
      max: 3
      chance: 0.5
    - item: legendary_sword
      min: 1
      max: 1
      chance: 0.01

npc_merchant:
  type: npc
  max_hp: 999999
  components:
    - Attributes
    - Dialogue
    - Shop
  dialogue_id: merchant_main
```

### Entity 工厂

```lua
-- entity_factory.lua
local EntityFactory = {}

local templates = {}  -- 从 YAML 加载

function EntityFactory.load(config_path)
    templates = yaml.load(config_path)
end

function EntityFactory.create(template_id, opts)
    local template = templates[template_id]
    if not template then
        return nil, "template_not_found: " .. template_id
    end

    local entity = Entity.new(template_id, opts)
    entity.template = template

    -- 按模板添加组件
    for _, comp_name in ipairs(template.components or {}) do
        local config = {}
        -- 从模板提取组件专属配置
        if comp_name == "AIBrain" then
            config.ai_type = template.ai_type
        elseif comp_name == "Combat" then
            config.attack_range = template.attack_range
        elseif comp_name == "Loot" then
            config.loot_table = template.loot_table
        elseif comp_name == "Dialogue" then
            config.dialogue_id = template.dialogue_id
        end

        entity:add_component(comp_name, config)
    end

    return entity
end

return EntityFactory
```

## 完整示例

### Room Service 使用

```lua
-- scripts/room.lua
local M = {}

function M.on_init(args)
    M.room_id = args.room_id
    M.entities = {}
    M.entity_counter = 0

    -- 加载场景配置
    M:load_scene(args.scene_id)

    -- 启动更新循环
    shield.timer(50, function()
        M:on_tick()
    end)

    return true
end

function M:load_scene(scene_id)
    local scene = SceneConfig.get(scene_id)

    -- 生成怪物
    for _, spawn in ipairs(scene.monsters) do
        for i = 1, spawn.count do
            local pos = randomize_position(spawn.position, spawn.radius)
            M:spawn_entity(spawn.template, pos)
        end
    end

    -- 生成 NPC
    for _, npc in ipairs(scene.npcs) do
        M:spawn_entity(npc.template, npc.position)
    end
end

function M:spawn_entity(template_id, position)
    M.entity_counter = M.entity_counter + 1
    local eid = M.entity_counter

    local entity, err = EntityFactory.create(template_id, {
        eid = eid,
        room = M.room_id,
        position = position,
    })

    if not entity then
        shield.log.error("spawn entity failed: " .. err)
        return nil
    end

    M.entities[eid] = entity
    return eid
end

function M:on_tick()
    for eid, entity in pairs(M.entities) do
        entity:update(50)

        -- 死亡处理
        if entity:is_dead() then
            self:on_entity_death(entity)
            M.entities[eid] = nil
        end
    end
end

function M:on_entity_death(entity)
    -- 生成掉落物
    local loot = entity:get_component("Loot")
    if loot then
        local drops = loot:generate_drops()
        -- 通知附近玩家
        self:broadcast_drops(entity.position, drops)
    end
end

-- RPC 接口
function M.get_nearby_entities(pos, radius)
    local result = {}
    for eid, entity in pairs(M.entities) do
        if distance(pos, entity.position) <= radius then
            result[#result + 1] = {
                eid = eid,
                template = entity.template_id,
                position = entity.position,
            }
        end
    end
    return result
end

function M.entity_take_damage(eid, damage)
    local entity = M.entities[eid]
    if not entity then
        return nil, "entity_not_found"
    end

    local attrs = entity:get_component("Attributes")
    if not attrs then
        return nil, "no_attributes"
    end

    local actual, killed = attrs:take_damage(damage)
    return {damage = actual, killed = killed}
end

return M
```

### Player 攻击怪物

```lua
-- player_service.lua 中
function M.attack_monster(room_id, monster_eid)
    -- 调用 Room Service 处理战斗
    local ok, result = shield.call("room." .. room_id, "entity_take_damage", monster_eid, M.attack_power)

    if ok then
        if result.killed then
            -- 怪物死亡，获取掉落
            self:add_drops(result.drops)
            self:add_exp(result.exp)
        end
    end
end
```

## 自定义组件

```lua
-- components/shop.lua（示例：商店组件）
local Shop = setmetatable({}, {__index = Component})
Shop.__index = Shop

function Shop.new(config)
    local self = Component.new(config)
    setmetatable(self, Shop)
    return self
end

function Shop:on_attach()
    self.shop_id = self.config.shop_id or self.entity.template.shop_id
    self.items = ShopConfig.get_items(self.shop_id)
end

function Shop:get_items()
    return self.items
end

function Shop:buy(player, item_id, count)
    local item = self:find_item(item_id)
    if not item then
        return nil, "item_not_found"
    end

    local total_price = item.price * count
    -- 检查玩家金币、扣款、发货逻辑
    -- ...

    return true, {item = item_id, count = count}
end

ComponentRegistry.register("Shop", Shop)
```

## 内存对比

| 方案 | 1000 怪物内存占用 | 说明 |
|------|------------------|------|
| Actor/Service | 1-2 GB | 每个怪物独立 Lua VM |
| Entity + Components | 1-5 MB | 纯 Lua 表，共享组件类 |

**结论：怪物、NPC 等大量同质实体应使用 Entity + Components，而非 Actor/Service。**
