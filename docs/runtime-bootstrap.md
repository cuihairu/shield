# 启动流程运行时语义

本文档包含 Shield 启动和关闭流程的运行时语义决策。

## 设计原则

- 启动顺序明确，依赖关系单向。
- 启动失败快速失败，输出清晰错误。
- 关闭顺序与启动相反，确保资源释放。
- 每个阶段有超时保护（当前仅 shutdown 阶段实现了超时预算；启动阶段没有阶段超时，见「启动超时」）。

## 启动流程

```
shield::run(argc, argv)
  │
  ├─ 1. 解析命令行参数
  │     - --config <path>     配置文件路径；可重复；默认 config/app.yaml
  │     - --node-id <id>      仅启用 shield_cluster 时允许
  │     - --log-level <level> 覆盖日志级别（可选）
  │     - --workers <n>       覆盖 runtime worker 数（可选）
  │     - --check-config      初始化、验证、启动配置 actors 后立即关闭
  │     - --version           显示版本
  │     - --help              显示帮助
  │
  ├─ 2. 加载配置
  │     - 读取 YAML 配置文件
  │     - （环境变量替换 ${VAR:default} 未实现——配置加载器不处理占位符）
  │     - 多配置文件合并（--config 可多次指定）
  │     - 配置验证（必填项、类型、范围）
  │     - 失败：输出错误，exit(1)
  │
  ├─ 3. 初始化日志
  │     - 根据配置初始化日志系统
  │     - 设置日志级别、输出目标
  │     - 记录启动信息
  │
  ├─ 4. 初始化 shield_base
  │     - 基础类型注册
  │     - 全局 monotonic clock 初始化
  │
  ├─ 5. 初始化 shield_core / CAF
  │     - 注册 CAF 类型（initialize_caf_types）
  │     - 创建 caf::actor_system（scheduler 线程数来自 --workers）
  │     - 失败：输出错误，exit(1)
  │
  ├─ 6. 加载插件（外部连接由插件自身建立）
  │     - bootstrap 不建数据库/Redis 连接池；数据层连接归插件实例所有
  │     - Redis driver 在插件 start 时建连接池并 ping（required 插件
  │       连接失败即启动失败）；MySQL 连接池按需建连（启动时不做
  │       连接测试，失败在首次查询时暴露）
  │     - required 插件 create/start 失败：输出错误，exit(1)
  │
  ├─ 7. 初始化网络层（如配置）
  │     - 创建 shield_transport
  │     - 创建 shield_net
  │     - Phase 1 绑定 TCP 监听；UDP/KCP/WebSocket 属于 deferred transport
  │     - 失败：输出错误，exit(1)
  │
  ├─ 8. 初始化集群层（仅启用 shield_cluster 时）
  │     - 创建 shield_cluster
  │     - 连接 peers 或启动发现
  │     - 配置非法、本地监听失败、节点身份冲突：exit(1)
  │     - 远端连接失败：可退化为单节点，但必须标记 unhealthy
  │
  ├─ 9. 启动运维入口（配置门控，非模块开关）
  │     - console.enabled = true 时启动 console server
  │       （Unix socket，默认 /tmp/shield-console.sock）
  │     - http.enabled = true 时启动 HTTP ops server
  │       （默认绑定 127.0.0.1:8080）
  │     - 没有 shield_ops 模块开关：bootstrap 硬编码 ops_enabled = false，
  │       配置中出现 `ops:` 段会因 optional module 校验直接拒绝启动
  │     - 端口/socket 绑定失败：记 ERROR 日志并继续启动（不 exit）
  │
  ├─ 10. 启动系统服务
  │      - 启动 bootstrap 服务（如有）
  │      - 启动配置的 actors
  │      - spawn 顺序按配置文件顺序
  │      - 单个 spawn 失败：警告，继续其他
  │      - 全部失败：exit(1)
  │
  ├─ 11. 进入事件循环
  │      - 主线程进入 CAF event loop
  │      - 处理消息、定时器、网络事件
  │      - 阻塞直到收到停止信号
  │
  └─ 12. 收到停止信号
        - 进入关闭流程
```

## 启动信号

支持以下停止信号：

