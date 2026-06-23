# 配置设计

本文只做配置分层说明，不再重复定义 schema。Shield 当前 **core runtime 配置** 的权威来源是 [配置运行时语义](runtime-config.md)。

当前源码和历史配置里仍可能存在旧字段；这些字段不能视为当前重构契约。

## 配置分层

### Core Schema

core 只定义单节点 runtime 必需配置：

- `app`
- `log`
- `lua`
- `actors`
- `database`
- `redis`
- `bootstrap`
- `shutdown`

字段级规则、校验、热更新边界和合并策略统一以 [runtime-config.md](runtime-config.md) 为准。

### Optional Module Schema

以下配置段不属于 core schema，由对应官方可选模块单独解释：

- `cluster` -> [集群语义](runtime-cluster.md)
- `global` -> [全局能力](runtime-global.md)
- `ops` -> [运维与调试](runtime-ops.md)
- `player` -> [玩家生命周期](runtime-player.md)
- `server_manager` -> [服务器状态](runtime-server.md)

core bootstrap 只负责把这些配置快照传给已启用模块，不在 core 中做字段解释和校验。

### Plugins Schema

`plugins` 段（`plugins.directory`、`plugins.instances`、`plugins.bindings`）不属于 core schema，也不属于 optional module。它由 `shield_bootstrap` 读取后整段交给 [插件系统 v1](plugin-system.md) 的 `PluginHost` 处理：扫描、catalog、依赖解析、配置 schema 校验、绑定解析都在插件系统内部完成。core 配置层只透传，不解释字段。

## 最小示例

```yaml
app:
  name: hello_world

log:
  level: info
  console: true

actors:
  - name: gateway
    script: scripts/gateway.lua
    instances: 1
    network:
      tcp: "0.0.0.0:8001"

  - name: echo
    script: scripts/echo.lua
    instances: 1

database:
  enabled: true
  driver: mysql
  host: localhost
  port: 3306
  database: game
  username: root
  password: ${DB_PASSWORD:}

redis:
  enabled: true
  host: localhost
  port: 6379
  db: 0
  password: ${REDIS_PASSWORD:}
```

## 已删除的旧方向

以下字段和方向不再进入当前 core 配置设计：

- `discovery`
- `metrics`
- `health`
- `middleware`
- `conditions`
- `annotations`
- DI/IoC 装配配置

如果未来确实需要，只能在非 core 扩展文档中重新定义，不能再反向塞回 `runtime-config.md`。
