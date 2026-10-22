# Shield Config System and Config Update Mechanism Analysis

> Analysis against the real code (2026-10-02, baseline c02a887). Every conclusion carries `file:line`
> evidence; unimplemented capabilities are explicitly marked "does not exist" — no speculation.

## 0. Summary of Conclusions

Shield's configuration model is **one-time assembly at startup**: multi-file YAML deep merge → full
validation → dispatch to each subsystem, after which in-process configuration is **read-only**.

| User question | Current state |
|---|---|
| Where does config come from | disk YAML (`config/app*.yaml`) + CLI overrides (`--config` multi-file in order, `--node-id` injection) |
| How is it distributed / hot-updated | **no hot update**. `reload_config()` is a stub, always returning `true` (src/config/config.cpp:1796) |
| Version management | **no config version number/hash/generation**. `app.version` and `server_manager.info.version` are business self-reported display fields, not used in change comparison |
| How do clients apply it | **not applicable**. The transport/gateway has no config push messages; the game client SDK is by design implemented per engine in separate repos (docs/engine-sdk-design.md:48 "implementation repos split by engine"), and is still at proposal stage overall (same doc :3 "Status: design proposal"), not in this repo (no unity/unreal/cocos/ directories) |
| Failure rollback | startup failure = refuse to start (`return false`, process exits, no partial start); runtime config is immutable, so no rollback needed |

## 1. Module Inventory

| Module | Responsibility | Key code |
|---|---|---|
| shield_config | YAML loading/deep merge/dotted-key storage/validation/actor declaration parsing | src/config/config.cpp, include/shield/config/config.hpp |
| shield_bootstrap | assembly order: load → validate → log/plugins/cluster/player/server → actor dispatch | src/bootstrap/bootstrap.cpp:409-1050 |
| shield_lua (Lua API) | read-only surface `shield.config` / `shield.player.config` / `shield.server.config` | src/lua/lua_api.cpp:1346, 2445, 2934 |
| shield_console / ops | the `root.config` command, `GET /ops/config?key=` | src/console/root_commands.cpp:342, src/console/ops_http_handler.cpp:912 |
| shield_player / shield_server | each snapshots startup parameters once via `from_global_config` | src/player/player_manager.cpp:36, src/server/server_manager.cpp:70 |
| shield_cluster | `parse_cluster_config()` parsed at startup | src/cluster/cluster_manager.cpp:22 |
| shield_plugin | `load_plugin_config()` parses the plugin section from global config | src/plugin/plugin_config.cpp:83 |

## 2. Data Flow (startup, one-time)

```
config/app*.yaml (磁盘)
      │  CLI: --config a.yaml --config b.yaml   （bootstrap.cpp:409-424，后文件覆盖先文件）
      ▼
Config::load_yaml ──► merge_yaml_nodes 递归合并（config.cpp:44-64）
      │                非 map 节点（标量/数组）整体替换
      ▼
flatten_yaml_node → 点分键哈希表（读 shared_lock / 写 unique_lock）
      │  CLI: --node-id → global_config().set("cluster.node_id")   （bootstrap.cpp:427，全仓唯一生产期写）
      ▼
validate_runtime_config（config.cpp:1115-1638）
      │  失败 → SHIELD_LOG_ERROR + return false → 进程退出
      ▼
派发（全部启动时一次读定）：
  ├─ 日志级别/sinks（bootstrap.cpp:431-457）
  ├─ PluginHost::startup（bootstrap.cpp:494-501）
  ├─ ClusterManager（bootstrap.cpp:504-516）
  ├─ PlayerConfig::from_global_config（player_manager.cpp:36）
  ├─ ServerManager config（server_manager.cpp:70）
  ├─ Lua 运行参数（有消费方的键）：sandbox.allow_os/allow_io（lua_runtime.cpp:202-204）、
  │    module_path（lua_runtime.cpp:222）、cache.*（lua_runtime.cpp:268-272）、
  │    script_path（bootstrap.cpp:85-86 脚本解析链）
  ├─ lua.vm.*（mode/max_vms/max_memory_mb）：**无运行期消费方**（见坑 9）
  └─ runtime_actors()（config.cpp:1645）
       ├─ 每个 actor → 服务 spawn：opts = {name, args, config=actor.options_json}
       │    （bootstrap.cpp:975-985 → lua_service.cpp:2224-2227 取 opts["config"] 传给脚本）
       ├─ network.* → 监听器参数（tcp、envelope/codec、限流、blocklist、TLS）
       └─ rpc.routes 跨 actor 合并去重 → RpcDescriptorTable（bootstrap.cpp:910-960，
            id/name 冲突在 spawn 前失败）
```

