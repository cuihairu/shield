# 玩家运行时语义

本文冻结第一版玩家承载与客户端 RPC 边界。PlayerService 是普通 Service 的业务约定，不是新的 runtime 对象类型。

## 核心决策

- 一个在线玩家默认对应一个 PlayerService、一个 CAF actor 和一个私有 Lua VM。
- 玩家状态是该 Service 私有 Lua table。
- Service 是唯一拥有 mailbox、`send/call` 和 CAF address 的可寻址单元。
- 不引入 Player Entity、Entity mailbox 或 Entity RPC；`PlayerSession`/`PlayerRef` 只作为 `shield_player` 模块层对象存在（见下文模块契约），不构成第二套 actor/mailbox runtime，远程 `PlayerRef` resolve 留 Phase 2+。
- 不为未经压测证明的问题预设 player pool、avatar shard 或 base/cell/avatar 分层。
- `shield_player` 可以提供登录、重连、持久化等辅助能力，但不能建立第二套 actor/object runtime。

## PlayerService

PlayerService 与普通 Lua Service 使用同一 module table、生命周期和 service messaging API：

```lua
local M = {}
local state

function M.on_init(args)
    state = {
        player_id = args.player_id,
        client = args.client_ref,
        data = args.player_data,
    }
end

function M.get_profile()
    return state.data.profile
end

return M
```

区别只在业务约定：

- `args.player_id` 来自认证结果；
- `args.client_ref` 是值语义回包引用(`shield.client.bind` 返回的 `ClientRef`),不是
  session 连接对象;
- module 注册目标逻辑服务名为 `player` 的客户端 RPC bindings；
- 断线、重连和登出控制消息由 Service adapter 送入同一 actor mailbox。

PlayerService 没有额外 mailbox，也没有独立于 Service 的 `send/call` API。

## 登录与绑定

推荐登录链路：

```text
client auth RPC
  → Gateway session.target = AuthService
  → AuthService handler（协程）
  → validate credentials
  → shield.client.bind(client, player_id, "player")
  → Gateway CAS：expected epoch 比对 → 写 target/player_id → epoch 递增
  → 协程恢复，拿到新 epoch 的 ClientRef
```

认证前只能调用 descriptor 明确标记为 pre-login 的 RPC。认证成功后的原子更新必须同时
写入 target、可信 `player_id` 和 epoch,不能只保存其中之一。

重复登录、顶号和恢复旧 PlayerService 都是 player 模块策略,但最终必须通过
`shield.client.bind` 的 Gateway epoch 校验更新 session owner;stale 引用的 bind 会以
`client_rpc.epoch_expired` 失败。

## 客户端 RPC handler

RPC 定义在编译期确定：

```text
route_id
  -> full method name
  -> logical service name
  -> direction
  -> request/response schema
  -> binding hint
```

PlayerService 启动时把属于 `player` 的方法编译为：

```text
route_id -> cached Lua handler
```

运行时目标 actor 收到 `ClientIngress` 后，先命中缓存 handler，再按该方法 schema 解码 body，然后直接调用：

```lua
function M.move(ctx, client, request)
    assert(client:player_id() == state.player_id)

    state.data.position = {
        x = request.x,
        y = request.y,
        z = request.z,
    }

    player_rpc.move_result(client, {
        accepted = true,
    })
end
```

示例中的 `player_rpc.move_result` 是由 descriptor 生成的 server-to-client helper。它自身绑定 route id 和 response schema；业务只传 client reference 与业务参数。

handler 不接收 route name、`route_id`、frame、codec 或通用 payload envelope，也不做字符串/if-else 二次路由。

## ClientContext

每个客户端 RPC handler 的第一个参数是只读 `ClientContext`。至少支持：

```lua
client:player_id() -- 可信 player id；预登录阶段为 nil
client:ref()       -- 可序列化 ClientRef
```

PlayerService 必须校验 context 中的 `player_id` 与自己的 owner 一致。runtime 也应在调用 handler 前执行相同 owner 校验，防止 session 绑定错误或 stale 投递。

`ClientRef` 可以保存到玩家状态或作为普通 service 消息参数传给当前 room/scene/map Service。它不是 actor reference，也不能用于 `shield.send/call` 的 target。

## 房间、场景与地图

房间、场景和地图都是普通 Service。所有客户端消息先进入 PlayerService，PlayerService 根据 route_id 选择 handler。如果某个 handler 需要转发给 room/scene/map，PlayerService 用 `shield.send` 发送到目标服务地址。

```text
Client → Gateway → PlayerService
  route_id = "player.move"    → PlayerService 自己处理
  route_id = "room.ready"     → PlayerService 转发给 RoomService
  route_id = "scene.position" → PlayerService 转发给 SceneService
```

room/scene/map 的动态路由保存在 PlayerService 私有状态中：

```lua
-- PlayerService 内部
state.room_address = nil
state.scene_address = nil

function M.on_enter_room(room_address)
    state.room_address = room_address
end

function M.room_ready(ctx, client, request)
    shield.send(state.room_address, "on_client_ready", client:player_id(), request)
end
```

