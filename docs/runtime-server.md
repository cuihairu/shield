# 服务器状态运行时语义

本文档包含 Shield 服务器级别状态管理的运行时语义决策。

## 设计原则

- ServerManager 是全局单例，管理服务器级别状态。
- 服务器状态与玩家状态分离。
- 提供运维控制能力（维护模式、关闭流程）。

## ServerManager

ServerManager 是全局单例 Service，负责管理服务器级别的状态。

### 职责

| 职责 | 说明 |
|------|------|
| 服务器状态 | 运行、维护、关闭等状态管理 |
| 运行时信息 | 启动时间、版本、节点ID等 |
| 状态变更通知 | 服务器状态变更时通知其他服务 |

### 架构

```
┌─────────────────────────────────────────────────────────┐
│                  ServerManager (单例)                    │
│  ┌────────────────────────────────────────────────────┐ │
│  │  服务器状态                                          │ │
│  │  - state: running / maintenance / shutdown         │ │
│  │  - started_at: 启动时间                             │ │
│  │  - version: 版本号                                  │ │
│  │  - node_id: 节点ID                                  │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│  PlayerManager │ GlobalData │ Ops                        │
└─────────────────────────────────────────────────────────┘
```

### Lua API

```lua
-- 获取 ServerManager
local server = shield.server()

-- 服务器状态
local state = server:state()                -- "running" | "maintenance" | "shutdown"
local uptime = server:uptime()              -- 运行时长（秒）
local version = server:version()            -- 版本号
local node_id = server:node_id()            -- 节点ID
local started_at = server:started_at()      -- 启动时间戳

-- 服务器状态控制
server:set_state("maintenance")             -- 设置状态
server:shutdown(30000)                      -- 30秒后关闭
```

### 状态机

```
┌────────────┐
│  starting  │
└─────┬──────┘
      │ init complete
      ▼
┌────────────┐
│  running   │◄─────────────┐
└─────┬──────┘              │
      │ maintenance         │
      ▼                     │
┌────────────┐              │
│maintenance │──────────────┘
└─────┬──────┘   resume
      │ shutdown
      ▼
┌────────────┐
│  shutdown  │
└────────────┘
```

### 状态说明

| 状态 | 说明 | 行为 |
|------|------|------|
| `starting` | 启动中 | 不接受玩家连接 |
| `running` | 正常运行 | 接受所有请求 |
| `maintenance` | 维护模式 | 不接受新登录，现有玩家可继续 |
| `shutdown` | 关闭中 | 通知所有玩家，准备关闭 |

### 状态变更钩子

```lua
-- 其他服务注册监听服务器状态变更
function M.on_init(args)
    shield.call("server_manager", "watch_state", shield.self())
end

-- 收到状态变更通知
function M.on_server_state_change(new_state)
    if new_state == "shutdown" then
        -- 准备关闭
        M:save_all_data()
    elseif new_state == "maintenance" then
        -- 进入维护模式
        M:notify_clients("maintenance")
    end
end
```

### 配置

```yaml
# ServerManager 配置
server_manager:
  enabled: true
  name: "server_manager"               # 服务名称
  state: "running"                     # 初始状态

  # 服务器信息
  info:
    name: "My Game Server"
    version: "1.0.0"
    region: "us-east"

  # 状态变更钩子
  on_state_change: true                # 是否通知其他服务
```

### 与其他 Manager 的关系

```
┌─────────────────────────────────────────────────────────┐
│                    ServerManager                         │
│  - 服务器状态                                            │
│  - 运行时信息                                            │
├─────────────────────────────────────────────────────────┤
│  PlayerManager          │  shield.global.*               │
│  - 玩家管理              │  - 全局数据、分布式锁          │
│                         │  - 排行榜、队列、限流器        │
├─────────────────────────────────────────────────────────┤
│  Services                                               │
│  - 业务服务                                             │
└─────────────────────────────────────────────────────────┘
```

## ops 暴露

```json
GET /ops/server

{
  "state": "running",
  "uptime": 3600,
  "version": "1.0.0",
  "node_id": "node-1",
  "started_at": "2026-06-10T12:00:00Z"
}
```

## 实现优先级

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 服务器状态管理 | P0 | 维护模式、关闭流程 |
| 运行时信息查询 | P0 | ops 集成 |
| 状态变更通知 | P1 | 服务联动 |
