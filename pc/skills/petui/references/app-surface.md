# PetUI 声明式应用界面

候选通用绘图见 [canvas.md](canvas.md)。它有独立能力协商和编译开关，正常目标
默认关闭；不得把预览/离线通过当成已开放任意 Canvas、原生代码或实机性能保证。

原生独立列表生命周期桥仍是开发底座，不是已开放的设备本地集合程序。
另有 UI 候选专用的 PC 驱动 `surface.collection_view`，严格契约见
[host-collection.md](host-collection.md)。它使用现有 PC composition 集合源，
不是原生银行/VM 的安装入口；普通固件不声明该能力并拒绝此字段。
不得新增 `surface.collection` 等未实现字段，或把离线测试称作 P4 输入/耗时已验收。

通用命名变量绑定、PC 数据源组合与对应配额见
[composition.md](composition.md)。新绑定必须协商 widgetVariableBindings；
旧版固定 dashboard 绑定与资源配额不变。

滚动卡片及固定导航使用 [app-viewport.md](app-viewport.md) 的独立能力，不改变旧节点坐标语义。
整数/枚举表单使用 [app-form.md](app-form.md) 的 opt-in `widgetAppForm`，`field_bind` 仅适用于该本地表单的 text/button 节点；确认前不改已提交值。
多区域独立滚动须额外声明 `widgetAppViewports=p4-app-viewports-v1`，最多三个不重叠视口，
全局节点/变量/素材预算不变；不支持任意 HTML、无限列表或惯性滚动。

多级详情和返回使用 [app-navigation.md](app-navigation.md) 的显式本地页面栈；
必须协商独立能力，不把顺序翻页冒充导航栈。

## 场景与数据层（0.7.90）

资源包可声明 `surface.scene_view` 或 `surface.data_view`，两者及 `game_view`
互斥，仍使用同一个应用渲染器。必须分别核验设备能力
`widgetSceneSurface=p4-scene-surface-v1` / `widgetDataSurface=p4-data-surface-v1`。
旧固件拒绝安装；新增能力后布局、颜色、按钮和图片均通过资源包迭代，不必重刷。

- `scene_view` 精确字段为 engine/x/y/w/h/colors/sprites，engine 为
  `p4-scene-surface-v1`；区域完全位于 640×480 内，宽高至少 64。
  colors 为 5 个 RGB565 整数，依次 ink/primary/secondary/light/shadow。
  使用原有通用 scene 的移动、碰撞、计分与结算，不以 UI 文案替代玩法。
  v1 仅支持开放场地（grid 不带 rows/solid_tones），不支持触屏切果专用核心。
  sprites 为最多 4 条 `{entity,image,columns,fps}`，entity 指向真实实体，
  image 引用 surface 的普通 PNG；columns 1–8、fps 1–20，宽度整除 columns，
  每格至少 8px。静态角色 columns=1。不能混用 scene.sprites/entity.sprite；
  贴图统一计入 surface 图片和 128 KiB 预算。不要声称静态角色具备表情动画。
- `data_view` 精确字段为 engine/x/y/w/h/layout/colors，engine 为
  `p4-data-surface-v1`；区域宽至少 320、高至少 300，完全位于画布内。
  layout 为 todos/metrics；colors 为 ink/muted/accent/done/card 五色。
  保留原有 `data.source/page_var` 与 5 行分页。todos 将 label+meta 拼为标题，
  value 为日期，detail 为状态；tone<0 完成、其余未完成。未收数据、过期、
  上游错误均显示真实状态，不伪造待办、系统指标或时间。
- 内容层绘制于静态背景之后、动态按钮文字之前；不要用后续不透明面板覆盖内容区。
  触屏按钮仍复用 transitions/buttons；PC 卡片不轮询用户私有数据，详情才读取。

适用于有自己的 640×480 应用排版、按钮、持久化状态或 PC 服务的工具。
基础能力为 `widgetAppSurface=p4-app-surface-v1`；布局仍由统一 PetUI
解释器运行，不允许包内 HTML、脚本、SVG 或任意代码。

## 精细图形扩展

