# 官方可选模块验收矩阵

本文定义官方可选模块的**验收矩阵**。它不是实现计划，也不是示例集合，而是后续编码必须满足的模块级契约。

目标：

- 防止 optional module 反向污染 `shield_core`
- 冻结模块启停边界
- 冻结 public surface、配置 ownership 和关键失败语义

`examples/hello_world/` 不替代本文测试。

## 通用验收项

| ID | 场景 | 操作 | 期望 |
| --- | --- | --- | --- |
| OMOD-000-01 | 不启用任何 optional module | 启动最小 runtime | core path 正常工作 |
| OMOD-000-02 | 配置中存在 optional 段但模块未注册或未启用 | bootstrap | 启动失败，错误包含模块名和配置路径 |
| OMOD-000-03 | optional module 初始化失败 | 启动 runtime | 错误定位到对应模块，不改写 core 错误语义 |
| OMOD-000-04 | optional module 关闭 | 调用最小 API | `shield.send/call/query`、gateway、插件 namespace 数据能力保持既有语义 |
| OMOD-000-05 | public header / Lua API 检查 | 构建/绑定测试 | optional module 不把 CAF 或旧架构 API 泄漏给用户 |
| OMOD-000-06 | optional module 被显式启用但配置非法 | bootstrap | 默认 fail fast；只有模块文档声明的 degrade 场景才允许继续启动并暴露 unhealthy |

## OMOD-CL `shield_cluster`

| ID | 场景 | 操作 | 期望 |
| --- | --- | --- | --- |
| OMOD-CL-001 | cluster 未启用 | `shield.query("local")` | 仍只查询本地 registry |
| OMOD-CL-002 | cluster 未启用 | 本地 `shield.send/call` | 语义与 core 完全一致 |
| OMOD-CL-003 | duplicate `NodeId` | 两节点握手 | 新连接被拒绝 |
| OMOD-CL-004 | 远端 name 解析 | `shield.cluster.query(node, name)` | 返回 remote `ServiceHandle` 或明确错误 |
| OMOD-CL-005 | remote handle send | `shield.send(remote_handle, ...)` | 复用本地 envelope / timeout 语义 |
| OMOD-CL-006 | remote handle call timeout | `shield.call(remote_handle, ...)` | 返回 `false, timeout` |
| OMOD-CL-007 | 节点离线 | pending call 存在 | 返回 `false, node_offline` |
| OMOD-CL-008 | 节点离线后恢复 | route cache 存在 | stale route 被清理并重新解析 |
| OMOD-CL-009 | cluster 关闭 | 远端发现接口 | 返回 `module_unavailable` 或同等级明确错误；不允许 silent success |
| OMOD-CL-010 | discovery 切换 | static / redis / k8s | 只影响节点发现，不改本地 service registry 规则 |
| OMOD-CL-011 | 远端连接失败但本地配置合法 | 启动 cluster | 可退化到单节点，本地服务继续运行，cluster 状态为 unhealthy |

## OMOD-GL `shield_global`

| ID | 场景 | 操作 | 期望 |
| --- | --- | --- | --- |
| OMOD-GL-001 | global 未启用 | 调用全局命名空间 | 返回 `module_unavailable` 或模块不可用错误 |
| OMOD-GL-002 | cache/queue/leaderboard 插件已启用、global 未启用 | 调用对应插件 namespace | 仍可正常使用 |
| OMOD-GL-003 | 分布式锁 | acquire/release | 基于模块契约完成，不暴露底层 Redis handle |
| OMOD-GL-004 | Redis 不可用 | distributed lock / rank / queue | 返回 Redis 相关错误，不回写 core 语义 |
| OMOD-GL-005 | rank reset | 到达重置周期 | 只影响 rank 数据，不修改 core scheduler 语义 |
| OMOD-GL-006 | queue 持久化 | 服务重启 | 队列按模块配置恢复或丢弃 |
| OMOD-GL-007 | rate limiter | 超额请求 | 返回模块级限流结果，不污染 `shield.call` 成功/失败语义 |
| OMOD-GL-008 | global 配置缺失 | 启动模块 | 定位为模块配置错误 |
| OMOD-GL-009 | 锁竞争/空队列等负结果 | acquire / pop | 作为模块契约结果返回，不伪装成 runtime transport error |

## OMOD-PL `shield_player`

| ID | 场景 | 操作 | 期望 |
| --- | --- | --- | --- |
| OMOD-PL-001 | player 未启用 | gateway 回调 | `on_client_bound/on_disconnect/on_client_unbound` 仍可独立工作 |
| OMOD-PL-002 | 启用 player | 登录成功 | `on_auth -> on_login` 顺序稳定 |
| OMOD-PL-003 | 客户端业务消息 | 编译绑定 `handler(ctx, client, request)` | 只传 request / player 状态，不走 legacy `msg_type` 入口 |
| OMOD-PL-004 | 跨 service 协作 | gateway -> player_manager | 只传 `session_id`/`ClientRef` 标记，不传裸连接句柄 |
| OMOD-PL-005 | 断线重连成功 | reconnect window 内重连 | 触发 `on_reconnect`，恢复 PlayerSession |
| OMOD-PL-006 | 重连窗口超时 | reconnect fail | 触发 `on_logout(reason=timeout)` |
| OMOD-PL-007 | 离线消息缓存 | 玩家离线后再上线 | 消息按模块策略投递并清空 |
| OMOD-PL-008 | 多设备策略 | second login | 行为符合 `single/multi/kick_old` 配置 |
| OMOD-PL-009 | PlayerManager 索引 | login/logout | uid/state 索引同步更新 |
| OMOD-PL-010 | 模块关闭 | 普通 service method dispatch | 不受 player hooks 影响 |
| OMOD-PL-011 | 认证拒绝 | `on_auth` 返回拒绝 | 表达为 player 契约结果，不伪装成 `handler_error` |

