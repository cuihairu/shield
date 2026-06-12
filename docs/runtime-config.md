# 配置运行时语义

本文档是 Shield **最小 runtime 配置入口** 的权威来源。官方可选模块可以定义自己的配置段，但不能把配置项反向塞进最小 runtime schema。

当前状态：本文冻结 Phase 1 配置契约；源码已实现启动期最小验证，旧字段仍需要按本文逐步清理。

## 配置原则

- YAML 只做声明式绑定，不承载业务逻辑。
- 最小 runtime schema 只覆盖单节点 runtime 必需能力。
- 配置驱动 Lua service、网络监听、data source、日志和 bootstrap timeout；每个配置段必须有明确 owner。
- 不在 core 中提供服务发现、插件、Prometheus、健康检查、DI、注解或条件装配配置。
- optional module 的配置由对应模块自己验证；未启用模块时，出现对应 optional 配置段必须启动失败，不能由 core 静默忽略。

## Phase 1 Schema

配置段 owner：

| 配置段 | owner |
| --- | --- |
| `app` | `shield_bootstrap` |
| `log` | `shield_log` |
| `lua` | `shield_lua` |
| `actors` | `shield_bootstrap` + `shield_core` + `shield_lua` |
| `actors[].network` | `shield_net` |
| `database` / `redis` | `shield_data` |
| `bootstrap` / `shutdown` | `shield_bootstrap` |

```yaml
app:
  name: my_game
  version: "1.0.0"

log:
  level: info                  # debug | info | warn | error
  format: text                 # text | json
  console: true
  file:
    enabled: false
    path: logs/shield.log
    max_size_mb: 100
    max_files: 10
    rotation: size             # size | daily | none

lua:
  vm:
    mode: per_service          # per_service
    max_vms: 10000
    max_memory_mb: 64
  sandbox:
    allow_os: false
    allow_io: false
  script_path: "scripts"     # Lua 脚本查找路径（默认: scripts）
  module_path: "scripts/?.lua;scripts/?/init.lua"  # Lua require() 模块搜索路径
  cache:
    enabled: true             # 是否启用脚本缓存（默认: true）
    max_size: 100            # 最大缓存文件数（默认: 100）
    ttl_seconds: 0            # 缓存过期时间，0 表示永不过期（默认: 0）

actors:
  - name: gateway
    script: scripts/gateway.lua
    instances: 1
    required: true
    network:
      tcp: "0.0.0.0:8001"
      # udp: "0.0.0.0:8002"
      # kcp: "0.0.0.0:8003"
      # websocket: "0.0.0.0:8004"
      max_connections: 10000
      max_frame_size: 65536
    transport: default
    options:
      route_table: login_routes
    restart:
      policy: on-failure       # always | on-failure | never
      max_retries: 5
      initial_delay: 1000
      max_delay: 30000
      multiplier: 2
    limits:
      max_mailbox_size: 10000
      max_coroutines: 1000
      max_pending_calls: 1000
      max_timers: 10000

  - name: player
    script: scripts/player.lua
    instances: 0               # dynamic only

database:
  enabled: true
  driver: mysql                # mysql | postgresql | sqlite
  host: localhost
  port: 3306
  database: game
  username: root
  password: ${DB_PASSWORD:}
  pool_size: 10
  max_pool_size: 50
  connect_timeout: 5000
  query_timeout: 30000
  idle_timeout: 300000
  max_lifetime: 3600000
  options:
    charset: utf8mb4

redis:
  enabled: true
  host: localhost
  port: 6379
  db: 0
  password: ${REDIS_PASSWORD:}
  pool_size: 10
  max_pool_size: 50
  connect_timeout: 5000
  command_timeout: 5000
  idle_timeout: 300000

bootstrap:
  timeout:
    config_load: 5000
    log_init: 5000
    core_init: 10000
    data_init: 30000
    net_init: 10000
    script_init: 10000
    service_spawn: 60000
  retry:
    database:
      max_retries: 3
      delay: 5000
    redis:
      max_retries: 3
      delay: 5000

shutdown:
  timeout:
    service_drain: 30000
    service_stop: 10000
    data_close: 10000
    total: 60000
```

