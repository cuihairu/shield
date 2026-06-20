# 游戏状态持久化与回档模式

> 状态：游戏业务模式草案，非当前 `shield_core`、`shield_data` 或 Phase 1 最小运行时契约。
>
> 本文整理网络游戏常见的数据存储、内存快照、增量日志、独立状态进程、回档和事务设计。它用于指导业务层和后续 optional module 设计，不定义新的 core API、Lua API 或配置 schema。
>
> 若本文与 [最终架构总纲](architecture.md)、[数据访问语义](runtime-data.md)、[玩家生命周期](runtime-player.md) 或 [运行时语义决策稿](runtime-semantics.md) 冲突，以那些权威契约为准。

## 定位

游戏后端的数据设计首先要回答的不是“使用 MySQL、Redis 还是 MongoDB”，而是：

- 谁是状态真相源。
- 写入是同步提交还是异步落盘。
- 崩溃后允许丢失多少最近状态。
- 需要支持个人玩家回档、分服回档还是全服回档。
- 事务边界是单玩家、单房间、单公会，还是跨服务、跨分片。

Shield 当前的 `shield_data` 只提供原始 DB/Redis 访问和连接池，不提供 ORM、业务 mapper、跨 service 事务或回档语义。游戏状态持久化应由业务 service、`shield_player` persistence adapter 或未来独立模块在 `shield_data` 之上实现。

不要把本文里的“游戏状态快照”和 `RuntimeSnapshot` 混淆。`RuntimeSnapshot` 是 ops/diagnostics 的只读运行时观测数据，不参与业务存档、恢复或回档。

## 术语

| 术语 | 含义 |
| --- | --- |
| 真相源 | 系统认定的最终正确状态来源，可能是 DB、内存状态服、事件日志或它们的组合 |
| 当前快照 | 某个聚合根在某时刻的完整可恢复状态，如一个玩家、一间房间、一个公会 |
| 历史快照 | 为回档、审计或灾备保留的旧版本完整状态 |
| 增量日志 | 状态变化的追加记录，可是业务事件、操作日志、字段 delta 或 WAL |
| Ledger | 账本式流水，常用于货币、道具、充值、交易，原则上只追加不覆盖 |
| Checkpoint | 分服、分片或全局维度的恢复点，通常配合增量日志使用 |
| PITR | Point-in-time recovery，通过数据库备份和 binlog/WAL 恢复到指定时间点 |

## 设计维度

选择持久化方案时至少要评估以下维度：

| 维度 | 关键问题 |
| --- | --- |
| 一致性 | 写入完成后，客户端是否立即认为状态永久生效 |
| 延迟 | 玩家请求是否阻塞等待 DB 提交 |
| 吞吐 | 高并发热点对象是否会打爆数据库或锁 |
| 恢复点目标 | 崩溃后最多允许丢多少秒或多少条操作 |
| 回档粒度 | 只需要全服恢复，还是要按玩家、道具、交易维度恢复 |
| 审计能力 | 能否解释“这个物品为什么存在”或“这笔货币为什么变化” |
| 事务边界 | 事务是在单 DB 内、单 actor 内，还是跨多个服务 |
| 运维复杂度 | 是否需要独立状态进程、日志回放工具、灾备演练和数据修复后台 |

## 主流存储模式

### 1. 传统关系型直写

关系型数据库是真相源。业务请求直接通过 `INSERT` / `UPDATE` / `DELETE` 修改数据库，内存只保留短期对象或缓存。

```text
client request
  -> game service
  -> DB transaction
  -> response
```

适合：

- 回合制、卡牌、SLG、养成、管理类。
- 在线状态和实时战斗较少，成长数据更重要。
- 团队希望优先获得简单、可审计、强一致的数据模型。

优点：

- 实现简单，工具链成熟。
- SQL 查询、报表、客服后台容易做。
- 单库事务能力强。
- 全服回档可以依赖数据库备份、binlog/WAL 和 PITR。

缺点：

- 高并发在线服容易出现 DB 热点。
- 每次请求等待 DB 会增加尾延迟。
- 高频字段更新会产生大量写放大。
- 如果没有历史表和 ledger，个人回档仍然困难。

