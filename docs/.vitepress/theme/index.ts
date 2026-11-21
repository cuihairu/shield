// shield docs theme — 默认主题 + 品牌配色（custom.css 见同目录）+ 浏览器语言自动跳转。
// VitePress 会自动发现本文件，无需在 config 里引用。
import DefaultTheme from 'vitepress/theme'
import { inBrowser } from 'vitepress'
import './custom.css'
import {
  BASE,
  CHOICE_KEY,
  choiceFromClick,
  redirectTarget,
  shouldRedirect,
} from './locale-redirect'

// 语言切换器是内置的普通 <a>（英文侧 href=/shield/zh/，中文侧 href=/shield/），
// 按「跨 locale 的链接点击」识别手动选择，不依赖 VitePress 内部类名，主题升级不会静默失效。
function rememberManualSwitch(): void {
  document.addEventListener(
    'click',
    (e: MouseEvent) => {
      const anchor = (e.target as HTMLElement | null)?.closest?.('a[href]') as
        | HTMLElement
        | null
      const choice = choiceFromClick(
        anchor?.getAttribute('href'),
        window.location.pathname,
      )
      if (!choice) return
      try {
        localStorage.setItem(CHOICE_KEY, choice)
      } catch {
        // 隐私模式等存储不可用时忽略：下次访问重新按浏览器语言判断
      }
    },
    true,
  )
}

function readChoice(): string | null {
  try {
    return localStorage.getItem(CHOICE_KEY)
  } catch {
    return null
  }
}

function redirectChineseVisitors(): void {
  if (
    shouldRedirect({
      pathname: window.location.pathname,
      language: navigator.language,
      choice: readChoice(),
    })
  ) {
    window.location.replace(redirectTarget())
  }
}

export default {
  extends: DefaultTheme,
  // enhanceApp 在客户端挂载前调用：重定向发生在渲染前，不会先闪英文页。
  // 本版本（1.6.4）主题接口只有 enhanceApp，没有 onClientSetup/onClientBeforeMount。
  enhanceApp() {
    if (!inBrowser) return
    redirectChineseVisitors()
    rememberManualSwitch()
  },
}