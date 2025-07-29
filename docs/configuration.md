# Shield 配置指南

本文档详细介绍 Shield 游戏服务器框架的配置选项，包括完整的配置文件说明、环境变量设置、以及不同部署场景的推荐配置。

## 📋 配置文件概览

Shield 使用 YAML 格式的配置文件，支持多环境配置和动态重载。默认配置文件为 `config/shield.yaml`。

### 配置文件结构

```yaml
# Shield 游戏服务器配置文件
# 版本: v1.0

# 组件配置 - 定义启用的服务组件
components:
  - "lua_vm_pool"    # Lua 虚拟机池
  - "gateway"        # 网关组件

# 网关配置
gateway:
  listener:          # 主监听器配置
    host: "127.0.0.1"
    port: 8080
  
  http:              # HTTP 服务配置
    enabled: true
    port: 8081
    max_connections: 10000
    request_timeout: 30000
  
  websocket:         # WebSocket 服务配置
    enabled: true
    port: 8082
    max_frame_size: 65536
    ping_interval: 30000
  
  threading:         # 线程配置
    io_threads: 4    # I/O 线程数，建议为 CPU 核心数

# Actor 系统配置
actor_system:
  node_id: "auto"    # 节点 ID，"auto" 为自动生成
  
  scheduler:         # 调度器配置
    policy: "sharing" # 调度策略: sharing, stealing
    max_threads: 8   # 最大工作线程数
  
  registry:          # 注册表配置
    cleanup_interval: 60    # 清理间隔 (秒)
    heartbeat_interval: 30  # 心跳间隔 (秒)

# Lua VM 池配置
lua_vm_pool:
  initial_size: 4          # 初始 VM 数量
  max_size: 16            # 最大 VM 数量
  min_size: 2             # 最小 VM 数量
  idle_timeout_ms: 30000  # 空闲超时 (毫秒)
  acquire_timeout_ms: 5000 # 获取超时 (毫秒)
  preload_scripts: true   # 是否预加载脚本

# 服务发现配置
service_discovery:
  type: "local"           # 类型: local, etcd, consul, nacos, redis
  
  local:                  # 本地服务发现配置
    heartbeat_interval: 30
    cleanup_interval: 60
    
  etcd:                   # etcd 配置
    endpoints: ["http://localhost:2379"]
    username: ""
    password: ""
    timeout: 5000
    
  consul:                 # consul 配置
    host: "localhost"
    port: 8500
    token: ""
    datacenter: "dc1"
    
  nacos:                  # nacos 配置
    server_addr: "localhost:8848"
    namespace: "public"
    username: ""
    password: ""
    
  redis:                  # redis 配置
    host: "localhost"
    port: 6379
    password: ""
    database: 0

# 日志配置
logger:
  level: "info"           # 日志级别: debug, info, warn, error
  console_output: true    # 是否输出到控制台
  file_output: false      # 是否输出到文件
  file_path: "logs/shield.log" # 日志文件路径
  max_file_size: 10485760 # 最大文件大小 (字节)
  max_files: 5           # 最大文件数量

# 网络配置
network:
  tcp:
    bind_address: "0.0.0.0"  # 绑定地址
    port: 8080              # 默认端口
    backlog: 1024           # 监听队列长度
    keepalive: true         # 是否启用 keepalive
    nodelay: true           # 是否禁用 Nagle 算法
    
  buffer:
    size: 8192              # 缓冲区大小
    max_connections: 10000  # 最大连接数
    
  ssl:                      # SSL/TLS 配置
    enabled: false
    cert_file: "certs/server.crt"
    key_file: "certs/server.key"
    ca_file: "certs/ca.crt"

# 监控配置
monitoring:
  enabled: true
  metrics_port: 9090        # Prometheus 指标端口
  health_check_port: 8888   # 健康检查端口
  
  prometheus:
    enabled: true
    path: "/metrics"
    
  traces:
    enabled: false
    jaeger_endpoint: "http://localhost:14268/api/traces"

# 性能配置
performance:
  memory:
    pool_enabled: true      # 是否启用内存池
    pool_size: 67108864    # 内存池大小 (64MB)
    
  cpu:
    affinity_enabled: false # 是否启用 CPU 亲和性
    priority: 0            # 进程优先级
    
  gc:
    lua_gc_step: 200       # Lua GC 步长
    lua_gc_pause: 200      # Lua GC 暂停

# 安全配置
security:
  rate_limiting:
    enabled: true
    requests_per_second: 1000  # 每秒请求限制
    burst_size: 100           # 突发大小
    
  authentication:
    enabled: false
    jwt_secret: "your-secret-key"
    token_expire: 3600        # Token 过期时间 (秒)
    
  encryption:
    message_encryption: false # 消息加密
    encryption_key: "32-byte-encryption-key-here"

# 数据库配置 (可选)
database:
  enabled: false
  type: "postgresql"        # postgresql, mysql, sqlite
  
  postgresql:
    host: "localhost"
    port: 5432
    database: "shield_game"
    username: "shield_user"
    password: "shield_pass"
    max_connections: 20
    
  redis_cache:
    enabled: true
    host: "localhost"
    port: 6379
    database: 1
    password: ""
    max_connections: 10

# 游戏特定配置
game:
  world:
    max_players: 10000      # 最大玩家数
    tick_rate: 60          # 游戏循环频率 (Hz)
    save_interval: 300     # 存档间隔 (秒)
    
  rooms:
    max_rooms: 1000        # 最大房间数
    max_players_per_room: 100 # 每房间最大玩家数
    room_timeout: 1800     # 房间超时 (秒)
    
  chat:
    max_message_length: 500 # 最大消息长度
    rate_limit: 10         # 消息频率限制 (条/分钟)
    filter_enabled: true   # 是否启用内容过滤
```

