# 玩家生命周期运行时语义

本文档包含 Shield 玩家生命周期管理、断线重连、离线消息缓存相关的运行时语义决策。

`shield_player` 是官方可选模块，不属于 `shield_core`，也不是当前 Lua API 最小契约。本文用于冻结未来边界，避免把玩家管理反向塞进 core。

当前状态：

- 本文冻结 `shield_player` optional module 的边界契约；具体 Lua API 进入 Phase 2+。
- `shield.player.setup` 主入口、`PlayerRef` 跨 service 引用、persistence adapter 为 P0 文档冻结项；实现顺序见文末"实现优先级"。
- 当前实现只承诺文档中的最小 player 契约冻结，不把 `PlayerSession`、重连窗口、离线消息缓存、`player_pool` 或多设备策略当作 Phase 1 已完成能力。
- `SessionHandle` 仍只留在 gateway / `shield_net` 内部；`shield_player` 的公开语义只暴露 `session_id`、`PlayerSession`（本地）和 `PlayerRef`（跨 service），不把 `SessionHandle` 作为跨 service 对象传递。
- 即使启用 `shield_player`，普通 Lua service 仍保持 module-table + named method 语义；本模块不恢复 legacy `on_message(src, type, data)` 统一入口。
- optional module 的横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。
- 如与 [Lua API 契约](lua-api.md) 冲突，以 `lua-api.md` 为当前最小契约。

## 设计原则

- 玩家生命周期与连接生命周期分离。
- 断线不等于离线，支持短暂断线重连。
- 离线期间的消息不丢失。
- 状态管理由框架提供，业务逻辑由 Lua 实现。
- 玩家 ready 是 `shield_player` 的玩家级状态，不是 service/application 的全局 `on_ready` 事件。

## Player 与 Service 的关系

**Player 是 Service 的增强模式，不是独立的 Actor 类型。**

| 概念 | 说明 |
|------|------|
| Service | Shield 的基本执行单元，拥有 Lua VM、mailbox、name |
| Actor | Service 的同义词，强调消息驱动模型 |
| Player | 绑定网络连接的 Service，有额外的生命周期管理 |

**Player Service = 普通 Service + 可选玩家态扩展**

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
│  │  - setup 钩子与 PlayerRef                           │ │
│  │  - 断线重连（P0 文档冻结）                         │ │
│  │  - 离线消息缓存（后续）                             │ │
│  │  - 数据持久化契约                                   │ │
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
shield.player.setup(M, {
    auth = function(self, session_id, auth_data) end,
    login = function(self, player) end,
    ready = function(self, player) end,
    client_message = function(self, player, payload) end,
    disconnect = function(self, player, reason) end,
    reconnect = function(self, player) end,
    logout = function(self, player, reason) end,
    save = function(self, player) end,
})

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
│            │              → loading → ready              │
│            │              → disconnecting                │
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
    │ rejected │   │ loading  │    │ rejected │
    └──────────┘   └────┬─────┘    └──────────┘
                        │ login_done
                        ▼
                   ┌──────────┐
                   │  ready   │
                   └────┬─────┘
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
│ ready   │ │  offline │
│(resume) │ │          │
└─────────┘ └──────────┘
```

### anonymous / spectator 扩展状态

匿名和旁观不是默认状态机的一部分，必须由 `player.session` 显式开启。默认配置下，连接仍必须经过 `authenticating`、`loading` 并进入 `ready`，否则拒绝。

```yaml
player:
  session:
    allow_anonymous: false
    allow_spectator: false
```

开启后仍不改变 P0 的 `shield.player.setup` 规则：`auth` 钩子仍然必填。业务通过 `auth` 返回值声明连接进入哪种玩家态：

```lua
-- 匿名游客
return true, {
    uid = "guest:" .. session_id,
    anonymous = true,
}