| 信号 | 说明 |
|------|------|
| SIGINT | Ctrl+C |
| SIGTERM | kill 命令 |
| `shield.server.shutdown(delay_ms)` | Lua API 主动关闭（`shield_server` 模块；经 ServerManager 停机交接后触发 runtime 停止，`delay_ms` 为延迟毫秒） |

Windows 下使用 `SetConsoleCtrlHandler` 替代信号。

## CLI 契约

`shield::run(argc, argv)` 是用户侧 C++ 稳定入口。`shield` 可执行文件和示例程序都应直接调用它，因此共享同一套参数集合。

Phase 1 参数：

| 参数 | 行为 |
| --- | --- |
| `--config <path>` / `-c <path>` | 添加一个配置文件；可重复；未传入时默认 `config/app.yaml` |
| `--log-level <level>` | 覆盖配置中的日志级别 |
| `--workers <n>` | 覆盖 runtime worker 数；`0` 表示自动 |
| `--node-id <id>` | 仅 `shield_cluster` 启用时有效；未启用时返回 CLI 错误 |
| `--check-config` | 初始化 runtime、加载配置、启动配置 actors，随后立即关闭并返回；用于 CI/smoke test |
| `--version` / `-v` | 输出版本并返回 0，不初始化 runtime |
| `--help` / `-h` | 输出帮助并返回 0，不初始化 runtime |

规则：