## 🔧 配置详解

### 1. 组件配置 (Components)

组件配置定义了服务启动时加载的功能模块：

```yaml
components:
  - "lua_vm_pool"    # 必需 - Lua 脚本执行环境
  - "gateway"        # 必需 - 客户端接入网关
  - "monitor"        # 可选 - 监控组件
  - "admin"          # 可选 - 管理接口
```

**可用组件列表**:
- `lua_vm_pool`: Lua 虚拟机池管理
- `gateway`: 网关服务 (TCP/HTTP/WebSocket)
- `monitor`: 监控和指标收集
- `admin`: 管理接口和工具
- `logger`: 日志服务 (通常自动启用)

### 2. 网关配置 (Gateway)

#### 监听器配置
```yaml
gateway:
  listener:
    host: "0.0.0.0"      # 监听所有接口
    port: 8080           # 主端口
    backlog: 1024        # TCP 监听队列长度
    reuse_addr: true     # 地址重用
    reuse_port: true     # 端口重用 (Linux)
```

#### HTTP 服务配置
```yaml
gateway:
  http:
    enabled: true              # 启用 HTTP 服务
    port: 8081                # HTTP 端口
    max_connections: 10000     # 最大连接数
    request_timeout: 30000     # 请求超时 (毫秒)
    max_request_size: 1048576  # 最大请求大小 (1MB)
    keep_alive: true          # HTTP Keep-Alive
    compression: true         # 响应压缩
    
    routes:                   # 路由配置
      - path: "/api/health"
        method: "GET"
        handler: "health_check"
      - path: "/api/game/*"
        method: "POST"
        handler: "game_action"
```

#### WebSocket 配置
```yaml
gateway:
  websocket:
    enabled: true              # 启用 WebSocket
    port: 8082                # WebSocket 端口
    max_frame_size: 65536     # 最大帧大小 (64KB)
    ping_interval: 30000      # Ping 间隔 (毫秒)
    pong_timeout: 5000        # Pong 超时 (毫秒)
    compression: true         # 帧压缩
    
    subprotocols:             # 支持的子协议
      - "shield-game-v1"
      - "shield-chat-v1"
```

### 3. Actor 系统配置

#### 节点配置
```yaml
actor_system:
  node_id: "game-server-01"   # 固定节点 ID
  # node_id: "auto"           # 自动生成 ID
  
  cluster:
    name: "shield-cluster"    # 集群名称
    seeds:                    # 种子节点
      - "127.0.0.1:8080"
      - "127.0.0.1:8081"
```

#### 调度器配置
```yaml
actor_system:
  scheduler:
    policy: "sharing"         # sharing: 工作共享, stealing: 工作窃取
    max_threads: 8           # 最大工作线程数
    thread_stack_size: 8388608 # 线程栈大小 (8MB)
    max_messages_per_run: 50  # 每次运行最大消息数
```

### 4. Lua VM 池配置

```yaml
lua_vm_pool:
  initial_size: 4              # 启动时创建的 VM 数量
  max_size: 16                # 池中最大 VM 数量
  min_size: 2                 # 池中最小 VM 数量
  idle_timeout_ms: 30000      # VM 空闲超时
  acquire_timeout_ms: 5000    # 获取 VM 超时
  
  preload_scripts: true       # 预加载脚本
  script_paths:               # 脚本路径
    - "scripts/"
    - "game_logic/"
    
  hot_reload:                 # 热重载配置
    enabled: true
    check_interval: 1000      # 检查间隔 (毫秒)
    file_extensions:          # 监控的文件扩展名
      - ".lua"
      - ".luac"
```

### 5. 服务发现配置