-- 旁观连接
return true, {
    uid = "spectator:" .. session_id,
    spectator = true,
    match_id = auth_data.match_id,
}
```

状态含义：

| 状态 | 进入条件 | 能力限制 |
|---|---|---|
| `anonymous` | `allow_anonymous=true` 且 auth 返回 `anonymous=true` | 可收发业务消息；默认不持久化；不能进入排行榜/队列等 uid 强绑定能力 |
| `spectator` | `allow_spectator=true` 且 auth 返回 `spectator=true` | 只读玩家态；框架禁止 `player:set_data`、`player:save` 等会修改 PlayerSession 的 API |

若配置未开启但 auth 返回对应标记，框架必须拒绝连接并返回 `Error{code="anonymous_disabled"}` 或 `Error{code="spectator_disabled"}`。匿名玩家升级为正式账号时必须重新走认证流程，成功后生成新的 `PlayerRef`；旧匿名 `PlayerRef` 立即失效。

业务层会改变世界状态的消息，必须在 `client_message` 中显式检查 `player.state == "spectator"` 并拒绝；框架不通过方法名推断业务写语义。

## shield.player.setup 主入口

业务 Player Service 应当通过 `shield.player.setup` 注册到框架，作为唯一推荐入口。

这部分定义 P0 冻结契约，不等同于当前源码已经覆盖全部分支。

### 基本形态

```lua
local M = {}

shield.player.setup(M, {
    -- 必填钩子：业务必须实现
    auth = function(self, session_id, auth_data) ... end,
    login = function(self, player) ... end,
    client_message = function(self, player, payload) ... end,

    -- 可选钩子：未提供时走框架默认实现（默认行为见下方"钩子调用时机"表）
    ready = function(self, player) ... end,
    disconnect = function(self, player, reason) ... end,
    reconnect  = function(self, player) ... end,
    logout     = function(self, player, reason) ... end,
    save       = function(self, player) ... end,

    -- 可选：持久化 adapter（详见"持久化 adapter"章节）
    persistence = {
        backend = "redis",
        key = function(uid) return "player:" .. uid end,
        fields = { "profile", "inventory", "quests" },
        auto_save_interval = 300000,
        on_save_error = "log",
    },

    -- 可选：关联的 PlayerManager 名称
    manager = "player_manager",
})

-- 业务方法独立命名空间，与 setup opts 字段隔离
function M.get_profile(player, uid)
    return player.data.profile
end

return M
```

### 设计约束

- `setup` 是 Player Service 唯一推荐入口；`shield.player.Base` 等高级风格留 Phase 2+，不在 P0 冻结。
- opts 字段一律去掉 `on_` 前缀（`auth`/`login`/`ready`/`client_message`/`disconnect`/`reconnect`/`logout`/`save`），与 module-level `on_init/on_shutdown/on_exit` 隔离命名空间；service-level hook 仍可保留 `M.on_init/on_shutdown/on_exit`。
- 必填钩子缺失即返回 `nil, Error{code="setup_invalid"}`，service 不进入 running 状态。
- 未在 setup 中提供的可选钩子由框架按明列的默认实现执行，**不允许**"未提供即 noop"的隐式行为。
- 业务方法（与 setup opts 同级的 `M.xxx`）保留普通 service method dispatch 语义，setup 不改写。

## 玩家生命周期钩子

### 钩子定义（setup opts 字段对应）

```lua
local M = {}