个人玩家回档：

- 保留 `player_snapshot_history` 或业务历史表。
- 对货币、道具、充值、交易保留独立 ledger。
- 回档时恢复玩家主状态，并对经济相关数据优先写补偿流水。

全服/分服回档：

- 使用全量备份 + binlog/WAL 做 PITR。
- 回档前停服冻结写入。
- 恢复后重建 Redis、排行榜、搜索索引等派生数据。

事务支持：

- 单库事务最强。
- 跨库、跨服、跨支付渠道仍需要 Saga、补偿和幂等。

### 2. DB 为真相源 + Redis 缓存

数据库仍是真相源，Redis 只做热点缓存、排行榜、短期会话、锁或队列。

```text
read:
  game service -> Redis -> DB fallback

write:
  game service -> DB transaction -> invalidate/update Redis
```

适合：

- 大多数中小型在线游戏。
- 读取远高于写入的玩家资料、配置、排行榜、邮件摘要。

优点：

- 保留 DB 强一致和回档能力。
- Redis 降低读压力。
- 工程模式成熟。

缺点：

- 缓存一致性需要严格规则。
- 缓存穿透、雪崩、过期重建需要治理。
- Redis 不能作为回档真相源。

个人玩家回档：

- 仍以 DB 历史快照和 ledger 为准。
- 回档后删除或刷新对应玩家 Redis key。

全服/分服回档：

- 仍以 DB PITR 为准。
- Redis 通常全量清空或按命名空间重建。

事务支持：

- 事务发生在 DB。
- Redis 更新应作为派生结果处理，失败后可重建。
- 不建议把 DB 事务和 Redis 命令强行绑成分布式事务。

### 3. 内存权威 + 周期快照

运行时内存是真相源，数据库只定期保存完整快照。很多长连接游戏、房间服和状态服会使用这种模式。

```text
client request
  -> player/room service memory state
  -> periodic snapshot flush
  -> DB/Redis/object storage
```

适合：

- 高频状态变化，如移动、战斗、房间局内状态。
- 单 actor 或单线程能串行拥有对象状态。
- 可以接受崩溃时丢失最近一小段未保存数据。

优点：

- 请求路径不等待 DB，延迟低。
- 单对象内天然串行，避免大量 DB 锁。
- 适合 actor/service 模型。

缺点：

- 只靠周期快照，崩溃后会丢最近修改。
- 个人回档只能回到最近快照，粒度粗。
- 如果快照过大，保存和 GC 成本高。
- 缺少增量日志时，很难解释状态变化原因。

个人玩家回档：

- 可以回到最近历史快照。
- 如果没有增量日志，无法精确恢复到某个操作前后。
- 经济数据必须单独写 ledger，否则容易无法审计。

全服/分服回档：

- 依赖全服或分片 checkpoint。
- 回档到某个 checkpoint 后，之后状态全部丢弃。
- 更适合“灾难恢复”，不适合精确修复。

事务支持：

- 单玩家、单房间、单公会由 owner actor 串行处理。
- 跨 actor 不使用传统 DB 事务，通常用 Saga、锁、预留状态和补偿。

### 4. 内存权威 + 快照 + 增量日志

这是大型在线游戏里更稳的折中方案。运行时以内存为权威，写入时追加操作日志或状态 delta，周期性生成完整快照。

```text
client request
  -> owner service validates command
  -> append op log / ledger
  -> mutate memory state
  -> periodic full snapshot
```

也可以采用先改内存再异步日志，但必须明确崩溃丢失窗口。更稳的做法是关键经济操作先落可靠日志，再对内存生效。

适合：

- MMO、ARPG、开放世界、强在线长连接游戏。
- 既要低延迟，也要支持审计和回档。
- 玩家、房间、公会等对象可以按 owner 分片。

优点：

- 运行时性能好。
- 恢复能力强，可用“快照 + 日志重放”恢复。
- 个人玩家回档粒度比纯快照细。
- 全服/分片恢复可以按 checkpoint 和日志位点推进。

缺点：

- 实现和运维复杂。
- 日志格式需要版本管理。
- 回放工具、校验工具和修复工具必须配套。
- 日志堆积、快照压缩和归档需要治理。

