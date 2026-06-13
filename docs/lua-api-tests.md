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

## LAPI-008 Data API

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-008-01 | fake DB enabled | `shield.db.query` | `true, rows` |
| LAPI-008-02 | DB disabled | `shield.db.query` | `false, module_unavailable` |
| LAPI-008-03 | SQL error | query | `false, db_query_failed` |
| LAPI-008-04 | fake Redis enabled | `shield.redis.get` | `true, value` |
| LAPI-008-05 | Redis disabled | `shield.redis.get` | `false, module_unavailable` |
| LAPI-008-06 | subscribe then exit | service exit | subscription canceled |

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
| LAPI-010-03 | `shield.db:query(...)` | fails; dot API required |
| LAPI-010-04 | service only defines `on_message(src, type, data)` | send method does not dispatch through legacy entrypoint |
| LAPI-010-05 | DI injection API | unavailable |

## LAPI-011 Player Lifecycle

适用前提：`shield_player` 已启用。未启用时整个 LAPI-011 矩阵跳过，且 `shield.player.*` 调用应返回 `module_unavailable`。

| Case | 设置 | 操作 | 断言 |
| --- | --- | --- | --- |
| LAPI-011-01 | setup 缺 `auth` 字段 | spawn player service | `nil, setup_invalid` |
| LAPI-011-02 | setup 缺 `login` 字段 | spawn player service | `nil, setup_invalid` |
| LAPI-011-03 | setup 缺 `client_message` 字段 | spawn player service | `nil, setup_invalid` |
| LAPI-011-04 | setup 缺 `disconnect` 字段 | 触发断线 | 进入重连窗口，默认实现被调用 |
| LAPI-011-05 | setup 缺 `logout` 字段 | 玩家离线 | `PlayerManager.unregister` 被调用 |
| LAPI-011-06 | setup 缺 `save` 字段且未配置 persistence | 触发定时保存 | no-op，无 `shield_data` 调用 |
| LAPI-011-07 | setup 覆盖 `disconnect` | 触发断线 | 业务实现被调用，默认实现不执行 |
| LAPI-011-08 | setup 完整 + persistence 启用 | 触发 `on_save` 默认实现 | adapter 调用 `shield_data` 持久化白名单字段 |
| LAPI-011-09 | setup 完整 + persistence 未启用 | 触发 `on_save` 默认实现 | no-op，无 `shield_data` 调用 |
| LAPI-011-10 | persistence `save` 失败（`on_save_error="log"`） | adapter 返回错误 | 错误码 `persistence_save_failed`，service 继续运行 |
| LAPI-011-11 | persistence `save` 失败（`on_save_error="panic"`） | adapter 返回错误 | 触发 `on_panic` |
| LAPI-011-12 | persistence 字段含 function | setup | `nil, setup_invalid` |
| LAPI-011-13 | `PlayerRef` LuaPack 编码 | encode | 独立 type tag，字段完整 |
| LAPI-011-14 | `shield.player.resolve` 本地在线玩家 | 解析 ref | 返回 `PlayerSession` |
| LAPI-011-15 | `shield.player.resolve` 本地已下线玩家 | 解析 ref | `nil, player_not_found` |
| LAPI-011-16 | `shield.player.resolve` 远端 ref（P0） | 解析 ref | `nil, remote_resolve_unimplemented` |
| LAPI-011-17 | `shield.player.resolve` 字段非法 | 解析 ref | `nil, invalid_player_ref` |
| LAPI-011-18 | cross-service 传 `PlayerRef` | send payload | receiver 拿到等价 ref |
| LAPI-011-19 | cross-service 传 `SessionHandle` | send payload | runtime 拒绝并返回错误 |
| LAPI-011-20 | cross-service 传完整 `PlayerSession` | send payload | runtime 拒绝并返回错误 |

## 验收要求

- 当前 CTest 已有 `shield_runtime_lua_smoke` 覆盖 YAML actors 启动、
  `shield.self/now/spawn/send/call/sender/names/exit/on_exit` 的单节点同步路径；
  `shield_runtime_registry_smoke` 覆盖 `query/register/unregister/names` 的本地 registry 路径；
  `shield_runtime_data_smoke` 覆盖启用 DB/Redis mock pool 后的 Lua data API 返回形态。
  它们不是完整 LAPI 矩阵的替代品。
- Lua API 契约变更必须先更新本文。
- 每个新增 API 至少补充一个成功用例和一个失败用例。
- 每个错误码必须在测试中至少出现一次。
- `examples/hello_world/` 可以更贴近用户体验，但不能替代上述测试。
