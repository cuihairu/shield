# 配置设计

本文描述重构后的目标配置模型。当前配置文件仍包含旧架构字段，不能视为稳定契约。

## 原则

- YAML 只做声明式绑定，不承载业务逻辑。
- 配置驱动 Lua 服务、C++ service、网络监听、数据源和日志。
- 不在 core 中提供服务发现、插件、Prometheus、健康检查等配置。
- 不通过配置引入 DI/IoC 或注解装配。

## 最小结构

```yaml
app:
  name: hello_world

log:
  level: info
  console: true

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"

  - name: echo
    script: scripts/echo.lua
    instances: 1

database:
  driver: mysql
  host: localhost
  port: 3306
  database: game
  username: root
  password: ""
  pool_size: 5

redis:
  host: localhost
  port: 6379
  db: 0
```

## Actors

`actors` 是核心配置。

```yaml
actors:
  - name: player
    script: scripts/player.lua
    instances: 0
```

- `name`: 服务名。
- `script`: Lua 脚本路径。
- `instances`: 启动时创建的实例数量；`0` 表示动态创建。
- `network`: 可选，仅网关类服务需要。
- `transport`: 可选 C++ transport 名称。

## Data

`database` 和 `redis` 是原始访问能力，不代表 ORM 或 mapper 框架。

高级数据访问、schema mapper、迁移系统都不属于当前 core 配置契约。

## Deprecated For Core

以下旧配置在当前重构 core 中不再作为目标：

- `discovery`
- `metrics`
- `health`
- `plugins`
- `middleware`
- `conditions`
- `annotations`

如果后续需要保留，必须在非核心扩展文档中重新定义。
