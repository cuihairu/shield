# 配置运行时语义

本文档包含 Shield 配置模型相关的运行时语义决策。

## 配置原则

- YAML 只做声明式绑定，不承载业务逻辑。
- 配置驱动 Lua 服务、网络监听、数据源和日志。
- 不在 core 中提供服务发现、插件、Prometheus、健康检查等配置。
- 不通过配置引入 DI/IoC 或注解装配。

## 完整配置 Schema

```yaml
# 应用信息
app:
  name: my_game              # 应用名称
  version: "1.0.0"           # 版本号（可选）

# 日志配置
log:
  level: info                # 日志级别：debug | info | warn | error
  console: true              # 是否输出到控制台
  file: "logs/app.log"       # 日志文件路径（可选）
  max_size: 100              # 单个日志文件最大大小 MB（可选）
  max_files: 10              # 最大保留日志文件数（可选）

# Actor 服务配置
actors:
  - name: gateway            # 服务名称（唯一）
    script: scripts/gateway.lua  # Lua 脚本路径
    instances: 1             # 启动实例数，0 表示动态创建
    network:                 # 网络配置（仅 gateway 类服务需要）
      tcp: "0.0.0.0:8001"   # TCP 监听地址
      # udp: "0.0.0.0:8002" # UDP 监听地址（可选）
      # kcp: "0.0.0.0:8003" # KCP 监听地址（可选）
      # websocket: "0.0.0.0:8004"  # WebSocket 监听地址（可选）
      kcp_options:           # KCP 专属配置（可选）
        nodelay: 1
        interval: 10
        resend: 2
        nc: 1
    transport: MyTransport   # 自定义 C++ transport（可选）
    options:                 # 服务自定义配置（可选）
      max_connections: 10000

  - name: player
    script: scripts/player.lua
    instances: 0             # 动态创建

  - name: room
    script: scripts/room.lua
    instances: 1

# 数据库配置
database:
  driver: mysql              # 数据库驱动：mysql | postgresql | sqlite
  host: localhost
  port: 3306
  database: game
  username: root
  password: ""               # 生产环境建议使用环境变量
  pool_size: 10              # 最小连接池大小
  max_pool_size: 50          # 最大连接池大小
  connect_timeout: 5000      # 连接超时（ms）
  query_timeout: 30000       # 查询超时（ms）
  idle_timeout: 300000       # 空闲连接超时（ms）
  max_lifetime: 3600000      # 连接最大生命周期（ms）
  options:                   # 驱动专属配置（可选）
    charset: utf8mb4

# Redis 配置
redis:
  host: localhost
  port: 6379
  db: 0
  password: ""               # 生产环境建议使用环境变量
  pool_size: 10
  max_pool_size: 50
  connect_timeout: 5000
  command_timeout: 5000
  idle_timeout: 300000

# 日志配置
log:
  level: info                # 日志级别：debug | info | warn | error
  format: json               # json | text
  console: true              # 输出到控制台
  file:
    enabled: true
    path: "logs/shield.log"
    max_size: 100            # 单个文件最大 MB
    max_files: 10            # 最大保留文件数
    rotation: daily          # daily | size | none
    compress: true           # 轮转后压缩
  # 服务级别覆盖
  services:
    gateway:
      level: debug
    payment:
      level: warn

# 集群配置（可选）
cluster:
  node_id: "node-1"          # 节点 ID（唯一）
  listen: "0.0.0.0:9000"    # 集群通信端口
  heartbeat_interval: 2000   # 心跳间隔（ms）

  # 节点发现方式（三选一）
  # 方式 1：静态配置（默认，零依赖）
  peers:
    - "node-2:9000"
    - "node-3:9000"

  # 方式 2：局域网广播发现（零依赖）
  # discovery:
  #   type: broadcast
  #   broadcast_port: 9001
  #   interval: 5000

  # 方式 3：Redis 服务发现（推荐小型游戏）
  # discovery:
  #   type: redis
  #   redis:
  #     host: "localhost"
  #     port: 6379
  #     prefix: "shield:nodes"
  #     ttl: 10

  # 方式 4：外部服务发现（大型部署）
  # discovery:
  #   type: kubernetes    # 或 etcd / consul
  #   namespace: "game"
  #   service_name: "shield-cluster"

# 运维配置（可选）
ops:
  enabled: true              # 是否启用运维模块
  bind: "127.0.0.1:9090"   # 运维端点绑定地址
  metrics: true              # 是否启用 metrics
  health: true               # 是否启用健康检查
  profile: false             # 是否启用 profile（生产环境默认关闭）
  console: false             # 是否启用交互式控制台（生产环境默认关闭）

# 启动配置（可选）
bootstrap:
  timeout:
    config_load: 5000        # 配置加载超时（ms）
    data_init: 30000         # 数据层初始化超时（ms）
    network_init: 10000      # 网络层初始化超时（ms）
    cluster_init: 30000      # 集群层初始化超时（ms）
    service_spawn: 60000     # 服务启动超时（ms）
  retry:
    database:
      max_retries: 3
      delay: 5000
    redis:
      max_retries: 3
      delay: 5000

# 关闭配置（可选）
shutdown:
  timeout:
    service_drain: 30000     # 服务 draining 超时（ms）
    service_stop: 10000      # 单个服务停止超时（ms）
    data_close: 10000        # 数据层关闭超时（ms）
    total: 60000             # 总关闭超时（ms）
```

## 配置验证

配置加载时进行验证：

| 验证项 | 规则 |
|--------|------|
| app.name | 必填，1-64 字符 |
| actors | 至少一个 actor |
| actors[].name | 必填，全局唯一 |
| actors[].script | 必填，文件必须存在 |
| actors[].instances | >= 0 |
| actors[].restart.policy | always / on-failure / never |
| database.port | 1-65535 |
| database.pool_size | >= 1 |
| database.max_pool_size | >= pool_size |
| redis.port | 1-65535 |
| redis.pool_size | >= 1 |
| redis.max_pool_size | >= pool_size |
| log.level | debug / info / warn / error |
| cluster.node_id | 必填（启用集群时） |

验证失败时拒绝启动，输出明确的错误信息。

## 配置热更新

第一版不支持配置热更新。

未来支持的热更新范围：

| 配置项 | 热更新 | 说明 |
|--------|--------|------|
| log.level | ✅ | 立即生效 |
| database.pool_size | ⚠️ | 需要重启连接池 |
| actors[].instances | ⚠️ | 需要 spawn/stop 服务 |
| actors[].script | ❌ | 需要重启服务 |
| cluster.node_id | ❌ | 不允许变更 |

## 环境差异

通过环境变量覆盖配置：

```yaml
database:
  host: ${DB_HOST:localhost}      # 环境变量，默认值
  port: ${DB_PORT:3306}
  password: ${DB_PASSWORD}
```

或使用多配置文件：

```bash
# 启动时指定配置文件
./server --config config/app.yaml --config config/production.yaml
```

## 敏感配置

生产环境敏感配置建议：

- 使用环境变量
- 使用密钥管理服务（Vault、AWS Secrets Manager）
- 配置文件不提交到版本控制

```yaml
# .gitignore
config/production.yaml
config/secrets.yaml
```
