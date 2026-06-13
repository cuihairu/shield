# Decision Log

本文集中记录已经收敛过的设计细节和仍需拍板的问题。关闭某个决策时，必须同步更新对应 authority document。

权威文档优先级：

1. [最终架构总纲](architecture.md)
2. [Lua API 契约](lua-api.md)
3. [配置运行时语义](runtime-config.md)
4. [运行时语义决策稿](runtime-semantics.md)
5. [官方可选模块契约](optional-modules.md)
6. [重构路线图](roadmap.md)

## Closed Decisions

### OD-001 CLI Contract

状态：closed。权威文档见 [启动流程运行时语义](runtime-bootstrap.md)。

决策：

- `--config <path>` 可缺省，默认 `config/app.yaml`。
- `--config` 可重复传入；多个文件按传入顺序合并，合并规则以 [配置运行时语义](runtime-config.md) 为准。
- `--node-id` 属于 `shield_cluster` optional module。未启用 cluster 时传入必须返回 CLI 错误。
- 旧 subcommand 只作为 legacy CLI 存在，不进入新的 `shield::run` 公共契约。
- `shield` 可执行文件和示例可执行文件都应直接调用 `shield::run(argc, argv)`，因此使用同一套参数集合。

### OD-002 Process Lifecycle

状态：closed。权威文档见 [启动流程运行时语义](runtime-bootstrap.md)。

决策：

