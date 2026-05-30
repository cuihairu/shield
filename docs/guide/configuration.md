# 配置管理

Shield 使用 YAML 配置文件，支持热更新。

## 配置文件

默认路径 `config/app.yaml`，通过 `--config` 参数指定：

```bash
./build/bin/shield server --config config/app.yaml
```

## 配置结构

```yaml
app:
  name: "Shield Server"

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

discovery:
  type: "static"
  static:
    nodes:
      - "127.0.0.1:8080"

metrics:
  enabled: true
  port: 9090
```

## 热更新

`FileWatcher` 监控配置文件变更，支持运行时重载部分配置项（如日志级别）。通过 `GET /status/config` 查看可重载范围。

## 配置优先级

1. 命令行参数（最高）
2. 环境变量
3. 配置文件
4. 默认值（最低）

## 多环境

参考模板配置：
- `templates/single-node/config/app.yaml`
- `templates/multi-node/config/gateway.yaml`
- `templates/multi-node/config/logic.yaml`
