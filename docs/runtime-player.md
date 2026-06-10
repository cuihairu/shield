# 玩家生命周期运行时语义

本文档包含 Shield 玩家生命周期管理、断线重连、离线消息缓存相关的运行时语义决策。

## 设计原则

- 玩家生命周期与连接生命周期分离。
- 断线不等于离线，支持短暂断线重连。
- 离线期间的消息不丢失。
- 状态管理由框架提供，业务逻辑由 Lua 实现。

## Player 与 Service 的关系

**Player 是 Service 的增强模式，不是独立的 Actor 类型。**

| 概念 | 说明 |
|------|------|
| Service | Shield 的基本执行单元，拥有 Lua VM、mailbox、name |
| Actor | Service 的同义词，强调消息驱动模型 |
| Player | 绑定网络连接的 Service，有额外的生命周期管理 |

**Player Service = 普通 Service + PlayerSession 管理**

```
┌─────────────────────────────────────────────────────────┐
│                   Player Service                        │
│  ┌────────────────────────────────────────────────────┐ │
│  │  Service 基础能力                                    │ │
│  │  - Lua VM                                          │ │
│  │  - Mailbox                                         │ │
│  │  - Name (可选)                                      │ │
│  │  - 消息处理 (send/call)                             │ │
│  └────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────┐ │
│  │  Player 增强能力                                    │ │
│  │  - PlayerSession (连接绑定)                         │ │
│  │  - 生命周期钩子 (on_auth/on_login/on_logout)        │ │
│  │  - 断线重连                                         │ │
│  │  - 离线消息缓存                                     │ │
│  │  - 数据持久化                                       │ │
│  └────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

**对比：**

```lua
-- 普通 Service（如 room、matchmaker）
local M = {}
function M.on_init(args) end
function M.on_exit(reason) end
function M.some_method() end
return M

-- Player Service（如 player、character）
local M = {}
function M.on_init(args) end
function M.on_exit(reason) end

-- 以下为 Player 特有的生命周期钩子
function M.on_auth(session, auth_data) end
function M.on_login(player) end
function M.on_message(player, msg_type, payload) end
function M.on_disconnect(player, reason) end
function M.on_reconnect(player) end
function M.on_logout(player, reason) end
function M.on_save(player) end

return M
```

## 玩家会话模型

```
┌─────────────────────────────────────────────────────────┐
│                    连接层 (shield_net)                   │
│  Connection ←→ Session                                  │
├─────────────────────────────────────────────────────────┤
│                    玩家层 (shield_player)                │
│  Session ←→ PlayerSession                               │
│            ├── 状态: connecting → authenticating         │
│            │              → online → disconnecting       │
│            │              → offline                      │
│            ├── 断线重连窗口                              │
│            └── 离线消息队列                              │
├─────────────────────────────────────────────────────────┤
│                    业务层 (Lua service)                  │
│  PlayerSession ←→ Player Service                        │
└─────────────────────────────────────────────────────────┘
```

## 玩家会话状态机

```
                    ┌──────────────┐
                    │  connecting  │
                    └──────┬───────┘
                           │ on_connect
                           ▼
                    ┌──────────────┐
          ┌────────│authenticating│────────┐
          │        └──────┬───────┘        │
          │               │ auth_ok        │ auth_fail
          ▼               ▼                ▼
    ┌──────────┐   ┌──────────┐    ┌──────────┐
    │ rejected │   │  online  │    │ rejected │
    └──────────┘   └────┬─────┘    └──────────┘
                        │
           ┌────────────┼────────────┐
           │            │            │
           ▼            ▼            ▼
    ┌────────────┐ ┌──────────┐ ┌──────────┐
    │ reconnect  │ │  kicked  │ │  logout  │
    │  window    │ │          │ │          │
    └──────┬─────┘ └──────────┘ └──────────┘
           │
     ┌─────┴─────┐
     ▼           ▼
┌─────────┐ ┌──────────┐
│ online  │ │  offline │
│(resume) │ │          │
└─────────┘ └──────────┘
```

## 玩家生命周期钩子

### Lua API

```lua
local M = {}

-- 玩家认证（连接后第一个钩子）
function M.on_auth(session, auth_data)
    -- auth_data: 客户端发送的认证信息（token、uid 等）
    -- 返回: true, player_data 或 false, error_reason

    local ok, player = verify_token(auth_data.token)
    if not ok then
        return false, "invalid_token"
    end

    -- 检查是否重复登录
    local existing = shield.player.query(player.uid)
    if existing then
        -- 踢掉旧连接
        existing:kick("duplicate_login")
    end

    return true, player
end

-- 玩家上线（认证成功后）
function M.on_login(player)
    -- player: PlayerSession 对象
    -- 加载玩家数据
    local data = load_player_data(player.uid)
    player:set_data(data)

    shield.log.info("player login: " .. player.uid)
