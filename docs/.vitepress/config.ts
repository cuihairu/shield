import { defineConfig } from 'vitepress'

export default defineConfig({
  base: '/shield/',
  lang: 'zh-CN',
  title: 'Shield',
  description: 'Skynet 启发的 Lua 优先游戏服务器运行时',
  cleanUrls: true,
  vite: {
    build: {
      // esbuild 0.25+ 对旧 target 的解构语法转换过于保守，目标浏览器其实都支持
      target: 'esnext',
    },
    optimizeDeps: {
      esbuildOptions: {
        target: 'esnext',
      },
    },
  },
  themeConfig: {
    logo: '/shield-logo.svg',
    nav: [
      {
        text: '快速开始',
        link: '/quickstart',
      },
      {
        text: '文档地图',
        link: '/documentation-map',
      },
      {
        text: '架构',
        link: '/architecture',
      },
      {
        text: '插件',
        link: '/plugins/',
      },
    ],
    sidebar: [
      {
        text: '入门与状态',
        items: [
          { text: '文档地图', link: '/documentation-map' },
          { text: '快速开始', link: '/quickstart' },
          { text: '路线图', link: '/roadmap' },
          { text: '开放决策', link: '/open-decisions' },
          { text: '常见问题', link: '/faq' },
          { text: '部署指南', link: '/deployment' },
          { text: '开发指南', link: '/development-guide' },
        ],
      },
      {
        text: '权威契约',
        items: [
          { text: '架构总纲', link: '/architecture' },
          { text: '核心设计理念', link: '/architecture-core-concepts' },
          { text: 'Lua API 契约', link: '/lua-api' },
          { text: '配置语义', link: '/runtime-config' },
          { text: '错误码参考', link: '/runtime-errors' },
          { text: '插件系统 v1', link: '/plugin-system' },
          { text: '运行时语义决策', link: '/runtime-semantics' },
          { text: '官方可选模块契约', link: '/optional-modules' },
          { text: 'Lua API 测试用例', link: '/lua-api-tests' },
          { text: '官方可选模块验收矩阵', link: '/optional-module-tests' },
        ],
      },
      {
        text: '插件参考',
        items: [
          { text: '插件清单与第三方开发指南', link: '/plugins/' },
          { text: 'database.sqlite', link: '/plugins/database-sqlite' },
          { text: 'database.mysql', link: '/plugins/database-mysql' },
          { text: 'database.postgresql', link: '/plugins/database-postgresql' },
          { text: 'database.mongodb', link: '/plugins/database-mongodb' },
          { text: 'cache.redis', link: '/plugins/cache-redis' },
          { text: 'queue.redis', link: '/plugins/queue-redis' },
          { text: 'leaderboard.redis', link: '/plugins/leaderboard-redis' },
          { text: 'auth.jwt', link: '/plugins/auth-jwt' },
          { text: 'metrics.prometheus', link: '/plugins/metrics-prometheus' },
          { text: 'health.http', link: '/plugins/health-http' },
          { text: 'matchmaking.elo', link: '/plugins/matchmaking-elo' },
        ],
      },
      {
        text: '核心运行时语义',
        items: [
          { text: '服务语义', link: '/runtime-service' },
          { text: '消息语义', link: '/runtime-messaging' },
          { text: '定时器语义', link: '/runtime-timer' },
          { text: 'Lua VM 语义', link: '/runtime-lua-vm' },
          { text: '网络语义', link: '/runtime-network' },
          { text: '数据语义', link: '/runtime-data' },
          { text: '日志语义', link: '/runtime-log' },
          { text: '启动流程', link: '/runtime-bootstrap' },
          { text: '安全语义', link: '/runtime-security' },
          { text: '网关设计', link: '/gateway' },
          { text: 'Starter 系统', link: '/starter-system' },
        ],
      },
      {
        text: '官方可选模块',
        items: [
          { text: '集群语义', link: '/runtime-cluster' },
          { text: '全局数据', link: '/runtime-global' },
          { text: '玩家生命周期', link: '/runtime-player' },
          { text: '服务器状态', link: '/runtime-server' },
          { text: '运维语义', link: '/runtime-ops' },
          { text: '运维与调试', link: '/ops' },
          { text: '服务发现', link: '/service_discovery' },
          { text: '可观测性', link: '/monitoring' },
        ],
      },
      {
        text: '工程参考',
        items: [
          { text: '配置指南', link: '/configuration' },
          { text: '目标目录结构', link: '/directory-structure' },
          { text: 'CMake 重构', link: '/cmake-refactor' },
          { text: '开发指南', link: '/development-guide' },
          { text: 'Skynet 对比', link: '/skynet-comparison' },
          { text: 'CAF 映射', link: '/caf-mapping' },
          { text: '最佳实践', link: '/best-practices' },
          { text: '性能设计', link: '/performance' },
          { text: 'UDP 支持', link: '/udp-protocol-support' },
          { text: 'Universal Serialization', link: '/universal-serialization' },
          { text: 'API 说明', link: '/api' },
          { text: '游戏后端教程', link: '/tutorial-game-backend' },
        ],
      },
      {
        text: '草案与归档',
        items: [
          { text: '实体与组件草案', link: '/runtime-entity' },
          { text: '游戏状态持久化与回档', link: '/runtime-persistence' },
          { text: 'Universal Serialization', link: '/universal-serialization' },
          { text: 'Schema Protocol', link: '/schema-protocol' },
          { text: 'Schema Types', link: '/schema-types' },
          { text: 'Schema Descriptor', link: '/schema-descriptor' },
          { text: 'Schema Mapper', link: '/schema-mapper' },
          { text: 'Schema Implementation', link: '/schema-implementation' },
        ],
      },
    ],
    outline: 'deep',
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
})
