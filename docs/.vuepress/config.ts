import { defaultTheme } from '@vuepress/theme-default'
import { defineUserConfig } from 'vuepress/cli'
import { viteBundler } from '@vuepress/bundler-vite'
import { path } from '@vuepress/utils'

export default defineUserConfig({
  lang: 'zh-CN',
  title: 'Shield',
  description: 'C++ 游戏服务器框架',

  bundler: viteBundler(),

  theme: defaultTheme({
    logo: '/logo.png',

    navbar: [
      {
        text: '指南',
        link: '/guide/',
      },
      {
        text: 'API',
        link: '/api/',
      },
      {
        text: '参考',
        link: '/reference/',
      },
      {
        text: 'GitHub',
        link: 'https://github.com/cuihairu/shield',
      },
    ],

    sidebar: {
      '/guide/': [
        {
          text: '开始',
          children: [
            '/guide/README.md',
            '/guide/installation.md',
            '/guide/quickstart.md',
          ],
        },
        {
          text: '核心概念',
          children: [
            '/guide/actor.md',
            '/guide/script.md',
            '/guide/network.md',
            '/guide/discovery.md',
          ],
        },
        {
          text: '配置',
          children: [
            '/guide/configuration.md',
            '/guide/deployment.md',
          ],
        },
      ],
      '/api/': [
        {
          text: 'API 参考',
          children: [
            '/api/README.md',
            '/api/core.md',
            '/api/actor.md',
            '/api/script.md',
            '/api/network.md',
          ],
        },
      ],
      '/reference/': [
        {
          text: '参考文档',
          children: [
            '/reference/README.md',
            '/reference/dependencies.md',
            '/reference/faq.md',
          ],
        },
      ],
    },

    // 编辑链接
    editLink: false,
    // 最后更新
    lastUpdated: false,
    // 贡献者
    contributors: false,
  }),
})
