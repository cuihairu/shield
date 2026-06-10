import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'Shield',
  description: 'Skynet 启发的 Lua 优先游戏服务器运行时',
  cleanUrls: true,
  themeConfig: {
    logo: '/logo.png',
    nav: [
      {
        text: '快速开始',
        link: '/quickstart',
      },
      {
        text: '架构',
        link: '/architecture',
      },
      {
        text: '文档',
        link: '/configuration',
      },
      {
        text: 'GitHub',
        link: 'https://github.com/cuihairu/shield',
      },
    ],
    sidebar: [
      {
        text: '入门',
        items: [
          { text: '快速开始', link: '/quickstart' },
          { text: '配置指南', link: '/configuration' },
          { text: '部署指南', link: '/deployment' },
          { text: '开发指南', link: '/development-guide' },
        ],
      },
      {
        text: '架构',
        items: [
          { text: '架构设计', link: '/architecture' },
          { text: '核心设计理念', link: '/architecture-core-concepts' },
          { text: '运行时语义决策', link: '/runtime-semantics' },
          { text: 'Skynet 对比', link: '/skynet-comparison' },
          { text: 'CAF 映射', link: '/caf-mapping' },
        ],
      },
      {
        text: '运行时语义',
        items: [
          { text: '服务语义', link: '/runtime-service' },
          { text: '消息语义', link: '/runtime-messaging' },
          { text: '玩家生命周期', link: '/runtime-player' },
          { text: '定时器语义', link: '/runtime-timer' },
          { text: 'Lua VM 语义', link: '/runtime-lua-vm' },
          { text: '网络语义', link: '/runtime-network' },
          { text: '数据语义', link: '/runtime-data' },
          { text: '日志语义', link: '/runtime-log' },
          { text: '配置语义', link: '/runtime-config' },
          { text: '启动流程', link: '/runtime-bootstrap' },
          { text: '运维语义', link: '/runtime-ops' },
          { text: '安全语义', link: '/runtime-security' },
          { text: '集群语义', link: '/runtime-cluster' },
        ],
      },
      {
        text: '运行时专题',
        items: [
          { text: '网关设计', link: '/gateway' },
          { text: '运维与调试', link: '/ops' },
          { text: '服务发现', link: '/service_discovery' },
          { text: '可观测性', link: '/monitoring' },
          { text: 'Schema Protocol', link: '/schema-protocol' },
          { text: 'Schema Types', link: '/schema-types' },
          { text: 'Schema Descriptor', link: '/schema-descriptor' },
          { text: 'Schema Mapper', link: '/schema-mapper' },
          { text: 'Schema Implementation', link: '/schema-implementation' },
        ],
      },
      {
        text: '补充',
        items: [
          { text: '游戏后端教程', link: '/tutorial-game-backend' },
          { text: 'API 说明', link: '/api' },
          { text: '常见问题', link: '/faq' },
          { text: '路线图', link: '/roadmap' },
          { text: '贡献指南', link: '/contributing' },
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
