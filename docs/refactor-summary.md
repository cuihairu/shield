# Shield 重构实施总结（历史快照）

> 状态：历史材料，非当前权威口径。
>
> 本文记录的是一次中间重构尝试的实施笔记，不能作为“当前源码已经完成重构”的证明。当前权威口径以 [最终架构总纲](architecture.md)、[Lua API 契约](lua-api.md)、[运行时语义决策稿](runtime-semantics.md)、[配置运行时语义](runtime-config.md)、[官方可选模块契约](optional-modules.md) 和 [重构路线图](roadmap.md) 为准。
>
> 若本文中的“已完成”“✓”“编译验证”等表述与权威契约或当前源码冲突，以权威契约和当前源码为准。

## 完成日期：2026/06/12

## 历史记录中的已完成模块

### 1. shield_base - 基础类型模块 ✓
**位置**: `src/shield_base/`, `include/shield/base/`

**包含内容**:
- `error.hpp` - Error 类型（code, message, retryable, detail）
- `result.hpp` - `Result<T>` 类型（无异常错误处理）
- `byte_buffer.hpp` - ByteBuffer（二进制数据）
- `time.hpp` - 时间类型（Duration, TimePoint, now()）
- `id.hpp` - ID 类型（ServiceId, TraceId, NodeId）

**依赖**: 仅标准库

### 2. shield_core - 纯净核心模块 ✓
**位置**: `src/core/`, `include/shield/core/`

**包含内容**:
- `service_handle.hpp` - ServiceHandle（隐藏 caf::actor）
- `service_registry.hpp` - ServiceRegistry（本地命名服务）
- `message.hpp` - MessageEnvelope, MessageResponse
- `caf_adapter.hpp` - CafAdapter（CAF 桥接层）

**依赖**: shield_base, CAF::core, CAF::io

**禁止依赖**: Lua, Asio/Beast, DB/Redis, yaml-cpp, log implementation

### 3. shield_log - 日志模块 ✓
**位置**: `src/log/`, `include/shield/log/`

**职责**:
- 提供运行时日志 facade 和 sink。
- 作为 `shield_config`、`shield_data`、`shield_net`、`shield_transport` 等模块的基础设施依赖。

**禁止事项**: 不反向依赖 `shield_core`、Lua、网络或数据访问。

### 4. shield_config - 配置模块 ✓
**位置**: `src/config/`, `include/shield/config/`

**职责**:
- YAML 配置加载。
- 运行时配置查询。
- 只承载 core 最小运行路径需要的配置；cluster/global/ops 配置由各自可选模块拥有。

### 5. shield_data - 数据访问模块 ✓
**位置**: `src/data/`, `include/shield/data/`

**职责**:
- 提供 raw DB/Redis access。
- 不提供 ORM、mapper、跨服务事务或业务缓存策略。

### 6. shield_transport - 传输适配模块 ✓
**位置**: `src/transport/`, `include/shield/transport/`

**职责**:
- frame、codec、encryption 等字节流适配。
- 不拥有连接生命周期。

### 7. shield_net - 网络模块 ✓
**位置**: `src/net/`, `include/shield/net/`

**职责**:
- listener、session、connection 管理。
- 依赖 `shield_transport` 处理字节流，不直接承载业务消息语义。

### 8. shield_lua - Lua 模块 ✓
**位置**: `src/lua/`, `include/shield/lua/`

**包含内容**:
- `lua_runtime.hpp` - LuaRuntime, LuaVM
- `lua_service.hpp` - LuaServiceManager, SpawnResult
- `lua_api.hpp` - API 注册接口

**依赖**: shield_base, shield_core, shield_log, shield_config, shield_data, shield_net, Lua

### 9. shield_bootstrap - 启动模块 ✓
**位置**: `src/bootstrap/`, `include/shield/bootstrap/`

**职责**:
- 组合选定 runtime modules。
- 提供 `shield::bootstrap::initialize` 和 `shield::bootstrap::run`。

### 10. CMake 重构 ✓
**位置**: `CMakeLists.txt`

**模块依赖图**:
```
shield_base (无外部依赖)
    ↓
shield_log → shield_base
shield_config → shield_base, shield_log
    ↓
shield_core → shield_base, CAF
shield_data → shield_base, shield_log, shield_config, DB/Redis
shield_transport → shield_base, shield_log, OpenSSL
    ↓
shield_net → shield_base, shield_log, shield_config, shield_transport, Asio/Beast
    ↓
shield_lua → shield_base, shield_core, shield_log, shield_config, shield_data, shield_net, Lua
    ↓
shield_bootstrap → 以上所有模块
    ↓
shield (可执行文件)

optional:
shield_cluster / shield_global / shield_ops 默认 OFF
```

### 11. Lua API 设计 ✓
**位置**: `examples/new_api_example.lua`

**新 API 语法**:
```lua
-- Service module 返回 table
local M = {}

function M.on_init(args)
    M.name = args.name
    shield.log.info(M.name .. " started")
end

function M.ping(value)
    local src = shield.sender()
    shield.send(src, "pong", value)
    return "pong:" .. value
end

function M.on_exit(reason)
    shield.log.info(M.name .. " exiting: " .. reason)
end

return M
```

## 新旧架构对比

| 方面 | 旧架构 | 新架构 |
|------|--------|--------|
| 模块划分 | 混乱的 shield_core | 清晰的 8 个独立模块 |
| CAF 暴露 | 公开暴露 caf::actor | 隐藏在 CafAdapter 后 |
| Lua API | shield.service("name") | 返回 table 的 module |
| 消息入口 | on_message(msg) | 普通函数 method |
| 集群耦合 | actor_registry 依赖 discovery | 单节点优先，`shield_cluster` 作为官方可选模块 |
| 依赖关系 | 复杂循环依赖 | 清晰单向依赖 |

## 单节点与多节点边界

“单节点优先”不是“只能单节点”。当前主路径先保证本地 service、message、timer、Lua VM、net/data/config/log/bootstrap 稳定；多进程/多机器能力由 `shield_cluster` 承接。

`shield_cluster` 的边界：
- 负责节点身份、节点心跳、远端路由 cache 和可选服务发现。
- 复用 `ServiceHandle`、`send/call`、timeout 和错误语义。
- 不改变本地 `ServiceRegistry`，不把 global registry 或 discovery 塞进 `shield_core`。
- 默认不启用；通过 `SHIELD_ENABLE_CLUSTER` 和独立模块显式进入构建/启动路径。

## 待迁移内容

以下旧模块需要删除或迁移：
- [ ] src/di/ - 删除
- [ ] src/annotations/ - 删除
- [ ] src/conditions/ - 删除
- [ ] src/core/plugin_* - 删除
- [ ] src/metrics/ - 移至 shield_ops（可选）
- [ ] src/health/ - 移至 shield_ops（可选）
- [ ] src/discovery/ - 移至 shield_cluster（可选）

## 下一步

1. **修复实现与文档偏差** - 清理旧 `_new` include、修正命名空间和明显编译错误。
2. **补齐依赖检查** - 确保 `shield_core` 不暴露 Lua/sol2、Asio/Beast、DB/Redis、yaml-cpp 或 log 实现。
3. **删除或迁移旧模块** - DI、annotations、conditions、plugin、metrics、health、discovery。
4. **更新示例和文档** - 示例只描述当前已实现或明确标注为目标 API。
5. **添加单元测试** - 覆盖模块边界、Lua API、net/session、transport frame、config 读取。

## 编译验证

运行以下命令验证重构：
```bash
cmake -B build -S .
cmake --build build
```

检查脚本：`cmake/CheckDeps.cmake`