shield.player.setup(M, {
    -- 玩家认证（连接后第一个钩子）
    auth = function(self, session_id, auth_data)
        -- auth_data: 客户端发送的认证信息（token、uid 等）
        -- 返回: true, player_data 或 false, error_reason
        local ok, player = verify_token(auth_data.token)
        if not ok then
            return false, "invalid_token"
        end

        -- 检查是否重复登录
        local existing = shield.player.query(player.uid)
        if existing then
            existing:kick("duplicate_login")
        end

        return true, player
    end,

    -- 玩家上线准备（认证成功后）
    login = function(self, player)
        -- player: PlayerSession 对象
        local data = load_player_data(player.uid)
        player:set_data(data)

        shield.log.info("player login: " .. player.uid)
    end,

    -- 玩家就绪（login 和默认注册/load 完成后）
    ready = function(self, player)
        shield.log.info("player ready: " .. player.uid)
    end,

    -- 玩家消息处理
    client_message = function(self, player, payload)
        -- payload 为协议层解码后的应用消息；示例中用 table 表示
        if payload.kind == "move" then
            handle_move(player, payload)
        elseif payload.kind == "chat" then
            handle_chat(player, payload)
        end
    end,

    -- 玩家断线
    disconnect = function(self, player, reason)
        -- reason 枚举见下方表格
        -- 进入重连窗口，不立即清理
        shield.log.info("player disconnect: " .. player.uid .. " reason: " .. reason)
    end,

    -- 玩家重连
    reconnect = function(self, player)
        -- 恢复会话状态，推送离线期间的消息
        shield.log.info("player reconnect: " .. player.uid)
    end,

    -- 玩家离线（重连窗口超时或主动登出）
    logout = function(self, player, reason)
        -- reason 枚举见下方表格
        save_player_data(player.uid, player:get_data())
        shield.log.info("player logout: " .. player.uid .. " reason: " .. reason)
    end,

    -- 玩家数据保存（定时或手动触发）
    save = function(self, player)
        save_player_data(player.uid, player:get_data())
    end,
})

return M
```

### 钩子调用时机与默认实现

| 钩子 | setup 字段 | 触发时机 | 阻塞 | 可选 | 默认实现（业务未提供时由框架执行） |
|------|-----------|----------|------|------|-----------------------------------|
| `on_auth` | `auth` | 连接建立后 | 是 | 否 | 无；业务必须提供，否则 `setup_invalid` |
| `on_login` | `login` | 认证成功后 | 是 | 否 | `PlayerManager.register(player)`；若配置 persistence 则触发首次 load |
| `on_ready` | `ready` | `login` 和默认注册/load 完成后，玩家进入 ready 前 | 是 | 是 | 标记 `player.state = "ready"`，允许接收客户端业务消息 |
| `on_client_message` | `client_message` | 收到客户端业务 payload | 是 | 否 | 路由到业务方法 dispatch（与普通 service method 一致） |
| `on_disconnect` | `disconnect` | 连接断开 | 否 | 否 | 进入重连窗口 + 启动离线消息缓存 |
| `on_reconnect` | `reconnect` | 重连成功 | 是 | 是 | 推送离线期间缓存消息 |
| `on_logout` | `logout` | 玩家离线 | 否 | 否 | 调用 `persistence.save`（若配置） + `PlayerManager.unregister(uid)` |
| `on_save` | `save` | 定时或手动触发 | 否 | 是 | 若配置 persistence 则调用 `persistence.save`；否则 no-op |

业务覆盖可选钩子时，**默认实现不执行**；如需保留默认行为，业务实现中应显式调用对应 helper（如 `shield.player.defaults.reconnect(player)`）。

`on_ready` 是玩家级 ready，不是 application ready，也不是 service-level `on_ready`。普通 service 不提供 `on_ready` hook；Player Service 只有在启用 `shield_player` 且通过 `shield.player.setup` 注册后才有玩家 ready 语义。`on_client_message` 只在 player 进入 ready 后分发，除认证/重连握手外的客户端业务消息在 loading 阶段必须排队或拒绝。

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

`PlayerSession` 是目标语义对象，不代表 Phase 1 已完整实现。

```lua
-- PlayerSession 是玩家会话的运行时表示
local player = shield.player.get(uid)

-- 基础属性
player.uid              -- 玩家 UID
player.session_id       -- 会话 ID
player.state            -- 状态: authenticating, loading, ready, disconnecting, offline
player.connect_time     -- 连接时间
player.last_active      -- 最后活跃时间
player.remote_addr      -- 客户端地址
player.device_id        -- 设备 ID（可选）

-- 数据管理
player:get_data()       -- 获取玩家数据
player:set_data(data)   -- 设置玩家数据
player:get(key)         -- 获取单个字段
player:set(key, value)  -- 设置单个字段

-- 网络发送（不暴露 SessionHandle）
player:send(payload)            -- 发送单条 payload
player:send_batch(payloads)     -- 批量发送

