---
home: true
title: Shield
titleTemplate: false
heroText: Shield
tagline: Skynet 启发的、Actor 模型的、Lua 优先的单节点优先游戏服务器运行时
actions:
  - text: 架构设计
    link: /architecture.html
    type: primary
  - text: 运行时语义
    link: /runtime-semantics.html
    type: secondary
  - text: API 说明
    link: /lua-api.html
    type: secondary
features:
  - title: 单节点优先
    details: 最小部署路径聚焦单进程/单节点游戏服务；集群能力通过官方可选模块显式扩展。
  - title: Lua 优先
    details: 游戏逻辑通过 Lua 服务编写，C++ 只承载运行时基础设施。
  - title: Skynet 风格语义
    details: 目标 API 聚焦 service、send、call、timer、spawn、exit 等核心能力。
  - title: CAF 内部传输
    details: CAF 是实现细节，不暴露给 Lua 业务代码和用户侧 C++ 扩展。
  - title: 小核心
    details: shield_core 只封装服务、消息、timer 和 coroutine 语义，其他能力由官方模块组合。
  - title: 设计阶段
    details: 文档描述重构目标；旧实现中的 discovery、metrics、plugin、DI 等不再视为 core。
  - title: API 契约优先
    details: Lua API 以独立契约和测试矩阵为准，示例只作为用户参考。
  - title: 运维分层
    details: shield_ops 独立承载 metrics、health、console、profile，与 shield_core 解耦。
footer: Apache License 2.0 | Refactor design stage
---