- 多个 `--config` 按命令行顺序加载并合并，合并规则见 [配置语义](runtime-config.md#多配置文件合并)。
- 默认 `config/app.yaml` 不存在时，启动失败并返回 1。
- legacy subcommand（如旧 `server`、`config`、`migrate`）不进入新的 `shield::run` 公共契约；保留期间只能作为 legacy CLI 代码存在。
- `shield::bootstrap::initialize` / `shutdown` 是 lower-level API，不负责命令行解析、信号处理或 help/version 输出。

## 启动超时

**未实现。** bootstrap 没有阶段超时机制：`bootstrap.timeout.*` 配置键没有任何消费者（schema 中的示例键是规划面，见 [配置语义](runtime-config.md#phase-1-schema) 的标注）。启动卡住的阶段不会被计时中断，也不会触发「清理已初始化资源后 exit(1)」。

关闭侧的 `shutdown.timeout.*` 是已实现的，见「关闭超时」。

## 启动日志

启动过程按实际实现输出以下关键节点日志（logger 为 `bootstrap`，行格式为 `<timestamp_ms> [LEVEL] <logger>: <message>`；带条件标注的行只在对应功能启用时出现）：

```txt
<ts> [INFO] bootstrap: Shield runtime initializing...
<ts> [INFO] bootstrap: Config loaded: config/app.yaml
<ts> [INFO] bootstrap: File logging enabled: logs/shield.log          （log.file.enabled）
<ts> [INFO] bootstrap: Plugin system started
<ts> [INFO] bootstrap: CAF actor system initialized
<ts> [INFO] bootstrap: Cluster transport listening on port 9000       （cluster 启用时）
<ts> [INFO] bootstrap: Service spawned: gateway
<ts> [INFO] bootstrap: TCP gateway listener started for actor 'gateway' on 0.0.0.0:8001
<ts> [INFO] bootstrap: TLS enabled for actor 'gateway' (minimum protocol TLS 1.2, handshake timeout 10000 ms)   （network.tls）
<ts> [INFO] bootstrap: Address blocklist active for actor 'gateway' with 2 rule(s)                              （blocklist）
<ts> [INFO] bootstrap: Console server listening on /tmp/shield-console.sock   （console.enabled）
<ts> [INFO] bootstrap: HTTP ops server listening on 127.0.0.1:8080            （http.enabled）
<ts> [INFO] bootstrap: Shield runtime initialized
```

`Service spawned:` 后面是 spawn 返回的 service id（配置了 `name` 的 actor 即该名字；未命名的 service id 形如 `<module>:<hash>`）。此外还有可选子系统初始化成功的日志（`Player subsystem initialized` / `Server subsystem initialized` / `Global subsystem initialized`）。

启动失败日志（均为实际存在的文案）：

```txt
<ts> [ERROR] bootstrap: Failed to load config: config/app.yaml
<ts> [ERROR] bootstrap: Invalid config: <字段路径与原因>
<ts> [ERROR] bootstrap: Plugin startup failed: <插件错误，如 plugin.create.failed: ...>
<ts> [ERROR] bootstrap: Failed to spawn actor 'player': <错误原因>
<ts> [ERROR] bootstrap: Failed to start TCP listener for actor 'gateway': <错误原因>
```

## 关闭流程

关闭顺序与启动相反：

```
收到停止信号
  │
  ├─ 1. 设置全局 draining 标志
  │     - readiness 变为 not ready
  │     - 新的 spawn 和外部入口返回 runtime_stopping
  │     - runtime 内部已有 service 可在 deadline 内继续 `shield.call`
  │
  ├─ 2. 停止接受新连接
  │     - 关闭网络监听
  │     - 不再接受新客户端连接
  │
  ├─ 3. 服务 drain（逆序）
  │     - 按 spawn 逆序停止服务
  │     - 调用每个 service 的 `on_shutdown(ctx)`
  │     - 等待 drain 完成或 `shutdown.timeout.service_drain` 超时
  │     - 超时或失败只记录并继续关闭
  │
  ├─ 4. 设置全局 stopping 标志并停止服务（逆序）
  │     - 新的 send/call 返回 runtime_stopping
  │     - 调用每个 service 的 `on_exit("stopping")`
  │     - 超过 `shutdown.timeout.service_stop` 后强制释放
  │
  ├─ 5. 清理集群
  │     - 通知其他节点本节点离开
  │     - 断开集群连接
  │
  ├─ 6. 关闭插件实例
  │     - 等待进行中的插件调用完成或超时
  │     - 关闭插件拥有的连接池和外部资源
  │
  ├─ 7. 关闭运维层
  │     - 停止 HTTP 端点
  │
  ├─ 8. 清理核心
  │     - 停止并释放所有 service actor（shutdown_all）
  │     - 释放所有 Lua VM
  │     - 释放 caf::actor_system
  │
  └─ 9. 输出关闭日志，exit(0)
```

`on_shutdown(ctx)` 是服务级 drain hook，不是 lifecycle event bus，也不通过 `shield.event` 广播。普通 service 没有 `on_ready`；application ready 由 bootstrap 在 required actors 启动成功并准备 accept 时判定。玩家 ready 是 `shield_player` 的 `PlayerSession` 状态，见 [玩家生命周期](runtime-player.md)。

## 关闭超时

配置见 [配置语义](runtime-config.md#phase-1-schema) 中 `shutdown.timeout` 部分。

超时后强制退出，输出未释放资源的警告。

## 关闭日志

```txt
[INFO] Shutdown initiated...
[INFO] Stopping services...
[INFO] Service stopped: gateway (reason=stopping)
[INFO] Service stopped: player (reason=stopping)
[WARN]  Service stop timeout: room, forcing...
[INFO] Cluster: disconnected
[INFO] PluginHost: plugin instances stopped
[INFO] Shield stopped (uptime=3600s)
```

## 启动失败恢复

### 重试策略

部分初始化失败可配置重试：

```yaml
bootstrap:
  retry:
    plugins:
      max_retries: 3
      delay: 5000
    cluster:
      max_retries: 0
```

### 降级运行

启用 `shield_cluster` 后，集群连接失败时：

- 输出警告。
- 以单节点模式运行。
- 定期重试连接。

数据库连接失败时：

- required 插件（如 `redis.driver`）在插件 start 阶段建池并 ping，失败即启动失败。
- MySQL 等懒建连插件首次使用时才连接，启动不受影响，调用时返回错误。

## 优雅重启

本节描述的能力尚未实现：进程只注册了 SIGINT/SIGTERM 两个停止信号（Windows 用 `SetConsoleCtrlHandler`），没有 SIGUSR1 处理器，也不存在「不重启进程的优雅重启」流程；配置文件热重载同样未实现（见 [配置语义](runtime-config.md#热更新)）。需要变更配置或代码时，走完整停止 + 重新启动。

## 进程退出码

| 退出码 | 说明 |
|--------|------|
| 0 | 正常退出 |
| 1 | 启动失败（配置错误、依赖不可用等） |
| 2 | 运行时致命错误 |
| 130 | SIGINT 中断 |
| 143 | SIGTERM 终止 |
