# Lua API 测试用例设计

本文定义 Lua API 契约的测试矩阵。`examples/hello_world/` 是用户参考示例，不作为 API 正确性的唯一验收；API 正确性必须由独立测试覆盖。

测试目标：

- 每个 `shield.*` API 都有成功、失败和边界行为测试。
- coroutine 挂起语义必须通过运行时测试验证。
- 错误返回必须验证 `ok=false` 和 `Error.code`。
- 旧 API 必须有负向测试，确保不会被重新引入。

## 测试目录建议

```txt
tests/lua_api/
├── CMakeLists.txt
├── lua_api_test_harness.cpp
├── scripts/
│   ├── lifecycle_service.lua
│   ├── messaging_echo.lua
│   ├── timer_service.lua
│   ├── data_service.lua
│   ├── gateway_service.lua
│   └── legacy_api_service.lua
└── test_*.cpp
```

Harness 要求：

- 能启动最小 `shield_core` + `shield_lua` runtime。
- 能加载 Lua service module。
- 能模拟 service 间 `send/call`。
- 能推进 fake clock 或使用可控 timer scheduler。
- 能注入 fake DB/Redis backend。
- 能捕获 Lua log、error、panic 和 dead letter。

## LAPI-001 Module Loader

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-001-01 | Lua 文件返回 table | spawn service | spawn 成功 |
| LAPI-001-02 | Lua 文件返回 nil | spawn service | `invalid_service_module` |
| LAPI-001-03 | Lua 文件语法错误 | spawn service | `script_load_failed` |
| LAPI-001-04 | module 顶层抛错 | spawn service | `script_load_failed` |

## LAPI-002 Lifecycle

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-002-01 | `on_init` 无返回值 | spawn | 成功 |
| LAPI-002-02 | `on_init` 返回 true | spawn | 成功 |
| LAPI-002-03 | `on_init` 返回 false, err | spawn | `init_failed`，name rollback |
| LAPI-002-04 | `on_init` 抛错 | spawn | `init_failed`，触发 panic 记录 |
| LAPI-002-05 | service exit | `shield.exit("normal")` | 调用 `on_exit("normal")` |
| LAPI-002-06 | `on_exit` 调用 `shield.call` | exit | 返回/记录 `api_not_allowed_in_exit` |
| LAPI-002-07 | runtime shutdown | shutdown | 先调用 `on_shutdown(ctx)`，再调用 `on_exit("stopping")` |
| LAPI-002-08 | `on_shutdown` 超时 | shutdown | 记录 timeout，继续调用 `on_exit` 并释放服务 |
| LAPI-002-09 | `on_shutdown` 调用 `shield.call` | shutdown drain | deadline 内允许挂起并恢复 |

## LAPI-003 Registry

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-003-01 | spawn with name | `shield.query(name)` | 返回等价 handle |
| LAPI-003-02 | 重复 name | spawn second | `name_conflict` |
| LAPI-003-03 | init 失败占名 | query name | 不可见 |
| LAPI-003-04 | `shield.register` 新 name | query | 可见 |
| LAPI-003-05 | service exit | query old names | `service_not_found` |
| LAPI-003-06 | invalid name | register | `invalid_name` |

## LAPI-004 Message Send

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-004-01 | target by name | send method args | receiver handler 收到业务参数 |
| LAPI-004-02 | target by handle | send | receiver 收到 |
| LAPI-004-03 | target missing | send | `false, service_not_found` |
| LAPI-004-04 | method missing | send | dead letter 记录 |
| LAPI-004-05 | mailbox full | send | `mailbox_full` |
| LAPI-004-06 | self-send | send self | 不 reentrant，下一调度点执行 |

## LAPI-005 Message Call

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-005-01 | callee returns one value | call | `true, value` |
| LAPI-005-02 | callee returns false, reason | call | `true, false, reason` |
| LAPI-005-03 | callee returns trailing nil | call | argc 保留 |
| LAPI-005-04 | callee throws | call | `false, handler_error` |
| LAPI-005-05 | method missing | call | `false, method_not_found` |
| LAPI-005-06 | timeout | call_timeout | `false, timeout` |
| LAPI-005-07 | late response | callee returns after timeout | response discarded |
| LAPI-005-08 | nested call | handler calls another service | runtime thread 不阻塞 |