end

-- 玩家消息处理
function M.on_message(player, msg_type, payload)
    -- 业务消息路由
    if msg_type == "move" then
        handle_move(player, payload)
    elseif msg_type == "chat" then
        handle_chat(player, payload)
    end
end

-- 玩家断线
function M.on_disconnect(player, reason)
    -- reason 枚举见下方表格
    -- 进入重连窗口，不立即清理

    shield.log.info("player disconnect: " .. player.uid .. " reason: " .. reason)
end

-- 玩家重连
function M.on_reconnect(player)
    -- 恢复会话状态
    -- 推送离线期间的消息

    shield.log.info("player reconnect: " .. player.uid)
end

-- 玩家离线（重连窗口超时或主动登出）
function M.on_logout(player, reason)
    -- reason 枚举见下方表格
    -- 保存玩家数据
    save_player_data(player.uid, player:get_data())

    -- 清理资源
    -- 通知其他服务

    shield.log.info("player logout: " .. player.uid .. " reason: " .. reason)
end

-- 玩家数据保存（定时或手动触发）
function M.on_save(player)
    -- 增量保存或全量保存
    save_player_data(player.uid, player:get_data())
end

return M
```

### 钩子调用时机

| 钩子 | 触发时机 | 阻塞 | 可选 |
|------|----------|------|------|
| `on_auth` | 连接建立后 | 是 | 否 |
| `on_login` | 认证成功后 | 是 | 否 |
| `on_message` | 收到业务消息 | 是 | 否 |
| `on_disconnect` | 连接断开 | 否 | 否 |
| `on_reconnect` | 重连成功 | 是 | 是 |
| `on_logout` | 玩家离线 | 否 | 否 |
| `on_save` | 定时保存 | 否 | 是 |

### reason 枚举

玩家级钩子的 reason 与服务级 `on_exit` 的 reason 独立（服务级 reason 见 [服务语义](runtime-service.md#on_exitreason)）。

**on_disconnect reason：**

| reason | 说明 |
|--------|------|
| `"client_close"` | 客户端主动断开 |
| `"network_error"` | 网络错误导致断开 |
| `"timeout"` | 读写超时 |
| `"kicked"` | 被服务端踢下线 |

**on_logout reason：**

| reason | 说明 |
|--------|------|
| `"normal"` | 正常登出（客户端主动退出） |
| `"timeout"` | 重连窗口超时 |
| `"kicked"` | 被管理员踢出 |
| `"replaced"` | 被新设备登录替换 |

## PlayerSession 对象

```lua
-- PlayerSession 是玩家会话的运行时表示
local player = shield.player.get(uid)

-- 基础属性
player.uid              -- 玩家 UID
player.session_id       -- 会话 ID
player.state            -- 状态: online, disconnecting, offline
player.connect_time     -- 连接时间
player.last_active      -- 最后活跃时间
player.remote_addr      -- 客户端地址
player.device_id        -- 设备 ID（可选）

-- 数据管理
player:get_data()       -- 获取玩家数据
player:set_data(data)   -- 设置玩家数据
player:get(key)         -- 获取单个字段
player:set(key, value)  -- 设置单个字段

-- 消息发送
player:send(msg_type, payload)  -- 发送消息
player:send_batch(messages)     -- 批量发送

-- 会话控制
player:kick(reason)     -- 踢下线
player:logout()         -- 主动登出
player:save()           -- 触发保存

-- 重连相关
player:is_online()      -- 是否在线
player:is_reconnecting() -- 是否在重连窗口
player:reconnect_token() -- 获取重连 token
```

## 断线重连

### 重连窗口

玩家断线后进入重连窗口，期间保留会话状态：

```yaml
player:
  reconnect:
    enabled: true
    window: 300000         # 重连窗口 5 分钟
    token_ttl: 60000       # 重连 token 有效期 1 分钟
    max_attempts: 3        # 最大重连尝试次数
```

### 重连流程

```
1. 玩家断线
   ├── 记录断线时间和原因
   ├── 保持 PlayerSession 存在
   ├── 进入重连窗口
   └── 开始缓存发往该玩家的消息

2. 玩家重连请求
   ├── 客户端携带: uid + reconnect_token
   ├── 验证 token 有效性
   ├── 检查是否在重连窗口内
   └── 成功: 恢复会话 | 失败: 要求重新登录

3. 重连成功
   ├── 恢复 PlayerSession 绑定到新 Connection
   ├── 推送离线期间缓存的消息
   ├── 调用 on_reconnect 钩子
   └── 恢复消息接收

4. 重连窗口超时
   ├── 标记玩家为 offline
   ├── 调用 on_logout 钩子
   ├── 保存玩家数据
   └── 清理会话
