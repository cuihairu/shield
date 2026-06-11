# Lua VM 运行时语义

本文档包含 Shield Lua VM 模型和热更新相关的运行时语义决策。

## Lua VM 模型

推荐模型：每个 service 一个 Lua VM。

规则：

- service 之间不共享 Lua global。
- service 之间只能通过消息通信。
- 本地消息也序列化，不能共享 Lua table 指针。
- 同一 Lua VM 内多 coroutine 协作式调度。
- 同一时间最多一个 OS thread 进入同一 Lua VM。

代价是内存更高，但边界清晰，利于隔离、热重启、IPC/cluster 一致语义。

可优化点：

- 共享只读 Lua bytecode cache。
- 共享 C++ codec 和 module loader。
- 通过配置限制最大 Lua VM 数量。

## Lua 热更新

### 设计原则

当前设计不做原地 live patch。

明确不做：

- 不修改正在运行 Lua VM 的函数表。
- 不迁移任意 Lua closure/upvalue。
- 不承诺旧 coroutine 自动切到新代码。

### Blue-Green Service Replacement

推荐的热更新模型是 blue-green service replacement：

```
┌─────────────────────────────────────────────────────────┐
│  1. Deploy new code                                     │
├─────────────────────────────────────────────────────────┤
│  2. Spawn new service (v2)                              │
│     - Load new code                                     │
│     - Initialize with same config                       │
│     - Pass migration state if needed                    │
├─────────────────────────────────────────────────────────┤
│  3. Switch name binding                                 │
│     - "player.1" now points to v2                       │
│     - New requests go to v2                             │
├─────────────────────────────────────────────────────────┤
│  4. Old service (v1) enter draining                     │
│     - Stop accepting new requests                       │
│     - Finish pending requests                           │
│     - Transfer state to v2 if needed                    │
├─────────────────────────────────────────────────────────┤
│  5. Old service (v1) exit                               │
│     - Cleanup resources                                 │
│     - Release Lua VM                                    │
└─────────────────────────────────────────────────────────┘
```

### 实现 API

```lua
-- 1. Spawn new service
local new_handle, err = shield.spawn("player", {
    name = "player.1.new",  -- 临时名称
    args = {
        migration_data = get_migration_data(),  -- 迁移数据
    },
})

if not new_handle then
    shield.log.error("failed to spawn new service: " .. err.message)
    return
end

-- 2. Transfer state (if needed)
local ok, err = shield.call(new_handle, "migrate", {
    players = M.players,
    state = M.state,
})

if not ok then
    shield.log.error("migration failed: " .. err.message)
    shield.call(new_handle, "shutdown")
    return
end

-- 3. Switch name binding
shield.unregister("player.1")
shield.register("player.1", new_handle)

-- 4. Old service draining
M.draining = true
wait_for_pending_requests()  -- 等待待处理请求完成

-- 5. Old service exit
shield.exit("upgraded")
```

### 状态迁移

**无状态服务：**
- 直接替换，无需迁移
- 适用于：gateway、room（状态在外部存储）

**有状态服务：**
- 需要迁移状态到新服务
- 适用于：player（内存中有状态）

迁移方式：

| 方式 | 说明 | 适用场景 |
|------|------|----------|
| 全量迁移 | 一次性传递所有状态 | 状态量小 |
| 增量迁移 | 逐步同步变更 | 状态量大 |
| 外部存储 | 状态存 Redis/DB | 高可用要求 |

### 配置热更新

部分配置支持热更新，无需重启服务：

```yaml
# 支持热更新的配置
log.level: true           # 立即生效
ops.metrics: true         # 立即生效

# 需要重启服务的配置
actors[].script: false    # 需要重启
database.host: false      # 需要重启连接池
```

### 限制

热更新的限制：

| 限制 | 说明 |
|------|------|
| 不能修改正在运行的代码 | 只能替换整个服务 |
| 不能迁移正在执行的 coroutine | 必须等待完成或超时 |
| 有状态服务需要迁移逻辑 | 业务层实现 |
| 名称切换有短暂不可用 | 通常 < 1ms |

### 与滚动更新的区别

| 维度 | 热更新 | 滚动更新 |
|------|--------|----------|
| 停机 | 无 | 有（重启） |
| 状态 | 可保持 | 丢失 |
| 复杂度 | 高 | 低 |
| 适用场景 | 高可用 | 无状态服务 |

推荐：
- 无状态服务使用滚动更新（更简单）
- 有状态服务使用热更新（保持状态）
