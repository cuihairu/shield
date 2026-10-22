# Official Optional Module Acceptance Matrix

This document defines the **acceptance matrix** of the official optional modules. It is not an implementation plan nor a collection of examples; it is the module-level contract that follow-up coding must satisfy.

Goals:

- prevent optional modules from polluting `shield_core` in reverse
- freeze the module enable/disable boundaries
- freeze the public surface, configuration ownership, and key failure semantics

`examples/hello_world/` does not replace the tests in this document.

## General Acceptance Items

| ID | Scenario | Operation | Expectation |
| --- | --- | --- | --- |
| OMOD-000-01 | no optional module enabled | start the minimal runtime | the core path works normally |
| OMOD-000-02 | an optional section exists in config but the module is unregistered or disabled | bootstrap | startup fails; the error includes the module name and config path |
| OMOD-000-03 | an optional module fails to initialize | start the runtime | the error is attributed to the corresponding module; core error semantics are not rewritten |
| OMOD-000-04 | an optional module is disabled | call the minimal API | `shield.send/call/query`, gateway, and plugin namespace data capabilities keep their existing semantics |
| OMOD-000-05 | public header / Lua API check | build/binding tests | optional modules leak no CAF or old-architecture API to users |
| OMOD-000-06 | an optional module explicitly enabled with invalid config | bootstrap | fail fast by default; only degrade scenarios declared in the module's documentation may continue starting and expose unhealthy |

## OMOD-CL `shield_cluster`

| ID | Scenario | Operation | Expectation |
| --- | --- | --- | --- |
| OMOD-CL-001 | cluster not enabled | `shield.query("local")` | still queries only the local registry |
| OMOD-CL-002 | cluster not enabled | local `shield.send/call` | semantics identical to core |
| OMOD-CL-003 | duplicate `NodeId` | two nodes handshake | the new connection is rejected |
| OMOD-CL-004 | remote name resolution | `shield.cluster.query(node, name)` | returns a remote `ServiceHandle` or a clear error |
| OMOD-CL-005 | remote handle send | `shield.send(remote_handle, ...)` | reuses local envelope / timeout semantics |
| OMOD-CL-006 | remote handle call timeout | `shield.call(remote_handle, ...)` | returns `false, timeout` |
| OMOD-CL-007 | node offline | a pending call exists | returns `false, node_offline` |
| OMOD-CL-008 | node recovers after offline | a route cache exists | stale routes are cleaned and re-resolved |
| OMOD-CL-009 | cluster disabled | remote discovery interface | returns `module_unavailable` or an equally clear error; silent success is not allowed |
| OMOD-CL-010 | discovery switch | static / redis / k8s | affects only node discovery; local service registry rules unchanged |
| OMOD-CL-011 | remote connection fails but local config is valid | start cluster | may degrade to single node; local services keep running; cluster state is unhealthy |

## OMOD-GL `shield_global`

| ID | Scenario | Operation | Expectation |
| --- | --- | --- | --- |
| OMOD-GL-001 | global not enabled | call the global namespace | returns `module_unavailable` or a module-unavailable error |
| OMOD-GL-002 | cache/queue/leaderboard plugins enabled, global not enabled | call the corresponding plugin namespace | still fully usable |
| OMOD-GL-003 | distributed lock | acquire/release | completed per the module contract; no underlying Redis handle exposed |
| OMOD-GL-004 | Redis unavailable | distributed lock / rank / queue | returns Redis-related errors; core semantics are not written back |
| OMOD-GL-005 | rank reset | reset period reached | affects only rank data; core scheduler semantics untouched |
| OMOD-GL-006 | queue persistence | service restart | queues are restored or dropped per module configuration |
| OMOD-GL-007 | rate limiter | over-quota requests | returns a module-level rate-limit result; `shield.call` success/failure semantics unpolluted |
| OMOD-GL-008 | global config missing | start the module | attributed to a module configuration error |
| OMOD-GL-009 | negative results like lock contention / empty queue | acquire / pop | returned as module contract results, never disguised as runtime transport errors |

## OMOD-PL `shield_player`

| ID | Scenario | Operation | Expectation |
| --- | --- | --- | --- |
| OMOD-PL-001 | player not enabled | gateway callbacks | `on_client_bound/on_disconnect/on_client_unbound` still work independently |
| OMOD-PL-002 | player enabled | login success | `on_auth -> on_login` order is stable |
| OMOD-PL-003 | client business message | compiled binding `handler(ctx, client, request)` | only request / player state passed; no legacy `msg_type` entry |
| OMOD-PL-004 | cross-service collaboration | gateway -> player_manager | only `session_id`/`ClientRef` tokens passed; no bare connection handles |
| OMOD-PL-005 | disconnect/reconnect success | reconnect within the window | `on_reconnect` fires; PlayerSession restored |
| OMOD-PL-006 | reconnect window timeout | reconnect fails | `on_logout(reason=timeout)` fires |
| OMOD-PL-007 | offline message cache | player returns after being offline | messages delivered and cleared per module policy |
| OMOD-PL-008 | multi-device policy | second login | behavior matches the `single/multi/kick_old` configuration |
| OMOD-PL-009 | PlayerManager index | login/logout | uid/state indexes updated in sync |
| OMOD-PL-010 | module disabled | ordinary service method dispatch | unaffected by player hooks |
| OMOD-PL-011 | auth rejected | `on_auth` returns reject | expressed as a player contract result, never disguised as `handler_error` |