## 3. Runtime Read Surface (all read-only)

| Surface | Form | Evidence |
|---|---|---|
| C++ anywhere | `shield::config::get/get_int/get_bool` (shared_lock live reads — if a writer ever appears, readers see it immediately) | config.cpp:1802-1818 |
| Lua services | `shield.config(key[, default])`: strings probed and converted as true/false/float/int; missing keys return default or nil | lua_api.cpp:1346-1420 |
| Lua services (module snapshots) | `shield.player.config()`, `shield.server.config()`: subset tables snapshotted at startup | lua_api.cpp:2445, 2934 |
| Service-private config | the spawn opts' `config` key = that actor's `options:` subtree (YAML→JSON) | bootstrap.cpp:979-985, lua_service.cpp:2227 |
| console | `root.config [key]` (no key = dump full JSON) | root_commands.cpp:342-346 |
| ops HTTP | `GET /ops/config?key=...` (**missing key does not dump everything**, only a hint string; the code comment admits simplified) | ops_http_handler.cpp:912-955 |

The only production write path in the whole repo: `bootstrap.cpp:427`'s `cluster.node_id` CLI injection
(`Config::set` under unique_lock). Lua/ops/console expose no setters.

## 4. Per-Topic Answers

### 4.1 Where config comes from
- Files: `config/{app,app-dev,app-prod,app-with-redis,app-with-sqlite}.yaml`.
- CLI multi-files stack in order, **later overrides earlier** (bootstrap.cpp:409-424); loading deep-merges
  into the global singleton, with `reset_config()` clearing then reinstalling (bootstrap.cpp:411).
- Merge semantics (config.cpp:44-64): when both sides are maps, recurse by key; if either side is a non-map
  (including arrays) → **overlay wholesale replacement**.
- Type system: `ConfigValue = variant<string,int64,double,bool,vector<string>>` (config.hpp:16), stored in a
  hash table by dotted keys after flatten; `get_string` converts numbers to strings (config.cpp:918-927);
  `to_json` outputs **flat dotted-key** JSON (nested structures use `subtree_json(config, path)`,
  config.hpp:144).

### 4.2 Distribution / hot update
- **No hot update exists**: `reload_config()` always returns `true`, its comment self-describing "Would
  need to track the original config path" (config.cpp:1796-1799); zero production callers, only the test
  `ReloadConfigReturnsTrue` asserting the stub (tests/coverage/test_cov_config.cpp:1165-1168).
- No file watching, no hot-reload signal — the process signal surface is only SIGINT/SIGTERM (shutdown
  only, src/shield.cpp:51-65); no SIGHUP/SIGUSR handling anywhere in the repo. Proof:
  `grep -rn "SIGHUP\|SIGUSR" src/ --include="*.cpp" --include="*.hpp" --include="*.h"` returns no hits.
  The only current way to change config = edit the file + restart the process.
- No cross-node config sync either: cluster message types are only Hello/HelloAck/Heartbeat/Routes/Envelope
  (include/shield/cluster/cluster_messages.hpp:31-85). RoutesMsg is the service route table syncing with
  heartbeats — runtime service discovery, not config distribution; node timeout determination in
  cluster_manager.cpp:58-75. Proof: `grep -rn "config\|Config" include/shield/cluster/cluster_messages.hpp`
  returns no config-related message types. No config-oriented cluster message exists in the repo.

### 4.3 Version management
- No config content hash, no generation/monotonic version, no change notification. The global config object
  carries no version field.
- Existing "version" fields are all business display:
  - `app.version` (self-reported in yaml, e.g. "0.1.0");
  - `server_manager.info.version` → `ServerManager::version()`, falling back to the compile-time fallback
    when empty, for the server list/ops display (server_manager.cpp:253-258);
  - the node version field of ops `/info`.
- One-time derived structures (actor declarations, the RPC descriptor table) are materialized at startup and
  never related to the YAML again — editing the disk file at run time has zero effect on the process.

### 4.4 How clients apply it
- Not applicable: no client-oriented config messages anywhere in transport/net/session/gateway
  (`EnvelopeConfig` is protocol codec parameters, fixed at startup, src/transport/protocol.cpp:483-517).
