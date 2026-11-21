// LANG-AUTO-001：浏览器语言自动跳转的纯判定逻辑。
// 与浏览器环境解耦（env 注入），便于在无 jsdom 时直接对判定表做断言。
// 站点仅两种 locale：英文 root（docs/）与简体中文（docs/zh/），见 .vitepress/config.ts。

export const BASE = '/shield/'
export const CHOICE_KEY = 'shield-docs-locale'

export interface LocaleEnv {
  /** window.location.pathname，例如 /shield/ 或 /shield/zh/quickstart */
  pathname: string
  /** navigator.language，例如 zh-CN / en-US / zh-Hant */
  language: string
  /** 用户手动选择过的语言（'en' | 'zh'）；未选择过为 null。
    任何非 null 值（含空串）都视为已选择，宁可不跳也不覆盖用户的显式决定。 */
  choice: string | null
}

/** 路径是否落在简体中文 locale 下 */
export function isZhPath(pathname: string, base: string = BASE): boolean {
  if (!pathname.startsWith(base)) return false
  const rest = pathname.slice(base.length)
  return rest === 'zh' || rest.startsWith('zh/')
}

/**
 * 路径是否为英文 root locale 的首页。
 * cleanUrls 下首页就是 /shield/；index / index.html 是同页的另一写法，
 * 一并视为首页（中文浏览器落在这些写法上同样应看到中文版）。
 */
export function isRootHome(pathname: string, base: string = BASE): boolean {
  if (!pathname.startsWith(base)) return false
  const rest = pathname.slice(base.length)
  return rest === '' || rest === '/' || rest === 'index' || rest === 'index.html'
}

/** 是否应把中文浏览器从英文首页带到中文首页 */
export function shouldRedirect(env: LocaleEnv, base: string = BASE): boolean {
  // 用户手动切换过语言，尊重其选择，不再自动跳转
  if (env.choice !== null) return false
  // 只处理英文首页：深层链接（含搜索直达、书签）意图明确，不做路径跟随
  if (!isRootHome(env.pathname, base)) return false
  // 站点没有繁体版，zh-Hant 同样落到简体中文，与只有两种 locale 的现状一致
  if (!env.language.toLowerCase().startsWith('zh')) return false
  if (isZhPath(env.pathname, base)) return false
  return true
}

/**
 * 重定向目标：中文首页。
 * 不做路径跟随——71 个英文页里有 2 页（如 dashboard-api）尚无中文译版，跟随会 404。
 */
export function redirectTarget(base: string = BASE): string {
  return `${base}zh/`
}

/**
 * 一次点击是否构成「手动切换语言」。
 * 是则返回应记住的 'en' / 'zh'，否则返回 null（同 locale 内的普通导航不算选择）。
 */
export function choiceFromClick(
  href: string | null,
  pathname: string,
  base: string = BASE,
): string | null {
  if (!href || !href.startsWith(base)) return null
  const nextIsZh = isZhPath(href)
  const hereIsZh = isZhPath(pathname)
  if (nextIsZh === hereIsZh) return null
  return nextIsZh ? 'zh' : 'en'
}