## OMOD-SV `shield_server`

| ID | 场景 | 操作 | 期望 |
| --- | --- | --- | --- |
| OMOD-SV-001 | server 模块未启用 | 启动 runtime | bootstrap / core 不依赖 `server_manager`；`shield.server.*` 返回 `module_unavailable` |
| OMOD-SV-002 | 启用 server | init complete | 状态自动进入 `running`（`mark_ready`），uptime/started_at 从该点计起 |
| OMOD-SV-003 | maintenance 切换 | `server:set_state("maintenance")` | 状态迁移成功并通知观察者；准入 gate 由业务层实现，现有 service 继续运行 |
| OMOD-SV-004 | shutdown 请求 | `server:shutdown(ms)` | 立即进入 `shutdown` 并通知观察者，ms 后走与 SIGINT 同路径的停止请求；drain 预算仍归 bootstrap |
| OMOD-SV-005 | 状态观察者 | `server:watch(fn)` 注册 | 状态变化在观察者 actor 线程收到 `fn(ctx, new_state)` 通知 |
| OMOD-SV-006 | 配置缺失/非法 | 启动模块 | 缺失用默认值；非法（空 name / info 超 64 字符）fail fast，定位为 `server_manager` 配置错误 |
| OMOD-SV-007 | 非法状态迁移 | `running -> starting` 等 | 返回 `invalid_state_transition`，不导致 runtime 崩溃、状态不变 |
| OMOD-SV-008 | server 模块未编译 | 调用任一 `shield.server.*` 入口 | 返回 `nil + {code="module_unavailable"}`，retryable=false |
| OMOD-SV-009 | 重复 watch / 未知 unwatch | 同 service 再次 `watch`；`unwatch` 已注销 id | 重复 watch 返回同一 id；未知 unwatch 静默成功 |
| OMOD-SV-010 | shutdown 非法参数 | `shutdown(-1)` / `shutdown(1.5)` / `shutdown("x")` | 返回 `invalid_argument`，不迁移状态、不触发停止 |
| OMOD-SV-011 | maintenance 下服务继续运行 | 置 maintenance 后 `send/call` | service messaging、gateway、插件 namespace 语义不变 |
| OMOD-SV-012 | 观察者 service 退出 | 迁移状态 | 失效观察者自动注销，不影响其他观察者与状态机 |
| OMOD-SV-013 | 配置非法 fail fast | `server_manager.info.name` 超长启动 | bootstrap 初始化失败，错误定位到 `server_manager` 配置路径 |
| OMOD-SV-014 | 只读观测暴露 | `root.server` / `GET /ops/status` | 输出 `{state, uptime_seconds, version, node_id, started_at_ms, name, info, watchers}`；未编译时 `root.server` 明确提示、响应无 server 块 |

### OMOD-SV 测试映射

- OMOD-SV-001 → `tests/lua_api/test_lua_api_server.cpp` stub 分支 + `shield_config_server_requires_module` 负向 CLI 用例
- OMOD-SV-002/007 → `tests/server/test_server_manager.cpp` ServerTransitions/ServerLifecycle/ServerSetState
- OMOD-SV-003/011 → ServerSetState + `tests/lua_api/test_lua_api_server.cpp`（迁移后 messaging 语义不变）
- OMOD-SV-004/010 → ServerShutdownScheduling + Shutdown suite（立即/重复/非法参数）
- OMOD-SV-005/009/012 → ServerWatch + Watch suite（幂等/自动注销/unwatch）
- OMOD-SV-006/013 → ServerConfigParsing + `InitializeFailsOnInvalidServerConfig`
- OMOD-SV-008 → stub 分支全入口断言
- OMOD-SV-014 → console/ops 套件 root.server 与 /ops/status server 块断言

## OMOD-OPS `shield_ops`

| ID | 场景 | 操作 | 期望 |
| --- | --- | --- | --- |
| OMOD-OP-001 | ops 未启用 | 访问 `/ops/*` | 无端点暴露 |
| OMOD-OP-002 | ops 启用 | `GET /ops/status` | 只读展示 runtime snapshot |
| OMOD-OP-003 | 敏感字段 | `/ops/config` / diagnostics | token、password、密钥被 redaction |
| OMOD-OP-004 | profile/console 默认关闭 | 生产配置启动 | 不自动开放高风险入口 |
| OMOD-OP-005 | auth 缺失 | 访问受控端点 | 请求被拒绝 |
| OMOD-OP-006 | 模块内部失败 | metrics/exporter 错误 | 不影响 core 运行和业务消息路径 |
| OMOD-OP-007 | 服务详情 | `/ops/services/:name` | 不暴露 CAF actor handle 或完整业务 payload |
| OMOD-OP-008 | 管理平面错误 | 非法 profile/control 请求 | 错误留在 HTTP/admin 平面，不回流为业务 Lua runtime error |
| OMOD-OP-009 | ops 管理端口绑定失败 | 启动 ops | fail fast，避免用户误以为观测入口已启用 |

## 与现有专题文档的关系

- 架构与所有权：见 [官方可选模块契约](optional-modules.md)
- cluster 细节：见 [集群运行时语义](runtime-cluster.md)
- global 细节：见 [全局能力运行时语义](runtime-global.md)
- player 细节：见 [玩家生命周期运行时语义](runtime-player.md)
- server 细节：见 [服务器状态运行时语义](runtime-server.md)
- ops 细节：见 [运维运行时语义](runtime-ops.md)