## LAPI-006 Context

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-006-01 | send from A to B | B calls `shield.sender()` | equals A handle |
| LAPI-006-02 | handler returns | saved callback reads sender | `nil` or `context_expired` |
| LAPI-006-03 | timer callback | `shield.sender()` | nil |
| LAPI-006-04 | trace enabled | call chain | trace id propagated |
| LAPI-006-05 | deadline enabled | call_timeout | remaining deadline visible |

## LAPI-007 Timers And Tasks

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-007-01 | timer_once | advance clock | callback once |
| LAPI-007-02 | timer fixed-delay | callback takes time | next run after callback completes |
| LAPI-007-03 | cancel timer | advance clock | callback not called |
| LAPI-007-04 | timer error | callback throws | `on_error` called，timer stops |
| LAPI-007-05 | sleep | coroutine sleeps | runtime can process other message |
| LAPI-007-06 | fork | task runs | owner service tracked |
| LAPI-007-07 | service exit | owned timers/tasks | canceled |

## LAPI-008 Plugin Data API

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-008-01 | `database.default -> db.main` binding configured | `shield.database.mysql("database.default"):query(...)` | `true, rows` or plugin-specific SQL result |
| LAPI-008-02 | binding missing | `shield.database.mysql("database.default")` | `nil, module_unavailable` |
| LAPI-008-03 | target instance failed/unavailable | data plugin namespace call | `nil, module_unavailable` |
| LAPI-008-04 | SQL plugin returns query error | `db:query(...)` | `false, db_query_failed` or plugin-mapped database error |
| LAPI-008-05 | `cache.session -> cache.redis` binding configured | `shield.cache.redis("cache.session"):get(key)` | `true, value` or `true, nil` for miss |
| LAPI-008-06 | cache binding missing | `shield.cache.redis("cache.session")` | `nil, module_unavailable` |
| LAPI-008-07 | queue binding configured | `shield.queue.redis("queue.events"):subscribe(...)`; service exit | subscription canceled |
| LAPI-008-08 | SQL transaction supported | `db:transaction(fn)` | commit forwards payload; rollback returns reason; closed tx rejected |
| LAPI-008-09 | document binding configured | `shield.database.mongodb("document.default"):find(...)` | `true, cursor/docs` |
| LAPI-008-10 | leaderboard binding configured | `shield.leaderboard.redis("leaderboard.default"):top_n(...)` | `true, entries` |

## LAPI-009 Gateway API

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-009-01 | gateway service | simulated connect | `on_connect(session)` called |
| LAPI-009-02 | client frame decoded | deliver payload | `on_client_message(session, payload)` called |
| LAPI-009-03 | disconnect | close session | `on_disconnect(session, reason)` called |
| LAPI-009-04 | send queue full | `session:send` | `false, session_send_queue_full` |
| LAPI-009-05 | stale session | send after close | `false, session_closed` |

## LAPI-010 Legacy API Rejection

| Case | 操作 | 断言 |
| --- | --- | --- |
| LAPI-010-01 | `shield.service("x")` | `nil` or `legacy_api_removed` |
| LAPI-010-02 | `shield.plugin.on(...)` | `nil` or `legacy_api_removed` |
| LAPI-010-03 | `shield.db.query(...)` / `shield.redis.get(...)` | unavailable or `legacy_api_removed`; use plugin namespace |
| LAPI-010-04 | `shield.db:query(...)` / `shield.redis:get(...)` | unavailable or `legacy_api_removed`; use plugin namespace |
| LAPI-010-05 | service only defines `on_message(src, type, data)` | send method does not dispatch through legacy entrypoint |
| LAPI-010-06 | DI injection API | unavailable |

## LAPI-010A Local Event API

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-010A-01 | 当前 service 注册 listener | `shield.event.emit(name, payload)` | listener 在同一 VM 内同步收到 payload |
| LAPI-010A-02 | listener unsubscribe | 调用返回的 `off()` 后 emit | listener 不再触发 |
| LAPI-010A-03 | listener 抛错 | emit | 触发 `on_error(err, {type="event"})`，其他 listener 继续 |
| LAPI-010A-04 | 另一个 service 注册同名 listener | 从当前 service emit | 另一个 service 不收到，证明不跨 VM |
| LAPI-010A-05 | lifecycle event name | emit `application_ready` / `shutdown` | 只作为普通本地事件名处理，不触发 runtime lifecycle |