## Actor 配置

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `name` | 是 | actor 类型和默认 service name 前缀 |
| `script` | 是 | Lua module 文件 |
| `instances` | 否 | 启动实例数；`0` 表示只允许动态 spawn |
| `required` | 否 | 启动失败是否导致 runtime 启动失败，默认 true |
| `network` | 否 | gateway 类 service 的 listener 配置 |
| `transport` | 否 | C++ transport 名称 |
| `options` | 否 | 传给 `on_init(args).config` 的业务配置 |
| `restart` | 否 | 服务异常退出后的重启策略 |
| `limits` | 否 | 单 service 资源限制 |

`actors[].network` 只声明该 service 是 gateway service。网络回调仍由 Lua module 上的 `on_connect`、`on_disconnect`、`on_client_message` 实现。

`actors[].script` 可以是绝对路径，也可以是相对路径。相对路径解析规则：

1. 如果是绝对路径，直接使用
2. 如果路径相对于当前工作目录存在文件，直接使用
3. 如果 actor 配置指定了 `source_dir`，尝试从该目录解析
4. 如果全局配置指定了 `lua.script_path`，尝试从该路径解析
5. 如果以上都不存在，使用原始路径（可能导致启动失败）

**Lua 模块加载路径**

`lua.module_path` 配置项用于设置 Lua 的 `package.path`，控制 `require()` 函数的模块搜索路径：

- 默认值：`"scripts/?.lua;scripts/?/init.lua"`
- 支持多个路径，用分号分隔
- `?` 会被替换为模块名

示例：
```yaml
lua:
  script_path: "scripts"
  module_path: "scripts/?.lua;scripts/?/init.lua;libs/?.lua"
```

这样 `require("utils.helper")` 会依次尝试：
- `scripts/utils/helper.lua`
- `scripts/utils/helper/init.lua`
- `libs/utils/helper.lua`

**Lua 脚本缓存**

`lua.cache` 配置项控制 Lua 脚本的缓存行为，可以显著提升重复加载相同脚本的性能：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `enabled` | `true` | 是否启用缓存 |
| `max_size` | `100` | 最大缓存文件数 |
| `ttl_seconds` | `0` | 缓存过期时间（秒），0 表示永不过期 |

缓存机制：
- 缓存基于文件路径和修改时间
- 文件修改后缓存自动失效
- 使用 LRU 策略淘汰旧缓存
- 缓存存储源代码，每次使用时重新编译

环境差异：
- **开发环境**：建议 `enabled: false` 以支持热重载
- **生产环境**：建议 `enabled: true` 以提升性能

## Data 配置

`database.enabled=false` 或缺省 database 段时：

- `shield.db.*` 返回 `false, module_unavailable`。
- runtime 不创建 DB 连接池。

`redis.enabled=false` 或缺省 redis 段时：

- `shield.redis.*` 返回 `false, module_unavailable`。
- runtime 不创建 Redis 连接池。

`shield_data` 只提供原始访问能力，不配置 ORM、mapper、migration 或跨服务事务。

## 验证规则

| 字段 | 规则 |
| --- | --- |
| `app.name` | 必填，1-64 字符 |
| `log.level` | `debug`、`info`、`warn`、`error` |
| `lua.vm.mode` | 当前只允许 `per_service` |
| `lua.script_path` | 可选，默认 `"scripts"` |
| `lua.module_path` | 可选，默认 `"scripts/?.lua;scripts/?/init.lua"` |
| `lua.cache.enabled` | 可选，默认 `true` |
| `lua.cache.max_size` | 可选，默认 `100`，范围 `1-10000` |
| `lua.cache.ttl_seconds` | 可选，默认 `0`，范围 `0-86400` |
| `actors` | 至少一个 actor |
| `actors[].name` | 必填，本配置内唯一 |
| `actors[].script` | 必填，文件必须存在 |
| `actors[].instances` | `>= 0` |
| `actors[].restart.policy` | `always`、`on-failure`、`never` |
| `actors[].network.*` | 监听地址必须是 `host:port` |
| `actors[].network.udp/kcp/websocket` | Phase 1 拒绝启动；这些 transport 属于 deferred extension |
| `database.port` | 1-65535 |
| `database.pool_size` | `>= 1`，且 `<= max_pool_size` |
| `redis.port` | 1-65535 |
| `redis.pool_size` | `>= 1`，且 `<= max_pool_size` |
| `shutdown.timeout.total` | 必须大于各分段 timeout |