-- 会话控制
player:kick(reason)     -- 踢下线
player:logout()         -- 主动登出
player:save()           -- 触发保存

-- 重连相关
player:is_online()      -- 是否在线
player:is_ready()       -- 是否已完成玩家级 ready
player:is_reconnecting() -- 是否在重连窗口
player:reconnect_token() -- 获取重连 token
```

`avatar` / `character` / `client` 的归属：

- `SessionHandle` 表示网络连接，只由 gateway / `shield_net` 持有。
- `PlayerSession` 表示本地玩家运行时对象，包含 uid、session_id、state、数据快照和发送能力。
- `PlayerRef` 是跨 service 轻量引用，只用于定位玩家，不携带完整玩家数据。
- `avatar`、`character`、`client` 这类业务对象应作为 `PlayerSession` 的数据字段或业务 Entity，由游戏自己定义；Shield 不把它们做成核心类型。

## PlayerRef 跨 service 引用

`PlayerRef` 是 `PlayerSession` 的轻量引用，用于跨 service 传递。

当前只冻结结构和本地解析边界；远端 resolve/operate 仍按路线图推进。

### 设计约束

- `PlayerRef` **不是** `ServiceHandle` 的替代品，只是 player 模块内部引用。
- 跨 service payload **只能**传 `PlayerRef`，**不能**传 `SessionHandle`，也**不能**传完整 `PlayerSession`。
- `PlayerRef` 可被 LuaPack 编码（独立 type tag），跨 service 通过 `shield.send/call` payload 传递。
- 接收方通过 `shield.player.resolve(ref)` 解析为本地 `PlayerSession`。

### 数据结构

```lua
-- PlayerRef {
--   uid: uint64,
--   node_id: uint32,
--   service_id: uint64,
--   epoch: uint64,
-- }
```

`epoch` 来自 shield_cluster 的 `node_epoch`；单节点部署时为 0。

### Lua API

```lua
-- 从 PlayerSession 取轻量引用
local ref = player:ref()

-- 跨 service 传递
shield.send("combat.1", "kick", ref)

-- 接收方解析
function M.kick(ref)
    local player, err = shield.player.resolve(ref)
    if not player then
        shield.log.warn("resolve failed: " .. err.code)
        return
    end
    player:kick("combat_request")