## LAPI-011 Player Lifecycle

适用前提：`shield_player` 已启用。未启用时整个 LAPI-011 矩阵跳过，且 `shield.player.*` 调用应返回 `module_unavailable`。

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-011-01 | setup 缺 `auth` 字段 | spawn player service | `nil, setup_invalid` |
| LAPI-011-02 | setup 缺 `login` 字段 | spawn player service | `nil, setup_invalid` |
| LAPI-011-03 | setup 缺 `client_message` 字段 | spawn player service | `nil, setup_invalid` |
| LAPI-011-04 | setup 完整且 `ready` 缺省 | 登录成功 | 默认 ready 标记 `player.state="ready"`，开始分发 `client_message` |
| LAPI-011-05 | setup 提供 `ready` 字段 | 登录成功 | 调用业务 ready 后进入 ready；不是 service/application `on_ready` |
| LAPI-011-06 | ready 前收到客户端业务消息 | deliver payload | 排队或拒绝，但不得调用 `client_message` |
| LAPI-011-07 | setup 缺 `disconnect` 字段 | 触发断线 | 进入重连窗口，默认实现被调用 |
| LAPI-011-08 | setup 缺 `logout` 字段 | 玩家离线 | `PlayerManager.unregister` 被调用 |
| LAPI-011-09 | setup 缺 `save` 字段且未配置 persistence | 触发定时保存 | no-op，无数据插件调用 |
| LAPI-011-10 | setup 覆盖 `disconnect` | 触发断线 | 业务实现被调用，默认实现不执行 |
| LAPI-011-11 | setup 完整 + persistence 启用 | 触发 `on_save` 默认实现 | adapter 通过配置的插件 binding 持久化白名单字段 |
| LAPI-011-12 | setup 完整 + persistence 未启用 | 触发 `on_save` 默认实现 | no-op，无数据插件调用 |
| LAPI-011-13 | persistence `save` 失败（`on_save_error="log"`） | adapter 返回错误 | 错误码 `persistence_save_failed`，service 继续运行 |
| LAPI-011-14 | persistence `save` 失败（`on_save_error="panic"`） | adapter 返回错误 | 触发 `on_panic` |
| LAPI-011-15 | persistence 字段含 function | setup | `nil, setup_invalid` |
| LAPI-011-16 | `PlayerRef` LuaPack 编码 | encode | 独立 type tag，字段完整 |
| LAPI-011-17 | `shield.player.resolve` 本地 ready 玩家 | 解析 ref | 返回 `PlayerSession` |
| LAPI-011-18 | `shield.player.resolve` 本地已下线玩家 | 解析 ref | `nil, player_not_found` |
| LAPI-011-19 | `shield.player.resolve` 远端 ref（remote resolve 未启用） | 解析 ref | `nil, remote_resolve_unimplemented` |
| LAPI-011-20 | `shield.player.resolve` 字段非法 | 解析 ref | `nil, invalid_player_ref` |
| LAPI-011-21 | cross-service 传 `PlayerRef` | send payload | receiver 拿到等价 ref |
| LAPI-011-22 | cross-service 传 `SessionHandle` | send payload | runtime 拒绝并返回错误 |
| LAPI-011-23 | cross-service 传完整 `PlayerSession` | send payload | runtime 拒绝并返回错误 |
| LAPI-011-24 | auth 返回 `anonymous=true` 且未开启 anonymous | 认证 | `nil, anonymous_disabled` |
| LAPI-011-25 | anonymous 已开启且 auth 返回 `anonymous=true` | 认证 | 状态进入 `anonymous`，默认不触发 persistence |
| LAPI-011-26 | auth 返回 `spectator=true` 且未开启 spectator | 认证 | `nil, spectator_disabled` |
| LAPI-011-27 | spectator 已开启 | 调用 `player:set_data` 或 `player:save` | 拒绝并返回 `spectator_readonly` |
| LAPI-011-28 | multi_device `single` 且 UID 已在线 | 新连接认证 | `false, already_online` |
| LAPI-011-29 | multi_device `kick_old` 且 UID 已在线 | 新连接认证 | 旧会话 `logout` reason 为 `replaced` |
| LAPI-011-30 | multi_device `multi` 且超过 `max_devices` | 新连接认证 | `false, too_many_devices` |
| LAPI-011-31 | `player_pool` 模式 | 同一 uid 多条消息 | 路由到同一 shard 且单玩家 handler 串行 |
| LAPI-011-32 | `shield.player.Base.extend(opts)` | spawn player service | 行为等价于 `shield.player.setup(M, opts)` |
| LAPI-011-33 | Base 覆盖可选 hook | 触发 hook | 默认实现不自动执行 |

