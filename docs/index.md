---
home: true
title: Shield
titleTemplate: false
heroText: Shield
tagline: Skynet 启发的 Lua-first 游戏服务器运行时
actions:
  - text: 快速上手
    link: /quickstart
    type: primary
  - text: 文档地图
    link: /documentation-map
    type: secondary
  - text: 插件参考
    link: /plugins/
    type: secondary
features:
  - title: 单节点优先
    details: 最小运行路径聚焦单进程/单节点游戏服务，多节点能力通过官方可选模块显式扩展。
  - title: Lua-first
    details: 游戏逻辑以 Lua service 编写，C++ 承载运行时基础设施、网络、配置和插件边界。
  - title: 小核心
    details: core 聚焦 service、message、timer 和 coroutine 语义；插件、运维、集群和玩家系统不反向进入 core。
  - title: 插件系统 v1
    details: 后端能力通过 manifest-first 插件包提供，使用稳定 C ABI、显式实例、显式 binding 和依赖注入。
  - title: 契约优先
    details: 架构、Lua API、配置、错误码和插件 ABI 以权威文档为准，示例只作为使用参考。
  - title: 可选模块
    details: cluster、global、player、server、ops 等能力按路线图推进，不属于默认最小运行路径。
footer: Apache License 2.0
---

## 当前口径

Shield 当前文档以 [文档地图](documentation-map.md) 为入口。读者应先区分权威契约、运行时语义、参考文档和草案归档，再进入具体专题。

当前实现路径以单节点 Lua service、配置验证、网络 gateway、插件系统 v1 和官方插件为主。官方可选模块按 [路线图](roadmap.md) 推进；旧 DI/IoC、annotations、events、middleware chain、旧插件 v0 等方向不再作为兼容目标。

## 快速入口

| 目标 | 阅读入口 |
| --- | --- |
| 第一次启动项目 | [快速上手](quickstart.md) |
| 判断文档优先级 | [文档地图](documentation-map.md) |
| 理解模块边界 | [架构总纲](architecture.md) |
| 编写 Lua service | [Lua API 契约](lua-api.md) |
| 配置运行时 | [配置运行时语义](runtime-config.md) |
| 使用数据库、缓存、队列等后端 | [插件参考](plugins/index.md) |
| 开发第三方插件 | [插件系统 v1](plugin-system.md) |
| 查看当前阶段 | [路线图](roadmap.md) |
| 跟进 `xmldef` 设计草案 | [Xmldef Toolchain Design](xmldef-toolchain-design.md) |

## 项目定位

Shield 面向游戏服务器项目，强调显式 wiring、可审计配置和 Lua 业务迭代。CAF、网络、配置、日志和插件加载属于运行时实现细节；业务侧主要面对 `shield.*` Lua API 和按配置启动的 service。