#### etcd 配置
```yaml
service_discovery:
  type: "etcd"
  
  etcd:
    endpoints:                # etcd 集群端点
      - "http://etcd1:2379"
      - "http://etcd2:2379"
      - "http://etcd3:2379"
    username: "shield_user"   # 认证用户名
    password: "shield_pass"   # 认证密码
    timeout: 5000            # 连接超时
    
    service_ttl: 30          # 服务 TTL (秒)
    lease_renewal: 10        # 租约续期间隔 (秒)
    
    tls:                     # TLS 配置
      enabled: false
      cert_file: "client.crt"
      key_file: "client.key"
      ca_file: "ca.crt"
```

#### Consul 配置
```yaml
service_discovery:
  type: "consul"
  
  consul:
    host: "consul.example.com"
    port: 8500
    token: "consul-acl-token"  # ACL Token
    datacenter: "dc1"         # 数据中心
    
    health_check:             # 健康检查
      interval: "10s"
      timeout: "3s"
      deregister_after: "30s"
      
    tags:                     # 服务标签
      - "game-server"
      - "version-1.0"
      - "environment-prod"
```

### 6. 监控配置

#### Prometheus 集成
```yaml
monitoring:
  enabled: true
  metrics_port: 9090
  
  prometheus:
    enabled: true
    path: "/metrics"
    namespace: "shield"       # 指标命名空间
    
    custom_metrics:           # 自定义指标
      - name: "game_players_total"
        type: "gauge"
        description: "Total number of players"
      - name: "messages_processed_total"
        type: "counter"
        description: "Total messages processed"
```

#### 健康检查
```yaml
monitoring:
  health_check:
    enabled: true
    port: 8888
    path: "/health"
    
    checks:                   # 检查项目
      - name: "database"
        timeout: 5000
      - name: "redis"
        timeout: 3000
      - name: "lua_vm_pool"
        timeout: 1000
```



### 配置文件优先级

1. 命令行参数 (最高优先级)
2. 环境变量
3. 配置文件
4. 默认值 (最低优先级)

```bash
# 指定配置文件
./shield --config /path/to/custom.yaml

# 使用环境变量
SHIELD_LOG_LEVEL=debug ./shield

# 组合使用
SHIELD_NODE_ID=server-01 ./shield --config prod.yaml
```

## 🚀 部署场景配置

### 1. 开发环境配置

**文件**: `config/development.yaml`

```yaml
# 开发环境配置 - 注重调试和快速迭代
components:
  - "lua_vm_pool"
  - "gateway"

gateway:
  listener:
    host: "127.0.0.1"
    port: 8080
  http:
    enabled: true  
    port: 8081
  websocket:
    enabled: true
    port: 8082
  threading:
    io_threads: 2

actor_system:
  node_id: "dev-node"
  scheduler:
    max_threads: 4

lua_vm_pool:
  initial_size: 2
  max_size: 4
  hot_reload:
    enabled: true
    check_interval: 500  # 快速热重载

service_discovery:
  type: "local"

logger:
  level: "debug"        # 详细日志
  console_output: true
  file_output: true

monitoring:
  enabled: true
  
performance:
  memory:
    pool_enabled: false  # 开发时禁用内存池便于调试
```

### 2. 测试环境配置

**文件**: `config/testing.yaml`

```yaml
# 测试环境配置 - 注重稳定性和监控
components:
  - "lua_vm_pool"
  - "gateway"  
  - "monitor"

gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  threading:
    io_threads: 4

actor_system:
  node_id: "test-node-01"
  scheduler:
    max_threads: 6

lua_vm_pool:
  initial_size: 4
  max_size: 8
  hot_reload:
    enabled: false      # 测试环境禁用热重载

service_discovery:
  type: "etcd"
  etcd:
    endpoints: ["http://etcd-test:2379"]

logger:
  level: "info"
  file_output: true
  max_files: 10

monitoring:
  enabled: true
  prometheus:
    enabled: true

security:
  rate_limiting:
    enabled: true
    requests_per_second: 500
```

### 3. 生产环境配置

**文件**: `config/production.yaml`