新增 `surface.graphics="p4-app-graphics-v1"` 时，设备必须明确声明
`widgetAppGraphics=p4-app-graphics-v1`。不能只检查 bounded runtime 版本。
没有此字段的旧包保留原来的配额及字体规则。

- 画布固定 640×480，最多 32 节点；紧凑 widget.json 最大 8191 字节。
- 节点基础字段为 type/x/y/w/h/color；所有矩形完全位于画布内。
- 支持 panel、text、meter、image，以及新增的 icon、button。
- 字体 `font_size` 10–96 像素；`line_height` 不小于字号且不超过 120，
  默认向上取整字号的 1.25 倍。`weight` 为 400 或 600。
- `align=left|center|right`；`valign=top|center|bottom`。
- button 负责背景、图标与文字的整体居中，不要把文字节点随意叠到色块上。
  文字来自 text 或 bind（二选一），foreground 为字与图标颜色；
  padding 0–32，默认 16；gap 0–24，默认 9；icon_size 8–48，默认 20。
- 圆角 radius 0–24，不能超过半宽/半高；stroke 为 RGB565，stroke_width 为 0–2。
  panel/button 的 opacity 为 0–255；panel 可用 opacity_end 和
  gradient=horizontal|vertical 声明单色渐变。透明按钮仍可有边框。
- icon 是产品自带的矢量线段，不依赖字体或 emoji；可选名称为
  home、leaf、desk、agent、compass、sun、mic、book、sound、spark、heart、
  pause、play、check、plus、list、arrow、cloud、clock、stretch。
  独立 icon 节点最多 48×48；不接收任意 path、URL 或 SVG。
- action 必须引用 runtime 中真实声明的动作，命中整个按钮，不只命中文字。
  全局退出和实时语音优先；不要添加包内退出动作。
- 底栏可用小尺寸、无 action 的 button 画键帽；说明必须符合真实输入绑定。

## 原生高清静态背景

首张 surface.images 可以声明 `encoding:"jpeg"`，源文件仍是 assets/ 下
真实、无透明通道的不透明 PNG，必须为 **640×480**，不超过 1 MiB。
它只能被第一个 image 节点作为无动作、无动效、通用页的全屏静态背景使用。
不要把按钮、状态文案或动态文字烘焙进背景。

PC 将此 PNG 编译为 P4J1：12 字节头、16 位宽高和有界 baseline JPEG；
从质量 88/84/80/76 中选择首个满足 100000 字节上限的结果，绝不自动缩小尺寸。
固件检查头、长度、SOF 尺寸与格式，复用 JPEG 解码器，首次绘制后缓存背景。
全部下发图片仍不超过 **128 KiB**；不增加每帧全屏解码或扩大游戏 sprite 配额。

其他小图沿用 P4I1（尺寸各 8–240，总像素 98304）。至多 4 张图；高清背景
占其中一个名额。原图可在 PC 预览，但交付必须额外核对编译后的 JPEG 与实际屏幕。
浏览器字体和设备字体不相同，浏览器截图不能替代设备的居中、裁切和字宽验收。
当前设备正文仍由紧凑 16px 中文字库采样绘制，大字号不是矢量字体；不要把
网页预览宣称为逐像素一致的设备效果。常用 `· ° ℃ – — − …` 使用受控原生
几何补字，避免退化成问号；功能图标必须使用 icon，不能依赖 emoji 字体。

静态应用层复用既有背景 framebuffer 作完整帧缓存，不要求声明滚动视口。
缓存精确比较解析后的 surface（含 field_bind 当前文字、var_bind 值、显隐、
禁用/按压状态和视口偏移）、dashboard 值、页码及进度；绑定变化会重新绘制。
旋转/浮动节点、游戏/scene/data、横版、切果、媒体和乐器动态层不进入此缓存。
确认弹窗及外层连接/触点/启动提示继续逐帧绘制，不写入缓存。此优化不改变
输入、存档、节点/变量上限或音频调度，也不代表首次绘制或实机帧耗时已获保证。

## 可选抗锯齿字形子集

`surface.typography="p4-app-type-v1"` 需要设备明确声明
`widgetAppTypography="p4-app-type-v1"`，不能安装到仅支持 graphics v1 的旧固件。
这一扩展仍在统一渲染器中，不把 TTF/OTF 字体或代码装进组件包。