- If a business needs to "push config to clients", the current approach is to implement it in services with
  the data plane (`shield.data`/global data) + RPC — that is data sync, not part of the config system.

### 4.5 Failure rollback
- **Startup** (the only phase with failure handling):
  - file unopenable/parse exception → `load_yaml` returns false → bootstrap `return false`
    (bootstrap.cpp:419-424, config.cpp:870-907);
  - validation failure (module sections vs build switches, app.name, log.level, the full actors schema,
    rpc id/name uniqueness, script file existence) → `return false` (bootstrap.cpp:482-486; error strings
    in config.cpp:213-811, 1142-1180);
  - rpc conflicts are rejected **before any actor spawns** (bootstrap.cpp:954-960) — no half-started
    topology can appear.
  - Because failure exits and success is immutable, "rollback" degenerates to "the process never started";
    no compensation logic needed.
- **Runtime**: config is immutable → no rollback needed. The only hazard is `reload_config()`'s fake success
  (see pit 2).

## 5. Pit List (all verified against code)

1. **Multi-file stacking replaces arrays wholesale**: when two configs both have an `actors:` list, it is a
   replacement, not a concat (config.cpp:57-58: non-map → `Clone(overlay)`). "Appending one actor" requires
   copying the whole list. Same for arrays like rate_limit/blocklist.
2. **`reload_config()` fake success**: always true (config.cpp:1796). If it is ever wired to ops/console as
   a hot-update switch, you get "reports success but nothing happened".
3. **`reset_config()` dangling references**: resetting inside a helper destroys the caller-held `Config&`
   (a known test pit; the shield-config-test-pitfall memory).
4. **Lossy type round-trips**: YAML numbers flatten, then `get_string` stringifies, then the Lua side of
   `shield.config` probes strings back to bool/number (lua_api.cpp:1357-1395): large int64s, digit strings
   with leading zeros (like `"01"`), and `e/E` inside ordinary strings all take the float probe path. When
   exact types matter, use `subtree_json`.
5. **`/ops/config` without a key returns no dump**: only the "Use ?key=..." hint (ops_http_handler.cpp:932-935,
   the comment admits simplified); full dumps use console's `root.config`.
6. **`source_dir` is overwritten by the last loaded file**: `impl_->source_dir` is rewritten on every
   `load_yaml` (config.cpp:885-892); actor script relative-path resolution `existing_script_path` follows
   the directory of the **last** config file (config.cpp:762-763, 1593) — easy to trip on with multi-file
   stacking + relative script paths.
7. **Validation happens only at startup**: `Config::set` has no validation path (its only caller is the
   controlled node_id injection); if Lua/ops write entries appear in the future, the validation gap shows
   immediately.
8. **The plugin section bypasses flatten consumption**: `load_plugin_config()` reads the `Config` object
   directly (plugin_config.cpp:83-86), likewise read once at startup; plugin DLLs have their own manifest
   `schema_version` (must be 1, src/plugin/manifest.cpp:77-81) — that is the plugin ABI version, unrelated
   to config versioning.
9. **`lua.vm.*` is validated only at startup**: `lua.vm.mode` is validated at startup only ("must be
   per_service", config.cpp:1189-1205), purely as a startup legality check; `lua.vm.max_vms` /
   `max_memory_mb` are read at the config layer and recorded in YAML but have zero runtime consumers — they
   participate in no VM resource limiting (grep of src/, zero hits). Changing the three keys changes no
   runtime behavior; VM resource caps are currently not config-driven (grep finds zero consumers; no
   implementation-level cap was found). Proof:
   `grep -rn "lua.vm.max_vms\|lua.vm.max_memory_mb" src/ --include="*.cpp" --include="*.hpp" --include="*.h"`
   returns no hits.

## 6. If Evolving (based on the current state, not an implementation recommendation)

Given the current code structure, the minimal surface config hot update needs: `Config` remembering its
source path list (the stub comment already points this out), reload running the two phases "load into a
shadow Config → validate → swap root/atomically replace storage" (readers already have live-read semantics
via shared_lock), and defining a rebuild policy for the one-time derived structures (actors/RPC
tables/listener parameters) — the third costs the most under the current "materialize at startup"
architecture and is the fundamental reason this framework chose immutable configuration.