end
```

### resolve 行为

| 场景 | 行为 |
|---|---|
| 本地且玩家在线 | 返回 `PlayerSession` |
| 本地但玩家已下线 | 返回 `nil, Error{code="player_not_found"}` |
| ref 来自远端节点且 remote resolve 未启用 | 返回 `nil, Error{code="remote_resolve_unimplemented"}`，业务层走 cluster RPC 自行处理 |
| ref 字段非法或 epoch 不匹配 | 返回 `nil, Error{code="invalid_player_ref"}` |

P0 仅冻结本地 resolve 与数据结构。

### 远端 resolve 边界

远端 `PlayerRef` resolve 进入 P2，但接口边界现在冻结，避免后续重新设计：

- `shield.player.resolve(ref)` 只负责本地解析；远端 ref 默认返回 `remote_resolve_unimplemented`。
- P2 若启用 `player.remote_resolve=true`，`resolve` 可以通过 `shield_cluster` 访问远端 `PlayerManager`，但返回值仍必须是 `PlayerRef` 或业务快照，不能把远端 `PlayerSession` 伪装成本地对象。
- 远端操作应优先通过 `shield.player.call_ref(ref, method, ...)` 或业务 service RPC 完成；该 API 属于 `shield_player`，底层仍复用 `shield.call` 的超时和错误语义。
- node epoch 不匹配时必须返回 `invalid_player_ref`，不能自动重试到新节点，避免旧引用误命中新玩家。
- `shield_cluster` 关闭时，所有远端 resolve/operate 返回 `module_unavailable`，本地 resolve 不受影响。

## 持久化 adapter

persistence adapter 是 `shield_player` 拥有的轻量持久化契约，**不属于**插件 host 或具体数据插件，但底层必须通过已配置的数据插件 binding 调用。

### 设计约束

- adapter 复用数据插件 namespace，**不重新定义** SQL、Redis 或文档库语义。
- adapter **不拥有**连接池；连接池归属对应插件 instance。
- adapter **不引入** ORM、mapper、schema 工具链；只接受可 LuaPack 编码的 table 白名单字段。
- adapter 失败复用对应数据插件错误码；player 域只新增 `persistence_save_failed` 等明确属于本模块的错误。
- 未配置 persistence 时，`on_save` 默认实现为 no-op。

### 配置形态

persistence 在 `shield.player.setup` 的 opts 中声明：

```lua
shield.player.setup(M, {
    -- ... 其他钩子 ...

    persistence = {
        backend = "redis",                  -- "redis" | "database"，必须已启用
        key = function(uid)                 -- 业务定义 key 模板
            return "player:" .. uid
        end,
        fields = { "profile", "inventory", "quests" },  -- 显式白名单
        auto_save_interval = 300000,        -- ms；0 = 关闭自动保存
        on_save_error = "log",              -- "log" | "panic"，失败策略
    },
})
```

### 行为契约

| 时机 | 行为 |
|---|---|
| `on_login` 默认实现 | 调用 `persistence.load(uid)`，把字段填充到 `player.data` |
| `on_save` 默认实现（自动触发） | 每 `auto_save_interval` 调用 `persistence.save(uid, fields)` |
| `on_save` 默认实现（手动触发） | 业务调用 `player:save()` 立即触发 |
| `on_logout` 默认实现 | 调用 `persistence.save(uid, fields)` 后再 `PlayerManager.unregister` |
| `persistence.save` 失败 | 按 `on_save_error` 配置：`log` 仅记录 + 计数；`panic` 触发 `on_panic` |

### 字段白名单

只接受 LuaPack 支持的类型（见 [消息语义](runtime-messaging.md#messagepayload)）。下列类型**禁止**作为 persistence 字段：

- `function`、`thread/coroutine`
- 普通 userdata
- 循环引用 table
- `SessionHandle` 或完整 `PlayerSession`

业务需要持久化这些类型时，必须自行序列化为 string/binary 并放入白名单字段。

## shield.player.Base 高级风格

`shield.player.Base` 属于 P2 语法糖，不是第二套生命周期模型。它必须基于 `shield.player.setup` 实现，不能绕过 setup 的校验、默认实现和错误语义。

```lua
local PlayerBase = shield.player.Base

local M = PlayerBase.extend({
    auth = function(self, session_id, auth_data)
        return verify_token(auth_data.token)
    end,

    login = function(self, player)
        player:set_data(load_player_data(player.uid))
    end,
})

return M
```

约束：

- `Base.extend(opts)` 等价于 `shield.player.setup(M, opts)` 后返回 `M`。
- hook 字段仍使用 `auth/login/ready/client_message/disconnect/reconnect/logout/save`，不恢复 `on_*` 作为 setup 字段。
- 业务覆盖可选 hook 时默认实现不自动执行；仍通过 `shield.player.defaults.*` 显式调用。
- 不支持多继承；需要组合能力时应在业务模块内组合普通 Lua table/function。
- P0/P1 文档和测试只以 `setup` 为准，Base 测试只验证它没有引入额外语义。

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
   ├── 标记玩家回到 ready
   └── 恢复业务消息接收

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
shield.player.send(uid, {
    kind = "mail",
    from = "system",
    title = "Welcome back!",
    content = "You have been away for 3 hours.",
})
-- 如果玩家离线，自动缓存

-- 消息结构
{
    id = "msg_12345",
    payload = {
        kind = "mail",
        content = "Welcome back!",
    },
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
        player:send({
            kind = "offline_messages",
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
shield.player.send(uid, {
    kind = "system_alert",
    data = data,
}, {
    priority = "high",
    persist = true,  -- 即使超过最大数量也保留
})

-- 低优先级消息（如世界聊天）
shield.player.send(uid, {
    kind = "world_chat",
    data = data,
}, {
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
    policy: single           # single | multi | kick_old，默认 single
    max_devices: 1           # policy=multi 时生效，默认 1
    device_key: device_id    # device_id | platform | custom，默认 device_id
    # single: 同一 UID 只允许一个设备在线
    # multi: 允许多设备同时在线
    # kick_old: 新设备登录时踢掉旧设备
```