- `fonts` 可含 1–2 个对象，各含 `image`、`chars`、`metrics`；image 引用普通 P4I1
  素材，不能引用 JPEG。图片宽高是 32 的倍数，仍不超过 240×240。
- 逐行排列 32×32 字形格；chars 为 1–49 个唯一 BMP 字符，拒绝 C0/C1 控制字符、
  代理项及跨字体重复字符；字数不能超过图片格数。metrics 与字符一一对应，
  每项为 `[advance, top, bottom]`，advance 为 4–32，`0≤top≤bottom≤32`，bottom 不包含在墨迹中。
- 字形使用 PNG alpha 覆盖率，由节点指定颜色，不含背景或整段 UI 截图。
  字体图片与装饰图片合计仍受 98304 像素、全部下发图片 128 KiB 的限制。
  优先覆盖大标题和数值，未覆盖字符回退设备内置字库；不能宣称任意动态文本都使用子集字库。
- 生成工具使用项目依赖中的思源黑体子集时，随包保留 SIL OFL 授权文件，
  在 `licenses/` 中声明来源，不删掉字体许可证。
- 底栏无动作 button 可声明 `key_for` 引用真实逻辑动作，保留 text 作为兼容内容；
  不与 bind/action 混用。宿主和设备从实际组件按键映射显示键帽，不再写死 SW1/SW2。
  全局按键仍优先；发布时另外检查组件绑定与当前全局设置没有冲突。

原生校验、固件解析、资源压缩和浏览器排版都必须通过；设备的最终字宽、
抗锯齿与帧率仍须实测，不以网页截图替代。

## 分格素材与条件显示

### 高清数字字形格（0.7.92）

设备报告 `widgetGlyphCells: "p4-glyph-cells-v1"` 后，字体对象可额外声明
`cell: [width,height]`。宽度只允许 16/32/48/64，高度只允许 32/48/64，
且宽不能大于高；缺省仍为 32×32。图片必须整除字形格、各维不超过 240，
字数仍最多 49，不能通过字形格改变总图片像素或传输字节预算。
`metrics` 始终是 **32/em 归一化** 的 advance/top/bottom，不是源图像素；
PC 与固件以 cell 高度映射实际采样。48px 数字可用 32×48 格，
避免把 16/32px 字形简单放大；小正文未收录字符仍回退内置字体。
必须分别验收字符邻格渗色、基线、裁切、数字宽度和设备显示，不能声称任意文字均高清。

`surface.interaction="p4-app-interaction-v1"` 依赖 graphics/type 扩展，设备须声明
`widgetAppInteraction=p4-app-interaction-v1`。适用于选装饰、植物成长与收藏展示，
不代表支持任意脚本、拖拽或动画时间线。

- 普通 image 节点可增加 `tile:{columns,rows,row,var,max_var?}`。
  columns/rows 为 1–8，row 为 0 起始行号；源图片宽高必须整除网格，单格至少 8×8。
  var/max_var 必须引用 runtime 已声明的整数变量；不能引用 JPEG 背景。
- 不提供 max_var 时列号为数学模 `var mod columns`（负值也归入合法列）。
  提供时按 `floor(var*columns/max_var)` 夹到 0…columns−1；分母不为正时取第 0 列。
  例如独立植物选择用取模，专注经过时间/总时长用比例选种子、幼苗、开花帧。
- 节点可声明 `show:{var,min,max}`，变量值在闭区间才绘制且可点击。
  min/max 范围 ±1,000,000,000，min≤max；JPEG 背景不能条件隐藏。
- 同一图集只占一张素材配额，但完整图集像素和字节仍计入原配额。
  不允许通过裁切声明绕过图片预算；不同装饰必须真的改变图像，不能只改描述文字。
- 预览、结构校验、原生导入和设备显示需分别验证；页面/状态改变、滑动、长按
  或按钮在按下后隐藏应取消点击，不能触发旧目标。

## 绑定与状态

text/button 只绑定 title/eyebrow/headline/metricLabel/metricValue/metricUnit/
badge/note/footer/progress；meter 只绑定 progress。不读取任意对象路径。
页面值 0–3，4 为通用页。计时与输入由 bounded runtime 的变量、状态和动作驱动；
图片、按钮和文案不能假装有未实现的服务、动画、进度或历史数据。

