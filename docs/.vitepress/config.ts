import { defineConfig } from 'vitepress'

export default defineConfig({
  base: '/shield/',
  title: 'Shield',
  description: 'Skynet 启发的 Lua 优先游戏服务器运行时',
  cleanUrls: true,
  // Internal audit notes link directly to repository evidence outside the
  // published docs root; keep them in the repo without treating them as pages.
  vite: {
    build: {
      // esbuild 0.25+ 对旧 target 的解构语法转换过于保守，目标浏览器其实都支持
      target: 'esnext',
      chunkSizeWarningLimit: 2048,
    },
    optimizeDeps: {
      esbuildOptions: {
        target: 'esnext',
      },
    },
  },
  // i18n 按 VitePress 官方约定：root locale（英文）在 docs/，中文在 docs/zh/。
  // 英文侧栏只列出 docs/ 下真实存在的页面；尚未产出英文译版的页面不进英文侧栏。
  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      title: 'Shield',
      description: 'Skynet-inspired Lua-first game server runtime',
      themeConfig: {
        nav: [
          { text: 'Quick Start', link: '/quickstart' },
          { text: 'Documentation Map', link: '/documentation-map' },
          { text: 'Architecture Decisions', link: '/architecture-decisions' },
          { text: 'API', link: '/api' },
        ],
        sidebar: [
          {
            text: 'Start & status',
            items: [
              { text: 'Documentation Map', link: '/documentation-map' },
              { text: 'Quick Start', link: '/quickstart' },
              { text: 'Decision Log', link: '/open-decisions' },
              { text: 'FAQ', link: '/faq' },
              { text: 'Deployment', link: '/deployment' },
              { text: 'Crash Reporting', link: '/crash-reporting' },
              { text: 'Development Guide', link: '/development-guide' },
            ],
          },
          {
            text: 'Contracts',
            items: [
              { text: 'Core Design Principles', link: '/architecture-core-concepts' },
              { text: 'Runtime Semantics Decisions', link: '/runtime-semantics' },
              { text: 'Optional Module Acceptance Matrix', link: '/optional-module-tests' },
            ],
          },
          {
            text: 'Plugin reference',
            items: [
              { text: 'queue.redis', link: '/plugins/queue-redis' },
              { text: 'matchmaking.elo', link: '/plugins/matchmaking-elo' },
              { text: 'protocol.json', link: '/plugins/protocol-json' },
              { text: 'protocol.msgpack', link: '/plugins/protocol-msgpack' },
              { text: 'protocol.protobuf', link: '/plugins/protocol-protobuf' },
              { text: 'protocol.flatbuffers', link: '/plugins/protocol-flatbuffers' },
            ],
          },
          {
            text: 'Core runtime semantics',
            items: [
              { text: 'Timer Runtime Semantics', link: '/runtime-timer' },
              { text: 'Lua VM Runtime Semantics', link: '/runtime-lua-vm' },
              { text: 'Network Runtime Semantics', link: '/runtime-network' },
              { text: 'Data Access Architecture', link: '/runtime-data' },
              { text: 'Log Runtime Semantics', link: '/runtime-log' },
              { text: 'Bootstrap Runtime Semantics', link: '/runtime-bootstrap' },
              { text: 'Security Runtime Semantics', link: '/runtime-security' },
              { text: 'Server State Runtime Semantics', link: '/runtime-server' },
              { text: 'Gateway Design', link: '/gateway' },
              { text: 'DB Usage Discipline', link: '/db-discipline' },
              { text: 'Starter System', link: '/starter-system' },
            ],
          },
          {
            text: 'Engineering reference',
            items: [
              { text: 'Configuration Design', link: '/configuration' },
              { text: 'CMake Refactoring (history)', link: '/cmake-refactor' },
              { text: 'Skynet Comparison', link: '/skynet-comparison' },
              { text: 'Best Practices', link: '/best-practices' },
              { text: 'Performance Design', link: '/performance' },
              { text: 'UDP Support', link: '/udp-protocol-support' },
              { text: 'API Overview', link: '/api' },
              { text: 'Game Backend Tutorial', link: '/tutorial-game-backend' },
            ],
          },
          {
            text: 'Drafts & archive',
            items: [
              { text: 'Base Components Boundaries (draft)', link: '/runtime-primitives' },
              { text: 'DB Async ABI (draft)', link: '/db-async-design' },
              { text: 'Xmldef Descriptor Spec', link: '/xmldef-descriptor-spec' },
              { text: 'Xmldef Compiler / Runtime MVP', link: '/xmldef-compiler-runtime-mvp' },
              { text: 'Xmldef Unity Generator Spec', link: '/xmldef-unity-generator-spec' },
            ],
          },
        ],
        outline: 'deep',
      },
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      title: 'Shield',
      description: 'Skynet 启发的 Lua 优先游戏服务器运行时',
      themeConfig: {
        nav: [
          { text: '快速开始', link: '/zh/quickstart' },
          { text: '文档地图', link: '/zh/documentation-map' },
          { text: '架构', link: '/zh/architecture' },
          { text: '插件', link: '/zh/plugins/' },
        ],
        sidebar: [
          {
            text: '入门与状态',
            items: [
              { text: '文档地图', link: '/zh/documentation-map' },
              { text: '快速开始', link: '/zh/quickstart' },
              { text: '路线图', link: '/zh/roadmap' },
              { text: '开放决策', link: '/zh/open-decisions' },
              { text: '常见问题', link: '/zh/faq' },
              { text: '部署指南', link: '/zh/deployment' },
              { text: '崩溃采集', link: '/zh/crash-reporting' },
              { text: '开发指南', link: '/zh/development-guide' },
            ],
          },
          {
            text: '权威契约',
            items: [
              { text: '架构总纲', link: '/zh/architecture' },
              { text: '核心设计理念', link: '/zh/architecture-core-concepts' },
              { text: 'Lua API 契约', link: '/zh/lua-api' },
              { text: '配置语义', link: '/zh/runtime-config' },
              { text: '错误码参考', link: '/zh/runtime-errors' },
              { text: '插件系统 v1', link: '/zh/plugin-system' },
              { text: 'Plugin Pool Stats', link: '/zh/plugin-pool-stats' },
              { text: '运行时语义决策', link: '/zh/runtime-semantics' },
              { text: '官方可选模块契约', link: '/zh/optional-modules' },
              { text: 'Lua API 测试用例', link: '/zh/lua-api-tests' },
              { text: '官方可选模块验收矩阵', link: '/zh/optional-module-tests' },
            ],
          },
          {
            text: '插件参考',
            items: [
              { text: '插件清单与第三方开发指南', link: '/zh/plugins/' },
              { text: 'database.sqlite', link: '/zh/plugins/database-sqlite' },
              { text: 'database.mysql', link: '/zh/plugins/database-mysql' },
              { text: 'database.postgresql', link: '/zh/plugins/database-postgresql' },
              { text: 'database.mongodb', link: '/zh/plugins/database-mongodb' },
              { text: 'cache.redis', link: '/zh/plugins/cache-redis' },
              { text: 'queue.redis', link: '/zh/plugins/queue-redis' },
              { text: 'leaderboard.redis', link: '/zh/plugins/leaderboard-redis' },
              { text: 'metrics.prometheus', link: '/zh/plugins/metrics-prometheus' },
              { text: 'health.http', link: '/zh/plugins/health-http' },
              { text: 'matchmaking.elo', link: '/zh/plugins/matchmaking-elo' },
            ],
          },
          {
            text: '核心运行时语义',
            items: [
              { text: '服务语义', link: '/zh/runtime-service' },
              { text: '消息语义', link: '/zh/runtime-messaging' },
              { text: '定时器语义', link: '/zh/runtime-timer' },
              { text: 'Lua VM 语义', link: '/zh/runtime-lua-vm' },
              { text: '网络语义', link: '/zh/runtime-network' },
              { text: '数据访问架构', link: '/zh/runtime-data' },
              { text: '日志语义', link: '/zh/runtime-log' },
              { text: '启动流程', link: '/zh/runtime-bootstrap' },
              { text: '安全语义', link: '/zh/runtime-security' },
              { text: '网关设计', link: '/zh/gateway' },
              { text: 'DB 使用纪律', link: '/zh/db-discipline' },
              { text: 'Starter 系统', link: '/zh/starter-system' },
            ],
          },
          {
            text: '官方可选模块',
            items: [
              { text: '集群语义', link: '/zh/runtime-cluster' },
              { text: '全局数据', link: '/zh/runtime-global' },
              { text: '玩家生命周期', link: '/zh/runtime-player' },
              { text: '服务器状态', link: '/zh/runtime-server' },
              { text: '运维语义', link: '/zh/runtime-ops' },
            ],
          },
          {
            text: '工程参考',
            items: [
              { text: '配置指南', link: '/zh/configuration' },
              { text: '目标目录结构', link: '/zh/directory-structure' },
              { text: 'CMake 重构（历史）', link: '/zh/cmake-refactor' },
              { text: 'Skynet 对比', link: '/zh/skynet-comparison' },
              { text: '最佳实践', link: '/zh/best-practices' },
              { text: '性能设计', link: '/zh/performance' },
              { text: 'UDP 支持', link: '/zh/udp-protocol-support' },
              { text: 'API 说明', link: '/zh/api' },
              { text: '游戏后端教程', link: '/zh/tutorial-game-backend' },
            ],
          },
          {
            text: '草案与归档',
            items: [
              { text: '基础组件边界（草案）', link: '/zh/runtime-primitives' },
              { text: '实体与组件草案', link: '/zh/runtime-entity' },
              { text: '游戏状态持久化与回档', link: '/zh/runtime-persistence' },
              { text: 'DB 异步 ABI 立项（草案）', link: '/zh/db-async-design' },
              { text: 'Schema 设计（合并稿）', link: '/zh/schema-design' },
              { text: 'Xmldef Toolchain Design', link: '/zh/xmldef-toolchain-design' },
              { text: 'Xmldef Descriptor Spec', link: '/zh/xmldef-descriptor-spec' },
              { text: 'Xmldef Phase 1 Plan', link: '/zh/xmldef-phase1-implementation' },
              { text: 'Xmldef Compiler / Runtime MVP', link: '/zh/xmldef-compiler-runtime-mvp' },
              { text: 'Xmldef Unity Generator Spec', link: '/zh/xmldef-unity-generator-spec' },
            ],
          },
        ],
        outline: 'deep',
      },
    },
  },
  themeConfig: {
    logo: '/shield-logo.svg',
    socialLinks: [
      { icon: 'github', link: 'https://github.com/cuihairu/shield' },
    ],
    search: {
      provider: 'local',
    },
    footer: {
      message: 'Apache License 2.0',
      copyright: 'Copyright © 2024-2026',
    },
  },
  // review/ 是审核底稿（含原始工具记录与未闭合标签），只留档不入站
  srcExclude: ['review/**', 'analysis/**', 'zh/analysis/**'],
})
