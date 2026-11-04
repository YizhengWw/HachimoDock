# 通用横版 Tile World v1

用于连续左右跑动、可变高度跳跃、地形碰撞、巡逻敌人、拾取物、移动平台、检查点和终点。不是任意脚本或完整商业游戏引擎，不支持斜坡、旋转物理、无限地图、关卡流式加载、任意敌人 AI 或任意着色器。资源包不得携带可执行代码。不要用组件 ID 添加专用分支。

## 必须协商

新包仍使用顶层 `p4-bounded-runtime-v4`，`kind=game`。设备必须明确声明 `widgetSideScene=p4-side-scene-v1`，同时支持 `p4-app-surface-v1`、`p4-app-graphics-v1` 和 `p4-input-v1`。不能用旧 `p4-motion-v1` 的蓄力跳跃能力冒充本能力。旧固件安装前阻止，不静默简化玩法。

## 声明与预算

`scene` 仅含：`engine,width,height,rows,tiles,actors,physics,actions,art,active_state,result_state,win_state,score_var,coins_var,lives_var,checkpoint_var`。所有字段均必需；引擎为 `p4-side-scene-v1`，未知字段拒绝。

- 地图宽20–160格、高8–16格，每格16逻辑像素。`rows` 为等宽的小写十六进制字符串，引用0–15号tile；0号为空白且不能奖励。最多16种tile。`flags` 为 `1=solid,2=one-way,4=breakable,8=hazard` 的位组合；one-way不能和solid/breakable混用。可选 `reward:0..10000,replacement:tile索引`；有奖励的替换tile不能再次含奖励，防止重复领奖。击打发生于玩家头顶碰撞。
- 最多24个actor。必需 `id,behavior,contact,x,y,width,height,skin`，可选 `speed,range_min,range_max,value,vertical,solid,stompable,edge_turn`。ID为1–15个ASCII标识符字符，位置/尺寸为整数，宽高1–32像素，必须在地图内。正好一个 `controlled`，出生位置不得与solid重合。`static` 不移动；`walker` 水平巡逻并受重力；`platform` 沿一条水平或垂直路径往返，必须solid。移动速度非零且绝对值不超过160，路径与包围盒均在地图内。
- `contact` 为 `none|damage|collect|goal|checkpoint`。伤害消耗一条生命、短暂受伤后检查点重生并有无敌期；可踩踏敌人通过 `stompable=true` 声明。拾取物只领取一次；检查点必须是声明的marker，不接受任意存档坐标。
- `physics` 全部是整数：`run_speed:32..240,acceleration:100..2000,friction:100..2400,gravity:200..1800,jump_speed:100..600,terminal_speed:240..640,lives:1..9`。固定20ms步进，单次最多补8步；暂停和离开页面不补算后台时间。Q8定点，无堆分配、音频、网络或随机脚本。
- 三个场景状态分别为运行、失败、成功，必须不同且存在于根states。另声明唯一的暂停往返transition。根初态必须是独立的ready状态。`tick=[]`，不混用host/game/data/media/instrument/local/lifecycle或pages。根transitions仅允许 `from,on,to`，其动作只来自下述四种。
- 四个输出变量为不同的int变量；可选根limits必须覆盖 `score:0..1e9,coins:0..65535,lives:0..初始生命,checkpoint:0..actor数量`。它们由核心更新，不用transition再次增减。

## 输入与生命周期

`actions={left,right,jump,pause}` 值是不同的资源动作名，须有根transition。物理映射为历史兼容事件 `knob.rotate_ccw/knob.rotate_cw` 对应左右摇杆，`button.sw1.short_press` 对应开始/跳跃/结束后重开，`button.sw2.short_press` 对应暂停/继续。`input.engine=p4-input-v1,input.hold=jump动作名`；左右动作应加入repeat。实体按住、松开/回中与场景连续轴同步，切页/全局退出清除输入，不能占用全局退出键。PC预览支持方向键、空格和P，不表示设备拥有键盘。

松开跳跃会缩短上升高度；有有限的coyote和jump-buffer。串口普通动作提供160ms方向脉冲/120ms跳跃脉冲，不能把串口测试称为实体长按验收。

可用既有 `persist` 保存checkpoint输出变量，并把各状态恢复到ready。检查点编号是actor顺序的一基索引，0为初始出生点；重排或删除marker时提高persist版本，避免把旧编号误指向另一marker。只恢复声明合法位置，不保存任意物理内存。失败或成功重开重置分数、生命和地图。

## 图集与布局

`art={image,columns,rows,scale,x,y,w,h,sky,tiles,skins}`。image引用surface.images中的真实PNG图集（不能是JPEG），尺寸必须为 `columns*16 × rows*16`；columns1–32、rows1–15，乘积≤256，整数scale1–3。视口必须在640×480内，最小160×128。sky为RGB565。`art.tiles` 一一映射地图tile到图集frame索引。

每个skin含 `idle,walk,air,hurt,fps`：四个动作序列分别1–8帧、fps1–20；可选显示width/height8–32（默认16）。每帧采样16×16图集格，再缩放到显示尺寸；横向镜像、跟随镜头和动画选帧由同一C核心决定。单个动作序列应占同一图集行，连续帧不允许空白、裁切或重心突变。最多8个skin。图集使用AppSurface的既有图片预算，不扩大旧grid-scene精灵配额。HUD用surface的通用文字/变量绑定叠加，不能用旧scene_view再次绘制。

图集先绘制、逐帧检查，再引用。包中只保留最终PNG和声明文件，不包含绘图脚本或来源不明素材。

v1镜头只水平跟随，地图缩放后的完整高度必须落在视口内（`h >= height*16*scale`），禁止把可行走地面裁到屏幕外。

## 验证

运行 `validate_generated_widget.py`、`smoke_test_widget_game.py`，均传入真实设备能力。新版smoke使用固件同源C/WASM，最多20种输入策略、每次3000步，只驱动方向与跳跃/松开，不直接改坐标。覆盖输入反馈、推进、得分、终局、重开和暂停；搜索失败不等于证明不可玩。通过也不等于实机验收，仍须核对实际画面、实体方向回中、SW1松开、SW2暂停/继续与全局退出。

`miniapp/state.sideScene` 提供相位、玩家位置/速度、方向、落地、跳跃保持、得分/金币/生命/检查点、时钟、revision及最近/最大核心步进耗时（微秒）。不包含录音、账号、文件或网络数据；耗时只代表物理核心，不等于整帧渲染耗时。
