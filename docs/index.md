---
layout: home
title: Shield
titleTemplate: false
hero:
  name: Shield
  tagline: Skynet 启发的 Lua-first 游戏服务器运行时
  actions:
    - text: 快速上手
      link: /quickstart
      theme: primary
    - text: 文档地图
      link: /documentation-map
      theme: secondary
    - text: 插件参考
      link: /plugins/
      theme: secondary
features:
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="12" r="3.8"/><circle cx="12" cy="12" r="8.6" stroke-dasharray="3.2 3.4"/></svg>'
    title: 单节点优先
    details: 最小运行路径聚焦单进程/单节点游戏服务，多节点能力通过官方可选模块显式扩展。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M17.5 14.5a6.5 6.5 0 1 1-8-8 5.2 5.2 0 0 0 8 8z"/><path d="m14.5 7.5.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z"/></svg>'
    title: Lua-first
    details: 游戏逻辑以 Lua service 编写，C++ 承载运行时基础设施、网络、配置和插件边界。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="12" r="3"/><path d="M12 4.5v3"/><path d="M12 16.5v3"/><path d="M4.5 12h3"/><path d="M16.5 12h3"/><path d="m6.7 6.7 2.2 2.2"/><path d="m15.1 15.1 2.2 2.2"/></svg>'
    title: 小核心
    details: core 聚焦 service、message、timer 和 coroutine 语义；插件、运维、集群和玩家系统不反向进入 core。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3.5v5"/><path d="M15 3.5v5"/><path d="M6 8.5h12v3.2a6 6 0 0 1-12 0z"/><path d="M12 17.7v3"/></svg>'
    title: 插件系统 v1
    details: 后端能力通过 manifest-first 插件包提供，使用稳定 C ABI、显式实例、显式 binding 和依赖注入。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M14 3.5H7.5a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h9a2 2 0 0 0 2-2V8z"/><path d="M14 3.5V8h4.5"/><path d="m8.8 14 2.2 2.2 4.2-4.4"/></svg>'
    title: 契约优先
    details: 架构、Lua API、配置、错误码和插件 ABI 以权威文档为准，示例只作为使用参考。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3.5" y="3.5" width="7.5" height="7.5" rx="1.5"/><rect x="13" y="13" width="7.5" height="7.5" rx="1.5"/><path d="M16.75 4.5v5.5"/><path d="M14 7.25h5.5"/><path d="M4.5 16.5h5.5"/></svg>'
    title: 可选模块
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
| 看产品化差距与已补齐项 | [产品化差距评估](product-gap.md) |
| 看架构是否合理 | [架构评估](architecture-review.md) |
| 判断文档优先级 | [文档地图](documentation-map.md) |
| 理解模块边界 | [架构总纲](architecture.md) |
| 编写 Lua service | [Lua API 契约](lua-api.md) |
| 配置运行时 | [配置运行时语义](runtime-config.md) |
| 了解运维与 Lua 诊断控制台设计 | [运维语义](runtime-ops.md) / [Lua 诊断控制台设计](ops-lua-console.md) |
| 使用数据库、缓存、队列等后端 | [插件参考](plugins/index.md) / [DB 使用纪律](db-discipline.md) |
| 开发第三方插件 | [插件系统 v1](plugin-system.md) |
| 查看当前阶段 | [路线图](roadmap.md) |
| 跟进 `xmldef` 设计草案 | [Xmldef Toolchain Design](xmldef-toolchain-design.md) |

## 项目定位

Shield 面向游戏服务器项目，强调显式 wiring、可审计配置和 Lua 业务迭代。CAF、网络、配置、日志和插件加载属于运行时实现细节；业务侧主要面对 `shield.*` Lua API 和按配置启动的 service。