策略语义：

| policy | 行为 | PlayerManager 索引 |
|---|---|---|
| `single` | 同一 UID 已在线时拒绝新连接；重连窗口内允许同设备恢复 | `uid -> PlayerSession` |
| `kick_old` | 新连接认证成功后踢掉旧连接，旧连接 `logout` reason 为 `"replaced"` | `uid -> PlayerSession` |
| `multi` | 同一 UID 允许多个设备同时在线，超过 `max_devices` 拒绝新连接 | `(uid, device_id) -> PlayerSession` |

`single` 是默认策略。未提供 `device_id` 时，框架使用 `session_id` 作为临时设备标识，但该连接不能参与同设备重连；业务需要稳定重连必须在 auth 数据中提供 `device_id`。

多设备不改变 `PlayerRef` 结构。`multi` 模式下，同一 UID 的不同设备必须生成不同 `service_id`，`shield.player.get(uid)` 返回主会话；需要枚举设备时使用 `shield.player.manager():get_devices(uid)`。

```lua
-- 单设备登录实现
shield.player.setup(M, {
    auth = function(self, session_id, auth_data)
        local uid = auth_data.uid
        local existing = shield.player.get(uid)

        if existing then
            if existing:is_reconnecting() then
                -- 在重连窗口内，允许重连
                return true, { reconnect = true }
            end

            -- ready/loading 等活跃状态，根据策略处理
            if config.multi_device.policy == "kick_old" then
                existing:kick("replaced_by_new_device")
            else
                return false, "already_online"
            end
        end

        return true, { uid = uid }
    end,
})
```

## 容量模型与 player_pool

`player_pool` 是目标扩展模型，不属于当前最小 runtime。

默认模型是 one-player-one-service：每个在线玩家对应一个 Player Service 和一个 Lua VM。它语义最清晰，隔离性最好，也是 P0/P1 的默认实现模型。

`player_pool` 是大规模在线的可选实现策略，不改变 public API：

```yaml
player:
  runtime_model: service_per_player   # service_per_player | player_pool
  pool:
    shard_count: 64
    max_players_per_service: 512
    mailbox_limit: 8192
```

模型约束：

| 模型 | 适用场景 | 约束 |
|---|---|---|
| `service_per_player` | 默认；中小规模、强隔离、调试友好 | 每个玩家独立 mailbox 和 Lua VM；容量必须通过基准确认 |
| `player_pool` | 10K+ 在线、内存敏感场景 | 一个 service 管理多个 `PlayerSession`；玩家之间必须按 uid 分片；单玩家 handler 仍串行 |

`player_pool` 不允许改变以下语义：

- `PlayerRef` 仍包含 `uid/node_id/service_id/epoch`，其中 `service_id` 指向 pool shard。
- 同一 uid 的消息必须稳定路由到同一个 shard，除非玩家离线后重新登录。
- `shield.send/call` 仍以 service 为目标；玩家级路由由 `shield_player` 在 shard 内完成。
- `PlayerManager` 仍按 uid 维护索引，但索引值可以是 `PlayerRef`，不能暴露 pool 内部 table 指针。

容量基准必须在实现前补齐，至少记录：

| 指标 | 要求 |
|---|---|
| 单 Player Service 常驻内存 | 统计 Lua VM、mailbox、PlayerSession、离线队列基础开销 |
| 单 pool shard 常驻内存 | 统计 shard Lua VM、玩家表、索引和队列开销 |
| 10K 在线压测 | 记录 RSS、P50/P95/P99 消息延迟、GC 时间和 mailbox backlog |
| 断线重连压测 | 记录 reconnect window 内缓存消息数量、过期清理成本 |

## 与 gateway 的关系