存档使用 `persist` 的版本、整数变量子集和完整安全恢复状态映射；不随资源包发布
用户存档。PC 应用使用受控 `pc/application.json`，校验通过的已安装组件默认启用，
首次使用与更新不再要求逐包授权；用户主动停用的服务不因更新自行开启。
私有存储与服务边界由宿主执行，系统权限及服务配置仍独立校验。
资源包驱动的城市/预报使用 `pc-app-services-v2`，详见 [location-forecast.md](location-forecast.md)；
适配器、城市选项和文案放在包内，用户城市与缓存留在宿主私有存储。
结构自测不代表录音、联网、朗读、USB 音频和跨组件功能已验收。
触屏区域按需求绑定真实 action，点击取消与硬件兼容规则见 [touch.md](touch.md)。

## 可交互待办服务

`pc/application.json` 的受控 `service: "todos"` 与现有 PC/语音待办共用原子存储。
要求 `host:true`、`p4-input-v1`，不与只读 `data` 列表混用；旧客户端不认识服务时拒绝安装。
布局、字体、图标、按钮和按键仍由资源包声明，不在服务中绑定组件 ID。

- 操作：`open` 查看当前事项/返回清单；`toggle` 完成/撤销完成；`filter` 切换未完成/已完成；
  `up/down` 选择事项；`previous/next` 翻清单页或详情页；`row_a..row_e` 点击当前页五行。
- 默认五行一页，最多读取现有存储的 500 项；不再把可交互清单截为前 20 项。
  全文分为两个最多 36 字符的正文块，每块最多 144 UTF-8 字节，按页完整显示，不截断原标题。
- 宿主模式 ready=清单、active=未完成详情、paused=空清单、history=已完成详情。
  `ui.actions` 控制空行、第一页/末页、无正文下一页等禁用状态。
- `toggle` 必须配置 `input.confirm`，PC 和设备捕获 `ui.context` 后明确确认。
  宿主核对稳定事项 ID、内容版本及原子 revision，旧确认/重复确认/其他事项不能产生修改。
  完成后留在原事项详情，可以立即撤销；不自动跳到另一项。
- 不提供新增、编辑、删除接口；这些仍由 PC 统一待办管理或已有语音工具处理。
  断连、存储损坏、修改冲突、写入失败不显示操作成功，不自动重试。
- 数据是本机私有内容，不进资源包、封面或社区；PC 卡片不轮询私有待办。
  该服务不启用麦克风、联网或播放。复用既有宿主和确认能力，不新增专用固件绘制分支。

# 既有小游戏画面扩展

维护已有 `snake`、`blocks`、`flappy` 包（以及新建原生 `gomoku` 包，见下节）时，可使用
`surface.game_view={engine:"p4-game-surface-v1",x,y,w,h}`；
必须确认设备声明 `widgetGameSurface=p4-game-surface-v1`。
该视口与既有 C 玩法核心相连，不是静态截图，也不允许任意代码。
不可与 host、scene、data、media、instrument 混用。边界位于 640×480 内，
宽高至少 64，设计时让每格至少 8px，视口边框应与实际格子区域对齐。
PC 预览和玩法自测使用相同固件核心编译的有界 WASM。
完整高清图片随资源包同步；OTA 仅内置无图启动布局，不覆盖已安装完整包。
不得将该维护扩展当成创建新预设游戏的入口。

维护飞鸟时可在 `game` 声明 `vertical_speed_percent`，仅接受 25–100 的整数，
需要 `widgetFlappyTuning=p4-flappy-tuning-v1`（0.7.89 起）。省略为 100，保持旧包
行为；50 表示相同时间内升降位移为原来一半，不改变管道横移、tick 或画面刷新。
参数属于资源包，后续调节只需重发同 ID 包。PC、固件与同源 WASM 均解释此参数，
旧固件必须在传输前拒绝，不能静默忽略。

# 原生五子棋画面