个人玩家回档：

```text
1. 找到目标时间点之前最近的 player snapshot
2. 加载为临时状态
3. 按 uid 重放 op log 到目标时间点
4. 校验版本、货币和道具 ledger
5. 覆盖 current state 或生成补偿操作
6. 写 rollback audit log
```

全服/分服回档：

```text
1. 停服或冻结目标 shard 写入
2. 选择 shard checkpoint
3. 重放 checkpoint 之后的增量日志到目标位点
4. 重建派生索引、排行榜和缓存
5. 做一致性校验
6. 重新开放服务
```

事务支持：

- 单 owner service 内通过串行命令执行保证一致。
- 多对象事务使用 request id、状态机、预留/确认/取消和补偿。
- 货币、道具、交易必须有 ledger 和幂等 key。

### 5. 独立状态进程或内存数据库进程

一些游戏会把状态存储做成单独进程，例如 player state server、world state server、global state server。业务逻辑通过消息访问状态进程，状态进程负责内存管理、快照和落盘。

```text
game service
  -> state process / state shard
  -> memory state
  -> snapshot + log persistence
```

适合：

- 状态对象多，业务服务希望无状态化。
- 需要统一管理保存、脏标记、压缩、迁移和回档。
- 团队有能力维护专用状态服务。

优点：

- 单写者模型清晰。
- 业务服务和状态存储职责分离。
- 可以集中做快照、增量日志、热迁移和后台保存。
- 多个业务服务共享同一状态入口。

缺点：

- 状态进程成为关键基础设施。
- 网络跳数增加。
- 状态 API 设计复杂。
- 需要处理状态进程宕机、迁移、重放和分片扩缩容。

个人玩家回档：

- 状态进程按 uid 管理版本、快照和 op log。
- 回档可以由状态进程提供 admin command。
- 回档时应暂停玩家写入或切换玩家到 maintenance state。

全服/分服回档：

- 按状态 shard checkpoint + log 位点恢复。
- 需要处理多个状态进程之间的一致位点。
- 跨服玩法和全局服务要有独立恢复策略。

事务支持：

- 单状态 shard 内天然串行。
- 跨 shard 事务仍需要 Saga、两阶段业务状态机或补偿。
- 不建议把状态进程伪装成一个大分布式关系数据库。

### 6. Event Sourcing / Ledger First

事件日志是真相源。系统只追加事件，当前状态由事件投影得到。货币、道具、充值、交易等系统常采用 ledger-first 思想，即使整体项目不完全 event sourcing。

```text
command
  -> validate against current projection
  -> append immutable event
  -> update projection/read model
```

适合：

- 强审计系统，如充值、货币、拍卖、交易、邮件领取。
- 需要解释每一次状态变化来源。
- 需要较强回放、稽核、风控能力。

优点：

- 审计能力最强。
- 个人回档和追责最清晰。
- 可以重建读模型。
- 幂等和补偿可以显式表达。

缺点：

- 开发复杂。
- 查询当前状态需要投影。
- 事件版本演进困难。
- 完整全服 event replay 成本高。

个人玩家回档：

- 可通过事件回放到目标时间点。
- 经济类更推荐写反向补偿事件，而不是删除历史事件。
- 需要区分“状态修复”和“历史事实修正”。

全服/分服回档：

- 理论上可按事件位点恢复。
- 实践中通常仍保留 checkpoint，避免从创服开始回放。
- 对跨服务事件要有全局顺序或可重放的局部顺序。

事务支持：

- 追加事件可以使用 DB 事务或日志系统原子写。
- 多聚合事务需要 outbox/inbox、幂等消费和补偿。
- 最终一致比强一致更常见。

## 总体对比

