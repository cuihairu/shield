# Lua VM Runtime Semantics

This document contains the runtime semantics decisions for the Shield Lua VM model and hot updates.

## Lua VM Model

Recommended model: one Lua VM per service.

Rules:

- Lua globals are not shared between services.
- Services communicate only through messages.
- Even local messages are serialized; Lua table pointers are never shared.
- Multiple coroutines within one Lua VM are scheduled cooperatively.
- At most one OS thread enters the same Lua VM at any time.

Therefore any `shield_ops` Lua inspect / snapshot / eval capability must execute on the target service's Lua VM owner thread; it must never read or manipulate that VM directly from an admin thread. See the Lua Diagnostics Console Design for the related management-plane design.

The cost is higher memory usage, but the boundaries are clean, which helps isolation, hot restarts, and consistent IPC/cluster semantics.

Optimization opportunities:

- Share a read-only Lua bytecode cache.
- Share the C++ codec and module loader.
- Limit the maximum number of Lua VMs via configuration.

## Lua Hot Updates

### Design Principles

The current design does no in-place live patching.

Explicitly not done:

- No modifying the function tables of running Lua VMs.
- No migrating arbitrary Lua closures/upvalues.
- No promise that old coroutines automatically switch to new code.

### Blue-Green Service Replacement

The recommended hot-update model is blue-green service replacement:

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

### Implementation API

Name ownership transfer uses **`shield.claim(name)`** (atomic takeover, see
the Lua API Contract). Note that assembling it from
`unregister + register` does not work — register only registers the **current**
service as owner; the old service cannot register the name on behalf of the new one,
and unregister-then-register leaves a resolution gap for the name. claim is called by
the **new service inside its own handler**; within the registry lock the owner is swapped
atomically with no gap at any point:

```lua
-- 1. The orchestrator spawns the new code (temporary name, auto-registered on spawn)
local new_handle, err = shield.spawn("player", {
    name = "player.1.new",
    args = {
        migration_data = get_migration_data(),  -- migration data
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
    shield.send(new_handle, "shutdown")  -- business self-cleanup
    return
end

-- 3. The new service atomically takes over the production name (inside its own migrate handler:
--    function M.migrate(ctx, state)
--        ... import state ...
--        return shield.claim("player.1")
--    end)
--    claim takes effect immediately: all subsequent by-name send/call land on the new service.
--    The old service receives the name-change notification (or is notified explicitly by the orchestrator) and enters drain.

-- 4. Old service draining
--    The actor model is naturally serial: the old service finishes in-flight messages and can exit;
--    during drain it takes no new business (business code gates new requests with M.draining).
M.draining = true

-- 5. Old service exit (inside the old service's own handler)
shield.exit("upgraded")   -- exit cleanup only reclaims names it still owns; "player.1"
                          -- already belongs to the new service and is unaffected
```

### State Migration

**Stateless services:**
- Replace directly, no migration needed
- Applies to: gateway, room (state kept in external storage)

**Stateful services:**
- State must be migrated to the new service
- Applies to: player (state held in memory)

Migration approaches:

| Approach | Description | Applicable scenarios |
|------|------|----------|
| Full migration | Transfer all state at once | small state volume |
| Incremental migration | Sync changes step by step | large state volume |
| External storage | Keep state in Redis/DB | high availability requirements |

### Config Hot Updates

Some configuration supports hot reload without restarting the service:

```yaml
# config supporting hot reload
log.level: true           # takes effect immediately
ops.metrics: true         # takes effect immediately

# config requiring a service restart
actors[].script: false    # restart required
database.host: false      # connection pool restart required
```

### Limitations

Limitations of hot updates:

| Limitation | Description |
|------|------|
| Cannot modify running code | Only whole-service replacement is possible |
| Cannot migrate in-flight coroutines | Must wait for completion or time out |
| Stateful services need migration logic | Implemented at the business layer |
| Name switch has brief unavailability | Usually < 1 ms |

### Versus Rolling Updates

| Dimension | Hot update | Rolling update |
|------|--------|----------|
| Downtime | none | yes (restart) |
| State | preservable | lost |
| Complexity | high | low |
| Applicable scenarios | high availability | stateless services |

Recommendations:
- Use rolling updates for stateless services (simpler)
- Use hot updates for stateful services (state preserved)
