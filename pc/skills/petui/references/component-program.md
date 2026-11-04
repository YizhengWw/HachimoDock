# 组件受限整数程序（按设备协商与资源门禁准入）

## 创作与同步入口

先读取目标设备真实 hello：`widgetProgram` 或 `widgetPrograms` 必须明确声明包使用的
`p4-component-program-v1/v2`，再交叉核验 `widgetProgramMaxCells`、
`widgetProgramMaxCode`、`widgetProgramMaxFuel` 与 `widgetProgramQuantum`。
v2 设备可同时明确兼容 v1；未知版本、缺失能力或超出任一报告上限均拒绝同步。
不能仅凭固件版本号、源码宏默认值或离线基线判断设备是否支持。

已报告能力的设备可以生成、校验和发布相应程序包；发布成功只表示本地库中存在
合法包，不能据此宣称设备时延、断电存档、声学或全部能力组合已验收。
以下私有对照段落描述各实验的差异，当前交付 profile 以文末公开候选及真实 hello 为准。

私有已安装后台控制器是独立的原生实验，不是 program 包字段：
`widgetProgramBackground=p4-program-background-v2` 只在私有父级成功初始化后协商。
显式 start/stop 必须绑定当前 bootId，启动另需精确已安装三项内容校验值，停止另需
精确 registration。普通/UI/Windows 固件不开放。当前全局仍只有一条 PROGRAM
租约，前台与后台不能同时占用；四槽表不等于四个权限。后台可跨页面投影同一实例，
主动停止不因显隐恢复，更新/删除先静止并缓存最新提交；没有包内自启动、开机任务
恢复或服务/外设混用。小型组件已通过静默设备生命周期、跨启动旧 bootId 拒绝、
保存恢复与主动停止锁存；不代表最大负载、任意组合、声音或 AEC 已验收。
不能据此生成 background 字段。

另一个默认关闭的 `widgetProgramParent=p4-program-parent-v1` 候选把前台设备程序
接到同一个原生后台 scheduler，每个主循环合计一次 64 指令调度，而不是每个子任务
各跑一次。仅此候选允许两条实际 PROGRAM 租约，燃料总配额仍为 4096；普通/UI
仍为零，旧私有后台仍为一条。隐藏前台先停止，显式后台继续；音频预约撤销两者且
不自动恢复。此能力是宿主执行策略，不是新增包字段，也不允许媒体/游戏/网络或
外设任意混用。仍需独立 P4 时延、存档和双 owner 静默验证，不能冒充已全面开放。

实验客户端管理入口还须协商 `widgetProgramInspect=p4-program-inspect-v1`。
启动前只读检查完整设备清单，再从设备的已验证 RAM 目录读取三项内容校验值；
不以 PC 本地预览或同名卡片代替已安装版本。一次操作只提交一次 start/stop。
回包丢失或身份不匹配时保留“结果未知”，不重发、不改用新 owner，用户可显式
刷新只读状态。registration 使用十进制字符串，避免 JavaScript 丢失 uint64 精度。
隐藏客户端界面只停止轮询，不暗中取消设备任务；没有任何自动启动。该管理链路
仍处于独立候选验收，浏览器模拟和合成串口测试不能代替真实设备验证。

`runtime.program` 已接入候选设备解释器、安装前验证、私有存档、生命周期和真实
组件预览，而不是另一个只运行独立算例的解释器。普通固件关闭该字段；UI 候选
当前只编译实现，`PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=0`，不声明
`widgetProgram`，PROGRAM 配额仍为零。不能伪造能力文件安装或把预览成功当作
设备程序已开放。必须分别通过 P4 分片、提交/音频互斥、栈与实机门禁后才能开放。

## 数据契约

显式候选 `p4-component-program-v2` 可配 `p4-integer-program-v2` 镜像：
cells ≤256、fuel ≤16384、code ≤1024；每轮 quantum 仍为 64。
新增 TEXT a,b（表索引钳位）、CELLGET a,b,c、CELLSET a,b,c，immediate 必须为零。
网格大小与 texts 数量来自包声明，不写入镜像头；网格地址越界使整次执行回滚。
VM 网格值保持 int32，显示投影才钳位到 cells.bits；预览适配器的
`gridSnapshot()` 返回这一投影。镜像首段按行优先映射声明网格，不能同时由
transition/tick/else 写入。v1 镜像即使运行于 v2 构建仍保持原预算和指令集合。

设备须明确报告 v2 能力；仅 v1 的设备必须拒绝。至多 64 cells 的存档继续使用
字节兼容的 P4S1；65–256 cells 使用显式 P4S2（1088 字节，LE16 cells、CRC32、
严格 schema/layout 匹配），文本信封为 p4s2。v2 app store 容量为 2304 字节，
完整快照仍通过同一 A/B 记录提交，不拆成多个独立键。旧固件拒绝新格式，不能
将损坏或不兼容存档视为新建。入队不是耐久确认，需现有 checkpoint 状态确认。
这些离线存档与预览验证不构成设备门禁开放或 HIL 验收。