进入房间或切图时，由 AuthService、PlayerService 或授权的目标 Service 通过 `shield.send` 通知 PlayerService 更新私有路由地址。session binding 始终指向 PlayerService，不因进入房间或场景而改变。

## 生命周期

PlayerService 使用普通 Service lifecycle，并可额外接收以下结构化 client control：

- `ClientBound`：认证完成或重连成功；
- `ClientDisconnected`：连接失效；
- `ClientReconnected`：新 epoch 已绑定到原 owner；
- `ClientUnbound`：登出、顶号或 owner 被替换。

这些是 runtime control message，不是客户端 wire RPC，不按 route 字符串分发，也不使用普通业务 body。

可选 Lua lifecycle hook 只表达状态变化：

```lua
function M.on_client_bound(client)
    state.client = client:ref()
end

function M.on_client_disconnected(reason)
    state.connected = false
end

function M.on_client_reconnected(client)
    state.client = client:ref()
    state.connected = true
end
```

是否保留断线玩家、保留多久、何时保存和退出，由 `shield_player` 策略或业务 Service 决定。断线不创建独立 PlayerSession 对象。

## 重连

重连必须满足：

1. 新连接重新认证；
2. Gateway 分配新 session epoch；
3. player owner 确认是否接受恢复；
4. Gateway 原子更新 `player_id`、`player` 地址和 epoch；
5. PlayerService 替换保存的 `ClientRef`；
6. 旧 epoch 的回包、关闭和路由更新全部失效。

离线消息可以保存在 PlayerService 私有状态或外部持久化/队列中。它不是客户端连接的发送队列,也不绕过注册的 `shield.client_rpc.*` server-to-client helper。

## 持久化

持久化是 PlayerService 的业务协作能力：

- 数据加载和保存通过普通 Service `send/call` 或数据插件 binding；
- adapter 不拥有连接池，不引入 ORM 或 Entity manager；
- 只持久化明确白名单的玩家数据；
- client identity、ServiceAddress、CAF handle 和 `ClientContext` 不进入持久化数据；
- 保存失败必须返回明确错误并由业务决定重试策略。

## 容量模型

第一版只实现 one-player-one-service。是否需要分片必须由真实压测决定，至少测量：

- 单 Lua VM 常驻内存；
- 在线玩家数与 actor 调度开销；
- mailbox 峰值与尾延迟；
- 登录/重连风暴；
- 持久化和定时任务压力。

只有数据证明该模型不可接受时，才另立架构决策讨论 shard/pool。`PlayerSession`/`PlayerRef` 是 `shield_player` 的模块层会话对象与值引用，不是 Entity 抽象。

## 明确不做

- PlayerSession 作为跨服务运行时对象。
- PlayerRef 查询或远程 resolve。
- Entity-like 玩家对象、base/cell/avatar 模型。
- 玩家对象自己的 mailbox、RPC、timer 或 coroutine runtime。
- 客户端消息通用回调及 Lua 二次 route dispatch。
- PlayerService 直接操作 socket、session 连接对象、codec、frame 或 envelope。
- 客户端通过 body 指定 player id、目标 Service 或 route。

## shield_player 模块契约（P0）

本节是 `shield_player` P0 的实施契约；决策依据见 [open-decisions](open-decisions.md) OD-008~OD-014 与 [官方可选模块契约](optional-modules.md)。模块未启用时 `shield.player.*` 返回 `nil, {code="module_unavailable"}`。

### 角色模型

- **认证入口 service**：gateway listener 的预登录 target。它也是普通 service，通过 `shield.player.setup` 注册 `auth` 钩子，并在 pre-login route 的编译绑定 handler 内显式调用 `player:authenticate(ctx, client, request)`。认证成功由该业务代码完成 bind（框架在 authenticate 内部完成裁决与 bind）。
- **玩家实例 service**：one-player-one-service（AD-02/OD-012）。actor 配置 `instances: 0` 只允许动态 spawn；authenticate 裁决通过后由框架 spawn（opts 携带 `player_id` 与 `client_ref`），`shield.client.bind` 把 session 单一 target 指向该实例名。

### setup 与钩子

`local player = shield.player.setup(M, opts)` 在 module-level 调用；opts 的钩子字段接受函数或模块方法名（去 `on_` 前缀）。必填钩子缺失返回 `nil, {code="setup_invalid"}`，service 不进入 running。

| 钩子 | 必填 | 签名 | 触发时机 |
| --- | --- | --- | --- |
| `auth` | 是 | `auth(ctx, client, request)` → `{player_id=..., device_id=?, anonymous=?, spectator=?}` 或 `true, player_id` 或 `false, code` | `player:authenticate` 内 |
| `login` | 是 | `login(ctx, client, auth_result)` | 首次 Bound（新会话）后 |
| `client_message` | 是 | `client_message(ctx, client, route_name, request)` → `true` 放行 / `false, code` 拒绝 | 每条认证后业务消息，在编译绑定 handler 之前 |
| `disconnect` | 是 | `disconnect(ctx, client, reason)` | `Disconnected` 控制消息 |
| `logout` | 是 | `logout(ctx, client, reason)` | 登出（`timeout` / `replaced` / 业务主动） |
| `ready` | 否 | `ready(ctx, client)` | login 后；默认实现置 `state="ready"` |
| `reconnect` | 否 | `reconnect(ctx, client)` | 重连窗口内恢复；默认恢复状态并投递离线消息 |
| `save` | 否 | `save(ctx, reason)` | 定时与登出；默认走 persistence adapter，未配置为 no-op |