```

### 客户端重连实现

```lua
-- 客户端伪代码
function client.reconnect()
    local ok, result = send_auth_request({
        uid = self.uid,
        reconnect_token = self.reconnect_token,
        is_reconnect = true,
    })

    if ok then
        -- 重连成功，处理离线消息
        for _, msg in ipairs(result.offline_messages) do
            self:handle_message(msg)
        end
    else
        -- 重连失败，重新登录
        self:login()
    end
end
```

### 重连 Token

```lua
-- 服务端生成重连 token
function M.on_disconnect(player, reason)
    local token = player:reconnect_token()
    -- 发送给客户端（如果可能）
    -- 或客户端定期保存最新的 token
end

-- token 结构
{
    uid = 12345,
    token = "random_string",
    created_at = 1234567890,
    expires_at = 1234567950,
    session_id = 67890,
}
```

## 离线消息缓存

### 缓存策略

```yaml
player:
  offline_messages:
    enabled: true
    max_messages: 100        # 每个玩家最大缓存消息数
    max_age: 3600000         # 消息最大存活时间 1 小时
    storage: memory          # memory | redis
```

### 消息缓存

```lua
-- 发送消息给离线玩家
shield.player.send(uid, "mail", {
    from = "system",
    title = "Welcome back!",
    content = "You have been away for 3 hours.",
})
-- 如果玩家离线，自动缓存

-- 消息结构
{
    id = "msg_12345",
    type = "mail",
    payload = { ... },
    created_at = 1234567890,
    expires_at = 1234571490,
    delivered = false,
}
```

### 消息推送

```lua
-- 玩家重连时推送离线消息
function M.on_reconnect(player)
    local messages = player:get_offline_messages()
    if #messages > 0 then
        player:send("offline_messages", {
            count = #messages,
            messages = messages,
        })
        player:clear_offline_messages()
    end
end
```

### 消息优先级

```lua
-- 高优先级消息（如系统通知、紧急邮件）
shield.player.send(uid, "system_alert", data, {
    priority = "high",
    persist = true,  -- 即使超过最大数量也保留
})

-- 低优先级消息（如世界聊天）
shield.player.send(uid, "world_chat", data, {
    priority = "low",
    max_age = 300000,  -- 5 分钟过期
})
```

## 配置示例

```yaml
actors:
  - name: player_manager
    script: scripts/player_manager.lua
    player:
      # 认证配置
      auth:
        timeout: 10000               # 认证超时 10 秒
        max_attempts: 5              # 最大认证尝试次数
        ban_duration: 300000         # 认证失败封禁 5 分钟

      # 重连配置
      reconnect:
        enabled: true
        window: 300000               # 重连窗口 5 分钟
        token_ttl: 60000             # token 有效期 1 分钟
        max_attempts: 3              # 最大重连尝试

      # 离线消息配置
      offline_messages:
        enabled: true
        max_messages: 100            # 最大缓存消息数
        max_age: 3600000             # 消息存活 1 小时
        storage: memory              # memory | redis

      # 会话配置
      session:
        idle_timeout: 300000         # 空闲超时 5 分钟
        heartbeat_interval: 30000    # 心跳间隔 30 秒
        save_interval: 300000        # 定时保存间隔 5 分钟

      # 并发限制
      max_online: 10000              # 最大在线玩家数
      max_per_ip: 5                  # 单 IP 最大连接数
```

## 多设备登录策略

```yaml
player:
  multi_device:
    policy: single           # single | multi | kick_old
    # single: 同一 UID 只允许一个设备在线
    # multi: 允许多设备同时在线
    # kick_old: 新设备登录时踢掉旧设备
```

```lua
-- 单设备登录实现
function M.on_auth(session, auth_data)
    local uid = auth_data.uid
    local existing = shield.player.get(uid)

    if existing then
        if existing:is_reconnecting() then
            -- 在重连窗口内，允许重连
            return true, { reconnect = true }
        else
            -- 在线状态，根据策略处理
            if config.multi_device.policy == "kick_old" then
                existing:kick("replaced_by_new_device")
            else
                return false, "already_online"
            end
        end
    end

    return true, { uid = uid }
end
```

## 与 gateway 的关系

```
┌─────────────────────────────────────────────────────────┐
│                     gateway 服务                         │
│  ┌────────────────────────────────────────────────────┐ │
│  │  连接管理                                            │ │
│  │  on_connect → 认证 → 绑定 PlayerSession             │ │
│  │  on_message → 路由到 PlayerSession                  │ │
│  │  on_disconnect → 触发断线处理                        │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   player_manager 服务                    │
│  ┌────────────────────────────────────────────────────┐ │
│  │  玩家生命周期                                        │ │
│  │  on_auth → on_login → on_message → on_logout       │ │
│  │                   ↓                                 │ │
│  │              on_disconnect → on_reconnect           │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   业务服务                               │
│  ┌────────────────────────────────────────────────────┐ │
│  │  player_service                                     │ │
│  │  处理具体业务逻辑                                    │ │
│  └────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## ops 暴露

