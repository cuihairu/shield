# 配置指南

Shield 使用 YAML 格式配置文件，默认路径 `config/app.yaml`。

## 最小配置

```yaml
app:
  name: "My Game"

log:
  global_level: "info"
  console:
    enabled: true

server:
  host: "0.0.0.0"
  port: 8080

lua:
  script_dir: "scripts/"
  auto_reload: true
  preload_scripts:
    - "init.lua"

gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  http:
    enabled: true
    port: 8082
    backend: "beast"
  websocket:
    enabled: true
    port: 8083
    path: "/ws"
  udp:
    enabled: true
    port: 8084
```

## 完整配置参考

### 应用配置

```yaml
app:
  name: "Shield Server"
```

### 日志配置

```yaml
log:
  global_level: "info"       # trace/debug/info/warning/error/fatal
  console:
    enabled: true
  file:
    enabled: true
    path: "logs/shield.log"
    rotation_size_mb: 100
```

### Lua 脚本配置

```yaml
lua:
  script_dir: "scripts/"
  auto_reload: true           # 热重载
  preload_scripts:
    - "init.lua"
```

### 网关配置

```yaml
gateway:
  listener:
    host: "0.0.0.0"
    port: 8080                # TCP 端口
  http:
    enabled: true
    port: 8082                # HTTP 端口
    backend: "beast"          # beast（推荐）
  websocket:
    enabled: true
    port: 8083                # WebSocket 端口
    path: "/ws"
    lua_script: "scripts/session_handler.lua"
  udp:
    enabled: true
    port: 8084                # UDP 端口
```

### 服务发现配置

```yaml
discovery:
  type: "static"              # static/redis/nacos/consul/etcd

  # 静态列表（开发用）
  static:
    nodes:
      - "127.0.0.1:8080"

  # Redis
  redis:
    host: "localhost"
    port: 6379

  # Nacos
  nacos:
    server_addr: "localhost:8848"
    namespace: "public"

  # Consul
  consul:
    host: "localhost"
    port: 8500

  # Etcd
  etcd:
    endpoints:
      - "http://localhost:2379"
```

### 指标配置（可选）

```yaml
metrics:
  enabled: true
  port: 9090
```

## 配置优先级

1. 命令行参数（最高）
2. 环境变量
3. 配置文件
4. 默认值（最低）

```bash
# 指定配置文件
./build/bin/shield server --config config/app.yaml
```

## 配置重载

`FileWatcher` 监控配置文件变更，支持运行时重载部分配置项（如日志级别）。通过 `GET /status/config` 查看可重载范围。

## 多环境配置

参考项目模板：

- `templates/single-node/config/app.yaml` — 单节点配置
- `templates/multi-node/config/gateway.yaml` — 网关节点配置
- `templates/multi-node/config/logic.yaml` — 逻辑节点配置