- `shield::run` 负责通用 signal handling、runtime loop、help/version 输出和退出码。
- Windows 使用 `SetConsoleCtrlHandler`，POSIX 使用 SIGINT/SIGTERM。
- `shield::run` 初始化 runtime 后阻塞等待停止信号；底层 bootstrap API 只负责 initialize/shutdown。
- 用户自定义 main 可以绕过 `shield::run`，直接调用 lower-level bootstrap API。
- 退出码以 [启动流程运行时语义](runtime-bootstrap.md#进程退出码) 为准。

### OD-003 Physical Directory Layout

状态：closed。权威文档见 [目录结构设计](directory-structure.md) 和 [重构路线图](roadmap.md)。

决策：

- 短期保持当前 `src/core`、`src/net`、`src/lua` 等物理目录，先用 CMake target 和 include 边界收敛依赖。
- 不在 Phase 1 同时做大规模文件搬迁和语义重构。
- optional module 的目标物理目录保留为 `src/optional/shield_cluster` 等形态，但只在对应模块实现时创建或迁移。
- public include 路径保持 `include/shield/...`，不随 target 名做破坏性改名。

### OD-004 Phase 1 Network Scope

状态：closed。权威文档见 [网络运行时语义](runtime-network.md)。

决策：

- Phase 1 只冻结 TCP session、`SessionHandle` 和 basic transport framing。
- UDP、KCP、WebSocket 保留为目标语义和后续扩展，不作为 Phase 1 验收阻塞项。
- HTTP 业务 gateway 不进入 core；HTTP 管理端点只在 `shield_ops` 显式启用时存在。

### OD-005 Optional Module Failure Policy

状态：closed。权威文档见 [官方可选模块契约](optional-modules.md) 和 [配置运行时语义](runtime-config.md)。

决策：

- 配置包含 optional module 配置段，但对应模块未构建或未启用时，启动失败。
- 显式启用的 optional module 初始化失败默认 fail fast。
- `shield_cluster` 第一版允许远端连接失败时 degrade 到单节点，但必须持续暴露 unhealthy 状态；配置非法、节点身份冲突和本地监听失败仍 fail fast。
- `shield_ops` 启用后绑定管理端口失败必须 fail fast。
- `shield_global`、`shield_player`、`shield_server` 初始化失败默认 fail fast。

### OD-006 Data API Phase Split

状态：closed。权威文档见 [数据访问运行时语义](runtime-data.md) 和 [Lua API 契约](lua-api.md)。

决策：

- Phase 1 只实现 `shield.db.query/query_one/execute`。
- Phase 1 Redis 只实现 `shield.redis.get/set/del/exists/publish/subscribe`。
- prepared statement 可以作为驱动内部优化，但不进入 Phase 1 Lua API。
- 事务、显式 prepare handle、Redis pipeline、Redis eval、sentinel/cluster 配置均为 Phase 2+。

### OD-007 Legacy Compatibility Window

状态：closed。权威文档见 [重构路线图](roadmap.md) 和 [Lua API 测试用例](lua-api-tests.md)。

决策：

- 不提供 public compatibility shim。
- legacy Lua API 删除以 `lua-api-tests.md` 的负向测试为门槛。
- legacy C++ target 删除以 examples/tests 不再引用为门槛。
- 旧 CLI subcommand 可以作为 legacy CLI 代码短期存在，但不进入新的 `shield::run` 公共契约。

### OD-008 Player Setup API Form

状态：closed。权威文档见 [玩家生命周期运行时语义](runtime-player.md) 和 [官方可选模块契约](optional-modules.md)。

决策：

- P0 只冻结 `shield.player.setup(M, opts)` 作为业务 Player Service 唯一推荐入口。
- `shield.player.Base` 作为高级风格留待 Phase 2+；P0 不冻结。
- setup 的 opts 字段去 `on_` 前缀（`auth`/`login`/`client_message`/`disconnect`/`reconnect`/`logout`/`save`），与 module-level `on_init/on_exit` 隔离命名空间。
- 必填钩子（`auth`/`login`/`client_message`/`disconnect`/`logout`）缺失即返回 `nil, Error{code="setup_invalid"}`，service 不进入 running 状态。
- 未在 setup 中提供的可选钩子由框架按文档明列的默认实现执行，不允许"未提供即 noop"的隐式行为。
- 业务覆盖可选钩子时，默认实现不自动执行；保留默认行为需显式调用 `shield.player.defaults.*` helper。

### OD-009 Player Persistence Adapter Boundary

状态：closed。权威文档见 [玩家生命周期运行时语义](runtime-player.md) 和 [官方可选模块契约](optional-modules.md)。

决策：

- persistence adapter 是 `shield_player` 拥有的轻量契约，不属于 `shield_data`。
- adapter 底层必须通过 `shield.db.*` 或 `shield.redis.*` 调用，不重新定义 SQL/Redis 语义。
- adapter 不拥有连接池；连接池归属 `shield_data`。
- adapter 不引入 ORM、mapper、schema 工具链；只接受可 LuaPack 编码的 table 白名单字段。
- adapter 失败复用 `shield_data` 错误码；player 域只新增 `persistence_save_failed` 等明确属于本模块的错误。
- 未配置 persistence 时，`on_save` 默认实现为 no-op。

### OD-010 Player Cross-Service Reference

状态：closed。权威文档见 [玩家生命周期运行时语义](runtime-player.md) 和 [官方可选模块契约](optional-modules.md)。

决策：

- 跨 service 玩家引用命名为 `PlayerRef`，不命名为 `PlayerHandle`，避免与 `ServiceHandle` 混淆。
- `PlayerRef` 是 player 模块内部引用，**不是** `ServiceHandle` 的替代品。
- `PlayerRef` 结构为 `{ uid, node_id, service_id, epoch }`；`epoch` 来自 shield_cluster 的 `node_epoch`，单节点为 0。
- 跨 service payload **只能**传 `PlayerRef`，禁止传 `SessionHandle` 或完整 `PlayerSession`。
- `shield.player.resolve(ref)` P0 仅冻结本地 resolve；远端 resolve 留 Phase 2+，由 `shield_cluster` + `shield_player` 协作定义。
- ref 失效返回 `nil, Error{code="player_not_found"}`，不抛错。

## Open Decisions

当前没有仍需设计拍板的基础语义项。后续若发现新的未决问题，先在本节记录，再同步更新对应权威文档。