## OMOD-SV `shield_server`

| ID | Scenario | Operation | Expectation |
| --- | --- | --- | --- |
| OMOD-SV-001 | server module not enabled | start the runtime | bootstrap / core do not depend on `server_manager`; `shield.server.*` returns `module_unavailable` |
| OMOD-SV-002 | server enabled | init complete | state automatically enters `running` (`mark_ready`); uptime/started_at count from that point |
| OMOD-SV-003 | maintenance switch | `server:set_state("maintenance")` | transition succeeds and watchers are notified; the admission gate is implemented at the business layer; existing services keep running |
| OMOD-SV-004 | shutdown request | `server:shutdown(ms)` | immediately enters `shutdown` and notifies watchers; after ms the stop request takes the same path as SIGINT; the drain budget still belongs to bootstrap |
| OMOD-SV-005 | state watcher | `server:watch(fn)` registered | state changes deliver `fn(ctx, new_state)` on the watcher's actor thread |
| OMOD-SV-006 | config missing/invalid | start the module | missing uses defaults; invalid (empty name / info over 64 chars) fails fast, attributed to a `server_manager` configuration error |
| OMOD-SV-007 | illegal state transition | `running -> starting` etc. | returns `invalid_state_transition`; no runtime crash, state unchanged |
| OMOD-SV-008 | server module uncompiled | call any `shield.server.*` entry | returns `nil + {code="module_unavailable"}`, retryable=false |
| OMOD-SV-009 | duplicate watch / unknown unwatch | the same service `watch`es again; `unwatch` of an unregistered id | duplicate watch returns the same id; unknown unwatch succeeds silently |
| OMOD-SV-010 | shutdown illegal argument | `shutdown(-1)` / `shutdown(1.5)` / `shutdown("x")` | returns `invalid_argument`; no state migration, no stop triggered |
| OMOD-SV-011 | services keep running under maintenance | set maintenance then `send/call` | service messaging, gateway, and plugin namespace semantics unchanged |
| OMOD-SV-012 | a watcher service exits | migrate state | stale watchers are unregistered automatically; other watchers and the state machine unaffected |
| OMOD-SV-013 | invalid config fails fast | start with an over-long `server_manager.info.name` | bootstrap initialization fails; the error is located at the `server_manager` config path |
| OMOD-SV-014 | read-only observation exposure | `root.server` / `GET /ops/status` | prints `{state, uptime_seconds, version, node_id, started_at_ms, name, info, watchers}`; when uncompiled, `root.server` prints a clear notice and the response has no server block |

### OMOD-SV Test Mapping

- OMOD-SV-001 → `tests/lua_api/test_lua_api_server.cpp` stub branch + the `shield_config_server_requires_module` negative CLI case
- OMOD-SV-002/007 → `tests/server/test_server_manager.cpp` ServerTransitions/ServerLifecycle/ServerSetState
- OMOD-SV-003/011 → ServerSetState + `tests/lua_api/test_lua_api_server.cpp` (messaging semantics unchanged after transition)
- OMOD-SV-004/010 → ServerShutdownScheduling + the Shutdown suite (immediate / duplicate / illegal argument)
- OMOD-SV-005/009/012 → ServerWatch + the Watch suite (idempotency / auto-unregister / unwatch)
- OMOD-SV-006/013 → ServerConfigParsing + `InitializeFailsOnInvalidServerConfig`
- OMOD-SV-008 → stub-branch assertions across all entries
- OMOD-SV-014 → console/ops suite assertions on root.server and the /ops/status server block

## OMOD-OPS `shield_ops`

| ID | Scenario | Operation | Expectation |
| --- | --- | --- | --- |
| OMOD-OP-001 | ops not enabled | access `/ops/*` | no endpoints exposed |
| OMOD-OP-002 | ops enabled | `GET /ops/status` | read-only display of the runtime snapshot |
| OMOD-OP-003 | sensitive fields | `/ops/config` / diagnostics | tokens, passwords, and keys are redacted |
| OMOD-OP-004 | profile/console default off | start with a production config | high-risk entries are not opened automatically |
| OMOD-OP-005 | auth missing | access controlled endpoints | requests are rejected |
| OMOD-OP-006 | internal module failure | metrics/exporter errors | core operation and the business message path unaffected |
| OMOD-OP-007 | service details | `/ops/services/:name` | exposes no CAF actor handle or full business payload |
| OMOD-OP-008 | management plane errors | illegal profile/control requests | errors stay on the HTTP/admin plane; they do not flow back as business Lua runtime errors |
| OMOD-OP-009 | ops management port binding fails | start ops | fail fast, so users aren't misled into thinking the observation entry is enabled |

## Relationship to Existing Topic Documents

- Architecture and ownership: see the Official Optional Module Contracts
- cluster details: see Cluster Runtime Semantics
- global details: see Global Capabilities Runtime Semantics
- player details: see Player Lifecycle Runtime Semantics
- server details: see [Server State Runtime Semantics](runtime-server.md)
- ops details: see Ops Runtime Semantics
