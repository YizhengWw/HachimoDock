# Pet-Claw 视觉风格规范（UI Style规范）

> 版本：V1  
> 依据页面风格参考：深色科技风（以 `Inter` 为主字体，强调高对比、浅灰白文案与蓝/红状态色）

## 1. 目标

为 `ref/src` 内前端原型建立一套统一、可复用、可扩展的视觉约束，避免硬编码散落。

- 统一颜色与透明度基线，便于品牌调整。
- 统一字号/间距/圆角规范，降低样式分歧。
- 对状态色、状态层级、交互反馈统一命名。
- 通过 CSS 变量接管全局样式，业务样式只消费语义变量，不再直接写散点数值。

## 2. 全局 Token 定义（单一真值）

以下变量需定义在 `:root`，并作为唯一源：

```css
:root {
  --font-inter: "Inter", "Inter Fallback";
  --font-mono: ui-monospace, Menlo, Monaco, "Cascadia Mono", "Segoe UI Mono", "Roboto Mono", "Oxygen Mono", "Ubuntu Monospace", "Source Code Pro", "Fira Mono", "Droid Sans Mono", "Courier New", monospace;
  --font-advercase: "advercase", "advercase Fallback";
  --max-width: 1100px;
  --border-radius: 12px;

  --color-transparent: transparent;
  --color-white: white;
  --color-black: black;
  --color-gray-dark: #222222;
  --color-gray-medium: #363636;
  --color-gray-light: #dadada;
  --color-gray-lighter: #ccc;

  --color-primary-blue: #2688f9;
  --color-secondary-blue: #3a86ff;
  --color-tertiary-blue: #3291ff;
  --color-accent-blue: rgb(59, 130, 246);
  --color-brand-blue: rgb(37, 99, 235);
  --color-success-green: green;
  --color-error-red: red;
  --color-warning-red: #f55c47;
  --color-warning-yellow: yellow;
  --color-info-blue: blue;
  --color-accent-coral: rgb(248, 113, 113);
  --color-accent-purple: rgba(107, 173, 255, 1);

  --color-billing-past-due-text: rgba(223, 99, 78, 1);
  --color-billing-past-due-bg: rgba(223, 99, 78, 0.18);
  --color-billing-past-due-border: rgba(223, 99, 78, 0.2);
  --color-billing-past-due-hover-bg: rgba(223, 99, 78, 0.12);
  --color-billing-modal-bg: rgba(167, 52, 32, 0.2);

  --color-white-90: rgba(255,255,255,0.9);
  --color-white-85: rgba(255,255,255,0.85);
  --color-white-80: rgba(255,255,255,0.8);
  --color-white-70: rgba(255,255,255,0.7);
  --color-white-50: rgba(255,255,255,0.5);
  --color-white-40: rgba(255,255,255,0.4);
  --color-white-30: rgba(255,255,255,0.3);
  --color-white-20: rgba(255,255,255,0.2);
  --color-white-15: rgba(255,255,255,0.15);
  --color-white-10: rgba(255,255,255,0.1);
  --color-white-08: rgba(255,255,255,0.08);
  --color-white-07: rgba(255,255,255,0.07);
  --color-white-06: rgba(255,255,255,0.06);
  --color-white-05: rgba(255,255,255,0.05);
  --color-white-04: rgba(255,255,255,0.04);
  --color-white-03: rgba(255,255,255,0.03);
  --color-white-full: rgba(255,255,255,1);
  --color-white-none: rgba(255,255,255,0);

  --color-black-90: rgba(0,0,0,0.9);
  --color-black-45: rgba(0,0,0,0.45);
  --color-black-35: rgba(0,0,0,0.35);
  --color-black-25: rgba(0,0,0,0.25);
  --color-black-24: rgba(0,0,0,0.24);
  --color-black-20: rgba(0,0,0,0.2);
  --color-black-15: rgba(0,0,0,0.15);
  --color-black-987: rgba(0,0,0,0.987);
  --color-black-951: rgba(0,0,0,0.951);
  --color-black-896: rgba(0,0,0,0.896);
  --color-black-825: rgba(0,0,0,0.825);
  --color-black-741: rgba(0,0,0,0.741);
  --color-black-648: rgba(0,0,0,0.648);
  --color-black-55: rgba(0,0,0,0.55);
  --color-black-352: rgba(0,0,0,0.352);
  --color-black-259: rgba(0,0,0,0.259);
  --color-black-175: rgba(0,0,0,0.175);
  --color-black-104: rgba(0,0,0,0.104);
  --color-black-049: rgba(0,0,0,0.049);
  --color-black-013: rgba(0,0,0,0.013);
  --color-black-none: rgba(0,0,0,0);

  --color-gray-dark-80: rgba(34,34,34,0.8);
  --color-gray-dark-full: rgba(34,34,34,1);
  --color-gray-dark-none: rgba(34,34,34,0);
  --color-gray-medium-75: rgba(61,61,61,0.75);
  --color-gray-medium-90: rgba(33,33,33,0.9);
  --color-gray-charcoal-95: rgba(48,48,48,0.95);
  --color-gray-charcoal-85: rgba(22,22,22,0.85);
  --color-gray-charcoal-50: rgba(8,8,8,0.5);
  --color-gray-charcoal-90: rgba(8,8,8,0.9);
  --color-gray-charcoal-none: rgba(8,8,8,0);
  --color-gray-medium-40: rgba(80,80,80,0.4);
  --color-gray-dark-hex-55: #22222288;
  --color-gray-dark-hex-65: #222222a6;
  --color-gray-medium-hex-65: #3a3a3aa6;
  --color-gray-medium-dark-65: #4e4e4ea6;
  --color-gray-darker-85: #0e0e0ed9;
  --color-gray-darkest-65: #131313a6;
  --color-gray-black-65: #030303a6;
  --color-white-hex-33: #ffffff55;
  --color-white-hex-13: #ffffff22;
  --color-white-hex-53: #ffffff88;

  --z-detail-view: 2010;
  --z-sidemodel: 2050;
  --z-panel: 2000;
  --z-sidebar: 2000;
  --z-stylemodal: 2100;
  --z-chat-topbar: 2005;
  --z-chat-topbar-left: 2025;
  --z-options-menu: 2020;

  /* 兼容现有样式的语义映射 */
  --bg: var(--color-gray-dark);
  --surface: var(--color-gray-dark-full);
  --surface-muted: var(--color-gray-dark-80);
  --surface-strong: var(--color-white);
  --line: var(--color-white-10);
  --text: var(--color-white);
  --text-soft: var(--color-white-80);
  --text-faint: var(--color-white-60, var(--color-white-50));
  --blue: var(--color-brand-blue);
  --blue-soft: var(--color-white-10);
  --orange: #f59e0b;
  --orange-soft: rgba(245, 158, 11, 0.14);
  --green: var(--color-success-green);
  --green-soft: rgba(34, 197, 94, 0.18);
  --amber: #f59e0b;
  --amber-soft: rgba(245, 158, 11, 0.12);
  --red: var(--color-warning-red);
  --red-soft: rgba(248, 113, 113, 0.18);
  --shadow-sm: 0 8px 28px rgba(0,0,0,0.3);
  --shadow-md: 0 18px 48px rgba(0,0,0,0.42);
  --radius-xl: 24px;
  --radius-lg: 18px;
  --radius-md: 14px;
  --radius-sm: 10px;

  --caret-color: var(--color-white);
  --max-width-page: var(--max-width);
}
```

