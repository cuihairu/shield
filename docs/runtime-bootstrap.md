# 启动流程运行时语义

本文档包含 Shield 启动和关闭流程的运行时语义决策。

## 设计原则

- 启动顺序明确，依赖关系单向。
- 启动失败快速失败，输出清晰错误。
- 关闭顺序与启动相反，确保资源释放。
- 每个阶段有超时保护。

## 启动流程

```
shield::run(argc, argv)
  │
  ├─ 1. 解析命令行参数
  │     - --config <path>     配置文件路径（必填）
  │     - --node-id <id>      覆盖 node_id（可选）
  │     - --log-level <level> 覆盖日志级别（可选）
  │     - --help              显示帮助
  │
  ├─ 2. 加载配置
  │     - 读取 YAML 配置文件
  │     - 环境变量替换 ${VAR:default}
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
  ├─ 5. 初始化 shield_core
  │     - 创建 ServiceRegistry
  │     - 创建 MessageRouter
  │     - 创建 TimerScheduler
  │     - 创建 CoroutineScheduler
  │     - 失败：输出错误，exit(1)
  │
  ├─ 6. 初始化数据层（如配置）
  │     - 初始化数据库连接池
  │     - 初始化 Redis 连接池
  │     - 测试连接
  │     - 失败：输出错误，exit(1)
  │
  ├─ 7. 初始化网络层（如配置）
  │     - 创建 shield_transport
  │     - 创建 shield_net
  │     - 绑定 TCP/UDP/KCP/WebSocket 监听
  │     - 失败：输出错误，exit(1)
  │
  ├─ 8. 初始化集群层（如配置）
  │     - 创建 shield_cluster
  │     - 连接 peers 或启动发现
  │     - 失败：警告但不阻止启动
  │
  ├─ 9. 初始化运维层（如配置）
  │     - 创建 shield_ops
  │     - 启动 HTTP 端点
  │     - 失败：警告但不阻止启动
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
| shield.shutdown() | Lua API 主动关闭 |

Windows 下使用 `SetConsoleCtrlHandler` 替代信号。

## 启动超时

每个阶段有独立超时，配置见 [配置语义](runtime-config.md#完整配置-schema) 中 `bootstrap.timeout` 部分。

超时后：

- 输出超时阶段和已耗时。
- 尝试清理已初始化的资源。
- exit(1)。

## 启动日志

启动过程输出关键节点日志：

```txt
[INFO] Shield starting...
[INFO] Config loaded: config/app.yaml
[INFO] Log level: info
[INFO] Database connected: localhost:3306/game
[INFO] Redis connected: localhost:6379
[INFO] Network listening: TCP 0.0.0.0:8001
[INFO] Cluster: node-1, listening 0.0.0.0:9000
[INFO] Ops: http://127.0.0.1:9090
[INFO] Service spawned: gateway (id=1)
[INFO] Service spawned: player (id=2)
[INFO] Shield started (pid=12345, uptime=0s)
```

启动失败日志：

```txt
[ERROR] Failed to load config: config/app.yaml not found
[ERROR] Database connection failed: Connection refused
[WARN]  Cluster connection failed, running standalone
```

## 关闭流程

关闭顺序与启动相反：

```
收到停止信号
  │
  ├─ 1. 设置全局 stopping 标志
  │     - 新的 spawn/send/call 返回 runtime_stopping
  │
  ├─ 2. 停止接受新连接
  │     - 关闭网络监听
  │     - 不再接受新客户端连接
  │
  ├─ 3. 停止服务（逆序）
  │     - 按 spawn 逆序停止服务
  │     - 向每个 service 发送停止通知
  │     - 等待 service 完成或超时
  │     - 强制停止超时的 service
  │
  ├─ 4. 清理集群
  │     - 通知其他节点本节点离开
  │     - 断开集群连接
  │
  ├─ 5. 关闭数据层
  │     - 等待进行中的查询完成或超时
  │     - 关闭数据库连接池
  │     - 关闭 Redis 连接池
  │
  ├─ 6. 关闭运维层
  │     - 停止 HTTP 端点
  │
  ├─ 7. 清理核心
  │     - 释放 ServiceRegistry
  │     - 释放 MessageRouter
  │     - 释放 TimerScheduler
  │     - 释放所有 Lua VM
  │
  └─ 8. 输出关闭日志，exit(0)
```

## 关闭超时

配置见 [配置语义](runtime-config.md#完整配置-schema) 中 `shutdown.timeout` 部分。

超时后强制退出，输出未释放资源的警告。

## 关闭日志

```txt
[INFO] Shutdown initiated...
[INFO] Stopping services...
[INFO] Service stopped: gateway (reason=stopping)
[INFO] Service stopped: player (reason=stopping)
[WARN]  Service stop timeout: room, forcing...
[INFO] Cluster: disconnected
[INFO] Database: connection pool closed
[INFO] Redis: connection pool closed
[INFO] Shield stopped (uptime=3600s)
```

## 启动失败恢复

### 重试策略

部分初始化失败可配置重试：

```yaml
bootstrap:
  retry:
    database:
      max_retries: 3
      delay: 5000            # 重试间隔（ms）
    redis:
      max_retries: 3
      delay: 5000
    cluster:
      max_retries: 0         # 不重试，降级运行
```

### 降级运行

集群连接失败时：

- 输出警告。
- 以单节点模式运行。
- 定期重试连接。

数据库连接失败时：

- 如果服务依赖数据库，启动失败。
- 如果服务不依赖数据库，可继续运行。

## 优雅重启

支持 SIGUSR1 触发优雅重启（Linux）：

```bash
kill -USR1 <pid>
```

行为：

1. 停止接受新连接。
2. 等待现有连接完成。
3. 重新加载配置（仅热更新项）。
4. 重新绑定网络监听。
5. 恢复接受新连接。

不支持热更新的配置项需要完整重启。

## 进程退出码

| 退出码 | 说明 |
|--------|------|
| 0 | 正常退出 |
| 1 | 启动失败（配置错误、依赖不可用等） |
| 2 | 运行时致命错误 |
| 130 | SIGINT 中断 |
| 143 | SIGTERM 终止 |
