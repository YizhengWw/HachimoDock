# 通用单角色平台运动（p4-motion-v1）

这是资源包驱动的受控连续运动能力，不是按组件 ID 定制的跳跃游戏。
设备须报告 `widgetMotion: "p4-motion-v1"`（0.7.93 起）；旧固件拒绝安装。
PC 预览和设备使用同一 C 核心。升级需要用户授权，不由组件自行执行。
实体 SW1 的 legacy PTT 冲突修复在 0.7.94；0.7.93 虽报告能力，仍有实体蓄力缺陷，
不应仅凭能力字符串宣布实机通过。修复不改变资源格式，已有包无需重制。

## 资源包可复用范围

同类单角色蓄力平台玩法，通过包内 body/platforms、重力、起跳速度、横向速度范围、
蓄力时长、landed/edge 规则、计分和存档变量，以及 surface 贴图/控件配置完成。
改变平台距离、主题、UI、难度或得分规则不需要增加组件专用固件代码。
`charge`、release/cancel 和输入所有权是通用原语；具体动作名由包声明，不固定为 `jump.charge`。
阅读类长内容、随机分类、宠物音色与日历属于 PC 声明式宿主服务，不移入固件或包内脚本。
新增引擎之外的物理行为仍须先扩展通用原语及对应能力协商，不伪装成包参数。

```json
"motion": {
  "engine": "p4-motion-v1", "body": "player", "platforms": ["home", "target"],
  "gravity": 18, "lift": 7, "speed": [2, 13],
  "charge_ms": 1200, "charge_var": "charge"
}
```

放在 `scene` 中。角色和平台引用实际 entities；仅一个运动角色，1–11 个静态平台。
角色初始须 active 且脚底在某平台顶面、中心位于其水平范围内。
速度使用每秒网格单位，位置为 Q8；gravity 4–64、lift 2–16、speed 两整数
1–24 且递增，charge_ms 300–3000。scene.tick_ms 16–40，最长补演 8 步，
更长阻塞时间丢弃，不在恢复时突然补跳。charge_var 为独立整数，值 0–100，
不能复用 score_var 或存档 record_var。

`input.hold` 的规则通过 `{"op":"charge","entity":"player"}` 开始蓄力。
进入组件已经准备好的场景必须沿用到首次蓄力/松开，不能首次开始时再次随机生成。
0.7.97 修正首次 charge 的重复 reset；仅已结束回合的重玩或显式 restart 重新生成轨道。
取消蓄力、重新按住及暂停恢复均不得更换已显示平台；开始时重置时钟，排除进入后等待时间。
重开键可用普通 action + `restart` 规则和 `input.confirm` 配置，不需要专用操作码。
0.7.97 起 motion 的确认弹窗取消未释放的蓄力并冻结运动时钟，取消/超时不补演弹窗时间；
确认重开清本局分数和轨道，`lifecycle.record_var` 的最高分不清除。PC 预览遵循相同规则。
松开由输入核心执行 launch；资源不能自行声明 launch/cancel。
松开后的横向速度在 speed 最小/最大值间按蓄力比例插值，初始竖直速度为 -lift。
只在下降跨越平台顶面且角色底部中央 50% 的脚部区间与平台有实际重叠时触发
`{"on":"landed","entity":"player","with":"target","do":[...]}`。
0.7.96 起按扫掠接触时刻同时截停横、纵坐标，修复中心点误判和单步横向越出台面。
仅边界相切、角色透明外缘重叠、上升穿越或从侧面碰台不算落台；不跨空隙吸附。
角色素材应把可见脚部安排在贴图底部中央半宽内，避免大量透明底边与逻辑脚底错位。
这是 PC/WASM 与设备共用的通用碰撞修复，既有 motion/track 包无需改变资源格式。
资源规则可计分、重置角色位置、随机放置下个平台或结束回合。
edge 可用于跌落结束；place 角色会清除速度和蓄力并重新检查落地。

