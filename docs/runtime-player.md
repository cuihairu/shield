# 玩家运行时语义

本文冻结第一版玩家承载与客户端 RPC 边界。PlayerService 是普通 Service 的业务约定，不是新的 runtime 对象类型。

## 核心决策

- 一个在线玩家默认对应一个 PlayerService、一个 CAF actor 和一个私有 Lua VM。
- 玩家状态是该 Service 私有 Lua table。
- Service 是唯一拥有 mailbox、`send/call` 和 CAF address 的可寻址单元。
- 不引入 Player Entity、Entity mailbox、Entity RPC、PlayerSession runtime 对象或 PlayerRef 远程 resolve。
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
- `args.client_ref` 是值语义回包引用，不是 `SessionHandle`；
- module 注册目标逻辑服务名为 `player` 的客户端 RPC bindings；
- 断线、重连和登出控制消息由 Service adapter 送入同一 actor mailbox。

PlayerService 没有额外 mailbox，也没有独立于 Service 的 `send/call` API。

## 登录与绑定

推荐登录链路：

```text
client auth RPC
  → Gateway session.target = AuthService
  → AuthService handler
  → validate credentials
  → spawn or locate PlayerService
  → atomically update session: { target = PlayerServiceAddress, player_id, epoch }
```

认证前只能调用 descriptor 明确标记为 pre-login 的 RPC。认证成功后的原子更新必须同时写入 target、可信 `player_id` 和 epoch，不能只保存其中之一。

重复登录、顶号和恢复旧 PlayerService 都是 player 模块策略，但最终必须通过 Gateway 的 epoch 校验更新 session owner。

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
function M.move(client, request)
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

function M.room_ready(client, request)
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

离线消息可以保存在 PlayerService 私有状态或外部持久化/队列中。它不是 `SessionHandle` 的发送队列，也不绕过注册的 server-to-client RPC helper。

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

只有数据证明该模型不可接受时，才另立架构决策讨论 shard/pool。当前 API 不为它预留 PlayerSession、PlayerRef 或 Entity 抽象。

## 明确不做

- PlayerSession 作为跨服务运行时对象。
- PlayerRef 查询或远程 resolve。
- Entity-like 玩家对象、base/cell/avatar 模型。
- 玩家对象自己的 mailbox、RPC、timer 或 coroutine runtime。
- 客户端消息通用回调及 Lua 二次 route dispatch。
- PlayerService 直接操作 socket、SessionHandle、codec、frame 或 envelope。
- 客户端通过 body 指定 player id、目标 Service 或 route。
