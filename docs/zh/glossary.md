---
title: 源码分层术语表
---

本表记录源码中实际成立的职责，不按目录名机械分层。每条证据指向确立该概念的实现；路径为仓库相对路径，行号对应提交 `e588c0e0a95800596aaefe3309d684944c20b15a`。

| 术语 | 英文 | 定义 | file:line |
| --- | --- | --- | --- |
| 基础值与标识 | Foundation values and identity | 无外部依赖的 ID、时间、缓冲区、Result 与结构化错误基础类型。 | `include/shield/base/base.hpp:5`, `include/shield/base/error.hpp:11` |
| 可观测诊断 | Diagnostics and observability | 控制台/滚动文件日志 sink，以及 Crashpad minidump 采集和上传留位语义。 | `include/shield/log/logger.hpp:14`, `src/crash/crash.cpp:31` |
| 配置总线 | Configuration bus | 进程级配置键值存储，以及各可选模块消费 YAML 前的分区校验。 | `include/shield/config/config.hpp:14`, `src/config/config.cpp:1111` |
| 进程装配 | Process assembly | 配置、插件、可选模块、服务与监听器的启动阶段、顺序回滚和关停编排。 | `src/bootstrap/bootstrap.cpp:402`, `src/bootstrap/bootstrap.cpp:306` |
| 消息契约 | Message contract | 服务调用、客户端 ingress/egress、生命周期控制与跨节点 envelope 的 CAF 类型契约。 | `include/shield/core/service_message.hpp:46`, `include/shield/cluster/cluster_messages.hpp:22` |
| Lua 服务域 | Lua service domain | Lua VM、服务 actor、协程 mailbox、call/timer、profile 与优雅 drain 的运行域。 | `src/lua/lua_service.cpp:1`, `src/lua/lua_runtime.cpp:1` |
| 客户端网关 | Client gateway | 接入限额与 TLS、帧协议和路由、单目标 session binding，以及验证后的客户端 egress。 | `include/shield/net/listener.hpp:76`, `src/lua/lua_gateway_bridge.cpp:81` |
| 可插拔能力 | Pluggable capability | 以 manifest 先行完成扫描、编排、加载、创建、启动的生命周期和版本化 C ABI。 | `src/plugin/plugin_host.cpp:1066`, `include/shield/plugin/abi.h:33` |
| 后端提供者 | Backend provider | 独立加载的 Redis、SQL、文档库、指标、健康、认证与协议 codec 提供包，连接资源归实例所有。 | `plugins/cache.redis/manifest.yaml:1`, `plugins/mysql/shield_db_mysql.cpp:2011` |
| 全局运行态 | Global runtime state | 跨服务 KV/缓存、锁、排行榜、队列、调度器和限流器，各域可选内存或 Redis 后端。 | `include/shield/global/global_manager.hpp:1`, `src/global/global_manager.cpp:319` |
| 多节点通信 | Multi-node communication | 节点握手、认证 heartbeat、路由快照、发现循环与远端服务 envelope。 | `include/shield/cluster/cluster_transport.hpp:3`, `include/shield/cluster/node_discovery.hpp:87` |
| 玩家索引 | Player index | 每玩家 Lua service 下层的进程内准入、重连、设备和位置索引。 | `include/shield/player/player_manager.hpp:79`, `src/player/player_manager.cpp:102` |
| 服务器状态 | Server state | 进程 starting/running/draining/shutdown 的状态机和 watcher 通知面。 | `include/shield/server/server_manager.hpp:20`, `src/server/server_manager.cpp:60` |
| 运维入口 | Operations ingress | 认证 HTTP 路由、dashboard 轮询、行命令和显式启用的 eval/profile 控制面。 | `src/console/ops_http_handler.cpp:805`, `src/console/root_commands.cpp:25` |
| 管理端应用 | Management client | 浏览器端 ops 页面和按会话保存 bearer token 的轮询应用。 | `console/src/services/api.ts:14`, `console/src/App.tsx:1` |

英文分析记录见 [Source Layer Analysis](/source-layer-analysis)。
