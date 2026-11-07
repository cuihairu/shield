---
title: Source Layer Glossary
---

This glossary records the responsibilities found in the source, not the names
of source directories. Each entry points to the implementation that establishes
the concept. Paths are repository-relative and line numbers refer to the state
at commit `e588c0e0a95800596aaefe3309d684944c20b15a`.

| Term | English | Definition | Evidence |
| --- | --- | --- | --- |
| 基础值与标识 | Foundation values and identity | Cross-cutting ID, time, buffer, result, and structured error primitives that are intentionally dependency-free. | `include/shield/base/base.hpp:5`, `include/shield/base/error.hpp:11` |
| 可观测诊断 | Diagnostics and observability | Logs with console or rotating-file sinks plus Crashpad minidump capture and upload placeholder semantics. | `include/shield/log/logger.hpp:14`, `src/crash/crash.cpp:31` |
| 配置总线 | Configuration bus | The process-wide typed key-value configuration store and its per-module validation before optional subsystems consume YAML. | `include/shield/config/config.hpp:14`, `src/config/config.cpp:1111` |
| 进程装配 | Process assembly | The ordered phases and compensation path that initialize configuration, plugins, optional modules, services, and servers. | `src/bootstrap/bootstrap.cpp:402`, `src/bootstrap/bootstrap.cpp:306` |
| 消息契约 | Message contract | The typed CAF payloads for service calls, client ingress/egress, lifecycle control, and cross-process envelopes. | `include/shield/core/service_message.hpp:46`, `include/shield/cluster/cluster_messages.hpp:22` |
| Lua 服务域 | Lua service domain | VM construction, service actors, coroutine mailboxes, calls, timers, profiling, and graceful drain for Lua-first logic. | `src/lua/lua_service.cpp:1`, `src/lua/lua_runtime.cpp:1` |
| 客户端网关 | Client gateway | Accept-time limits and TLS, header-first framing/routing, single-target session binding, and validated client egress. | `include/shield/net/listener.hpp:76`, `src/lua/lua_gateway_bridge.cpp:81` |
| 可插拔能力 | Pluggable capability | Manifest-first scan/plan/load/create/start lifecycle, the versioned C ABI, host API, and JSON-schema configuration. | `src/plugin/plugin_host.cpp:1066`, `include/shield/plugin/abi.h:33` |
| 后端提供者 | Backend provider | Loadable provider packages that own external Redis, SQL, document, metric, health, auth, and codec resources. | `plugins/cache.redis/manifest.yaml:1`, `plugins/mysql/shield_db_mysql.cpp:2011` |
| 全局运行态 | Global runtime state | Cross-service KV/cache, locks, ranks, queues, scheduler, and rate limits, with per-domain memory or Redis backends. | `include/shield/global/global_manager.hpp:1`, `src/global/global_manager.cpp:319` |
| 多节点通信 | Multi-node communication | Peer handshakes, authenticated heartbeats, route snapshots, node discovery, and remote service envelopes. | `include/shield/cluster/cluster_transport.hpp:3`, `include/shield/cluster/node_discovery.hpp:87` |
| 玩家索引 | Player index | The process-local admission, reconnect, device, and service-location index beneath the per-player Lua service. | `include/shield/player/player_manager.hpp:79`, `src/player/player_manager.cpp:102` |
| 服务器状态 | Server state | The legal state machine and watcher notification surface for starting, running, draining, and stopping the process. | `include/shield/server/server_manager.hpp:20`, `src/server/server_manager.cpp:60` |
| 运维入口 | Operations ingress | Authenticated HTTP routes, dashboard polling, line commands, and opt-in Lua eval/profile control paths. | `src/console/ops_http_handler.cpp:805`, `src/console/root_commands.cpp:25` |
| 管理端应用 | Management client | The browser-side ops pages and session-scoped bearer-token polling application. | `console/src/services/api.ts:14`, `console/src/App.tsx:1` |

The Chinese analysis record lives at [源码分层分析](/zh/source-layer-analysis).