| 模式 | 真相源 | 运行时性能 | 回档能力 | 事务能力 | 实现复杂度 | 典型场景 |
| --- | --- | --- | --- | --- | --- | --- |
| 传统关系型直写 | DB | 中 | 中到强 | 强 | 低 | 卡牌、SLG、管理类 |
| DB + Redis 缓存 | DB | 中到高 | 中到强 | 强 | 中 | 大多数中小型在线游戏 |
| 内存权威 + 周期快照 | 内存 | 高 | 弱到中 | 单 owner 强 | 中 | 房间、战斗、短周期状态 |
| 内存权威 + 快照 + 日志 | 内存 + 日志 | 高 | 强 | 单 owner 强，跨 owner 中 | 高 | MMO、ARPG、开放世界 |
| 独立状态进程 | 状态进程 | 高 | 强 | shard 内强 | 高 | 大规模长连接在线服 |
| Event Sourcing / Ledger | 事件日志 | 中 | 极强 | 追加强，跨聚合中 | 很高 | 经济、交易、审计系统 |

## 公开引擎和框架参考

本节只整理公开资料能确认的设计倾向。不同项目会二次改造引擎，实际线上架构可能和默认框架差异很大。

| 引擎/框架 | 公开资料可确认的存储模型 | 更接近本文哪类模式 | 对 Shield 的启发 |
| --- | --- | --- | --- |
| BigWorld | 内置数据库层，持久化实体属性由 entity definition 标记；实体通过 `writeToDB` 异步写入；DBMgr 管理持久化；支持 secondary database 和 database snapshot 工具 | 独立 DBMgr + entity 快照 + secondary DB | 适合参考“Base/Cell 运行态 + DBMgr 持久化层 + 二级数据库灾备”的边界 |
| KBEngine | 多进程分布式架构包含 `dbmgr`；文档描述 `dbmgr` 做高性能多线程数据访问，默认 MySQL；`baseapp` 定时备份 entity 到数据库，并做互备和灾难恢复 | 独立 dbmgr + entity 定时快照/备份 | 适合参考“玩家/实体在 base/cell 运行，DBMgr 统一存取”的 MMO 模式 |
| Skynet | 轻量 actor/Lua 框架；官方仓库提供 MySQL、Redis 等 Lua 数据库客户端库，但不内置固定的 DBMgr 或玩家持久化模型 | 框架提供 DB 客户端，项目自行实现 db service | 适合 Shield 当前方向：core 不接管存档；业务或 optional module 自己实现 db service、缓存和快照 |
| Pomelo | Node.js 游戏服务器框架，核心关注 connector、session、server 间 RPC、组件体系；持久化一般由项目用 MySQL/MongoDB/Redis 等自行接入 | 框架级路由 + 项目自定义 DB 层 | 说明通用游戏框架通常不应把 ORM/回档塞进 core |
| Nakama | 后端服务内置用户、账号、存储对象、排行榜等能力，部署依赖 PostgreSQL；业务可用内置 storage API 保存游戏数据 | DB 为真相源 + 服务内置存储 API | 适合账号、排行榜、通用后端能力；不适合作为实时 MMO world state 的唯一模型 |
| Colyseus | 房间状态以内存 Room 为核心，状态同步给客户端；持久化、匹配数据和房间外数据通常由开发者接入外部存储 | 房间内存权威 + 外部持久化 | 适合参考“房间态不直接等于长期存档”的边界 |
| Photon Server / Photon Realtime | 主要提供实时通信、房间、匹配和状态同步；长期玩家数据通常接入外部账号/后端服务 | 实时房间内存态 + 外部 DB | 说明实时网络层和长期存档层应分离 |
| Orleans / Akka 等 actor 框架 | 提供 actor/virtual actor 和持久化 provider 能力，但游戏业务的 snapshot、journal、ledger 仍需自行建模 | Actor persistence / snapshot + journal | 可借鉴单 actor 串行和 snapshot/journal 思路，但不能直接替代游戏存档设计 |

### BigWorld

BigWorld 的公开 Server Programming Guide 明确有独立的 database layer，用于保存和恢复 entities。文档强调 database layer 不是给每个角色动作频繁访问的，而应主要用于 entity 创建、销毁和关键交易点；普通游戏完整性依赖灾难恢复机制。

可确认的设计点：

- Entity definition 里用 `<Persistent>` 标记需要持久化的属性。
- DBMgr 在启动时可以为持久化属性分配表和字段。
- `writeToDB` 是异步操作，完成后回调通知结果。
- 每个 entity type 有主表；数组、元组等复杂属性映射为子表。
- secondary database 可把 active entity 的变更写到 BaseApp 机器本地 SQLite，再在 entity inactive 或系统恢复时 consolidation 回主库。
- database snapshot 工具会复制 primary database 和 secondary databases，但文档也说明它不是严格一致的全系统快照。