```yaml
# 生产环境配置 - 注重性能和可用性
components:
  - "lua_vm_pool"
  - "gateway"
  - "monitor"
  - "admin"

gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  http:
    enabled: true
    port: 8081
    max_connections: 50000
  websocket:
    enabled: true
    port: 8082
    max_frame_size: 131072  # 128KB
  threading:
    io_threads: 16          # 生产环境更多线程

actor_system:
  node_id: "auto"           # 自动生成唯一 ID
  scheduler:
    policy: "stealing"      # 工作窃取更高效
    max_threads: 32        # 充分利用 CPU

lua_vm_pool:
  initial_size: 16
  max_size: 64
  hot_reload:
    enabled: false          # 生产环境禁用热重载
  preload_scripts: true

service_discovery:
  type: "etcd"
  etcd:
    endpoints:
      - "http://etcd1.prod:2379"
      - "http://etcd2.prod:2379" 
      - "http://etcd3.prod:2379"
    username: "${ETCD_USERNAME}"
    password: "${ETCD_PASSWORD}"
    tls:
      enabled: true

logger:
  level: "warn"             # 生产环境减少日志
  console_output: false
  file_output: true
  max_file_size: 104857600  # 100MB
  max_files: 50

network:
  buffer:
    max_connections: 100000 # 支持更多连接
  ssl:
    enabled: true           # 生产环境启用 SSL
    cert_file: "/etc/ssl/shield.crt"
    key_file: "/etc/ssl/shield.key"

monitoring:
  enabled: true
  prometheus:
    enabled: true
  traces:
    enabled: true
    jaeger_endpoint: "http://jaeger.prod:14268/api/traces"

performance:
  memory:
    pool_enabled: true
    pool_size: 268435456    # 256MB 内存池
  cpu:
    affinity_enabled: true  # 启用 CPU 亲和性
    priority: -5           # 高优先级

security:
  rate_limiting:
    enabled: true
    requests_per_second: 10000
    burst_size: 1000
  authentication:
    enabled: true
    jwt_secret: "${JWT_SECRET}"
  encryption:
    message_encryption: false  # 根据需要启用

database:
  enabled: true
  postgresql:
    host: "db.prod.example.com"
    port: 5432
    database: "shield_prod"
    username: "${DB_USERNAME}"
    password: "${DB_PASSWORD}"
    max_connections: 50
  redis_cache:
    enabled: true
    host: "redis.prod.example.com"
    max_connections: 20

game:
  world:
    max_players: 100000
    tick_rate: 60
  rooms:
    max_rooms: 10000
    max_players_per_room: 200
```

### 4. 集群配置示例

**网关节点**: `config/gateway-cluster.yaml`

```yaml
# 专用网关节点配置
components:
  - "gateway"               # 只启用网关组件

gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  threading:
    io_threads: 16          # 网关节点专注 I/O 处理

actor_system:
  node_id: "gateway-${HOSTNAME}"
  
service_discovery:
  type: "consul"
  consul:
    host: "consul.cluster"
    tags:
      - "gateway"
      - "frontend"

# 不启用 Lua 相关配置
lua_vm_pool:
  initial_size: 0
  max_size: 0
```

**逻辑节点**: `config/logic-cluster.yaml`

```yaml
# 专用逻辑节点配置  
components:
  - "lua_vm_pool"           # 只启用逻辑处理组件

# 不启用网关组件
gateway:
  listener:
    host: "127.0.0.1"       # 只监听本地
    port: 0                 # 禁用监听

lua_vm_pool:
  initial_size: 32          # 逻辑节点大量 VM
  max_size: 128

actor_system:
  node_id: "logic-${HOSTNAME}"
  scheduler:
    max_threads: 64         # 逻辑节点专注计算

service_discovery:
  type: "consul"
  consul:
    tags:
      - "logic"
      - "backend"
```

## 🔍 配置验证

### 配置文件验证

```bash
# 验证配置文件语法
./shield --config config/shield.yaml --validate

# 输出解析后的配置
./shield --config config/shield.yaml --dump-config
```

### 配置测试

```bash
# 测试服务发现连接
./shield --config config/shield.yaml --test-discovery

# 测试数据库连接
./shield --config config/shield.yaml --test-database

# 全面配置测试
./shield --config config/shield.yaml --test-all
```

## ⚡ 性能调优建议

### CPU 密集型负载

```yaml
actor_system:
  scheduler:
    policy: "stealing"      # 工作窃取
    max_threads: 32        # CPU 核心数 * 2

performance:
  cpu:
    affinity_enabled: true  # CPU 亲和性
    priority: -10          # 高优先级
```

### I/O 密集型负载

```yaml
gateway:
  threading:
    io_threads: 16         # 更多 I/O 线程

network:
  buffer:
    size: 16384           # 更大缓冲区
    max_connections: 50000

performance:
  memory:
    pool_enabled: true
    pool_size: 536870912  # 512MB 内存池
```

### 内存优化

```yaml
lua_vm_pool:
  max_size: 32            # 限制 VM 数量
  idle_timeout_ms: 15000  # 更短超时

performance:
  gc:
    lua_gc_step: 100      # 更频繁的 GC
    lua_gc_pause: 150     # 更短的 GC 暂停
```

---

## 📝 配置最佳实践

1. **使用环境变量**: 敏感信息通过环境变量传递
2. **分层配置**: 基础配置 + 环境特定配置
3. **配置验证**: 部署前验证配置文件正确性
4. **监控配置**: 始终启用监控和日志
5. **安全配置**: 生产环境启用所有安全特性
6. **性能调优**: 根据负载特点调整线程和缓冲区
7. **备份配置**: 配置文件纳入版本控制

通过合理的配置，Shield 可以适应从开发测试到大规模生产的各种环境需求！