## 3. 全局基础样式（在 `styles.css` 中落地）

- `*:not(svg, path)`/`* { box-sizing: border-box; }`
- `body`：
  - `background: var(--bg);`
  - `color: var(--text);`
  - `min-height: 100vh;`
  - `max-width: 100vw;`
  - `overflow-x: hidden;`
  - `overscroll-behavior: none;`
  - `touch-action: none;`
- 链路字体：`font-family: var(--font-inter), system-ui, sans-serif;`
- 光标色：`caret-color: var(--caret-color);`

## 4. 组件级规范（首选语义变量）

- `surface` 相关：卡片/面板统一使用 `var(--surface)`、`var(--line)`、`var(--radius-xl)`。
- `status`：
  - 成功：`var(--green)`，背景使用 `var(--green-soft)`。
  - 警告：`var(--color-warning-red)` 或 `var(--color-warning-yellow)`，背景使用 `var(--color-warning-red)` 的 10%~18% 透明版本。
  - 错误：`var(--red)`，背景使用 `var(--red-soft)`。
- 文本：
  - 正文：`var(--text)`；辅助文字：`var(--text-soft)`；弱信息：`var(--text-faint)`。
- 尺寸：
  - 页面 max width：`min(100%, var(--max-width))`。
  - 圆角主值：`var(--border-radius)`，重点卡片可用 `var(--radius-xl)`。
- 交互与动效：
  - 重点按钮使用 12~16px 高亮边角圆角。
  - 状态切换建议 `<180ms` 与淡入/颜色过渡。

## 5. 开发约束

1. 所有新样式必须优先使用 CSS 变量，不允许直接写以下类值：`#2563eb`、`#0f172a`、`#f8fafc` 等项目中已定义语义值除非与产品需求强绑定。
2. 同一语义色在不同模块重复出现时，使用同一 token（例：状态色、背景色、弱边框色、正文/副文）。
3. 任意修改或新增 token 时，先更新本文件中的声明，并同步到 `ref/src/styles.css` 的 `:root`。
4. 组件间留白与布局变化优先从 `gap`、`padding`、`max-width` 与 `radius` 语义变量推进，不直接在每个类里写死。

## 6. 变更落地步骤

1. 先接入 `:root` 令牌（本文第 2 节）。
2. 将 `styles.css` 中现有色值映射到语义变量（已在该文件开始处完成）。
3. 新增组件时按第 4 节用最小差异样式编写（优先 `surface/line/text/status/button` 系列 token）。
4. 新建页面时固定 `max-width: var(--max-width)` 并居中布局。
5. Review 时先对变量使用率做检查：禁止出现低于 5 处重复硬编码值。

## 7. 采纳清单

- 语义 token 缺失：按需补充 `--border-width-*`、`--spacing-*`、`--font-size-*` 后补。
- 页面层级：统一通过 `--z-*` 管理，避免裸数字 `z-index`。
- 多主题扩展：若后续引入浅色皮肤，可通过 `data-theme` 分支变量覆盖，不改动业务类名。