## 验收要求

- 当前 CTest 已有 `shield_runtime_lua_smoke` 覆盖 YAML actors 启动、
  `shield.self/now/spawn/send/call/sender/names/exit/on_exit` 的单节点同步路径；
  `shield_runtime_registry_smoke` 覆盖 `query/register/unregister/names` 的本地 registry 路径；
  插件系统相关 smoke 覆盖 manifest scan、instance、binding、`register_lua` 分发和缺失 binding 的 `module_unavailable` 返回形态。
  它们不是完整 LAPI 矩阵的替代品。
- Lua API 契约变更必须先更新本文。
- 每个新增 API 至少补充一个成功用例和一个失败用例。
- 每个错误码必须在测试中至少出现一次。
- `examples/hello_world/` 可以更贴近用户体验，但不能替代上述测试。

## 剩余延迟用例

以下用例仍依赖后续实现或专用 mock harness，当前尚未完整验证。已完成的历史延迟项保留删除线，作为状态记录。

| Case | 延迟原因 |
| --- | --- |
| ~~LAPI-005-06~~ | ~~call timeout 未实现~~ 已实现：`check_call_timeouts` 已接入 `pump_once`，LAPI-005-06 覆盖协程 timeout + 同步降级 ✅ |
| ~~LAPI-005-07~~ | ~~late response 丢弃~~ call timeout 已实现，超时后 caller 已 resume；callee 返回时 `resume_caller` 在 `pending_calls` 中找不到 session，静默丢弃。行为正确 ✅ |
| ~~LAPI-005-08~~ | ~~nested call~~ 协程路径支持：caller yield 后 worker 处理 callee mailbox，`CallApiFromLuaWrapsRuntimeResult` 测试覆盖嵌套 call ✅ |
| LAPI-006-04 | trace id 传播：`shield.trace()` 返回固定值 `"trace:0"`，完整链路传播属于 Phase 2+。当前返回值可被调用，不会报错 |
| ~~LAPI-006-05~~ | ~~deadline 可见性~~ 已实现：`shield.deadline()` 从 dispatch context 读取，通过消息传播 ✅ |
| ~~LAPI-007-04~~ | ~~`on_error` hook 调用~~ 已实现：`invoke_hook` 调用 service table 上的 `on_error`，`OnErrorHookCalledOnHandlerThrow` 测试覆盖 ✅ |
| ~~LAPI-007-05~~ | ~~`shield.sleep` coroutine 语义~~ 已由 LAPI-007-08 覆盖 ✅ |
| LAPI-008-02/03/06 | 缺失 binding / 目标 instance unavailable 的 `module_unavailable` 需要插件 mock harness 覆盖 |
| LAPI-008-04 | SQL error 路径需要由 SQL 插件测试或统一 mock 插件覆盖 |
| LAPI-008-07 | subscribe then exit：需要 queue plugin mock/集成测试覆盖订阅生命周期 |
| ~~LAPI-009-01~05~~ | ~~Gateway session 模拟~~ 已覆盖：connect/message/disconnect/queue_full/stale_send 共 6 个测试 ✅ |
| LAPI-009-real-session | 真实 TCP session 到 Lua `SessionHandle` userdata 的封装仍未覆盖；当前 bootstrap 已接入 TCP listener 与 LuaGatewayBridge，但测试仍使用 table 模拟 |
| ~~LAPI-002-06~~ | ~~`on_exit` 中调用 `shield.call` 返回 `api_not_allowed_in_exit`~~ 已实现：`_is_in_exit()` + Lua wrapper guard，`OnExitCallGuard` 测试覆盖 ✅ |