这类设计更像：

```text
BaseApp/CellApp memory entity
  -> async writeToDB
  -> DBMgr / primary DB
  -> optional secondary DB on BaseApp host
  -> consolidation / snapshot tools
```

参考资料：

- [BigWorld Server Programming Guide: The Database Layer](https://raw.githubusercontent.com/v2v3v4/BigWorld-Engine-1.9.1/master/bigworld/doc/generated_html/server_programming_guide/ch09.html)
- [BigWorld Server Programming Guide: table of contents](https://raw.githubusercontent.com/v2v3v4/BigWorld-Engine-1.9.1/master/bigworld/doc/generated_html/server_programming_guide/index.html)

### KBEngine

KBEngine 的公开文档把服务器拆成 `loginapp`、`baseapp`、`cellapp`、`dbmgr`、`interfaces` 等组件。Server Layout 文档中 `dbmgr` 位于 MySQL/Redis/MongoDB 之上，并描述它负责高性能多线程数据访问，默认数据库是 MySQL。`baseapp` 负责客户端与服务端交互，并承担定时备份 entity 到数据库、baseapp 互备和灾难恢复。

可确认的设计点：

- `dbmgr` 是独立组件。
- `baseapp` / `cellapp` 是运行态逻辑容器，DB 不直接成为每个操作的同步路径。
- 实体数据通过 entity、databaseID、database interface 等 API 加载和保存。
- 公开 API 支持从数据库创建 entity、查询 entity 是否已 checkout、删除数据库 entity 等操作。

这类设计更像：

```text
client
  -> loginapp
  -> baseapp / cellapp memory entity
  -> periodic backup / write entity
  -> dbmgr
  -> MySQL / Redis / MongoDB
```

参考资料：

- [KBEngine Server Layout](https://kbengine.github.io//docs/concepts/layout.html)
- [KBEngine API: base module database entity functions](https://documentation.help/KBEngine-API/KBEngine2.html)
- [KBEngine GitHub README](https://github.com/kbengine/kbengine)

### Skynet

Skynet 和 BigWorld/KBEngine 的区别很重要。Skynet 是轻量级 Lua actor 框架，不是带固定 DBMgr 和实体持久化规则的 MMO 引擎。官方仓库提供 `skynet.db.mysql`、`skynet.db.redis` 等数据库客户端库，项目通常会基于 actor/service 自己封装 `db_service`、`cache_service`、`player_service`。

可确认的设计点：

- Skynet README 将其定位为支持 actor model 的 Lua 框架，常用于游戏。
- 官方仓库包含 MySQL 和 Redis 客户端库。
- 是否采用“单独存储进程”“内存快照”“传统 DB 直写”，取决于具体项目，不是 Skynet core 固定语义。

常见项目实践更像：

```text
gateway/agent service
  -> player service memory state
  -> custom db service actor
  -> MySQL / Redis
```

或：

```text
logic service
  -> custom cache/db service
  -> Redis hot state
  -> MySQL snapshot/history
```

对 Shield 的启发是：保持 `shield_core` 小而稳定，`shield_data` 只提供底层 DB/Redis 能力，玩家存档、快照、回档和事务策略由业务层或 optional module 自己定义。

参考资料：

- [Skynet GitHub README](https://github.com/cloudwu/skynet)
- [Skynet MySQL client](https://github.com/cloudwu/skynet/blob/master/lualib/skynet/db/mysql.lua)
- [Skynet Redis client](https://github.com/cloudwu/skynet/blob/master/lualib/skynet/db/redis.lua)

### 归纳

这些引擎大致分成三派：

| 派别 | 代表 | 特点 |
| --- | --- | --- |
| MMO 引擎内置 DBMgr | BigWorld、KBEngine | 引擎定义 entity 持久化、DBMgr、备份和灾难恢复流程 |
| Actor 框架不内置存档 | Skynet、Akka、Orleans 风格框架 | 框架提供 actor/service 和基础 DB 客户端，业务自己定义状态和持久化 |
| 实时房间/后端服务分层 | Colyseus、Photon、Nakama | 房间或实时态与长期账号/存储能力分开，长期数据走外部 DB 或内置 storage API |

Shield 当前更应该靠近第二类：先把 service/message/timer/net/data 边界做好，不在 core 中引入 DBMgr。未来如果要做官方存档能力，更适合新增 `shield_persistence` 或扩展 `shield_player`，而不是扩大 `shield_data`。

## 推荐混合模型

成熟游戏通常不是单一模式，而是按数据类型分层：

| 数据类型 | 推荐真相源 | 推荐保存方式 | 回档策略 |
| --- | --- | --- | --- |
| 玩家基础资料 | DB current + history | 当前快照 + 历史快照 | 单玩家快照恢复 |
| 背包、装备、任务 | 内存 current + snapshot/log | 周期快照 + op log | 快照 + 日志重放 |
| 货币、钻石、充值 | Ledger | 只追加流水 + 当前余额投影 | 补偿流水优先 |
| 房间局内状态 | 内存 | 结束结算保存，必要时局内 checkpoint | 通常不做长期回档 |
| 战斗回放 | 事件/输入日志 | 输入流、随机种子、关键帧 | 重放验证，不直接改玩家存档 |
| 公会、队伍、拍卖 | owner service + DB/log | 状态机 + op log | 按聚合根恢复或补偿 |
| 排行榜 | Redis/派生表 | 从玩家/ledger 派生 | 回档后重建 |
| 缓存、在线状态 | Redis/内存 | TTL 或可重建 | 不作为回档来源 |

推荐基础形态：

```text
online request
  -> owner service memory state
  -> append business op log for critical changes
  -> update ledger for economy changes
  -> periodic snapshot to DB/object storage
  -> Redis/index/rank as rebuildable derived state
```

## 玩家存档表结构草案

以下是关系型数据库表达，不要求 Shield runtime 内置。

```sql
CREATE TABLE player_current (
    uid BIGINT PRIMARY KEY,
    version BIGINT NOT NULL,
    snapshot_blob JSON NOT NULL,
    level INT NOT NULL DEFAULT 1,
    zone_id BIGINT NULL,
    updated_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP NOT NULL
);

CREATE TABLE player_snapshot_history (
    uid BIGINT NOT NULL,
    snapshot_version BIGINT NOT NULL,
    snapshot_blob JSON NOT NULL,
    reason VARCHAR(64) NOT NULL,
    operator VARCHAR(64) NULL,
    created_at TIMESTAMP NOT NULL,
    PRIMARY KEY (uid, snapshot_version)
);

CREATE TABLE player_op_log (
    event_id BIGINT PRIMARY KEY,
    uid BIGINT NOT NULL,
    request_id VARCHAR(128) NOT NULL,
    from_version BIGINT NOT NULL,
    to_version BIGINT NOT NULL,
    op_type VARCHAR(64) NOT NULL,
    delta_blob JSON NOT NULL,
    trace_id VARCHAR(128) NULL,
    created_at TIMESTAMP NOT NULL,
    UNIQUE (uid, request_id)
);

CREATE TABLE currency_ledger (
    ledger_id BIGINT PRIMARY KEY,
    uid BIGINT NOT NULL,
    currency_type VARCHAR(32) NOT NULL,
    delta BIGINT NOT NULL,
    balance_after BIGINT NOT NULL,
    reason VARCHAR(64) NOT NULL,
    request_id VARCHAR(128) NOT NULL,
    trace_id VARCHAR(128) NULL,
    created_at TIMESTAMP NOT NULL,
    UNIQUE (uid, currency_type, request_id)
);

CREATE TABLE rollback_audit (
    rollback_id BIGINT PRIMARY KEY,
    scope VARCHAR(32) NOT NULL,
    target_id VARCHAR(128) NOT NULL,
    from_version BIGINT NULL,
    to_version BIGINT NULL,
    reason VARCHAR(255) NOT NULL,
    operator VARCHAR(64) NOT NULL,
    created_at TIMESTAMP NOT NULL
);
```

关键字段规则：

- `version` 用于乐观锁和回放边界。
- `request_id` 用于幂等，尤其是充值、发货、领取奖励。
- `trace_id` 用于串联客户端请求、服务间调用和数据写入。
- `reason` 用于客服、风控、运营活动和后台修复审计。
- 货币、付费道具、交易类数据必须有 ledger，不应只存在于 `snapshot_blob`。

## 保存路径

### 同步强保存

请求成功前必须写入 DB 或可靠日志。

适合：

- 充值到账。
- 扣钻购买。
- 拍卖交易。
- 稀有道具发放。
- 跨玩家资产转移。

规则：

- 必须有幂等 key。
- 必须写 ledger 或 op log。
- 返回成功后不能因为进程崩溃丢失。

### 异步快照保存

请求先更新内存，稍后保存完整快照。

适合：

- 经验、任务进度、位置、普通背包状态。
- 可以接受几秒级恢复点的数据。

规则：

- service 维护 dirty 标记。
- 保存使用 `where version = old_version` 或 owner 串行提交。
- 保存失败进入重试队列，不能静默丢弃。
- 退出、下线、关服 drain 时触发强制保存。

### 混合保存

关键变更写 op log/ledger，普通字段跟随周期快照。

适合：

- 大多数长连接在线游戏。

规则：

- 经济行为先写可靠流水。
- 玩家完整状态周期性快照。
- 回档时用快照恢复主状态，用 ledger 校验经济状态。

## 个人玩家回档

个人回档应优先选择“恢复状态 + 写审计 + 必要补偿”，不要直接无记录地覆盖数据库。

### 适用场景

- 客服误操作。
- 活动脚本给错奖励。
- 玩家存档损坏。
- 单玩家异常刷道具。
- 单玩家任务或副本进度异常。

### 推荐流程

```text
1. 冻结目标玩家写入
2. 读取当前 player_current 和 version
3. 选择目标历史快照或目标时间点
4. 如有 op log，重放到目标时间点
5. 校验 currency_ledger、item_ledger、邮件、交易状态
6. 生成新版本 current snapshot
7. 写 rollback_audit
8. 清理或刷新 Redis/cache/rank
9. 解冻玩家
```

### 决策规则

| 数据类型 | 推荐处理 |
| --- | --- |
| 基础属性、任务、位置 | 可以按玩家快照覆盖 |
| 背包普通道具 | 可按快照覆盖，但要记录差异审计 |
| 付费货币、充值道具 | 优先补偿流水，不删除历史 |
| 交易、拍卖、邮件 | 不能只回滚单玩家，要处理关联对象 |
| 排行榜 | 回档后重算或重建派生数据 |
| 已对外发放奖励 | 优先补偿，避免破坏其他玩家状态 |

## 全服、分服和分片回档

全服回档是灾备操作，不应依赖普通业务接口临时拼凑。

### 传统 DB 路径

```text
1. 停服或进入全局维护
2. 选择目标时间点
3. 恢复最近全量备份
4. 使用 binlog/WAL 做 PITR
5. 启动校验脚本
6. 清空或重建 Redis/cache/rank/search
7. 逐步开放服务
```

### 快照 + 日志路径

```text
1. 停止目标 shard 写入
2. 选择 checkpoint 和日志位点
3. 恢复 checkpoint
4. 重放增量日志到目标位点
5. 校验状态版本和 ledger balance
6. 重建派生状态
7. 恢复玩家登录
```

### 注意事项

- 全服回档必须有明确公告和运营流程。
- 跨服数据、支付回调、第三方订单不能简单回滚。
- 回档后可能需要补发或扣回部分奖励。
- 如果事故范围可控，优先做逻辑修复和补偿，少做全服回档。

## 事务设计

### 单聚合事务

单玩家、单房间、单公会、单拍卖行 shard 可以由一个 owner service 串行处理。

```text
command -> owner mailbox -> validate -> mutate -> persist/log -> reply
```

优点：

- 不需要大量锁。
- 执行顺序清晰。
- 与 actor/service 模型匹配。

限制：

- 只能保证 owner 内部一致。
- 跨 owner 操作需要额外协议。

### 数据库本地事务

同库内的扣款、发货、写 ledger 可以使用 DB 事务。

```sql
BEGIN;
UPDATE player_balance SET balance = balance - ? WHERE uid = ? AND balance >= ?;
INSERT INTO currency_ledger (...);
INSERT INTO item_ledger (...);
COMMIT;
```

规则：

- 所有外部请求必须带幂等 key。
- 事务内只放必要写入，避免长事务。
- 不要在 DB 事务中等待远程 service 或第三方接口。

### Outbox / Inbox

本地事务提交业务变更和待发送消息，后台可靠投递。

```text
DB transaction:
  update local state
  insert outbox event

dispatcher:
  read outbox
  send to target service
  mark delivered

receiver:
  dedupe by event id
  apply idempotently
```

适合：

- 扣款后通知发货。
- 支付回调后通知玩家服务。
- 跨服务最终一致。

### Saga 与补偿

跨服务、跨 shard、跨玩家交易不建议默认使用 2PC。更常见的是 Saga。

```text
reserve asset
  -> confirm target state
  -> commit asset
  -> on failure cancel reservation
```

规则：

- 每一步都必须幂等。
- 每一步都有明确状态。
- 失败后有补偿或人工处理队列。
- 中间状态对玩家可见时，要有清晰提示。

### 不推荐的做法

- 跨多个 actor 持有锁等待 DB。
- 在 DB 事务内调用远程 service。
- 用 Redis 锁伪装强事务。
- 只改当前余额，不写货币流水。
- 用删除历史事件的方式修复经济问题。

## 与 Shield 架构的映射

| Shield 层 | 建议职责 |
| --- | --- |
| `shield_data` | 继续只提供 raw DB/Redis、连接池、超时和错误码 |
| `shield_player` | 拥有玩家 persistence adapter、自动保存、玩家回档 hook 的未来扩展 |
| 业务 Lua service | 定义状态结构、dirty 标记、快照序列化和业务 op log |
| `shield_global` | 提供排行榜、队列、锁等派生能力，不作为存档真相源 |
| `shield_ops` | 展示保存延迟、失败队列、dirty 数量、checkpoint 状态，不执行业务回档 |
| 运维工具 | 执行离线恢复、PITR、日志重放、校验和数据修复 |

建议边界：

- 不把 ORM、复杂 mapper、回档系统塞进 `shield_data`。
- 不让 `shield_core` 感知玩家存档、房间存档或全服 checkpoint。
- 玩家持久化是 `shield_player` 或业务 service 的职责。
- 全服灾备是部署和运维能力，不是普通 Lua API。

## 按游戏类型选择

| 游戏类型 | 推荐模型 |
| --- | --- |
| 卡牌、养成、回合制 | DB 为真相源 + Redis 缓存 + ledger |
| SLG | DB current/history + Redis 缓存 + 分区任务队列 + 关键系统 ledger |
| MMO/ARPG | owner service 内存权威 + 快照 + op log + ledger |
| 房间对战 | 房间内存权威 + 输入/战斗日志 + 结算强保存 |
| 开放世界 | world shard 内存权威 + checkpoint + 增量日志 |
| 强交易经济游戏 | ledger/event sourcing 优先，当前状态作为投影 |

## 实现优先级建议

1. 先实现当前快照保存和加载。
2. 为关键经济系统补 ledger 和幂等 request id。
3. 增加历史快照和客服可查审计。
4. 增加玩家级 op log 和单玩家回档工具。
5. 增加保存失败重试队列和 ops 指标。
6. 增加分片 checkpoint 和日志重放演练。
7. 最后再考虑完整 event sourcing 或独立状态进程。

## 最小落地检查表

上线前至少回答这些问题：

- 进程崩溃最多丢多少秒玩家状态。
- 玩家下线、关服、服务崩溃分别如何保存。
- 同一玩家多进程同时保存时如何防止覆盖。
- 充值、扣费、发货是否全部幂等。
- 货币和道具是否有 ledger。
- 是否能查出某个道具从哪里来。
- 是否能把单个玩家恢复到昨天 18:00。
- 是否能恢复整服到某个 binlog/WAL 或 checkpoint 位点。
- 回档后 Redis、排行榜、搜索索引如何重建。
- 数据修复是否有审计和审批。