验证失败时拒绝启动，输出字段路径和原因。

## 环境变量展开

支持 `${VAR}` 和 `${VAR:default}`：

```yaml
database:
  host: ${DB_HOST:localhost}
  password: ${DB_PASSWORD:}
```

规则：

- 未设置且无 default 时，值为空字符串。
- 环境变量展开发生在 schema 验证前。
- 敏感字段不应在日志中输出明文。

## 多配置文件合并

启动可以指定多个配置文件：

```bash
shield --config config/app.yaml --config config/production.yaml
```

合并规则：

- 后面的文件覆盖前面的 scalar 和 map 字段。
- `actors` 按 `name` 合并；同名 actor 后者覆盖字段。
- 不允许两个文件定义同名 actor 且 script 不同，除非后者显式设置 `override: true`。

当前实现状态：源码已实现 map/scalar 的后者覆盖前者以及启动期验证；`actors` 按 `name` 智能合并仍是目标规则，当前同名 actor 会在合并后的配置验证中被拒绝。

## 热更新

Phase 1 只承诺极少量本地热更新：

| 配置项 | 热更新 | 行为 |
| --- | --- | --- |
| `log.level` | 是 | 立即生效 |
| `log.console` / `log.file` | 否 | Phase 1 需要重启 |
| `database.pool_size` | 否 | Phase 1 需要重启 |
| `redis.pool_size` | 否 | Phase 1 需要重启 |
| `actors[].instances` | 否 | Phase 1 需要重启 |
| `actors[].script` | 否 | 需要 service 重启或未来 hot reload 机制 |
| `actors[].network` | 否 | 需要 listener 重建 |
| `lua.vm.*` | 否 | 需要重启 |
| `lua.script_path` | 否 | 需要重启 |
| `lua.module_path` | 否 | 需要重启 |
| `lua.cache.enabled` | 否 | 需要重启 |
| `lua.cache.max_size` | 否 | 需要重启 |
| `lua.cache.ttl_seconds` | 否 | 需要重启 |
| `bootstrap.*` / `shutdown.*` | 否 | 需要重启 |

全局配置推送、运维热更新、cluster-wide 配置同步属于 `shield_global` 或 `shield_ops` 后续设计，不进入 core schema。

## Optional Module Schema

以下配置段不属于 core schema：

| 配置段 | 所属模块 | 文档 |
| --- | --- | --- |
| `cluster` | `shield_cluster` | [集群语义](runtime-cluster.md) |
| `global` | `shield_global` | [全局数据](runtime-global.md) |
| `ops` | `shield_ops` | [运维语义](runtime-ops.md) |
| `player` | `shield_player` | [玩家生命周期](runtime-player.md) |
| `server_manager` | `shield_server` | [服务器状态](runtime-server.md) |

optional module 启用后，由该模块读取并验证自己的配置段。core bootstrap 只负责把未消费的 optional 配置快照传给已启用模块。

如果配置文件包含 optional module 配置段，但对应模块未构建或未启用，启动必须失败并输出明确错误；不能静默忽略。

## 删除的旧配置

以下旧配置直接删除，不保留兼容：

- `plugins`
- `middleware`
- `discovery`
- `metrics`
- `health`
- `conditions`
- `annotations`
- `di`
- `ioc`
- `event_bus`