program 必须精确包含 engine/image/outputs/actions/timers。
engine 为 `p4-component-program-v1`。image 精确包含 engine/schema/cells/fuel/code，
使用 [program-preview.md](program-preview.md) 中的同源 C 整数核心与指令五元组；
schema 为非零 uint32，cells 为 0–64，fuel 为 1–4096，code 为 1–256 条。
禁止 INTENT，包括不可达分支。包只能提供数据，不能提供 JS、用户 WASM、原生代码、
指针、授权、设备 owner 或回执。

outputs 恰好八项，每项为 null 或已声明 int 变量名；至少一项绑定，目标互异。
接收范围来自实际 vars/limits，不能由程序自行放宽。绑定变量不得同时被 transition、
tick、persist.vars、表单 committed/draft/focus/active 写入。未绑定变量保持原值。
键盘的 index_var/label_var 也属于表单瞬态写入者，不能绕过该排他检查。
本阶段仅允许 app surface/input v1 的本地应用；host/local/scene/game/media/
instrument/data 的混用尚未开放。表单和导航可使用不相交的字段及动作。
单独实验的宿主字段隔离例外见 [variable-owners.md](variable-owners.md)，必须额外协商
widgetVariableOwners；它不取消其他 profile 的排他约束，也不开放任意服务/外设。

actions 为 1–24 个安全 ASCII 动作名，每项为 0–4 个 int32 字或 `{ "var":"变量名" }`。
变量引用必须是已声明 int，派发时捕获当前值。每个程序动作对应的 transition 必须
精确为 `{ "from":"*", "on":"动作名" }`，不再并行执行另一份修改；不得与表单
动作重名，不得使用 host. 动作。timers 为 0–8 项，精确字段
id/delayMs/intervalMs/words，采用同源计时器核心；ID 1–65535 且唯一，delay/interval
为 uint32，interval=0 为单次，否则至少100ms，words 至多4个 int32。
布尔、数字字符串和小数不能冒充整数。

## 生命周期与存档

原生运行时、宿主、计时器、事件组和调度器使用固定实例地址；不得复制、搬迁、
序列化或重新初始化活动上下文。误复制的副本不能执行、取消或释放原实例资源。
这些地址只在可信原生存储内，不进入组件包、诊断或持久存档。
会话启动中收到停止请求会锁存，分配成功后先静止并撤销该 owner，不能把该请求
覆盖为启动成功或接受计时器/输入。启动中重入及副本读取不具有操作权；父级仍必须
用真实模型/生命周期锁串行化调用，这些检查不是跨线程互斥锁。

候选组件会话现通过通用 group/scheduler 进入执行器，不再旁路直调计时器和 pump。
每个主循环回合至多一个计时器入队、一个 begin/最多64条指令/commit 阶段。
显式新激活只能在旧组物理退役、精确注册移除后建立新代次；调度注册序号不重置，
旧 ticket 不得取消新 owner。时钟异常后的 rearm 仅是显式新生命周期的空调度器
准备，不恢复旧任务、旧计时器或在途事件。这还不是安装级多组件后台注册能力。
可信设备 factory 另绑定固定 750us 的协作式时间策略，每条指令前检查；到时保留
pc/寄存器/私有候选，下轮继续，不重新 begin、不重放事件。时钟异常停止，连续八轮
无指令进展也停止并保留已提交状态；只有明确新激活可建立新 owner。该策略不能
抢占回调、中断或 Flash/cache 停顿，不能作为硬实时上限或声学验收。
没有同步耗尽燃料的设备循环。整次成功才原子发布完整输出；越界、除零、燃料耗尽、
撤销等保留已提交数据，停止且不自动重试。手动停止、隐藏或音频撤销不自动恢复；
重新明确打开建立新 owner，旧事件不能投递给新实例。
新程序 profile 的前台生命周期同时暂停普通 tick：隐藏后锁存停止，重新出现或恢复
焦点本身不能继续修改模型或接受输入。只有明确重新激活建立新生命周期；不补跑
隐藏时段的 tick。旧的非程序组件计时语义不变。表单草稿在隐藏/导航时取消；已提交
程序输出和已提交字段分别保留，不相互覆盖。
新运行时发布后、安装/激活命令完成导航之前，存储清理可以短暂释放状态锁。
此阶段的新生命周期保持待启动，不能将旧页面的隐藏状态锁存到新 owner，也不能
提前执行。命令完成后的真实隐藏仍停止；已运行的旧 owner 不因此忽略停止或暂停。
四槽等待队列满时程序动作在设备/预览同时禁用，预览明确显示等待；已接受事件不
丢弃、不自动重试。读取可用性不是授权，最终入队仍检查真实 owner 和序号。

