---
layout: home
title: Shield
titleTemplate: false
hero:
  name: Shield
  tagline: Skynet-inspired Lua-first game server runtime
  actions:
    - text: Quick Start
      link: /quickstart
      theme: primary
    - text: Documentation Map
      link: /documentation-map
      theme: secondary
features:
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="12" r="3.8"/><circle cx="12" cy="12" r="8.6" stroke-dasharray="3.2 3.4"/></svg>'
    title: Single node first
    details: The minimal run path focuses on single-process/single-node game services; multi-node capability is added explicitly through official optional modules.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M17.5 14.5a6.5 6.5 0 1 1-8-8 5.2 5.2 0 0 0 8 8z"/><path d="m14.5 7.5.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z"/></svg>'
    title: Lua-first
    details: Game logic is written as Lua services; C++ carries the runtime infrastructure, networking, configuration, and plugin boundaries.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="12" r="3"/><path d="M12 4.5v3"/><path d="M12 16.5v3"/><path d="M4.5 12h3"/><path d="M16.5 12h3"/><path d="m6.7 6.7 2.2 2.2"/><path d="m15.1 15.1 2.2 2.2"/></svg>'
    title: Small core
    details: core focuses on service, message, timer, and coroutine semantics; plugins, ops, cluster, and the player system never enter core in reverse.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3.5v5"/><path d="M15 3.5v5"/><path d="M6 8.5h12v3.2a6 6 0 0 1-12 0z"/><path d="M12 17.7v3"/></svg>'
    title: Plugin System v1
    details: Backend capabilities are provided through manifest-first plugin packages with a stable C ABI, explicit instances, explicit bindings, and dependency injection.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M14 3.5H7.5a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2h9a2 2 0 0 0 2-2V8z"/><path d="M14 3.5V8h4.5"/><path d="m8.8 14 2.2 2.2 4.2-4.4"/></svg>'
    title: Contract first
    details: Architecture, Lua API, configuration, error codes, and the plugin ABI are governed by authoritative documents; examples are usage references only.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3.5" y="3.5" width="7.5" height="7.5" rx="1.5"/><rect x="13" y="13" width="7.5" height="7.5" rx="1.5"/><path d="M16.75 4.5v5.5"/><path d="M14 7.25h5.5"/><path d="M4.5 16.5h5.5"/></svg>'
    title: Optional modules
    details: Optional modules such as cluster, global, player, server, and ops default to OFF build switches and are not part of the default minimal run path.
footer: Apache License 2.0
---

## Current Conventions

Shield's current documentation enters through the [Documentation Map](documentation-map.md). Readers should first distinguish authoritative contracts, runtime semantics, reference documents, and draft archives before diving into a specific topic.

The current implementation path centers on single-node Lua services, configuration validation, the network gateway, Plugin System v1, and official plugins. The official optional modules P0 have landed (see the Roadmap); old directions such as DI/IoC, annotations, events, the middleware chain, and old plugin v0 are no longer compatibility targets.

## Quick Entries

| Goal | Reading entry |
| --- | --- |
| Start the project for the first time | [Quick Start](quickstart.md) |
| See productization gaps and what has been closed | [Productization Gap Assessment](product-gap.md) |
| Judge whether the architecture is sound | [Architecture Review](architecture-review.md) |
| Determine document precedence | [Documentation Map](documentation-map.md) |
| Understand module boundaries | Architecture Overview |
| Write a Lua service | Lua API Contract |
| Configure the runtime | Runtime Config Semantics |
| Learn ops and the Lua diagnostics console design | Ops Semantics / Lua Diagnostics Console Design |
| Use databases, caches, queues and other backends | Plugin Reference / [DB Usage Discipline](db-discipline.md) |
| Develop third-party plugins | Plugin System v1 |
| See the current phase | Roadmap |
| Follow the `xmldef` design drafts | Xmldef Toolchain Design |

## Project Positioning

Shield targets game server projects, emphasizing explicit wiring, auditable configuration, and Lua business iteration. CAF, networking, configuration, logging, and plugin loading are runtime implementation details; the business side mainly faces the `shield.*` Lua API and services started per configuration.