- 未提供的可选钩子由框架执行上表默认实现（不允许隐式 noop）；业务覆盖后默认实现不自动执行，保留默认行为需显式调用 `shield.player.defaults.*`。
- `client_message` 是玩家消息准入守卫：ready 前到达的业务消息直接拒绝（丢帧 + warn + 计数，LAPI-011-06 允许的拒绝路径），进入 ready 后守卫放行才执行编译绑定 handler。离线期间的可靠 s2c 推送用框架门面 `player:push(route_name, payload)`：在线直接经 s2c helper 发出，离线入队（上限 `message_queue_limit`，满则丢弃 + warn），reconnect 默认实现按序 flush。守卫拒绝与 gateway 入站校验失败同形态：丢帧 + warn + 计数，不回写错误帧。`shield.player.defaults.allow` 提供显式恒放行实现。
- setup 过的 service 的 gateway 层 module hooks（`on_client_bound` 等）照常工作，互不替代；业务不要同时维护两套。

### 认证与状态机

`player:authenticate(ctx, client, request)`（协程内调用）按序执行：

1. 调 `auth` 钩子；返回 `false/nil, code` 即认证拒绝（OMOD-PL-011：表达为 player 契约结果，不是 `handler_error`）。
2. `anonymous/spectator` 声明未开启 → `nil, {code="anonymous_disabled"/"spectator_disabled"}`。
3. PlayerManager 裁决（`multi_device`）：`single` 且 uid 在线 → `false, {code="already_online"}`；`kick_old` → 旧实例以 `logout("replaced")` 退出后继续；`multi` 超过 `max_devices` → `false, {code="too_many_devices"}`；uid 处于重连窗口内 → 恢复路径（复用实例，不重复 spawn）。
4. spawn 玩家实例（恢复则复用）并 `shield.client.bind(client, uid, <实例名>)`；bind 失败（如 `client_rpc.epoch_expired`）原样传播。
5. 成功返回 `true, ref`（新 epoch 的 `ClientRef`），业务据以回包。

状态机：`connecting → authenticating → online → ready`；`anonymous`/`spectator` 是 opt-in 状态变体（anonymous 不触发 persistence；spectator 拒绝写操作 `spectator_readonly`）；`Disconnected` → `disconnected`（重连窗口计时）→ 窗口内恢复回 `ready`，窗口超时 → `logout("timeout")`。重连恢复由 player 模块按 uid 状态判定，不依赖 gateway 的 `Reconnected` 控制消息（该消息仍预留）。

### PlayerSession 与 PlayerRef

- `PlayerSession` 是 `shield_player` 的会话记录（C++ 侧，PlayerManager 持有元数据：uid/state/kind/device_id/service_id/node_id/epoch）；玩家业务数据仍是该 service 私有 Lua table（经 setup 返回的门面 `player:set_data/get_data` 访问，`save` 默认实现按白名单字段序列化）。
- `PlayerRef = {uid, node_id, service_id, epoch}`（OD-010）。跨 service payload 只传 `PlayerRef`（JSON 标记 `__shield_player_ref` 双向物化，与 `ClientRef` 同机制）；裸连接句柄与完整 `PlayerSession` 不可跨 service 传递。
- `shield.player.resolve(ref)`：P0 仅本地。字段非法 → `nil, {code="invalid_player_ref"}`；非本节点 → `nil, {code="remote_resolve_unimplemented"}`；本地无此会话 → `nil, {code="player_not_found"}`。成功返回只读会话门面（`uid()/state()/kind()/service_id()/ref()`）。
- `shield.player.get(uid)` 返回本机主会话门面；`shield.player.manager` 暴露 `unregister/get/get_devices` 索引操作。

### 配置段（owner：shield_player）

```yaml
player:
  multi_device: single        # single | kick_old | multi
  max_devices: 2              # multi 时的设备上限
  anonymous: false            # opt-in
  spectator: false            # opt-in
  reconnect_window_ms: 30000
  message_queue_limit: 64     # player:push 离线队列上限；ready 前入站消息不排队，直接拒绝
  persistence:
    binding: ""               # 数据插件 binding 逻辑名；空 = 未配置（save 默认 no-op）
    fields: []                # 白名单字段（player 数据表中允许持久化的键）
    on_save_error: log        # log | panic
    save_interval_ms: 60000   # 0 = 仅登出时保存
```

persistence adapter（OD-009）：通过插件 namespace + binding 逻辑名访问数据能力，不拥有连接池、不引入 ORM；P0 约定目标实例提供 `player_save(uid, fields)` 调用（通用 adapter 形态随 toolchain 演进）。保存失败：`on_save_error="log"` 记录 `persistence_save_failed` 且 service 继续运行；`"panic"` 触发 `on_panic`。
