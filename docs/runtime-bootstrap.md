# Bootstrap Runtime Semantics

This document contains the runtime semantics decisions for Shield's startup and shutdown flows.

## Design Principles

- The startup order is explicit; dependencies are one-directional.
- Startup failures fail fast with clear errors.
- The shutdown order is the reverse of startup, ensuring resource release.
- Each stage has timeout protection (currently only the shutdown stage implements timeout budgets; the
  startup stage has no stage timeouts, see "Startup Timeouts").

## Startup Flow

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

## Startup Signals

The following stop signals are supported:

| Signal | Description |
|------|------|
| SIGINT | Ctrl+C |
| SIGTERM | the kill command |
| `shield.server.shutdown(delay_ms)` | Lua-initiated shutdown (`shield_server` module; triggers runtime stop after the ServerManager shutdown handover; `delay_ms` is the delay in milliseconds) |

On Windows, `SetConsoleCtrlHandler` replaces signals.

## CLI Contract

`shield::run(argc, argv)` is the stable user-side C++ entry point. The `shield` executable and example
programs should all call it directly, so they share one parameter set.

Phase 1 parameters:

| Parameter | Behavior |
| --- | --- |
| `--config <path>` / `-c <path>` | add a config file; repeatable; defaults to `config/app.yaml` when absent |
| `--log-level <level>` | override the log level from configuration |
| `--workers <n>` | override the runtime worker count; `0` means auto |
| `--node-id <id>` | valid only when `shield_cluster` is enabled; a CLI error otherwise |
| `--check-config` | initialize the runtime, load configuration, start the configured actors, then immediately shut down and return; for CI/smoke tests |
| `--version` / `-v` | print the version and return 0 without initializing the runtime |
| `--help` / `-h` | print help and return 0 without initializing the runtime |

Rules:

- Multiple `--config` files load and merge in command-line order; merge rules in
  Configuration Semantics.
- If the default `config/app.yaml` does not exist, startup fails with 1.
- Legacy subcommands (the old `server`, `config`, `migrate`) do not enter the new `shield::run` public
  contract; while retained they exist only as legacy CLI code.
- `shield::bootstrap::initialize` / `shutdown` are lower-level APIs; they do no command-line parsing, signal
  handling, or help/version output.

## Startup Timeouts

**Not implemented.** bootstrap has no stage-timeout mechanism: the `bootstrap.timeout.*` config keys have no
consumers (the example keys in the schema are planning surface, see the annotation in
Configuration Semantics). A startup stage that hangs is never
interrupted on a timer, and no "clean up initialized resources then exit(1)" is triggered.

The shutdown-side `shutdown.timeout.*` is implemented; see "Shutdown Timeouts".

## Startup Logs

Startup logs the following key milestones per the actual implementation (logger `bootstrap`, line format
`<timestamp_ms> [LEVEL] <logger>: <message>`; lines with condition annotations appear only when the
corresponding feature is enabled):

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

What follows `Service spawned:` is the spawn-returned service id (for actors configured with a `name`,
that name; unnamed service ids look like `<module>:<hash>`). There are also optional subsystem
initialization-success logs (`Player subsystem initialized` / `Server subsystem initialized` /
`Global subsystem initialized`).

Startup failure logs (all of these strings actually exist):

```txt
<ts> [ERROR] bootstrap: Failed to load config: config/app.yaml
<ts> [ERROR] bootstrap: Invalid config: <字段路径与原因>
<ts> [ERROR] bootstrap: Plugin startup failed: <插件错误，如 plugin.create.failed: ...>
<ts> [ERROR] bootstrap: Failed to spawn actor 'player': <错误原因>
<ts> [ERROR] bootstrap: Failed to start TCP listener for actor 'gateway': <错误原因>
```

## Shutdown Flow

The shutdown order is the reverse of startup:

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

`on_shutdown(ctx)` is a service-level drain hook, not a lifecycle event bus, and is not broadcast through
`shield.event`. Ordinary services have no `on_ready`; application ready is determined by bootstrap when the
required actors start successfully and it prepares to accept. Player ready is `shield_player`'s
`PlayerSession` state, see Player Lifecycle.

## Shutdown Timeouts

Configuration in the `shutdown.timeout` section of
Configuration Semantics.

After the timeout, force exit with a warning about unreleased resources.

## Shutdown Logs

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

## Startup Failure Recovery

### Retry policy

Some initialization failures support configurable retries:

```yaml
bootstrap:
  retry:
    plugins:
      max_retries: 3
      delay: 5000
    cluster:
      max_retries: 0
```

### Degraded operation

With `shield_cluster` enabled, when cluster connection fails:

- log a warning.
- run in single-node mode.
- retry the connection periodically.

On database connection failure:

- required plugins (like `redis.driver`) build their pool and ping during plugin start; failure fails
  startup.
- lazy-connect plugins like MySQL connect on first use; startup is unaffected and calls return errors.

## Graceful Restart

The capabilities in this section are not yet implemented: the process registers only the two stop signals
SIGINT/SIGTERM (Windows uses `SetConsoleCtrlHandler`); there is no SIGUSR1 handler and no "graceful restart
without restarting the process" flow; config file hot reload is likewise unimplemented (see
Configuration Semantics). Changing configuration or code requires a full stop +
restart.

## Process Exit Codes

| Exit code | Description |
|--------|------|
| 0 | normal exit |
| 1 | startup failure (config error, dependency unavailable, etc.) |
| 2 | fatal runtime error |
| 130 | interrupted by SIGINT |
| 143 | terminated by SIGTERM |
