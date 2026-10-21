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

因此任何 `shield_ops` 下的 Lua inspect / snapshot / eval 能力，都必须回到目标 service 的 Lua VM owner 线程执行；不能从 admin 线程直接读取或操作该 VM。相关管理面设计见 [Lua 诊断控制台设计](ops-lua-console.md)。

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

name 所有权转移用 **`shield.claim(name)`**（原子接管，见
[Lua API 契约](lua-api.md#shieldclaimname)）。注意不能用
`unregister + register` 拼——register 只把 **当前** service 注册为 owner，
旧 service 无法替新 service 注册名字，且先注销再注册会让 name 出现解析
空窗。claim 由**新 service 在自己的 handler 里调用**，registry 锁内原子
换 owner，全程无空窗：

```lua
-- 1. 编排方 spawn 新代码（临时名字，spawn 时自动注册）
local new_handle, err = shield.spawn("player", {
    name = "player.1.new",
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
    shield.send(new_handle, "shutdown")  -- 业务自清理
    return
end

-- 3. 新 service 原子接管生产 name（在它自己的 migrate handler 里：
--    function M.migrate(ctx, state)
--        ... 导入状态 ...
--        return shield.claim("player.1")
--    end）
--    claim 成功即生效：后续按 name 的 send/call 全部落到新 service。
--    旧 service 收到 name 变更通知（或由编排方显式通知）进入 drain。

-- 4. Old service draining
--    actor 模型天然串行：旧 service 把在途消息处理完即可退出；
--    drain 期间不再接新业务（业务侧用 M.draining 挡新增请求）。
M.draining = true

-- 5. Old service exit（旧 service 自己的 handler 里）
shield.exit("upgraded")   -- 退出清理只收回它仍拥有的名字，"player.1"
                          -- 已归新 service，不受影响
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
