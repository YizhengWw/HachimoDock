# 有界本地页面导航

开发中能力 `widgetAppNavigation=p4-app-navigation-v1`。必须读取实际设备能力，不能
因为页面字段通过离线校验就把新包发给旧固件。

本地应用在 runtime 声明 `navigation:"p4-app-navigation-v1"`，使用 2–4 个既有 pages（quota v2 为 2–6，quota v3 为 2–12）、
`surface.engine=p4-app-surface-v1` 和 `p4-input-v1`。暂不与 host/local/media/scene/game/data/
instrument profile 混用；这不是已经补齐的宿主表单或任意 App 路由系统。
唯一的独立实验例外是 [variable-owners.md](variable-owners.md) 的显式宿主字段隔离，
需额外能力协商；没有该声明时仍拒绝 host 混用，其他排他 profile 不变。

transition 可增加 `from_page:"已声明页面"`。省略时任意页；它与 from 状态条件同时满足，
不匹配的动作不修改变量、不切状态、不触发确认后的实际操作。

page 的新对象形式只能包含一项：`{push:"详情页"}` 保存当前页并进入目标；
`{back:true}` 返回最近父页；`{replace:"目标页"}` 替换当前页、保留父链；
`{reset:"首页"}` 清空父链并进入目标。历史最多 16 层（契约 app-navigation-v1 revision 2，maxDepth=16），满栈 push 和根页 back 整条
transition 禁用，不执行附带 set/inc/to。重复进入同一页是合法的有界历史记录。

旧字符串 page/next/prev 的行为保持兼容；在显式导航包中执行它们会清空历史。
返回键与实体输入、触屏共享动作可用性，不增加包内系统退出动作。
历史仅保存在当前运行实例 RAM，不覆盖原有 persist 存档；组件重载/设备重启后从
原有恢复页开始新历史。页面内变量、滚动 offset 不因返回隐式重置，也不自动快照每页
滚动位置；需要该行为时显式建模，不能假称完整导航状态已经持久化。

共享向量 `contracts/app-navigation-v1.json` 覆盖多级详情、替换、返回、错误页禁用、
根页和栈满无副作用。C/Python/JS 使用相同操作，Rust/Python/C 校验和能力门禁都须覆盖。
设备导航栈核心位于 `pet_p4_navigation_core.h`，PC 与本地工具通过固定产品 WASM
执行同一份 C 的 allowed/apply，而不是执行用户代码；每次调用装入完整页面和历史快照，
不把另一个组件的历史保存在共享实例中。旧字符串路由仍保留，Python 是独立契约模型，
不能据此宣称所有运行时都已同源。非法历史不自动清空，操作失败不修改既有状态。
真实设备页面/按键/重连与性能另外验收；结构通过不等于实机通过。

revision 2 将历史深度从 8 放宽至 16；C 固定数组、WASM bridge、JS 和 Python 同步修改。能力字符串尚未区分 revision，旧固件仍只有 8 层，不能据此宣称旧设备支持 16 层。需统一新固件构建及 HIL 后发布。