此版本不支持多物体刚体、旋转碰撞、坡面、弹簧、移动平台或横向反弹。
不得同时声明网格 solid_tones、scene.sprites、drag、非零实体 vx/vy、
tick/collision/blocked 规则或 move/velocity/accelerate/bounce 操作。
主角贴图通过 app surface.scene_view.sprites 引用透明 PNG；平台可使用几何 shape。
资源仍受全部原有尺寸、字体、紧凑 JSON 和图片预算约束。

验收必须从 buttons/input 的真实动作映射运行：按住反馈、松开发射、落台计分、
跌落结算、重开、暂停保留状态、拖出/失焦取消、空中不排队，以及旧固件能力拒绝。
`smoke_motion.mjs` 使用与固件同源的 WASM 核心，不以假物理演示代替测试。
浏览器通过不代替设备手感、触屏坐标与实体键验收。

## 连续平台轨道（0.7.95）

可选 `motion.track` 需要额外协商 `widgetPlatformTrack="p4-platform-track-v1"`。
旧版 motion v1 包继续运行；仅有 motion 能力的固件不能安装 track 包。

```json
"track": {
  "engine": "p4-platform-track-v1",
  "camera_x": 4,
  "gap": [2, 5],
  "width": [2, 4],
  "tones": [1, 2, 3, 4]
}
```

- 4–8 个平台槽位与一个主角，不包含额外实体。平台均 active、与初始角色脚底同高；
  初始承托平台保留声明位置，其他槽位按实体顺序在前方生成。不得依赖它们声明的 x。
- camera_x 是角色左缘跟随线，整数 2…grid.width/2；镜头只前进、固定步长平滑跟随，
  角色与平台整体平移，不把角色放回起点。世界位置在镜头附近重定位，避免长局坐标溢出。
- gap 为平台边缘间距范围，整数 1–6；width 范围整数 2–4。范围为闭区间且下限≤上限。
  `(gap.max+width.max)*gravity <= 2*lift*speed.max` 是基本可达性限制，仍需模拟多次落台。
  高度来自实体配置，本版本不支持变高、移动平台或斜坡。
- tones 为 2–4 个不重复的 1–4 色阶；实际 RGB565 来自 surface.scene_view.colors。
  相邻平台不使用同一色阶。平台离开左侧后才回收，复用固定数组，不随游戏时长增加内存。
- 每个平台首次落台触发 `landed`，回跳同一平台不重复触发；槽位回收后重新获得触发资格。
  计分仍由每个声明平台的 landed 规则配置，不把 score+1 或动作名称硬编码进固件。
- 禁止 place/show/hide 和移动/速度类规则改变轨道实体；不要用“落台后 place 主角”模拟前进。
  保留 charge、计分、tone、run/stop/restart 等有界规则，落空使用 edge 结算。
- 轨道暂停不移动镜头、不生成新平台；重开重置坐标和当前分数，最佳成绩仍用 persist。
  参数、布局、贴图和平台配色均在资源包；后续同类玩法不再增加专用固件分支。

回归要求：连续多次落台、世界位置不倒退/不瞬移、对象数量固定、生成目标可达、
颜色/宽度/间距确实变化、同台不刷分、暂停/取消/重开、实体与触屏输入、旧设备拒绝。
项目测试入口 `pc/scripts/test-platform-track.mjs` 用与固件同源的 C/WASM 跑 1,000 次连续落台；
`test-motion-runtime.mjs` 同时保留无 track 的旧版行为回归，不代表实机帧率已通过。
`test-motion-edge-runtime.mjs` 覆盖实际包的全部 20ms 蓄力档位及延迟帧补演；
`firmware/tests/p4_motion_landing_test.py` 对生产碰撞步做边缘扫掠、镜头平移、
16–40ms 步长、双向横移及上升/侧面误吸附回归。两者都需与连续落台回归一起运行。