以下图示描述目标协作，不代表 Phase 1 已把所有玩家态能力实现完毕。

```
┌─────────────────────────────────────────────────────────┐
│                     gateway 服务                         │
│  ┌────────────────────────────────────────────────────┐ │
│  │  连接管理                                            │ │
│  │  on_connect → 认证 → 绑定 session_id                │ │
│  │  on_client_message → 路由到 PlayerSession           │ │
│  │  on_disconnect → 触发断线处理                        │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                   player_manager 服务                    │
│  ┌────────────────────────────────────────────────────┐ │
│  │  玩家生命周期                                        │ │
│  │  on_auth → on_login → on_ready → on_client_message │ │
│  │                         ↓           → on_logout    │ │
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

约束：

- `SessionHandle` 只保留在 gateway / 网络层内部映射中。
- `player_manager`、业务 service 和离线消息队列都只通过 `session_id` 或 `PlayerSession` 协作。
- 跨 service payload 不传 `SessionHandle`。

## ops 暴露

ops 暴露是可选观测面，不属于最小运行时。

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
│  │  - kick(uid, reason) / broadcast(payload)          │ │
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
local players = pm:query({ state = "ready", level_min = 10 })

-- 统计
local count = pm:count()
local stats = pm:stats()
-- stats: { online = 5000, reconnecting = 10, total_today = 12345 }

-- 踢人
local ok, err = pm:kick(uid, "maintenance")

-- 广播
pm:broadcast({ kind = "system_notice", text = "Server maintenance in 5 minutes" })
pm:broadcast_to(filter, { kind = "system_notice", text = "VIP bonus!" })

-- 遍历
pm:for_each(function(player)
    if player.level >= 100 then
        player:send({ kind = "achievement", type = "level_100" })
    end
end)
```

### PlayerSession 注册流程

以下注册流程描述的是目标框架行为。

```lua
shield.player.setup(M, {
    login = function(self, player)
        -- 框架自动调用 PlayerManager.register(player)
        -- 业务层无需手动注册
        load_player_data(player.uid)
    end,

    logout = function(self, player, reason)
        -- 框架自动调用 PlayerManager.unregister(player.uid)
        -- 业务层无需手动注销
        save_player_data(player.uid)
    end,
})
```

### 配置

该配置属于 player 模块配置，不进入 core schema。

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

这些交互是模块协作边界，不代表当前实现已经提供完整 player 业务 API。

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

### PlayerManager ops 暴露

该端点属于 `shield_ops` 观测面，不属于最小运行时。

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
| `shield.player.setup` 主 API + 默认实现表 | P0 | 业务唯一推荐入口，默认行为必须明列；文档冻结，不代表当前 runtime 已实现 |
| 基础钩子（auth/login/ready/client_message/disconnect/logout） | P0 | setup 钩子，对应 player 级生命周期；文档冻结 |
| `PlayerRef` + 本地 `shield.player.resolve` | P0 | 跨 service 轻量引用；远端 resolve 留 P2+；文档冻结 |
| persistence adapter 契约 | P0 | shield_player 拥有的轻量 adapter，底层走数据插件 binding；文档冻结 |
| 断线重连 | P1 | 游戏必备，但不计入当前最小 runtime 已完成项 |
| PlayerManager | P1 | 全局玩家管理，和 `setup` 同步收敛 |
| 离线消息缓存 | P1 | 提升体验 |
| 定时保存（`on_save` 默认实现触发） | P1 | 数据安全，依赖 persistence adapter |
| anonymous/spectator 状态（opt-in） | P1 | 契约已冻结；默认状态机不变，配置显式开启 |
| 一玩家一 service 容量基准 + 可选 player_pool 模式 | P1 | 契约已冻结；实现前必须补容量基准 |
| 多设备策略 | P2 | 契约已冻结；默认 single |
| `shield.player.Base` 高级风格 | P2 | 契约已冻结；setup 之上的语法糖 |
| 远端 `PlayerRef` resolve | P2 | 契约已冻结；shield_cluster + shield_player 协作 |
