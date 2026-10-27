# 按组件需求使用触屏

目标设备为 640×480 横向画布。当前产品 CST826 电容触控已接入 I²C，
但生成与验收仍读取实际设备的 `touchInput.ready=true` 和
`widgetPointer="p4-pointer-v1"`，不由屏幕型号猜测就绪。

## 应用界面的点击

- 需要 `surface.engine="p4-app-surface-v1"` 与设备对应能力；精细图标按钮另需
  `widgetAppGraphics="p4-app-graphics-v1"`，布局约束见 [app-surface.md](app-surface.md)。
- 给 surface 节点声明 `action`，引用 runtime transition 及 buttons 中同一个逻辑动作。
  按钮、卡片或小图片可以成为点击区域；不为每个触屏位置新增物理事件绑定。
  点击与实体按键必须进入同一个状态机/PC 服务，不能只在预览中改画面。
- 命中矩形为节点的 x/y/w/h，当前页及通用页生效，重叠时后绘制的有动作节点优先。
  全屏静态 JPEG 背景不允许绑定 action。纯展示文字、底栏键帽默认不绑定动作。
- 单指按下后抬起才执行一次；持续不足 700 ms，横纵位移分别不超过 14 px。
  拖出阈值、CANCEL、组件/页/状态切换、资源更新都取消本次点击。
  这不是长按、滚动、拖拽布置、缩放或多点触控接口；需要这些行为先扩展契约与全部解释器。
- 按钮建议至少 44×44 px；保留间距与明确的反馈，不把一个很小的图标当唯一命中区。
  布置类可用“选择对象—选择位置—确认”的点击流程，不宣称尚未实现的自由拖动。

## 按需选用，保留硬件操作

主/副动作、卡片选择、暂停继续适合点击；展示型组件不必填满触屏按钮。
核心功能同时保留 SW1/SW2 或摇杆路径，无触控或触控暂未就绪时仍可用。
依赖触控才能完成的组件则必须阻止安装到不支持的设备并说明原因。
全局退出和实时语音仍优先；组件不得接管它们或绘制假退出按钮。

连续划线游戏使用独立受控核心，详见 [touch-slice.md](touch-slice.md)，
不能将它的 DOWN/MOVE/UP 处理当作应用包可以运行任意脚本的入口。
`screen.region.tap/long_press` 的历史事件名不等于已提供任意区域或手势 DSL。

## 验收

分别检查真实动作后的变化、重复点击、边界/重叠、拖动取消、切页取消、
按键与触屏结果一致，以及保存后退出重进。PC 模拟不能替代设备方向、偏移、
手感与快速输入验证。录音、播放、派发任务只由相应明确操作触发，默认授权不自动执行。
# Media and instrument touch (firmware 0.7.91)

A media surface may declare `input: "p4-media-touch-v1"` only when the device
advertises `widgetMediaTouch: "p4-media-touch-v1"`. A visible node can carry one
declared action from `media.toggle`, `media.previous`, `media.next`, `media.queue`,
`media.up`, `media.down`, or `media.select`. Hit testing uses the current media
page; changing component/page/state invalidates an in-progress gesture. Keep
targets at least 44 × 44 logical pixels and do not place invisible actions over
unrelated content. This does not grant arbitrary HTTP, audio, filesystem, or
script execution: the existing bounded media service still owns playback.

Instrument widgets use the same native tap recognizer to dispatch their
declared `instrument.strike`; a drag or long hold is not another strike.
Physical-key and touch paths must converge on the same real action.

