# 交互式诊断控制台

## 概述

shield 提供了一个交互式诊断控制台，用于线上服务的实时观测和诊断。该控制台分两层：

- **Root 层 (C++)**：观测框架内部状态（插件、配置、集群、日志、Lua 服务列表）
- **Script 层 (Lua)**：attach 到运行中服务的 Lua VM，执行任意 Lua 代码（类似 IPython）

传输层使用 Unix Domain Socket（Windows 下降级为 TCP loopback），行协议（换行分隔 JSON）。

## 架构

```
┌─────────────────────────────────────────────────────┐
│  CLI Client (replxx)                                │
│  shield-console [--sock path]                       │
└──────────────┬──────────────────────────────────────┘
               │ Unix Domain Socket (newline-delimited JSON)
               ▼
┌─────────────────────────────────────────────────────┐
│  ConsoleServer (boost::asio::local::stream_protocol)│
│  ┌─────────────┐  ┌─────────────┐                   │
│  │ConsoleSession│  │ConsoleSession│  ...             │
│  └──────┬──────┘  └──────┬──────┘                   │
│         └────────┬───────┘                          │
│           CommandDispatcher                          │
│     ┌────────────┼────────────┐                     │
│     ▼            ▼            ▼                     │
│  RootCommands  LuaCommands  HelpHandler             │
│  (C++ queries) (Lua REPL)   (help)                  │
└─────────────────────────────────────────────────────┘
```

## 配置

在 `config.yaml` 中启用：

```yaml
console:
  enabled: true
  socket_path: /tmp/shield-console.sock   # Unix socket 路径
```

启动后，使用 `shield-console` 连接：

```bash
shield-console --sock /tmp/shield-console.sock
```

## 命令参考

### 命令模式

| 命令 | 说明 |
|------|------|
| `help` | 列出所有可用命令 |
| `root.status` | 服务/插件/集群概览 |
| `root.services` | 列出所有 Lua 服务 |
| `root.service <name>` | 服务详情 |
| `root.plugins` | 插件包/实例/绑定列表 |
| `root.plugin <id>` | 插件详情（含 last_error） |
| `root.config [key]` | 配置查询（无参数=全部 JSON） |
| `root.cluster` | 集群节点状态 |
| `root.log.level [level]` | 读取/设置日志级别（debug/info/warn/error） |
| `attach <service>` | 进入服务的 Lua VM 交互式 REPL |
| `eval <code>` | 在独立 Lua 沙箱中执行一次性代码 |
| `exit` / `quit` | 断开连接 |

### Lua REPL 模式（attach 后）

```
shield> attach gameserver1
lua:gameserver1> return shield.self()
"gameserver1"
lua:gameserver1> local x = 10
lua:gameserver1> return x * 2
20
lua:gameserver1> for i=1,3 do print(i) end
1
2
3
lua:gameserver1> if x > 5 then
...   print("yes")
... end
yes
lua:gameserver1> detach
```

| 命令 | 说明 |
|------|------|
| `detach` | 退出 Lua REPL，回到命令模式 |
| `exit` | 同 detach |
| 任意 Lua 代码 | 在服务的 VM 中执行，支持多行语句 |

**特性**：
- 多行输入：输入未完成的语句（如 `if ... then`）会自动等待下一行，提示符变为 `...`
- 状态持久：`local` 变量在会话期间保持
- 超时保护：单次执行 5 秒超时，防止死循环阻塞 worker

## 响应协议

每行一个 JSON 对象，`type` 字段标识类型：

| type | 说明 | 示例 |
|------|------|------|
| `result` | 命令执行结果 | `{"type":"result","data":42}` |
| `error` | 错误 | `{"type":"error","message":"Service not found"}` |
| `attached` | 已 attach 到服务 | `{"type":"attached","service":"gameserver1"}` |
| `detached` | 已 detach | `{"type":"detached"}` |
| `continue` | 多行输入，等待更多行 | `{"type":"continue"}` |
| `output` | Lua print() 输出 | `{"type":"output","text":"hello"}` |

## 线程安全模型

- **线程安全子系统**（PluginHost、Config、ClusterManager、Logger）：console 线程直接调用
- **Service actor 子系统**：通过 CAF/service actor 调度投递到对应 runtime owner，`promise/future` 回传结果
- actor 调度延迟通常可忽略

## 实现文件

### 传输层
- `include/shield/net/console_session.hpp` / `src/net/console_session.cpp`
- `include/shield/net/console_server.hpp` / `src/net/console_server.cpp`

### 命令层
- `include/shield/console/command_dispatcher.hpp` / `src/console/command_dispatcher.cpp`
- `include/shield/console/root_commands.hpp` / `src/console/root_commands.cpp`
- `include/shield/console/lua_commands.hpp` / `src/console/lua_commands.cpp`

### CLI 客户端
- `tools/shield-console/main.cpp`

### 测试
- `tests/net/test_console_server.cpp`

### 依赖修改
- `include/shield/lua/lua_service.hpp` — 新增 `exec_lua()` 方法
- `include/shield/lua/lua_runtime.hpp` — 新增 `exec_lua()` 方法
- `src/bootstrap/bootstrap.cpp` — GlobalState 添加 console_server，生命周期管理
- `vcpkg.json` — 添加 `replxx` 依赖