程序状态使用独立平台命名空间的 A/B 私有存档，不挤占或覆盖旧组件存档。
不存在的存档才建立明确初值：cells 全零、输出取已绑定变量初值。cells 与输出
并不隐式互相复制。schema/cells/输出范围不兼容、损坏、忙或不可用均不能兜底清零。
已有组件升级先捕获最新已提交状态并静止旧 owner，再准备文件，避免使用较旧磁盘
结果覆盖新进度。保存入队不是已经落盘；队列、在途事件、回执不持久化和重放。

`miniapp/state.boundedProgram` 仅包含当前激活 revision 的分类状态、燃料及耗时，
不包含程序、存储格、事件或 native owner。未启动/拒绝启动时不能展示上一组件回执。
sliceBudgetUs/sliceStalled 为当前实际 owner 的时间策略诊断；没有 owner 时不沿用旧值。
耗时为当前激活的单次与峰值观察，不是声学、最坏执行时间或完整调用图证明。

实际设备 factory 已绑定固定状态发布器：同一资源锁内检查 exact PROGRAM lease，
并一次复制不超过512字节的完整提交状态，与实时音频预约/撤销形成原子边界。
VM 指令、驱动、分配、等待和用户回调不在临界区；失去权限只保留旧状态，不重试。
离线并发测试不能代替 P4 临界延时或声学验收，执行门禁仍关闭。

## 预览与验证

真实 ComponentAppPreview 仅在声明 program 时懒加载产品 C/WASM 核心。旧组件
不加载该核心。每20ms一个预览回合；隐藏/失焦停止，重新明确激活用私有已提交 seed
建立新实例，不恢复在途队列。预览不会读取凭据、调用服务或修改用户数据。

生成工具通过 component_program.py 验证契约；bounded_app.smoke 优先路由到
component-program-host.mjs 的同源 C 验证，不再让普通 Python 状态机忽略程序。
自测最多128个状态、32步、200000次 pump，报告截断和 deviceVerified=false。
它验证实际可见反馈，不代表所有路径、硬件性能或所有服务组合已通过。
单字段表单的上一项/下一项、仅单字符纯文本键盘的增减键属于结构上永久禁用，
自测通过同一静态规则显式报告 disabledActions，而不是伪造可见反馈。其他触屏、
滑动和实体动作仍须验证；不能用该报告豁免死按钮或不可达页面。混合向量见
contracts/component-program-form-v1.json，覆盖程序事件/计时器、独立文本草稿和导航。
共享向量为 contracts/component-program-v1.json。原生 sanitizer、四端拒绝向量、
真实父级生命周期、浏览器和安装能力门禁须同时通过。

## 公开程序候选 profile

`esp32_p4_program_public_candidate` 继承 `esp32_p4_capability_parent` 的渲染、
存储和能力实现链；不再是早期 r137 的普通 evboard 基线。显式资源表仍为
**一个** PROGRAM 租约、总燃料 **4096**、每主循环合计 **64** 条指令，
PERIPHERAL 为 `{0,0}`。即使 v2 私有解释器允许更大 fuel，公开候选也必须执行
hello 中的 4096 上限，不能把两个逻辑子任务算成两份可独立使用的资源。

设备端后台启动仍使用带真实 bootId 与已安装内容校验的 `widget/program-start`；
包不能自行声明自启动或恢复权限。PC 托管的启动策略不等于离线设备开机自启动。
变量 owner 混用、RAM resume、音频撤销后自动恢复仍未开放。独立的 UTC 闹钟
可以按其显式能力排通知，不能据此启动隐藏程序或扩大 PROGRAM 权限。

evboard/UI 等未启用执行开关的构建不因这个候选而获得权限；每次安装都以目标
实际协商为准。旧版本的单量子通过记录不替代本轮同源完整帧、帧间隙、栈、
内存、持久化与声学验收；所有门槛保持原值，程序量子为 ≤750µs。

v2 原生提交使用调用方拥有的双缓冲与小型 selector：在 broker 临界区外复制到
备用槽，再在同一个 broker mux 下重查 lease、generation、revision 后切换。
撤销或冲突时不切换 live，成功提交后的旧槽在下一次串行提交前保持只读。
所有运行时/宿主快照通过 `pet_program_state_read()`、在原有 model owner 锁下读取；
不能读取绑定 facade 的内联数组。v1 仍保留原来的 ≤512 字节临界区复制检查。
这不改变资源配额、RTOS/audio、SAVE/prefetch 栈大小及空闲门槛，仍需实机时延验收。

存档文本 codec 直接在调用方输出缓冲区内展开、从严格十六进制 reader 解码，
不再使用 format 大小的局部 wire。设备 persistence 文本及 A/B 冷加载的两个
记录使用每次调用独占的 PSRAM 工作区；OOM 不覆盖 seed、不写入默认值，并释放
自有权限及 loading latch，借用权限仍由调用方持有。