`game.type="gomoku"` 的新包必须声明 `surface.game_view={engine:"p4-game-surface-v1",x,y,w,h}`，
需要设备同时声明 `widgetGameSurface=p4-game-surface-v1` 与 `widgetGamePresets` 含 `gomoku`。
固件按 `cell=floor(min(w,h)/15)` 取整并居中：`ox=x+(w-15*cell)/2`，`oy=y+(h-15*cell)/2`；
推荐 `w=h=420`（cell=28）。每格至少 8 px。

- 棋盘线、星位与木纹画在首张 `encoding:"jpeg"` 的 640×480 背景里，线必须穿过每格中心
  `(ox+i*cell+cell/2, oy+j*cell+cell/2)`，否则棋子会偏离交叉点。
- 游戏层只绘制：黑/白圆形棋子（带高光与阴影）、当前光标（描边方框）、最近一手红点、
  五连高亮底色。空格保留背景，不要在背景中烘焙棋子或文案。
- HUD 走 dashboard：`metricValue` 绑定 `score_var` 显示手数（`state_mask` 只在 playing 显示），
  result 状态用 `show:{var:<score_var>,min,max}` 分别显示 1000–1999 黑胜、2000–2999 白胜、
  3000–3999 和棋的文案；ready 状态显示开始提示。PC 预览与玩法自测使用同一 C 核心编译的 WASM。

# Generic runtime additions (firmware 0.7.91)

These additions require capability negotiation; a new package must not be sent to
older firmware merely because its basic `widgetApp` capability exists.

- `widgetLifecycle: "p4-lifecycle-v1"` enables a root `lifecycle` object:
  `{"pause_action":"session.pause","record_var":"best"}`. Use this only with a
  scored `game` or `scene`. Declare playing → paused and paused → playing
  transitions for that action and give it a physical binding. The record must be
  a distinct integer variable included in `persist.vars`. Leaving the component
  pauses its clock; returning does not replay elapsed frames. Persist records,
  not a fake partially restored game board. Map saved playing/paused states to
  ready when the native board is not persisted.
- `widgetSurfaceAtlas: "p4-wide-atlas-v1"` permits a palette atlas up to 512 × 240
  pixels. Widths over 240 use the P4I2 compiled format (16-bit dimensions); older
  P4I1 assets remain supported. The combined palette-image budget stays **98,304
  pixels**, the compiled image budget stays **128 KiB**, and there are at most
  four images. This is not permission to ship more/larger textures unchecked.
  Full-screen JPEG backgrounds and compact glyph atlases keep their existing
  rules. Use native text for clarity when the available glyph atlas would need
  excessive enlargement.

Test pause/resume, leaving during play, process restart, corrupted saved data,
record retention, and image-budget rejection in both the PC and firmware
interpreters. Offline capability fixtures are not evidence of a connected
device's capabilities.

## 通用服务桥界面（候选）

原生服务状态、稳定键列表、类型化 meter 参数动作、显示时间轴及动态图片统一走
[service-bridge.md](service-bridge.md)。纯标量桥可以复用 legacy host、原有能力与配额，
不声明新增 services/host_contract/presentation；动态 image/timeline/event 参数或
local-media 才强制显式 `surface.services="p4-service-bridge-v1"`。此声明
依赖 app-surface/app-graphics/variable-bindings、host-context 和 input；需读取真实设备
`widgetServiceBridge`，集合仍单独协商，不能由普通 surface 或客户端版本推断。
`runtime.host_contract` 与 PC application 声明精确配对，更新任一端须重新绑定并同步；
旧界面不能执行新版宿主动作；配对 framing 将所有数值零统一为 `i0;`，其他有限浮点保留类型。

image 节点的 `image_bind` 只指向桥计划 images 别名；动态图片失败时保留静态 image，
不能混用 tile/motion/rotate_ms 或编码动画资源。最多两张 8–192 像素不透明图片，
宿主管理缓存，不接路径/URL。meter 的 `value_param/value_min/value_max` 仅桥能力
下用于已声明的 int 事件参数，非任意表达式或设备自由文本输入。时间轴只外推
确认状态，不能触发执行或充当播放回执；既有节点、图片、包体预算不放宽。
`PET_SERVICE_BRIDGE_ENABLED=0` 为默认，仅独立候选开启；PC 预览、离线向量和
编译结果不等于真实输入、USB、音频或 HIL 已验收。
