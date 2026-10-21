# 服务器状态运行时语义

本文冻结 `shield_server` 可选模块 P0 的实施契约：服务器状态机、运行时信息、状态变更通知与关闭交接。决策依据见 [open-decisions](open-decisions.md) OD-016/OD-017；模块横向边界见 [官方可选模块契约](optional-modules.md)。

`shield_server` 是官方可选模块，不属于 `shield_core`。编译开关 `SHIELD_ENABLE_SERVER`（默认 OFF）。模块未启用时 bootstrap / core 不依赖 `server_manager`，runtime 照常启动、运行、优雅关闭。

## 核心决策

- ServerManager 是进程级 C++ 单例（照 PlayerManager 形态：`global()/set_global()`，bootstrap 持 `unique_ptr`）。它不是 service：没有 mailbox、没有 CAF actor、不 spawn service、不需要 Starter。
- 服务器状态与玩家状态分离。ServerManager 只保存服务器级状态与只读运行时信息。
- 状态真相在 C++ 侧；Lua 通过 `shield.server.*` 门面只读查询或显式迁移。
- 状态变更通知走既有 service messaging 的 system 通道投递给观察者 service，不引入全局事件总线。
- `shutdown(ms)` 只做"迁移 + 通知 + 定时请求停止"三件事；真正的 drain 预算与优雅关闭顺序仍归 bootstrap 的 `shutdown.timeout.*` 路径。
- 维护模式 P0 只提供 `state()` 查询真相；登录准入 gate 由业务 / `shield_player` 层自行实现，core 与 gateway 不感知维护状态。

## 状态机

| 状态 | 说明 | 行为 |
|------|------|------|
| `starting` | 启动中 | bootstrap 初始化期间的初始状态 |
| `running` | 正常运行 | init complete 后自动进入 |
| `maintenance` | 维护模式 | 业务语义；runtime 与 service 路径不受影响 |
| `shutdown` | 关闭中 | 终态；进入后不再接受任何迁移 |

合法迁移（`transition_allowed`，纯函数，表驱动）：

```text
starting -> running       (bootstrap init complete 自动触发, mark_ready)
starting -> shutdown      (启动期即可请求关闭)
running  -> maintenance
running  -> shutdown
maintenance -> running
maintenance -> shutdown
```

非法迁移（返回错误，不崩溃、不改状态）：

- `starting -> maintenance`：启动未完成不允许直接进维护。
- 任何 `-> starting`：`starting` 只能是初始状态。
- 任何 `-> shutdown` 之后的一切迁移：`shutdown` 是终态。

同值 `set_state`（如 `running -> running`）幂等成功且不触发通知。

```text
┌────────────┐  init complete   ┌────────────┐
│  starting  │ ───────────────► │  running   │◄──────────┐
└─────┬──────┘                  └─────┬──────┘           │
      │ shutdown                      │ maintenance      │ resume
      ▼                               ▼                  │
┌────────────┐             ┌──────────────┐             │
│            │ ◄────────── │ maintenance  │ ────────────┘
│  shutdown  │             └──────┬───────┘
│  (终态)    │ ◄─────────────────┘ shutdown
└────────────┘
```

## 配置

owner 为 `server_manager` 配置段（[配置运行时语义](runtime-config.md) Optional Module Schema）。全部字段可选，未配置使用默认值：

```yaml
server_manager:
  name: "server_manager"       # 观测展示用节点名，非 service 名
  info:
    name: ""                   # 业务展示名
    version: ""                # 业务版本；shield.server:version() 的首选来源
    region: ""                 # 部署区域标签
```

校验规则：`name` 为空字符串非法；`info.*` 超过 64 字符非法。校验失败按 optional module 默认策略 fail fast（OD-005），错误定位到 `server_manager` 配置路径。

不存在 `enabled` / `state` / `on_state_change` 字段：启用与否只由编译开关决定；初始状态恒为 `starting`；通知开关由 watch 注册行为表达。

## Lua API

`shield.server` 是 `shield_server` 提供的门面（照 `shield.player` 模式）。模块未编译时每个入口返回 `nil + {code="module_unavailable"}`；模块已编译但 runtime 未安装实例时同样返回 `module_unavailable`。

```lua
local server = shield.server

-- 只读查询
server:state()        -- "starting" | "running" | "maintenance" | "shutdown"
server:uptime()       -- 从 running 起点的 steady 秒数（浮点）
server:version()      -- info.version，空则回退 app.version，双空为 ""
server:node_id()      -- cluster node_id；standalone 为 ""
server:started_at()   -- running 起点的 wall-clock 毫秒时间戳
server:config()       -- {name=..., info={name=..., version=..., region=...}}

-- 状态控制（返回 true 或 nil + Error）
server:set_state("maintenance")   -- 非法迁移 → {code="invalid_state_transition"}
server:shutdown(30000)            -- 30 秒后请求进程停止

-- 状态观察
local id = server:watch(fn)       -- 注册观察者，返回 watch id
server:unwatch(id)                -- 注销
```