```json
GET /ops/players

{
  "online": 5000,
  "reconnecting": 10,
  "offline_with_messages": 500,
  "by_state": {
    "authenticating": 5,
    "online": 4985,
    "reconnecting": 10
  },
  "offline_message_stats": {
    "total_cached": 1234,
    "pending_delivery": 500,
    "expired_today": 50
  }
}
```

## PlayerManager

PlayerManager 是全局单例 Service，负责管理所有在线玩家。

### 职责

| 职责 | 说明 |
|------|------|
| 玩家注册/注销 | Player 上下线时向 PlayerManager 注册/注销 |
| 全局查询 | 按 UID、状态、条件查询玩家 |
| 在线统计 | 统计在线人数、状态分布 |
| 批量操作 | 踢人、广播消息、批量通知 |
| 索引维护 | 维护 UID → PlayerSession 的索引 |

### 架构

```
┌─────────────────────────────────────────────────────────┐
│                  PlayerManager (单例)                    │
│  ┌────────────────────────────────────────────────────┐ │
│  │  索引                                               │ │
│  │  - uid_index: Map<UID, PlayerSession>              │ │
│  │  - state_index: Map<State, Set<PlayerSession>>     │ │
│  └────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────┐ │
│  │  能力                                               │ │
│  │  - register(player) / unregister(uid)              │ │
│  │  - get(uid) / query(filter)                        │ │
│  │  - count() / online_list()                         │ │
│  │  - kick(uid, reason) / broadcast(msg)              │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│  Player 1 (Service) │ Player 2 (Service) │ ...         │
└─────────────────────────────────────────────────────────┘
```

### Lua API

```lua
-- 获取 PlayerManager
local pm = shield.player.manager()

-- 查询玩家
local player = pm:get(uid)
local players = pm:query({ state = "online", level_min = 10 })

-- 统计
local count = pm:count()
local stats = pm:stats()
-- stats: { online = 5000, reconnecting = 10, total_today = 12345 }

-- 踢人
local ok, err = pm:kick(uid, "maintenance")

-- 广播
pm:broadcast("system_notice", { text = "Server maintenance in 5 minutes" })
pm:broadcast_to(filter, "system_notice", { text = "VIP bonus!" })

-- 遍历
pm:for_each(function(player)
    if player.level >= 100 then
        player:send("achievement", { type = "level_100" })
    end
end)
```

### PlayerSession 注册流程

```lua
-- Player Service 的 on_login 中自动注册
function M.on_login(player)
    -- 框架自动调用 PlayerManager.register(player)
    -- 业务层无需手动注册

    -- 业务逻辑
    load_player_data(player.uid)
end

-- Player Service 的 on_logout 中自动注销
function M.on_logout(player, reason)
    -- 框架自动调用 PlayerManager.unregister(player.uid)
    -- 业务层无需手动注销

    -- 业务逻辑
    save_player_data(player.uid)
end
```

### 配置

```yaml
# PlayerManager 配置
player_manager:
  enabled: true
  name: "player_manager"           # 服务名称
  max_players: 50000               # 最大在线玩家数
  index_cleanup_interval: 60000    # 索引清理间隔（ms）

# Player Service 配置（关联 PlayerManager）
actors:
  - name: player
    script: scripts/player.lua
    player:
      manager: "player_manager"    # 关联的 PlayerManager
      auth:
        timeout: 10000
      reconnect:
        enabled: true
        window: 300000
```

### 与其他 Service 的交互

```lua
-- 其他 Service 查询玩家信息
local player = shield.call("player_manager", "get", uid)
if player then
    shield.log.info("player online: " .. player.uid)
end

-- 其他 Service 踢人
shield.call("player_manager", "kick", uid, "violation")

-- 其他 Service 广播
shield.send("player_manager", "broadcast", {
    type = "system_notice",
    text = "Welcome to the game!"
})
```

### ops 暴露

```json
GET /ops/players

{
  "manager": {
    "name": "player_manager",
    "status": "running"
  },
  "stats": {
    "online": 5000,
    "authenticating": 5,
    "reconnecting": 10,
    "total_today": 12345,
    "peak_today": 8000
  },
  "top_regions": [
    { "region": "us-east", "count": 2000 },
    { "region": "eu-west", "count": 1500 }
  ]
}
```

## 实现优先级

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 基础钩子 (on_auth, on_login, on_logout) | P0 | 核心功能 |
| 断线重连 | P0 | 游戏必备 |
| PlayerManager | P0 | 全局玩家管理 |
| 离线消息缓存 | P1 | 提升体验 |
| 多设备策略 | P2 | 按需实现 |
| 定时保存 | P1 | 数据安全 |