### set_state

- 参数必须是 `starting/running/maintenance/shutdown` 之一的字符串；否则返回 `nil + {code="invalid_state"}`。
- 非法迁移返回 `nil + {code="invalid_state_transition", message="invalid state transition: a -> b"}`。
- 合法迁移返回 `true` 并触发通知；同值幂等返回 `true` 且不通知。

### shutdown

`server:shutdown(ms)`：

1. 校验参数：`ms` 必须是 ≥ 0 的整数，否则 `nil + {code="invalid_argument"}`。
2. 当前状态迁移到 `shutdown`（从 `running/maintenance/starting` 均合法；已在 `shutdown` 则返回 `nil + {code="shutdown_already_scheduled"}`）。
3. 立即对所有观察者投递一次 `"shutdown"` 通知。
4. `ms` 毫秒后调用进程停止请求——与 SIGINT/SIGTERM 同一停止路径（`shield::request_stop`）。

drain 预算、service 退出顺序与超时强杀仍由 bootstrap 的 `shutdown.timeout.*` 决定，`shutdown(ms)` 不接管也不缩短这些预算。重复调用返回 `shutdown_already_scheduled`，不会重复计时。

### watch / unwatch

```lua
-- 在服务 module 内注册观察
function M.on_init(args)
    shield.server:watch(function(ctx, new_state)
        if new_state == "shutdown" then
            M:save_all_data()
        end
    end)
end
```

- 回调签名是 `M.on_server_state_change` 的等价物：watch 直接接受函数，dispatch 时首参为 ctx（与 handler dispatch 同规则），第二参为新状态名字符串。
- 同一 service 重复 `watch` 幂等：返回同一个 id，不重复注册。
- 回调在观察者自己的 service actor 线程串行执行，与该 service 的其他消息不并发。
- 投递经 system 消息通道（`send_system`），与 gateway 桥同路径；观察者 handler 缺失时是静默 no-op，不报错。
- 观察者 service 退出后由 runtime 自动注销（`service_not_found` → 删除注册）；runtime 正在关闭时的投递失败不注销，留给停机路径处理。
- `unwatch(id)`：不存在或已注销的 id 静默成功（幂等）。

## 运行时信息

| 字段 | 来源 |
|------|------|
| `state` | ServerManager 状态机 |
| `uptime` | running 状态起点起的 steady clock 秒数 |
| `started_at` | running 起点的 wall-clock ms（`0` 表示尚未进入 running） |
| `version` | `server_manager.info.version` → 回退 `app.version` → 回退 `""` |
| `node_id` | cluster 模块的 node_id；standalone 为 `""` |
| `name` / `info` | `server_manager` 配置快照 |

`uptime` / `started_at` 从进入 `running` 计起（`mark_ready` 时刻），不是进程启动时刻；`starting` 期间 uptime 为 0。

## ops / console 暴露

P0 不设独立 `/ops/server` 端点。服务器状态作为只读快照并入两处既有观测面：

- `GET /ops/status` 响应增加 `server` 块：`{state, uptime_seconds, version, node_id, started_at_ms, name, info, watchers}`。
- 诊断控制台新增 `root.server` 命令，输出同一 JSON；`root.status` 概览同步含 server 摘要。

模块未编译 / 未启用时 `root.server` 输出明确提示，`/ops/status` 不含 `server` 块。ops 只读，不提供从 HTTP/console 修改状态的入口（状态控制只走 Lua API）。

## 错误码

| 错误码 | 场景 | retryable |
|--------|------|-----------|
| `module_unavailable` | `SHIELD_ENABLE_SERVER` 未编译，或模块未初始化 | 否 |
| `invalid_state` | `set_state` 参数不是合法状态名 | 否 |
| `invalid_state_transition` | 状态机不允许的迁移 | 否 |
| `shutdown_already_scheduled` | `shutdown` 重复调用 | 否 |
| `invalid_argument` | `shutdown` 参数不是非负整数 | 否 |

错误归属 `shield_server` 模块域（[架构](architecture.md) 错误归属表）；关服流程中的 runtime stop 失败仍属于 bootstrap/core 错误域。

## 明确不做

- 跨节点状态广播（各节点独立持有自己的 ServerManager）。
- core/gateway 内建维护准入 gate（业务层自行实现）。
- 独立 `/ops/server` HTTP 端点与 ops 写操作。
- ServerStarter（ServerManager 不参与 Starter 编排）。
- 全局事件总线（通知只投递给显式注册的观察者）。

外部停机（SIGINT/SIGTERM、console stop 等）由 bootstrap 在 `shutdown()` 入口统一把 ServerManager 迁移到 `shutdown` 态（已落地，P1）：观察者通知照常投递，但外部停机不提供 Lua 观察窗口——回调能否在拆除前执行取决于调度，不作为契约。
