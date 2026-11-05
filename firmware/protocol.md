# ESP-P4 USB Protocol

## Device approval modal

The host may send a single pending approval request. Example (synthetic values):

```json
{"topic":"approval/request","payload":{"requestId":"example-request","agent":"example-agent","tool":"example-tool","reason":"example operation","expiresAtMs":0}}
```

The device shows a “需要授权” modal above the current page. Pending storage is
fixed: requestId 96 bytes, agent/tool 64 bytes each and reason 192 bytes,
including each terminating NUL. The requestId is required. The configured
`page_enter` short-press allows; the configured `page_back` short-press denies.
Other gestures cannot decide or pass through the modal. Holds/long presses
cannot approve. A decision clears pending state before the next input event;
the last decided requestId is retained to reject its replay.

```json
{"topic":"approval/decision","payload":{"requestId":"example-request","decision":"allow","source":"esp32"}}
```

`expiresAtMs` is normally Unix epoch milliseconds. The device estimates that
clock from the latest `system/heartbeat.tsMs`; an already boot-relative deadline
is also accepted. A missing/zero deadline, or an epoch deadline without a clock
anchor, uses `PET_P4_APPROVAL_TIMEOUT_MS` (30,000 ms). With a clock anchor, the
remaining epoch duration is capped by `PET_P4_APPROVAL_MAX_HOLD_MS` (300,000 ms).
An already expired request returns `approval_expired`; a request matching the
last decided ID returns `approval_already_decided`. Neither opens a new modal.
The optional explicit `neverExpires: true` selects a zero deadline: that request
remains pending until a decision or explicit clear, rather than inheriting the
default timeout. Rendering observes pending state and does not decide requests.

Private negotiated control framing adds `capabilities.serialControlOffer` on
UART/JTAG only. `bind`/`ack` may carry exactly `serialControl:{engine,nonce}`;
the engine is `p4-control-fnv1a32-v1`, nonce exactly 32 lowercase hex bytes.
The reply's guarded HELLO includes matching `serialControlIntegrity` and
`serialControlNonce`. A new probe waits for its own confirmation; old firmware
without an offer retains the historical framed/plain handshake. Later ACKs
retain the confirmed nonce; wrong/malformed confirmations never resolve task
waiters. A proposal-less legacy handshake resets only that physical route.
Native USB does not claim unsupported framing. This is not authentication,
upstream protection, automatic task replay, or a released capability claim.
Private boot-lifetime winning-frame diagnostics retain numeric node costs,
cache deferral and LCD compare/submit/wait for that same frame, without clearing
maxima, subtracting waits or exporting screen text, paths or identities.

Private bulk diagnostics additionally retain `registryUs`, `queryAtPeak` and
`saveAtPeak` numeric intervals inside `runtime.programParentPeak`. Frame
phases include app snapshot/wait/draw timings when compiled. These are bounded
read-only observations, not payloads, request identities, permissions or proof
that an overlapping actor caused a stall. All original full-window gates stay.

Private `phase-parent` adds numeric `runtime.renderLifetime.sameFramePhases`
for the exact boot-lifetime winning frame and `runtime.programParentPeak` for
the worst aggregate native registry turn (start/end, scheduler, collect and
capture wall time, entry count and invalid-clock count). Nested timings are
not additive CPU time and never relax the original aggregate/frame gates.
No owner, program, event words or private save content is exported. These
optional diagnostics grant no new package capability or peripheral access.

Private `widgetVariableOwners="p4-variable-owners-v1"` is advertised only with
`PET_VARIABLE_OWNERS_ENABLED=1`; ordinary builds remain closed. The package's
explicit `ownership.host` subset is supplied by composition v2/v3 snapshots,
while disjoint program/form/tick/save fields stay native. Program input words
are captured atomically from one fresh typed HOST reply; host-dependent pointer
dispatch also retains that exact observation. Pure native actions do not route
to PC or wait for its action list. Existing frame/checksum/instance/sequence,
ten-second freshness, queue acceptance, cancellation and no-replay semantics
remain unchanged. This grants no additional media, peripheral or resource
quota. Full profile and validation boundaries: `pc/skills/petui/references/variable-owners.md`.

私有 `boot-cache-parent` 仅把原有目录 JSON、逐组件完整校验及已选资源读取接入
独立原生缓存工作租约。每项物理读取结束后归还；缺少注册或租约时走原物理
校验，不把缓存准入失败误报成资源损坏。无文件名/不存在/验证结果缓存，不跳过
校验或旧目录逐项恢复，不续期、重试、改文件格式或放开普通组件权限。
已选资源的缓存租约先结束，随后私有存档按原规则申请自己的租约。

私有 `boot-trace-control` 和 `boot-cache-parent` 仅在显式
`diagnostics/query {includeBootTrace:true}` 返回 `bootTrace`：`elapsedUs[8]`
依次是 app_main 后的 storage/catalog/builtins/manifest/hardware-returned/
first-frame/transport-started/image-valid 单调时刻，单位微秒。
`observedMask` 标明真正记录过的项；缺失项的 0 不是“已耗时零”。`complete`
只表示八项都被观测且时钟有效，不是系统健康证明。已有效镜像可能早于传输
任务启动，故不按数组下标假定事件先后。负向/倒退/溢出时钟被标记，不重置
采样或伪造完成；分配失败不发布半个对象。无内容、路径、凭据、录音或播放。
不修改原启动校验期限、看门狗、优先级、音频及 AEC，尚待编译和实机验收。

私有 `storage-save-parent` 将同一有界精确字节缓存接到独立申请的原生存档
STORAGE_WORK 租约；借用组件安装租约的存档不另起缓存窗口，不接管手工诊断。
原始作用域地址、任务及代次限定停止权限；物理关闭返回后、归还租约前结束，
撤销后的清理也不会停止别人的新窗口。仍只缓存五秒，不续期、不重试，不改
fsync、双代存档、录音/播放/AEC 或任务优先级。普通目标关闭，未获实机验收。
私有阶段目标的存储准入日志只记录 owner-open/acquire 分类码；激活拒绝的
gate=1/2/3/4/5/6/7/8/9 分别表示入口占用/音频/命令所有权/存储准入/
存档缓存/解锁阶段/分配/重新上锁/发布前复核，不把失败改为成功或自动重发。

私有 `ota-metadata-probe` 从可信 OTA 控制器的启动读取和已知原生结果提供
运行镜像状态；热诊断不再调用会建立/撤销 Flash 映射的 SDK 状态读取。
`firmware/status.metadataCached` 和 `bootPartitionAvailable` 显式区分观测来源、
可用性；SDK 切换启动分区失败时不继续发布旧分区为已确认结果。SHA、芯片范围、
实际镜像验证、8 秒稳定确认和失败回滚不变。普通目标仍走原路径，未经实机验收
不得推广；缓存不是写入授权或额外设备认证。RTC 末尾增加 image-state 阶段36，
此前的分配阶段覆盖过 SDK 状态调用，不能据旧阶段直接推断 malloc 故障。

私有原生擦除阶段目标仅在 `diagnostics/status.nativeErasePhaseCoverage` 提供
选中芯片回调覆盖位（1=sector、2=idle、4=yield）；不暴露地址，也不读写寄存器。
原 RTC 阶段枚举末尾增加 partition/erase/yield/sector/idle/cache-flush，旧编号不变。
原 SDK 调用次数、参数、错误和持久化语义不变，不将阶段记录当成完成回执。

私有 `render-pack-probe` 只比较 H264 八像素打包的对齐读写，不新增协议或权限。
三处地址先校验对齐，未对齐和尾部回原路径；现有双缓冲、PPA/LCD 围栏不变。
普通/UI/混用固件默认关闭。PC 的未解析文本诊断另对严格固件日志前缀记录固定
故障类别和计数，不保存原文、任务名、地址、路径或 Key；文本提示不视为重启
证据。显式只读压力测试失败后可发一个全新 identity 查询区分启动周期，原失败
保留，不重发失败查询或业务任务，观察不可用时保持未知。

私有 framework-mixed-acceptance 仅组合已审查的统一程序父级、自动存储缓存及
带字节校验的 256 字节分段链路，用于联合验收。仍为两条 PROGRAM 租约、燃料
总额 4096、每轮合计 64 指令；不开放任意外设、媒体或自启动。继承的 33 个目录
槽只为安装两份测试资源而不删用户内容，回到 32 槽固件前先清理精确自有测试包。
普通固件不变，不能以配置存在替代实机或声学验收。

私有 render-rotate-probe 对照分块 CPU 旋转与原 PPA 路径，保持逐像素位置、
RGB565 顺序、现有双缓冲及 LCD 提交/刷新围栏。不新增帧缓存或线程，不改变
截图协议。非法尺寸、容量或重叠在写入前拒绝并回到原路径。普通配置默认关闭；
编译与像素一致测试不代表实际吞吐、长帧或声学验收。

私有 render-queue-probe 在固定 SDK5.5.1 的 PPA 原生入队边界观测，只匹配当前
渲染任务、原生 client 和事务 cookie。逐帧数值日志新增 prepare/queue_cb，且须
对应 QUEUE_VALID 标记；重复、时钟倒退或区间溢出不作为有效耗时。入队至回调
包含排队和 ISR 开销，不称为纯 DMA 时间。不改缓冲区、阻塞模式、围栏和优先级。
普通配置仍关闭，不构成帧门禁、语音或 AEC 验收。

私有整页缓存的内部条目压缩为 8 字节，保留所有 32 位对齐地址的页号、完整代次、
有界引用和填充状态；元数据共 17408 字节。引用达到 255 时回源读取，不回绕。
失效不重置复制期间的引用或填充标记。无协议或持久格式变化，普通配置仍关闭；
减少 8192 字节静态内存不等同于完成语音/AEC 验收。

私有 IO 观察器由 `esp32_p4_storage_io_probe` 及其私有对照子配置编译，普通固件不协商。
`storageIoTrace=p4-storage-io-trace-v1` 要求三个原生长期任务注册完成；不授予
程序/外设执行，也不引入缓存、文件格式或删除策略。SDK5.5.1 的真实 SPIFFS
read/write/erase HAL 在观察配置中仅调用原函数一次，同参数/返回/errno。
`debug/storage_io_begin/stop/status/read` 严格要求 requestId、当前 bootId；
begin另需durationUs(1..5000000)，其余需非零generation，read另需actor(1..4)、
operation(0读/1写/2擦)。只读取已停止且所有物理调用返回的窗口，不拼接未知任务。
计数按actor聚合；sizeBuckets为≤16/≤256/≤4096/>4096字节的调用次数。
requestedBytes、wallUs为精确uint64十进制字符串；前者含失败调用的请求量，
不是已接受或成功写入量，后者包含抢占，不能当作CPU或纯Flash耗时。
结果不包含地址/名字/图片/内容。开始前音频忙则拒绝；不调用录音、播放或模型。

`esp32_p4_storage_uniform_control/probe` 是尚未验收的同内存私有对照，不用于发布。
它们额外协商 `storageUniformCacheApplying=false/true`，仅在上述最长5秒窗口内
压缩成功读取的精确、对齐512字节块：前508字节必须相同，末4字节原样保留。
不预读、不缓存文件名或“文件不存在”。命中不访问物理介质，因而窗口内不能检测
新出现的读故障；窗口外与未命中错误仍走原HAL，不把错误当空数据。
所有写入/擦除尝试（包括失败/部分修改）先使重叠块失效；填充受修改序号保护。
越界、大于16块的修改或序号耗尽只关闭缓存，不取消/重试原IO。原生父层仅在启动
挂载一次，窗口内不允许重新挂载或替换介质。cacheHits独立统计，calls/requestedBytes/
wallUs只统计物理调用，不能把缓存命中混作物理读取。普通配置不分配缓存内存。

压缩参考在真实设备仅命中25/19次，收益不足，仍关闭；模拟收益不作为采用依据。
`esp32_p4_storage_page_control/probe` 是另一个尚待验收的完整字节对照，分别协商
`storagePageCacheApplying=false/true`。同内存：固定2048个512字节PSRAM数据槽和
有界DRAM元数据。只保存原HAL成功返回的精确字节，不压缩、不预读、不做名字索引。
命中复制前固定槽位引用；填充复制前固定预留槽，整个PSRAM/调用方缓冲复制均在
元数据临界区之外。写入使重叠槽失效但不能重用仍有引用/填充中的槽；修改序号变化
使旧填充无法发布。窗口、挂载限制、错误语义及物理/命中分计规则同上。普通配置
既不分配数据槽也不启用该路径；不允许与压缩参考同时开启。
私有 `esp32_p4_storage_page_stride` 只替换上述固定槽的数值地址散列，协商
`storagePageCacheHash=stride4096-v1`（参考为 mixed-v1）。精确地址键、窗口、引用、
写前失效、内存和权限均不变；不能据此推断文件存在或扩大执行权限。仍待独立实机比较。
私有 `esp32_p4_storage_page_parent` 另协商 `storagePageCacheAutomatic=true`，
仅在真实 STORAGE_WORK 预约成功后，用精确 scope 地址/任务序号启动最多5秒的缓存窗口，
真实 scope 完成并重新取得命令锁后停止同一代次。复制、旧 scope、未发布持久结果、
其他诊断窗口均不能被它采用或关闭；缓存忙、未初始化或过期只回退原始 IO，不能改变
安装、恢复、错误或资源权限语义。普通配置保持关闭，自动路径仍待单独实机验收。
`debug/storage_io_latest` 是只读元数据查询，严格仅收 requestId/bootId，返回最新代次
及现有状态字段；没有代次则 window_unavailable，不启动、清空、停止或重试工作。

私有 `serialControlIntegrity=p4-control-fnv1a32-v1` 给下行串口控制 JSON 行增加
固定21字节前缀 `!P4C1:ddddd:hhhhhhhh:`：五位十进制原文长度（含前导零）与八位
小写十六进制 FNV1a32，均只覆盖后续原始 JSON 字节，不含前缀和换行。长度1..32768，
超限不降级成无校验串口正文。接收端先校验原始字节，再作 UTF-8/JSON 解析；不找
后缀、不修复损坏字符。声明能力的 HELLO 本身也须有前缀，同连接后续 HELLO 不得降级。
音频/媒体 no-drain、原生 USB JSON、截图 begin/chunk/end/error 的独立校验路径不变。
前缀只排队，不新增短包 drain 或任何等待预算；部分前缀不继续发送无保护正文。
诊断仅记录长度、分类和限频计数，不记录正文/校验值/ID/路径。它检测意外损坏，
不是身份认证或上行保护。未知业务结果不重发；只读状态核对另行发起。
原 transport audit 的 bodyBytes/fingerprint/驱动数值仍仅表示原 JSON 正文，不含
新增前缀；不能据此宣称整条物理链路送达。控制查询确认也不等于声音/AEC或发布通过。

私有 `esp32_p4_render_lcd_probe` 协商 `renderLcdSplit=p4-lcd-split-v1`，只对同一
窗口峰值帧补充画面比较、面板提交、刷新等待的墙钟时间及有效标志；原 lcd_us 含义、
等待期限、错误处理、完整画面比较和缓冲区所有权不变。普通配置没有新增计时调用。
这些值包括抢占，不能当成纯 CPU/硬件耗时，也不代表长帧门禁或声学验证已通过。
`esp32_p4_render_span_probe` 仅另开默认关闭的 opaque RGB565 批量写入对照，
不新增通信能力，不改变原像素、裁切、图层次序、等待或帧门禁。

私有候选 `widget/program-query/start/stop` 严格要求 requestId；启动另需 widgetId 与
widgetChecksum/buttonsChecksum/spritesChecksum（三个8位十六进制字符串），停止另需
widgetId 与精确 registration（无前导零的非零 uint64 十进制字符串）。启动/停止都
必须携带当前 bootId（16位小写十六进制），跨启动的旧请求拒绝而不执行。查询仅需
requestId。回复 `widget/program-status` 带当前 bootId，并仅含组件ID、代次、状态、
检查点待保存标志和耗时，不含程序/存储格。
仅私有父级初始化完成才协商 widgetProgramBackground=p4-program-background-v2，
不接受旧私有 v1 控制请求。MaxActive 来自真实 broker（当前1），不是四个原生槽
就等于四个执行权限。小型组件已通过静默生命周期及 v2 跨启动旧 bootId 拒绝测试，
不代表最大负载、服务混用或声学验收。

另有 `widgetProgramInspect=p4-program-inspect-v1` 时，`widget/program-inspect` 只接受
requestId/widgetId，从已验证 RAM 目录读取已安装三项校验值，不扫描文件、不启动组件。
回复同一 `widget/program-status`，增加 installedContent 对象（widgetId 和上述三项
8位小写十六进制校验值）。恢复受阻、资源不完整或能力未初始化均拒绝。
客户端须先检查完整清单，再 inspect 后显式单次启动；停止须使用已观察的 bootId 与
registration。回包身份不符、丢失或结果未知不能自动重发或改为停止新 owner。

私有 background 候选开始接通已安装纯程序：显式原生启动、不导航、不自动开机恢复，
启动使用精确目录校验与最终 RAM 检查点，不丢弃在途前台输入。切换页面不搬迁活跃实例；
同 ID 旧前台存档不能覆盖后来的后台进度。更新/删除必须静止并保存隐藏 owner。
全局 PROGRAM 配额仍为1/4096，私有缓存置换仍关闭。协议/PC控制与实机验证继续补齐，
当前不能宣称后台调度或任意能力混用已经完整可用。

另有默认关闭的统一父级候选 `widgetProgramParent=p4-program-parent-v1`，把固定
前台程序接到同一个原生后台 scheduler。`widgetProgramAggregateQuantum=64`
表示每主循环所有子任务合计一次调度；`widgetProgramAggregateFuel=4096` 是实际
全局配额，而不是每子任务各 4096。仅该候选 MaxActive 为2，普通/UI仍为零、旧私有
后台仍为1。隐藏前台在聚合 pump 之前停止；明确后台不受显隐影响。无新增包字段、
自动重启或网络/媒体/外设执行权限，音频预约照常撤销所有子 owner。独立实机门禁
未通过前不能把原生测试当作性能、AEC或任意能力混用证明。

`esp32_p4_program_parent_acceptance` 是独立静默验收镜像，固定33个目录槽位，
仅为在保留隐藏用户数据时容纳两个测试组件；普通与其他候选仍32槽位。目录格式、
原子发布、旧数据和程序权限不变。返回普通镜像前必须删除确切的自有测试包、保留
其存档并验证原目录回到32以内。不能把该测试配额当作公开容量或通用程序权限。

默认关闭的原生 program registry 正在接入：固定实例、精确内容/代次句柄、存档 pin、
单次聚合调度和独立 RAM 检查点。真实 broker 配额不变，普通包不能借此获取执行权限。
普通固件仍不声明后台能力；私有一租约原型已实测，不代表任意能力混用已开放。

截图失败补充限定数值 writerReason/Part/Requested/Accepted/Calls/Return/Drain/
ElapsedTicks。分别记录音频、100ms发送预算、租约撤销、驱动零/异常返回、drain、
TX锁、编码或路由失败；ElapsedTicks 为 RTOS tick，不能直接当作毫秒。
错误提示发送不得覆盖原失败记录；终态只向相同快照/路由提供这些元数据。
未放宽100ms、3秒空闲、30秒寿命、CRC或只读重传上限，不重发业务任务。
pull 模式若仅因20ms TX锁忙且零调用/零接受字节，保留同一快照，不错归终态。
已开始的块返回 `snapshot transport busy` 及上述零字节数值证据；客户端只对
相同快照ID/块号在原三次尝试、25秒总预算内退避重读。部分写入、driver/drain、
错误身份、缺少元数据或未知错误仍失败；不得借机重采图或重发业务。begin锁忙
仅允许以相同nonce/route/尺寸重新观察已捕获图片，仍受原快照寿命约束。

r69 私有 UART 节奏对照同时覆盖截图写入，仍保留100ms写入、原快照寿命及校验。
r63 的截图失败来自未节奏化路径，不能据此证明节奏化对截图无效，也未证明桥接器根因。
默认固件、USB JTAG 和语音/媒体路径不变；不重采截图、不重发业务任务、不放宽期限。

私有存档命名域增加原生生命周期 pin，仅约束 RAM 缓存，不是包/串口授权。
仅成功加载且干净、未加载中、无失败、无 pin 的私有槽可复用；文件不删除，
32 个旧组件槽不变。查询和失败的存储预约不能置换缓存；旧 token 不能释放新 owner。
复用默认关闭，直到全部长生命周期调用方接入 pin；旧前台程序仍不置换缓存。
该机制本身不开放后台调度或外设执行，后台注册仍需独立验收。

私有 lookup0/lookup1 对照保留8槽，不改变目录、存档格式或任何存在性/错误结论。
lookup1 仅提示原 SDK 全名查找的起点，仍执行原文件头/标志/完整名字及环绕扫描。
默认目标无提示表、无包装函数；私有 storage.lookupHint 只含数值计数与墙钟耗时，
不输出路径/名字/哈希/地址，不是包能力，也不代表吞吐或声学验收已通过。

仅独立 program 私有候选构建开放一条前台纯计算 PROGRAM 租约、总燃料4096；依据为
r64 真主循环最大镜像与耗尽测试。普通/UI/Windows 不开放、外设仍关闭；音频实时预约
仍先撤销程序，不能因采样标志尚未更新继续提交。没有声明后台/媒体混用或声学验收。
NATIVE_PROBE 与 PROGRAM/PERIPHERAL 在同一 broker 中双向物理互斥；被撤销但尚未
返回的执行仍占位。诊断不能在已有程序旁额外增加一个执行回合，不改变原配额或音频保护。

截图最后一次终止仅保留固定 ID/物理路由和分类：idle/lifetime expired、lease revoked、
transport failed、already closed。仅同一 ID/路由的后续读取可得到该分类；其他请求仍是
通用 unavailable，不泄露历史。活动快照的 ID/路由错误不释放原图。未改变3秒空闲、
30秒总寿命、CRC、撤销或重试门限，也不自动重新截图。

main 固定诊断结果 v2 增加 maxImageCases/fuelExhaustionCases：全长256指令、64存储格
镜像的正常提交，以及4096燃料耗尽且全部已提交数据/修订号不变。每个阶段仍跨主循环
回合执行，结果发送仍在协议线程；不接受调用者镜像，不自动重试，不开放 PROGRAM 配额。

独立 linkpaced 构建仅对 drained UART 控制发送做 64 字节/一 tick 的节奏对照。
普通/UI/Windows 保持原发送方式，audio/media no-drain 队列不变；不是正式修复或声学验收。

debug/program_main_probe 同样只接受空 payload 和固定原生返回路由。协议线程仅排队，
主循环在 UI 锁外每回合做一个有界操作，完成后发布 RAM 就绪标志；结果的编码与串口
发送只在协议线程执行，不得让主循环等待 UART/USB。debug/program_main_result 的
executionTask=main 仅指固定向量执行，计时不包括回执发送，不代表任意程序准入或混用验收。

原生会话可在第一次准入前绑定固定地址的共享调度父级：父级每回合只调度一次，
会话只读取自己的结果，不得借 collect 继续运行、取走其他会话回执或隐式重启父级。
真实组件父级已采用此路径，先做可见性停止检查；原有独立调度调用仍兼容。
未新增包字段、wire 命令、后台安装注册或非零 PROGRAM 配额。

独立 2M UART 参考构建仅用于静默链路对照，不修改普通/UI/Windows 的 4M 默认值。
低速窗口通过不能单独证明驱动根因、4M 问题已解决，或语音/AEC 已验收。

本机 debug/program_session_probe 只接受空 payload，运行固化 C 向量的 8 次动作与
一次性计时器，经真实 session/group/scheduler 与 NATIVE_PROBE 租约；结果为
debug/program_session_result。不能提供代码、状态、回调、资源票据或授权，不写用户
数据、不播放录音、不重试任务。报告的是协议线程 session_step 时间，不是 UI 主线程
或能力混用验收；普通 PROGRAM 配额及包执行准入仍保持关闭。

候选 UI 构建可无损压缩原字库位图，启动时一次性解压到静态 PSRAM；普通构建仍用
原位图作对照。字形、映射、间距及许可证不变，输入/音频/绘制开始前必须完成长度、
结束状态和 Adler 校验，失败不重试或进入健康确认。没有新增包能力、wire 字段或授权。

组件文件和私有存档按同一已打开文件的有界长度读取，前后核对长度，不额外探读 EOF。
存档元数据 I/O 错误或读取中长度变化均视为不可用，不据此创建空存档或覆盖旧代。
原目录扫描错误与独立重扫规则、校验和、格式、租约及最终关闭检查保持不变。
不新增 wire 字段、重发或迁移；是否减少设备重扫与耗时仍以实机对照为准。

本机诊断另有只读的 debug/transport_audit_reply_read（空 payload），返回
debug/transport_audit_reply_result：上一次显式审计结果回复的独立 RAM 发送观测。
它不重发任务、不重置主审计环、不递归记录自己的查询；尚无观测时明确不可用。
只含长度、前后关联指纹和驱动返回值，不含帧内容；仍不是交付或密码学校验证明。
早期调试分派同样先检查 32 层容器、Unicode 与转义 NUL，再进入递归解析器；
无效输入沿用普通 invalid_json 拒绝路径，截图解析不得忽略尾随非空白内容。

候选组件程序会话改由共享 group/scheduler 路由输入和单回合执行；显式新激活只能
在旧注册物理退役后重新准备空调度器，保留递增序号，不能恢复旧队列或取消新代。
执行准入仍未开放，不新增协议命令或能力声明。冷路径的声明验证、诊断序列化和
HELLO 能力构造采用体积优化，不改变音频派发、VM、像素绘制或存储格式。

独立调度诊断构建才提供 `debug/sched_start|stop|status|read`，普通/UI 构建不启用
这些接口，也不声明为组件能力。start 精确接受 `{durationUs:1..600000}`；stop/status
精确接受非零 uint32 generation；read 另接受 index 0..1023，每次只读一个数值事件。
根对象只有 topic/payload，不接受额外字段、重复字段、数字字符串、小数、布尔或 NUL。
start 在音频忙或窗口活动时拒绝，不自动重启窗口；读取不重试任务、安装、写文件或
启动音频。返回启动时钟、代次、数量、溢出/覆盖等标记以及数值 actor/peer/marker。
包装仅覆盖跨对象的部分调度/互斥与 SPIFFS 调用；必须保留 partial-coverage 标记，
不能把调度选择当作实际恢复时刻或把等待全部归为 CPU/Flash 耗时。包不能访问它。

JSONL 接收若超出行容量或含 NUL，会丢弃到该行结束，不能把残留尾部当新命令。
原生 USB 的 JSON 帧也拒绝内嵌 NUL，协议解析必须消费完整 JSON 文本；合法 JSON 后的
非空白尾随字节不能触发命令。二进制资源分片不受文本规则影响，不自动重发未知结果。

组件安装/删除仍采用原命令与最终 ACK，不新增自动重发。协议工作线程显式持有 command
owner token 时，存储事务可在文件准备、目录提交和清理期间暂时释放 UI 状态锁；同一事务
始终占用存储槽，其他目录变更与激活不能插入。提交成功后必须发布，不能因后来的取消
把成功提交当失败回滚。此修改不变更传输 schema、已有组件包或存档格式。
同 ID、同非零保存版本的活跃组件热更新，在任何备用文件写入前检查声明兼容性：
不得删除持久字段、改变字段类型、缩窄整数范围、丢失状态/保存映射或已保存页面。
新增字段、状态和页面、扩大整数范围以及按名字重排仍允许；发布时合并文件操作期间
新产生的实时持久状态。拒绝时保留旧包、存档与暂存传输；这不是跨版本存档迁移能力。

编译中的 P4P1 纯计算镜像 loader/runtime 与只读原生时钟/摇杆快照服务目前没有包能力或
wire 执行入口。PROGRAM/PERIPHERAL 仍为零配额；不得通过 hello 或安装器声称支持。
可信 host 适配器只在原生代码中绑定精确资源租约、owner 代次、取消与单次 pump；
设备 factory 只绑定同一 broker 与音频状态读取，不启动程序。P4S1 私有存档 codec
拒绝损坏或布局不兼容的存档并保留旧输出；没有新增 wire 存档、安装或外设授权接口。
这些代码仍未接入组件包协议和实际生命周期锁，不代表用户可安装执行程序。
有界定时事件核心仅由原生/离线预览显式输入单调时间，一轮至多投递一个事件，
不新增 wire 定时器或后台授权。延迟合并、不集中补跑；取消、时钟倒退或序号错误
锁存停止。接收模型的输出范围在事务提交前完整检查，旧存档不兼容时保留并拒绝
启动，不自动重置。普通组件格式、存档与输入/音频路径不变。
原生有界事件总线也没有 wire 注册/发布入口，主题和 owner 句柄仅由可信父策略分配。
跨订阅者入队全有或全无，接收前重验原生代次，旧接收实例的消息不带入新实例；
忙/满保留事件，其他拒绝结果停止该接收注册，不猜测成功或自动重发。它不是持久任务
队列、外部提交接口或可直接生成安装的跨组件能力，不改变现有组件消息格式。
摇杆只读诊断保留原字段，通过固定三槽 observation 缓冲读取完整已发布快照；
缓冲暂不可用时 ready=false，不把新坐标与旧范围拼接。它不触发 ADC 采样、
校准或输入事件，不修改按键映射或音频/AEC 参数，也不新增外设授权/协议命令。
程序存档的原生适配已连接现有双代保存 worker，使用平台分配的独立 namespace，
不占用旧组件的 32 个缓存槽位，也不改变旧文件名。内存接纳与落盘状态明确分离；
读取失败不推断为空，不开放 wire namespace/写存档接口，不恢复旧事件或重复提交。
原生 prepare/publish 桥接校验镜像并恢复存档，在发布时绑定父事务的 operation、组件 ID、
内容身份和精确存储租约。被拒绝的启动不替换旧 staging/committed 状态；成功后只消费一次。
这不是新增安装命令，父安装器仍负责完整包验证、原子目录提交和发布锁。
原生程序生命周期组同样没有 wire 入口：一个已准入 host 仅接受一个控制器绑定，
输入、定时器及订阅消息共用单调序号；忙/满不消费序号，已接纳事件不因结果未知重投。
手动停止与观察到的租约/音频撤销会收拢所属工作，旧组不能取消新代 host、定时器或注册。
失败重启保留原绑定，成功的新 owner 才解除旧绑定。历史模型和回执仍可用于显式保存，
不等于恢复执行或获得 UI 发布权限；没有新增后台任务、安装能力或真实设备调度。
可信父级仍须提供同一生命周期/模型/总线锁以及实际 UI 租约，不能把此原生库当作
跨线程音频启动排他、公平调度或 P4 时延验收已完成的声明。
PROGRAM/PERIPHERAL 同属实时排他背景资源；未来受审配额开放时，实时预约在同一 broker
中撤销其 owner，且已撤销工作的占用仅在实际返回后释放，不依赖滞后的音频状态采样。
离线验证/目标架构编译不能替代真实 P4 分片耗时、生命周期及音频隔离门禁。

仅静默诊断可发送 `debug/transport_audit_start` 与 `debug/transport_audit_read`，
两者均要求完整 JSON、仅 topic/payload 两键和空 payload。默认不记录；start 的短 ACK
发出后才开始，最多观察 64 个物理 route，RAM 环只保留最近 16 条。read 先停止并在
transport 锁内复制固定元数据，放锁后编码返回。start/read 音频忙则保留历史并拒绝；
已有 audio/media/no-drain 发送到来同样停止观察，不因音频结束自动恢复。
每条含 seq、route（1 UART / 2 USB Serial-JTAG / 3 native USB）、bodyBytes、fnv、
hashAvailable、driverObserved、bodyAccepted/newlineAccepted、bodyLast/newlineLast、
bodyDrain/newlineDrain 与 invalidReturn。正文/换行各自保持原有写入和 drain 次数，
日志、音频、截图专用发送及审计自身不采集。native USB 的 void API 标记未观测，不能
伪装驱动接受成功；超 32768 字节的正文不算指纹。没有正文、密钥或用户存档落盘。
驱动接受只代表进入驱动接口，不证明物理发送、PC 收齐或任务执行；FNV 仅用于相关性
对照，不是密码学完整性或去重凭据。`tools/compare_transport_audit.py` 只比对捕获区间
的原始字节（仅剔除 LF），报告缺帧/短写/坏 JSON 的证据，不自动重发任何任务。

可信原生诊断 `debug/program_probe` 只接受完整 JSON
`{"topic":"debug/program_probe","payload":{}}`，不得附加代码、case、owner、grant 或用户状态。
它不是包引擎或程序安装能力。协议工作线程在短状态锁内只读检查资源安装、OTA 与重启忙，
释放该锁后才调用探针；音频采集、播放、实时对话或音乐任一活跃时拒绝。
结果只通过该请求的 send/context 路由返回，不广播，不重试：
`{"topic":"debug/program_probe_result","payload":{"ok":true,"error":"","cases":[...]}}`。
四组固定 case 为 filter_map_reduce（89 fuel）、backedge_4096（预算耗尽）、
validate_256（仅验证 256 条指令，usedFuel=0）及 backedge_4096_deadline（同一循环另加
750us 协作式提前让出）；每组 8 个样本，执行按至多 64 fuel 分片，
每个成功片后让出 1 tick。首片包括 begin 校验，末片包括该 case 的提交；不改变总 fuel。
case 含 kind、samples、minUs、maxUs、sumUs、result、usedFuel；前三项耗时为完整程序
所有执行片的累计时间，不含让出。另含 wallMinUs、wallMaxUs、wallSumUs（完整程序含让出），
quantumMaxUs、quanta（执行片峰值和总数）。payload 还含 quantumFuel=64、quantumMaxUs、
quanta（包括取消/旧代检查的片），以及 guardBudgetUs=750。
case 另含 deadlineYields/zeroProgressYields，未启用时间策略的组
为零；连续八片没有执行任何指令时明确 budget_stalled，诊断单样本最多512片。
这些上限不抢占 ISR/Flash 停顿，不把受调度影响的 wall time 冒充独占 CPU 时间。
payload 还含 cancelBeforeCommitRejected、staleOwnerRejected、stateBytes、workspaceBytes、
taskStackMinBytes、totalWallUs。片耗时覆盖真实 broker/音频检查；totalWallUs 包括片间让出，
不包括最终回包传输。栈值为协议工作线程自创建以来最小剩余字节数，不是探针的增量栈消耗。
slowestQuantum 保存真实最慢的同一片（并列取首片），字段为 kind、sample、slice（后两项从 1 起），
beginUs、stepUs、commitUs、freshCurrentUs、stackScanUs、wallUs；没有有效片则为 null。
五段之和等于该片 wallUs，也等于顶层 quantumMaxUs，不拼接各段独立峰值。validate_256 的
结构验证计入 beginUs，验证前后显式检查合计入 freshCurrentUs；执行中每指令权限检查仍在
stepUs 内，commit 自身检查仍在 commitUs 内。kind 还可为 cancel_before_commit 或 stale_owner。
原每片栈扫描保留；阶段计时包含观测开销、抢占和中断，不等于任务独占 CPU 时间，不能据此
单独证明高优先级任务抢占。任一阶段时钟为负或倒退即 clock_error，不继续执行或回滚已提交结果。
同一时刻仅一次探针，开始间隔至少 5 秒；独占 NATIVE_PROBE 的 1 个 lease / 4096 fuel 单位。
每条执行指令重新检查真实 lease 与四项音频状态，撤销后只在物理执行退出时释放；
变回空闲不自动恢复。所有模型/outbox 都是探针专用静态内存，没有外部效果或用户存档写入。
samples 和 min/max/sum 只累计完整核验成功的测量；result/usedFuel 则保留该 case 最后一次
实际 core 结果和 fuel（从未执行才为 not_run/0）。若私有提交后才观察到音频，整体仍为
ok:false/error:audio_busy，但 case 保持 committed/89，即使 samples=0；立即停止后续样本，
不回滚、不重试，也不撤销其他服务 owner。此时部分测量不能用于八样本性能验收。
错误固定为 invalid_request、transfer_busy、busy、rate_limited、audio_busy、resource_busy、
cancelled、probe_failed、clock_error 或 reply_overflow；未执行测量的入口错误只有 cases 空数组。
分片是严格指令数量上限，不是抢占式硬实时 deadline；新设备验收需逐片实测小于 2ms，
仍报告完整墙钟，不能把含让出的程序声称为低于旧 10ms 门禁。
此诊断用于性能/边界证据，不改变 PROGRAM/PERIPHERAL 的零配额，也不证明完整可安装 VM。

截图按块读取需协商 `screenshotTransfer=pull-fnv1a32-v1`。发送
`debug/screenshot`，payload 指定 `mode:"pull-v1"`、`fullResolution` 和
1–32 位 ASCII 字母/数字/下划线/短横线 `requestId`。返回 begin 含 id、requestId、
mode 及原完整性字段；同路由、相同 requestId 重试只返回原快照，不重新取帧。
`debug/screenshot_read {id,index}` 只返回一个不可变块，index==chunks 返回 end；
客户端仅在块长度/base64/checksum 校验通过后接纳，失败只能有界重读同一块。
`debug/screenshot_close {id}` 释放；错误会终止或明确拒绝，绝不把损坏字节当图片。
私有候选显式协商 `screenshotCloseAck=p4-snapshot-close-v1` 时，客户端改发
`debug/screenshot_close {id,requestId,ack:true}`，requestId 必须与 begin 一致；
同路由、同 id、同 requestId 的原快照完成物理释放后才返回
`debug/screenshot_closed {id,requestId,closed:true}`。错误 token 不释放快照。
客户端在原截图总预算内只发送一次关闭并等待匹配回执，之后才查询身份；没有回执仍失败，
不自动重试关闭、重拍或放宽校验。未协商的旧客户端/固件保留原无回执清理行为。
候选错误诊断只保留有界请求长度/FNV 和请求/当前快照 id 的长度/FNV，不保留请求文本；
这些数字只用于关联，不是身份认证、成功回执或历史故障根因证明。
关闭回执属于控制消息；要求 `serialControlIntegrity` 的连接仍须校验其前缀，
不会把新回执加入未校验原始截图数据的豁免表。底层 drain 失败仍报告失败。
一帧独占快照/bulk lease，最多 800 次 read、3 秒无读/30 秒总寿命后在协议工作线程
下一安全边界释放；音频忙同样撤销并释放。不承诺中断阻塞驱动调用的硬实时 deadline。
不同路由或 id 不能读取/释放别人的快照。旧 push 调用仍兼容；本能力不授权网络、音频或程序执行。

候选扩展 `widgetAppForm=p4-app-form-v1`：本地 App Surface 可声明 1–3 个整数或
选项字段，草稿与已提交变量分开。明确接受时原子提交所有字段，取消/隐藏丢弃草稿，
只持久化声明的已提交值；旧存档不会被静默归一化。字段显示与事件规则由 C/JS/Python
共享用例核对，完整契约见 `pc/skills/petui/references/app-form.md`。不代表任意文本键盘、
动态列表或放开媒体/宿主/游戏互斥，设备未声明能力时安装端拒绝传输。

候选扩展 `widgetAppViewports=p4-app-viewports-v1`：1–3 个独立、不重叠的纵向内容区，
各有不同的整数 offset_var，节点用 view 选择区域；与旧 viewport/in_view 不能混用。
仍共用原节点、变量与图形配额，不放开媒体/游戏互斥。区域不隐式获得手势焦点。
宿主隐藏立即清除视图快照，重入更换实例；相同序号只有等待命令首个回复可接受，
重复状态或命令回包不刷新有效期。变量实际变更推进本机输入版本，防止滚动往返复活旧点击。

截图诊断 `debug/screenshot_begin` 增加 integrity=chunk-fnv1a32-v1、totalBytes、chunkBytes；
每个 screenshot_chunk 增加 rawSize 与按原始字节计算的 FNV1a32 checksum。
结束 checksum 保持原 RGB565 像素算法。写入/编码/排空失败即中止，尽力发送
screenshot_error（failedIndex/sentBytes）；sentBytes 只计完整块，不表示接收方已收齐。
接收方必须校验块序号、长度、逐块及整帧校验，超时/错误不输出成功截图。
截图在音频忙时拒绝，不更改正常音频/媒体的队列传输策略。

开发中 `widgetAppNavigation=p4-app-navigation-v1`：本地应用可声明 navigation 同值，
transition 用 from_page 条件及 page 的 push/back/replace/reset 对象。历史最多 8 层，
空栈返回/满栈进入不执行整条动作；旧字符串翻页保持兼容。不是宿主表单或跨组件服务入口。
详见 `pc/skills/petui/references/app-navigation.md`，旧设备必须在同步前拒绝。

组件选择元数据以 `catalog_sequence` 绑定 NVS 小记录，不在普通打开时替换 SPIFFS 文件。
新目录提交使旧选择记录失效，避免新安装被旧选择覆盖；原 last-ID 文件只在没有新记录时
作为只读迁移输入，损坏/过期 NVS 不回退陈旧选择。组件资源和进度文件不删除或迁移。
冷切换由已有协议工作线程准备，文件读取、解析、存档恢复和清理不占用 UI 状态锁；
物理输入排队成功不代表打开成功，准备期间保留原页面，离开/更换选择会取消迟到的跳转。
运行态发布与贴图快照使用可调度的静态任务互斥锁，不再为大块复制关闭中断；
只允许任务上下文访问，ISR 和已有短计数锁不改变。复制本身的 CPU/总线耗时仍需实机测量。
只在身份、目录代次与资源租约仍有效时短时发布；已完成的发布不会因选择记录保存失败而回滚或重做。
本机 `P4 activation` 日志只记录结果及 prepare_us/remainder_us，不输出组件内容、用户记录或密钥。
这些阶段耗时不等于渲染帧时间，不替代真实设备的长帧验收。

开发中 `widgetActivation=p4-widget-activation-v1` 支持 `widget/activate`：
`{widgetId,show?:boolean,requestId?}`。只打开已安装且可见的组件，可选进入 app 页，
返回标准 protocol/ack；不下载安装、删除或更改输入映射。冷加载在资源安装事务或音频忙时拒绝；
已加载的媒体/持久应用可沿用原有纯内存恢复路径，不为导航额外读写文件。
此接口不是组件包自行导航或启动音频的授权；旧固件不支持时不能改用覆盖安装来冒充导航。

开发中 `widgetHostContext=p4-host-context-v1`：runtime 可声明同值 host_context，
仅 host + input-v1 接受。普通 app/event 命令携带最近已应用快照的 context；
confirmed 仍仅表示用户实际确认。该字段不是密钥，也不授予权限。组合 v2 用于
版本前置校验与事务去重；旧固件在安装前拒绝，不允许缺少上下文时直接执行。

App 视口扩展：`widgetAppViewport=p4-app-viewport-v1` 支持一个最多 2048px 高
的纵向内容区，使用已声明整数 offset_var；绘制和命中共用夹取及裁切。
视口外节点仍是固定屏幕坐标；位置变化取消先前按下。配额与旧坐标语义不变，
不声明惯性、任意 HTML 或无限列表。见 `pc/skills/petui/references/app-viewport.md`。

横版场景扩展：`widgetSideScene=p4-side-scene-v1` 与 `widgetScenes` 中同名条目
共同声明通用tile-world能力；上限160×16格、24actors，20ms固定步进。
`miniapp/state.sideScene` 报告相位、玩家位置/速度、方向、落地与保持跳跃、
score/coins/lives/checkpoint、elapsedMs/revision、lastStepUs/maxStepUs。
耗时只覆盖物理核心，不代表整帧渲染；旧组件不包含此字段。详情见
`pc/skills/petui/references/side-scene.md`。无此能力的固件必须在安装前拒绝新包。

开发中通用扩展：`widgetVariableBindings=p4-variable-bindings-v1` 表示图形
surface 可声明同名 bindings 标记，text/button/meter 可用 var_bind 读取已声明变量。
host 快照可带至多 8 个 variables 标量，整数 ±10⁹、字符串最多 63 UTF-8 字节；
组件/实例/序号/10 秒有效期沿用现有规则，不保存到设备 Flash。显隐与点击判定使用
同一变量快照。无此能力的设备应在传输前拒绝，不静默降级。
能力限额补充 maxInputTransitions=24、maxLegacyTransitions=12、
maxSurfaceNodes=32、maxLegacySurfaceNodes=20、maxLegacyWidgetJsonBytes=4095；
maxTransitions 仍为兼容值 12，不把有条件的扩展上限误报成所有包都可使用。
`widget/inventory.maxInstalled` 表示该设备实际可容纳的可见组件总数；保留的隐藏旧数据
仍占用物理槽，必须从这个值扣除，不能让 PC 多报空闲容量。隐藏项不会被自动清除，
删除当前组件也只回退到同一可见性规则允许的条目。

0.7.103-p4：内置清单、启动器图片及完整出厂资源统一为 29 项。末尾追加 `decision-capsule`、`answer-book`、`hope-train`；通用 `widgetLocalTools=p4-local-tools-v1` 契约保持不变。OTA 仍保留轻量资源引导与现有用户存档，完整出厂镜像才携带全部图片。

R172 候选的 `widgetLocalToolsMasks=p4-local-masks-v2` 明确声明修复后的内容池排除与存档表示。任一 deck 组超过 16 项时，PC 安装前必须核验此能力，不能只依赖旧 quota/maxLocalItems。最多 24 项保留原七变量/version1；v3 的 25–64 项使用显式十三变量/version2，以 24/20/20 bit 分词完整保存排除项，旧包和工厂 29 项不变。此字段是协议能力，候选尚须构建及实机验收；具体字段见 `pc/skills/petui/references/local-tools.md`。

R172 候选的 `renderLifetime.sameFramePhases.contentStagesInclusive` 增加 bubbleSelectUs、pinnedBlitUs、bubbleDrawUs、sessionCacheFillUs、sessionCacheDrawUs、sessionCacheScanUs、sessionCacheCopyUs、voiceUs。全部跟随整帧 maxUs 的同一个获胜帧保留，较小帧不会混入独立阶段峰值；select/blit/voice 是 draw 的嵌套耗时，不能将八项简单相加。记录不重置 lifetime、样本数或原预算，也不从整帧扣除计时开销；新增数值字段本身不代表冷帧或声学验收通过。

现代 `asset/slot-query` 的 observationSchema、queryNonce、expectedBoardDeviceId、expectedBootId、transferId 必须逐项唯一、类型正确且与当前设备匹配。忙碌回复使用 `ok=false, proof=false, quiescent=false`，保留 queryNonce/boardDeviceId/bootId，并给出 errorCode=`appearance_query_busy`；这是身份绑定的“尚无证明”，不含 slots，不表示安全静默。操作结果未知优先给出 `appearance_query_outcome_unknown`，不得当成可重试的忙碌、升级提示或清理授权。PC 仅对前者在同一连接代次内做有界只读等待，完整正向静默证明前不归档旧未知操作、不重放写入。

0.7.100-p4：新增 `widgetMediaQueueMax=128` 与 `widgetMediaLifecycle=p4-media-lifecycle-v1`。媒体队列超过 20 项时，key 至多 80 UTF-8 字节、标题至多 90 字节；上限 128，刷新按 key 保留选中项，不修改音频会话。`media/event` 新增 `operation=visibility`，包含 `source/boot/epoch/visible`；进入或离开递增 epoch，可见时每 2 秒发送心跳。宿主只有资源包声明自动播放、当前可见且新的进入事件时才准备播放，停止/离开取消未开始的播放；目录刷新不重新触发播放。能力与配置为通用媒体机制，不绑定特定组件 ID。

0.7.83-p4 媒体重入：从组件中心重新进入当前已验证的媒体组件只恢复界面，不重写目录或失效图片缓存，避免后台音频因同步 Flash 操作欠载。切到其他组件、安装更新及游戏重开仍走原有加载/校验路径；媒体会话与全局退出语义不变。

0.7.82-p4 发送约束：运行时日志与 JSON 协议必须在同一传输锁内进入同一 UART/USB 驱动队列；不得混用直接 console FIFO 写入与驱动排队。媒体/实时音频回执包含完整换行后立即返回，不等待队列排空；日志限长且保留换行。不以麦克风采集状态决定协议完整性保护，电台播放及启停过渡同样受保护。

组件中心（0.7.98）：出厂 SPIFFS 完整预置 25 项，顺序由 `factory-config.json` 决定，前四项为 `stock-watchlist`、`wooden-fish`、`upcoming-todos`、`computer-status`；停用、下架组件不重新启用。容量上限 32 项，目录 JSON 上限 16383 字节，单 widget 上限 8191 字节。OTA 内只保留轻量启动目录，不覆盖设备已有完整包；新机完整镜像包含全部贴图，已有设备通过 PC 同步完整资源。目录页使用 2×2 卡片、缓存材质背景与小缩略图，轻点打开、左右滑动翻页；摇杆选择与确认/全局返回保留。PC 宿主服务、公开知识库不搬入设备固件，仍需要 PC 在线。

## PC 音频播放器（0.7.66-p4）

0.7.69-p4：离开随身听组件允许后台播放。在宠物主页触发配置的全局 `page_back` 时，设备立即停止音乐并清空音乐缓冲，上报 `media/event {operation:"stop"}`，同时 input/event 标记 action=media_stop、handledLocally=true。即使 PC 尚在准备歌曲也上报，以取消对应任务；PC 不在接收线程阻塞等待 USB，补发的清理只针对当时的音乐会话。保留曲库，不自动下一首；实时对话中的退出优先于停止音乐。

能力 `widgetMedia=p4-media-v1` 表示支持 v4 工具的 `media.source=audio.player` 原生播放器。
`media/begin/chunk/end/query/control/stop/metadata/cover/library` 的每个请求使用唯一 requestId，
回复 `media/status` 包含同一 requestId、ok、sessionId、state、positionMs、bufferedMs、volume。
begin 声明 music- 前缀会话、sampleRate=48000、format=ima-block-v1、durationMs/offsetMs/volume。
begin/library 的 queue 与 keys 是同长度、最多 20 项的标题与稳定曲目 ID 数组，全部校验后才替换。
设备 `media/event` 携带 operation，选择歌曲携带 key，不依赖易变化的列表序号。

chunk 携带匹配 sessionId、从 0 单调递增的 seq 和独立 IMA ADPCM base64 块：
LE16 初始采样值、步长索引、零保留字节、LE16 样本数、低 nibble 优先数据；
最多 3840 个样本/1926 字节，48 kHz 单声道约 80 ms。旧会话或重复序号不写入音频。
cover 为 192×192 RGB565 LE，offset 连续且每片不超过 2048 字节；封面失败不阻断音乐。
进度来自实际播放字节，400 ms 缓冲起播（短曲收到 end 可提前播放），PC 有界补充。
语音优先：开始采集前暂停音乐并恢复双向 16 kHz codec；不将音乐混入会话 AEC。
媒体控制/数据请求维护 5 秒失联停止租期，library 刷新不续期。音乐不写 flash。
详细 PC 服务、技能和测试边界见 [播放器说明](../pc/docs/music-player.md)。

## PC 推送的组件实时数据（0.7.60-p4）

能力 `widgetData: "p4-data-list-v1"` 表示支持 `widget/data` 完整快照。
payload 为 `schema/source/ttlMs/status/message/rows`；最多 20 行、每页 5 行，
4 个命名数据源保存在 RAM。校验全部成功才替换，失败不续期；有效期使用设备单调时钟。
0.7.61-p4 起可选 `date` 字段接收 `YYYY-MM-DD`，只在列表顶部绘制；省略时保持旧发送端兼容。每行名称和元信息保持普通颜色，仅 value/detail 使用 tone。
列表组件在 v4 runtime 声明 `data.source` 与整数 `data.page_var`，无需修改旧组件。
PC 在文件传输占锁时推迟快照，过期数据明确提示，不启用设备网络。
完整字段约束、股票提供器与隐私边界见 [实时数据说明](../pc/docs/widget-live-data.md)。

## Transport

The P4 runtime keeps one JSON object per line over the ESP32-P4 USB-UART path
for flashing, rescue, and early logs. When the board Type-C jumper is switched
to the ESP32-P4 native USB OTG path, runtime data should use the high-speed
vendor bulk protocol `pet-usb-native-v1` instead.

JSON messages still use the same outer shape as the current Pet Manager USB
transport:

```json
{"topic":"<topic>","payload":{}}
```

## Handshake

### Duplex audio capability extension

`features.audioAec`, `audioVad`, `audioFullDuplex` are true only when the
ESP-SR microphone/reference pipeline initializes. A compatible client requests
`audio/conversation` with `halfDuplex:false`; missing capability retains the
legacy half-duplex behavior. `audio/status` reports live AEC availability.
Processed `audio/chunk` carries `aec:true` and boolean `vadSpeech` alongside
the unchanged 640-byte PCM/checksum fields. Raw capture is never marked AEC.
Runtime AEC failure emits `aec_fallback` and gates playback capture; the PC
stops a duplex session rather than recognizing raw loudspeaker echo.
`features.audioAecRecovery:true` additionally advertises capture-owner recovery:
the client may request duplex even when a previous fault made live availability
false. Each new `audio/conversation` entry attempts DSP preparation once, not on
every frame. `audio/status.audioAecPreparing:true` is a transitional state, not
an AEC failure; the client waits up to 3 seconds before playing its greeting.
`audio/status.aecReason` and `audio/diagnostic.aecReason` use fixed codes:
`none`, `init_failed`, `stream_reset_failed`, `reference_overflow`,
`processed_queue_overflow`, `dsp_too_slow`. A runtime fault stops capture without
uploading raw echo; a new conversation can rebuild the DSP without rebooting.

Firmware 0.7.74 adds numeric DSP diagnostics to status/diagnostic responses:
`dspFrames`, `dspLastUs`, `dspMaxUs`, `dspAecUs`, `dspNsUs`, `dspVadUs` and
`aecReferenceSamples`. Durations are microseconds; frame counts and maxima
reset on a new DSP generation. Stage durations describe the last processing
call, not a session average. These fields contain no audio or transcript and
are not repeated on every PCM chunk. v3 uses FD_HIGH_PERF + VERYAGGR, while
v1 retains VOIP_LOW_COST. USB frames remain 20 ms in both cases.

Audio messages carry `boardDeviceId`. Conversation status, begin, diagnostics,
errors and chunks also carry `conversationSessionId`, echoing the control
request's `sessionId` (maximum 63 bytes). This is separate from the existing
capture `sessionId`. Chunks bind to the conversation snapshot before the codec
read, so a PTT/conversation switch cannot relabel old PCM. Clients discard
another board's audio and stale conversation timelines. Older firmware without
these fields remains supported; raw PCM is never accepted in duplex mode,
and startup readiness has a bounded timeout.
Late `audio/play_chunk` and `audio/play_end` for an inactive playback session
are ignored after cancellation; they cannot append to a new reply. See
`pc/docs/realtime-duplex.md` for acoustic acceptance criteria.

Firmware 0.7.51 reports the actual silicon revision as an integer
`major * 100 + minor` in `capabilities.firmwareUpdate.chipRevision` and in
`diagnostics/status.runtime.chipRevision`. It is not inferred from the board SKU.
The desktop selects the v1 or v3 application image from these values. Both host
operating systems use the same 4M transport and existing chunk/pacing rules.

`firmware/begin` may include `chipRevisionMin` and `chipRevisionMax`. The device
checks them before opening the OTA partition, then independently validates the
first application's ESP image header before writing. Images must explicitly
bound one family (pre-v3 below 200, or v3 300–399); unbounded or mixed-family
images are rejected. Older desktop clients may omit the begin fields, but the
image-header check still applies. New desktop clients reject unknown chip
identity before starting an update; only diagnostics identifying the existing
pre-v3 SDK runtime allow the legacy v1 migration path.

Board to PC:

```json
{
  "topic": "hello",
  "payload": {
    "boardDeviceId": "p4-<board-id>",
    "runtime": "esp-p4",
    "deviceModel": "ESP32-P4 RISC-V Dual-Core + ESP32-C6",
    "fw": "0.7.42-p4",
    "buildId": "0.7.42-p4+290f402abcd1",
    "gitSha": "290f402abcd1",
    "buildDirty": false,
    "protocolSchema": 6,
    "wireProtocol": "pet-usb-jsonl-v3",
    "hardware": {
      "soc": "ESP32-P4 RISC-V Dual-Core",
      "hpCpu": "HP@360MHz(Max 400MHz)",
      "lpCpu": "LP@40MHz",
      "onChipSram": "768KB L2MEM + 32KB LP SRAM + 8KB TCM",
      "psram": "32MB In-package stacked",
      "flash": "32MB QSPI NOR Flash"
    },
    "capabilities": {
      "version": 1,
      "usbOnly": true,
      "nativeUsb": true,
      "nativeProtocol": "pet-usb-native-v1",
      "bulk": true,
      "maxAppearanceSlots": 2,
      "appearanceSlotReuse": true,
      "rawAppearanceSlot1": true,
      "transport": {
        "usbOnly": true,
        "usbUart": true,
        "usbUartBaud": 4000000,
        "rawAssetChunks": true,
        "rawAssetChunkBytes": 65536,
        "nativeUsb": true,
        "bulk": true,
        "nativeProtocol": "pet-usb-native-v1"
      },
      "display": {
        "width": 640,
        "height": 480,
        "physicalWidth": 480,
        "physicalHeight": 640,
        "pixelFormat": "rgb565"
      },
      "assetFormats": ["p4-h264-v1", "p4-mjpeg-v1", "p4-pcm-wav-v1"],
      "mp4": false,
      "widgets": true,
      "widgetDelete": true,
      "widgetInventory": true,
      "componentCatalogGeneration": true,
      "widgetRuntime": "p4-bounded-runtime-v4",
      "widgetRuntimes": ["p4-bounded-runtime-v3", "p4-bounded-runtime-v4"],
      "widgetScene": "p4-grid-scene-v2",
      "widgetScenes": ["p4-grid-scene-v1", "p4-grid-scene-v2"],
      "widgetSprites": true,
      "componentCatalogMax": 16,
      "widgetGames": ["blocks", "snake", "flappy", "gomoku"],
      "widgetGamePresets": ["blocks", "snake", "flappy", "gomoku"],
      "widgetLimits": {
        "maxVars": 8,
        "maxStates": 6,
        "maxPages": 4,
        "maxTransitions": 12,
        "maxTicks": 8,
        "maxButtons": 8,
        "maxSceneEntities": 12,
        "maxSceneRules": 20,
        "maxSceneOpsPerRule": 4,
        "maxSceneSprites": 4,
        "maxSceneSpritePixels": 4096,
        "maxWidgetJsonBytes": 4095,
        "maxButtonsJsonBytes": 2047,
        "fetchers": false,
        "readers": false
      },
      "voice": true,
      "stats": true,
      "inputs": true,
      "inputConfig": true,
      "audio": true,
      "audioCapture": {
        "codec": "ES8311",
        "format": "pcm_s16le",
        "sampleRate": 16000,
        "channels": 1,
        "bitsPerSample": 16,
        "frameMs": 20,
        "transport": "usb-jsonl-pcm-v1",
        "maxCaptureMs": 30000,
        "playback": true
      },
      "touch": true,
      "touchInput": {
        "controller": "GT911",
        "ready": true,
        "coordinates": "logical-640x480",
        "tap": true,
        "longPress": true,
        "swipe": true
      },
      "firmwareOta": true,
      "firmwareUpdate": {
        "transport": "usb-jsonl-ota-v1",
        "slots": 2,
        "maxImageBytes": 2621440,
        "chunkBytes": 4096,
        "checksum": "sha256",
        "rollback": true,
        "validationMs": 8000,
        "projectName": "pet_manager_p4_runtime"
      },
      "protocolAck": true,
      "screenshot": true,
      "appearance": {
        "formats": ["p4-h264-v1", "p4-mjpeg-v1", "p4-pcm-wav-v1"],
        "maxSlots": 2,
        "slotReuse": true,
        "rawSlot1": true,
        "builtinSlot": 0,
        "customSlot": 1,
        "builtinProtected": true,
        "mp4": false,
        "audio": true
      },
      "features": {
        "mainPage": true,
        "statsPage": false,
        "screenshot": true,
        "controls": true,
        "widgets": true,
        "widgetDelete": true,
        "widgetInventory": true,
        "miniAppPage": true,
        "miniAppState": true,
        "componentCenter": true,
        "voiceCapture": true,
        "audioPlayback": true,
        "touchInput": true,
        "touchGestures": true,
        "firmwareOta": true,
        "protocolAck": true
      },
      "controls": {
        "sw1": 50,
        "sw2": 49,
        "sw3": 5,
        "encoderPress": 4,
        "encoderB": 3,
        "encoderA": 2,
        "activeLow": true,
        "configPersistent": true,
        "touchscreen": true
      },
      "screenPages": ["main", "components", "app"]
    }
  }
}
```

`p4-bounded-runtime-v4` / `p4-grid-scene-v2` add modern semantic shapes and
bounded animated sprites while the advertised arrays preserve v3/v1 package
compatibility. A package may declare at most four horizontal PNG sprite sheets,
eight frames per sheet, and 4096 decoded pixels across all frames. Pet Manager
compiles each source PNG to `runtime/sprites/<id>.p4s` (RGB565 little-endian plus
alpha) before USB downlink. Sprite files are checksum-verified, committed in the
same dual-generation component transaction as `runtime/widget.json` and
`buttons.json`, and restored after reboot; raw PNG files are never interpreted
by firmware.

Hardware-dependent flags are emitted from successful initialization rather
than board-model assumptions. Capture prefers ES7210 and falls back to the
ES8311 ADC/DAC path; a board reports `voice=false` only when neither capture
path can initialize.

`fw` comes from the ESP-IDF application descriptor. `buildId` combines that
version with the 12-character Git commit and appends `-dirty` for a build made
from modified sources. `protocolSchema` is the PC/P4 compatibility contract;
schema `5` adds the ESP32-P4 four-direction joystick events while retaining
the legacy encoder event aliases, explicit build identity, revisioned Session,
transactional component-catalog, and A/B firmware contracts. Older firmware
may omit these fields, in which case Pet Manager reports an unknown build
instead of guessing from the semantic version alone.

## Native USB Bulk

Native USB device mode enumerates as vendor bulk device `0x303A:0x4040`
and exposes a Microsoft OS 2.0 `WINUSB` compatible-ID descriptor. Windows
8.1+ therefore binds the inbox WinUSB driver without a project-specific INF
or Zadig step; macOS/Linux continue to use their native libusb backends.
(`OpenClaw P4 Native USB`). It exposes one bulk OUT endpoint for PC-to-board
frames and one bulk IN endpoint for board-to-PC JSON replies. This is the
preferred transport for large P4 appearance packs.

Frame header, little-endian:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | magic `P4BU` |
| 4 | 1 | version `1` |
| 5 | 1 | kind |
| 6 | 2 | flags, currently `0` |
| 8 | 4 | sequence |
| 12 | 4 | payload length |

Kinds:

- `1` JSON: payload is the existing UTF-8 JSON message without the trailing newline.
- `2` file begin: payload is JSON metadata with `transferId`, `path`, `size`, and `checksum`.
- `3` file data: payload is raw binary file bytes.
- `4` file end: payload is optional JSON metadata.
- `5` commit: payload is JSON metadata with `transferId`, `fileCount`, and `totalBytes`.
- `6` identity ping: payload is UTF-8 JSON containing a fresh, unpredictable
  `nonce`. The board replies on kind `1` with `native/pong`, echoing that nonce
  and its immutable `boardDeviceId`:

```json
{"nonce":"<host challenge>"}
```

```json
{
  "topic": "native/pong",
  "payload": {
    "protocol": "pet-usb-native-v1",
    "boardDeviceId": "p4-<board-id>",
    "nonce": "<host challenge>",
    "protocolSchema": 6,
    "buildId": "0.7.42-p4+0123456789ab"
  }
}
```

Before any mutating native operation, the desktop enumerates every matching
VID/PID candidate, challenges each one, and selects the single response whose
`boardDeviceId` exactly equals the requested board. It then reopens and
challenges that candidate again before returning the transport. Missing IDs,
stale/mismatched nonces, unsupported protocols, no match, or duplicate devices
claiming the same ID all fail closed before a slot or file is modified. Firmware
that predates this identity response can still use USB-UART but cannot receive
native writes.

UART and USB Serial/JTAG keep independent framing buffers, and request replies
are written only to the ingress that supplied the command. Unsolicited runtime
events are broadcast to UART, native USB, and USB Serial/JTAG after that path
has sent at least one valid byte. UART, USB Serial/JTAG, and native generic JSON
commands enter one bounded protocol queue; its worker owns JSON parsing and
runtime-state mutation. Native bulk file frames remain serialized by the same
state mutex. The main loop copies a stable render snapshot while holding that
mutex, then releases it before H.264 decode, overlay drawing, rotation, and LCD
refresh, so rendering cannot starve command ACKs.

Normal boot uses `format_if_mount_failed=false`. A damaged or mismatched SPIFFS
volume is preserved and reported as a mount failure; only an explicit factory
recovery/erase operation may format or replace it.

Appearance slot `0` is the protected factory Westie pack. All non-built-in
appearance transfers target replaceable slot `1` and atomically switch the
active marker only after commit. Before `asset/begin` is acknowledged,
firmware clears slot `1` using a fresh SPIFFS directory scan after every
deletion; cleanup failure rejects the transfer instead of leaving skipped stale
files to consume the next pack's capacity. This path never erases protected
slot `0`.
When the active slot contains a valid pre-ready-marker manifest with a
deterministic `packId`, startup promotes that same slot by writing its ready
marker. It does not clear either slot during migration.
Every current manifest carries a deterministic SHA-256 `packId` over its
non-manifest payload. `asset/slot-query` returns the active slot and the
validated `packId` stored in each slot. If the requested pack is already in the
inactive slot, `asset/activate` with the exact `slot` and `packId` switches the
marker and reloads that manifest without rewriting its files. Activation is
rejected while another appearance/firmware transfer or reboot is active.
Firmware without the advertised `appearanceSlotReuse` capability keeps using
the full transactional transfer path. When `rawAppearanceSlot1=true`, every
cache-miss full sync targets the dedicated raw slot `1`. `asset/abort` closes
any open file, invalidates incomplete raw metadata, clears inactive staging,
releases the runtime transfer gate, and replies with `asset/ack` phase `abort`;
the previously active appearance pack remains valid.
If either serial or native-USB appearance traffic stops for 15 seconds before
commit, firmware applies the same inactive-slot cleanup locally and releases
the render gate. This is a crash/disconnect safeguard; normal host failures
still send `asset/abort` immediately.

PC to board:

```json
{"topic":"ack","payload":{"desktopDeviceId":"desktop-..."}}
```

## Supported Input Topics

- `state/<agent>`: updates current lifecycle state, event, and status marker.
- `speech/text`: updates the speech bubble title, body, status, and status text.
  A terminal `done` lifecycle remains visible on the device for 60 seconds
  before the runtime returns it to `idle`, so completed replies stay readable.
- `session/current`: updates the live desktop conversation queue, selected
  position, and temporary explicit Session binding. The main page renders up
  to three cards around the selection. Each item carries `id`, `title`,
  `content`, `state`, `transitionRevision`, and `terminalRemainingMs`; later
  `speech/text` updates use `sessionId` to replace
  the matching card's progress content. Titles use one line and progress uses
  up to two lines, with UTF-8-safe pixel-width `...` truncation. Holding the
  configured `voice_ptt` button replaces the selected card with a waveform.
  When the main page has no Session cards, the same recording feedback appears
  in a standalone bottom panel instead of the idle bubble. Queue refresh,
  expiration, or clearing does not end a physical voice hold; releasing the
  button removes the panel and restores the normal idle bubble.
  The desktop reconciles adjacent snapshots and publishes at most eight cards;
  no manual card-count setting is involved. A cold snapshot admits only active
  lifecycle states. Historical `done`, `error`, and `idle` Sessions remain in
  the independent desktop routing list and never enter the device queue.
  A Session that was active in the immediately preceding visible queue may
  transition to `done` or `error` and remain for exactly 60 seconds. Ordered
  lifecycle events may omit unrelated active Sessions, but each periodic full
  Agent scan is authoritative: an active Session missing from that snapshot is
  removed instead of being retained indefinitely. An explicit `idle` transition
  also removes it immediately. Hidden, manually cleared, and cross-Agent cards are
  not eligible for later terminal retention.
  The desktop assigns a stable, JSON-safe `transitionRevision` whenever the
  lifecycle changes. Terminal snapshots carry the remaining PC-side deadline,
  clamped to `0..60000` milliseconds, rather than starting another full hold.
  Firmware ignores older revisions and preserves the original deadline for a
  repeated revision. A newer terminal revision uses only the supplied remaining
  TTL. This also lets a rebooted board restore a still-visible terminal card
  without extending it. Legacy terminal items without revision metadata still
  require the same `agentId` and a matching active ID in the previous on-device
  queue. Unknown and idle items are rejected, and firmware never
  creates a fallback idle card from the background routing title. Once the last
  retained terminal card expires, the renderer returns to `休息中`.
  `activeSessionIds` separately carries the current bounded active set. Across
  accepted lifecycle states, incoming refreshes update matching cards in place
  instead of adopting the desktop's changing `lastModified` order. First-seen
  relative order therefore remains stable; new visible Sessions append, and
  only hidden or expired cards are removed.
  While this queue is nonempty, the selected card's lifecycle is also the
  authority for appearance and lifecycle audio selection. Aggregate
  `state/<agent>` values remain available for protocol bookkeeping, but cannot
  make the visible working Session play another Session's idle or terminal clip.
  Firmware additionally removes active cards when no `session/current` snapshot
  arrives for 12 seconds; already terminal cards keep only their original
  remaining 60-second deadline. This protects the screen from stale working
  cards if the desktop bridge stops publishing while other USB traffic remains.
  On `main`, device previous/next actions update the selected Session card and
  report its exact ID back to the desktop. On `components`, the same actions
  select the previous/next catalog entry without changing desktop Session
  routing. SW2's default `component_center` action toggles the two peer pages.
  On a followed-Agent change, the desktop sends the new `agentId` with an empty
  `sessions`/`activeSessionIds` snapshot before loading that Agent's queue.
  Firmware treats this as an immediate cross-Agent clear. `displayEnabled:
  false` explicitly clears the visible queue. Older senders that omit the field
  default to display enabled.
  Host activity received concurrently with a render frame is treated as online
  even when that frame captured its timestamp just before the activity arrived.
- `control/screen-page`: switches the P4 renderer between `main`, the on-device
  `components` catalog, and the active bounded mini-app `app` page.
  Other values receive an `invalid_page` protocol NACK.
- `stats/update`: updates the bounded P4 statistics model. The same token and
  runtime metrics are also extracted from `state/<agent>` payloads. For the
  built-in Token dashboard, Pet Manager filters to the current followed Agent,
  aggregates its local-calendar-day Session usage as `dailyTokenUsage`, and
  normalizes that object into the bounded `tokenUsage` field on a dedicated
  `runtime/stats` message before Session-specific state routing is applied.
  Bridge startup publishes this aggregate through a display-free idle snapshot,
  so opening Pet Manager does not require a new Agent message before data appears.
  Activating a mini-app replays the latest bounded stats model immediately, so
  a Token dashboard opened after the telemetry arrived never waits for another update.
  Cross-day Codex rollouts subtract the exact local-midnight cumulative baseline
  rather than exposing lifetime Session usage.
- `input/config`: validates and persists the P4 input map. The compatibility
  envelope `control/command` with `type: button_config` is also accepted. While
  a component is open, its mapped gameplay action wins; the current global
  `page_back` gesture is the only system navigation allowed to escape it.
  Other unmapped system-navigation actions such as `component_center` are
  acknowledged as disabled instead of falling through and closing the
  component. Outside a component, those global actions keep their normal role.
- `input/config-query`: returns the complete authoritative input map on
  `input/config-state`, correlated by `requestId`.
- Input configuration and component inventory requests keep their large
  scratch state outside the UART/USB receive-task stack. Receive tasks only
  frame bytes on 8 KiB stacks; a dedicated 12 KiB protocol worker serializes
  bounded responses without sharing the display call stack.
- `control/command` with `type: audio_bridge`: enables or disables board-microphone capture.
- `audio/control`: starts or stops capture for diagnostics; `audio/query` returns
  the active capture codec plus ES8311 playback state on `audio/status`.
- `asset/slot-query`, `asset/activate`, `asset/begin`, `asset/chunk`,
  `asset/raw-chunk`, `asset/file`, `asset/commit`,
  `asset/patch-commit`, `asset/stat`, `asset/abort`: transactional appearance
  transfer lifecycle plus exact cached-pack discovery/reactivation. A cancelled
  host transfer must send `asset/abort` so the device discards the incomplete
  inactive-slot staging data. Pet Manager records the transaction, file,
  raw-chunk checksum/retry, device acknowledgement, and terminal result as a
  bounded JSONL log under the current app-data `logs/usb-transfer.jsonl` path.
- `widget/begin`, `widget/chunk`, `widget/commit`: installs a bounded declarative
  `.clawpkg` subset and replies on `widget-install-ack`. The P4 runtime rejects
  unknown grammar, fetchers, readers, unsupported package files, and inputs that
  exceed the advertised limits. Every chunk carries `decodedSize` and an FNV-1a
  `checksum`; the chunk ACK echoes `path` and `index`, so the PC can reject stale
  acknowledgements and retry transient corruption without advancing another file.
  Pet Manager serializes bulk installs, and a stale transfer cannot invalidate the
  currently active staging transaction.
  Successful installs are retained in a 32-slot physical SPIFFS catalog; the
  reported visible capacity accounts for retained hidden legacy data. A
  slot keeps two bounded package generations, and the catalog itself keeps two
  checksummed, monotonically sequenced snapshots. An existing `widgetId` is
  written to the package generation not referenced by the current catalog; a
  new id uses the first free slot. Firmware reopens and validates both files
  from Flash before one new catalog snapshot makes the package visible. The
  previous snapshot and package generation remain a boot-time rollback point,
  and startup selects the newest snapshot whose complete package set validates.
  Firmware advertises this behavior as
  `capabilities.componentCatalogGeneration=true`. The active component id is
  part of the same snapshot, so install, delete, and selection never expose a
  mixed widget/buttons/catalog/active-id state. Commit may take several seconds
  when SPIFFS garbage collection is active; firmware yields between writes.
  For the built-in `token-usage` package only, Pet Manager removes its Linux host
  reader/fetcher declarations before transfer. Firmware then updates its four
  declared display variables from the validated bounded stats model; arbitrary
  third-party readers and fetchers remain rejected. Chunk retries are idempotent,
  and a failed integrity check rolls back the partial chunk before retrying.
  A tick may use paired `when` and `then` objects for bounded completion logic:
  `when` names an integer variable and an inclusive `lte` threshold, while `then`
  may select a declared state and apply the same bounded `set`/`inc` effects as a
  transition. Each transition or tick top-level effect group accepts at most four
  combined `set`/`inc` entries; a tick's `then` group has its own four-effect
  limit. State/page changes do not consume an effect slot. This lets timers stop
  or advance phases without arbitrary code.
- `widget/delete`: optional `preserveSave:true` requires advertised
  `widgetDeletePreserveSave=true` (0.7.99+). It retains private device saves under
  their original component ID for same-ID reinstall; packages must be backed up
  separately. It does not export saves or touch PC private records. Omitted/false
  keeps the existing erase-save semantics. Unsupported clients must not assume
  this policy on an older board.
  Removes the exact installed catalog component idempotently and replies
  on `widget-install-ack` with the same `transferId` and `phase: "delete"` only
  after its package and catalog entry are cleared and the screen returns to
  `main`. Deleting the active entry selects the nearest remaining component
  when possible; deleting another entry does not interrupt the active app.
  Firmware that advertises `capabilities.widgetDelete=true` implements this
  contract.
- `widget/list`: requires a bounded `requestId` and replies on
  `widget/inventory` with that same identity. P4 advertises
  `capabilities.widgetInventory=true`, `supportsMultiple=true`, and
  `maxInstalled=32` on 0.7.85+ (16 on earlier builds); the response enumerates the persisted catalog, marks the
  active component, and uses each bounded runtime title as its optional name.
  Kind and version remain `null` because `component.json` is not persisted.
- `miniapp/event`: dispatches a declarative action directly to the active app.
  Eligibility, host freshness and confirmation deadlines use the device's
  monotonic clock; an optional legacy desktop `tsMs` is not used for timing.
- `app/event`: bounded application-host request containing `boardDeviceId`,
  `componentId`, ephemeral `instance`, increasing `sequence`, `action`, `page`,
  and `visible`. The desktop requires the connected board identity, deduplicates
  actions, and invalidates queued actions on exit/switch/reentry. Status polls
  are not permission to record, play, or dispatch desktop work.
  `app/snapshot` replies echo component/instance/sequence and bounded dashboard
  values; an earlier status cannot overwrite a newer action. No automatic action
  retransmission occurs on timeout. Component-host heartbeats bypass the work
  queue so a slow action cannot keep playback alive after exit.
- `miniapp/query`: returns the active bounded view model on `miniapp/state` for
  PC status display and hardware diagnostics. A configured hardware action can
  explicitly proxy `screen.region.tap` or `screen.region.long_press` to the active
  app, so widgets remain operable without reserving SW1 or another physical input.
- `asset/*`: accepts current `p4-h264-v1` Annex-B streams plus optional
  `p4-pcm-wav-v1` cues under `p4/audio/`. Legacy `p4-mjpeg-v1` remains
  readable for compatibility; Linux `videos/*.mp4` or `videos/*.wav` assets
  are rejected explicitly.
- `firmware/begin`, `firmware/chunk`, `firmware/commit`, `firmware/abort`:
  writes a SHA-256-verified ESP-IDF image to the inactive 3MiB OTA slot in the current factory layout. Legacy devices retain their physical 2.5MiB slots until an explicit USB reprovision; the desktop checks actual slot capacity and retains 128KiB headroom before starting. The
  desktop uses 4092-byte padding-free decoded chunks when schema 5 and the
  advertised 4KiB receiver are both present, otherwise it starts at the legacy
  2046-byte size. A transient Base64 rejection retries the same sequence at the
  current size; three repeated rejections reduce it through 2046, 1020, then
  510 bytes without restarting the transaction, and 32 successful chunks
  restore the next larger size. Schema-5 firmware has an idle-clock race, so
  the desktop starts at 510 bytes, promotes through the larger sizes after
  stable ACK streaks, and automatically rebuilds a transaction if the old
  firmware clears it. Schema-6 firmware starts directly at 4092 bytes. The firmware
  timeout reaper ignores a loop timestamp older than the most recent chunk so
  unsigned subtraction cannot clear a healthy transfer. Before a new update,
  Pet Manager queries
  `firmware/status`; if a disconnected host left an active transaction, it
  aborts that exact reported `transferId` before starting the replacement.
  Every upload is also pinned to its original USB connection generation, so an
  old worker cannot resume onto a newly opened serial connection. Lost begin
  acknowledgements are aborted and retried within the same update action.
  During firmware and appearance writes the renderer uses a lightweight
  transfer-only screen that does not read changing assets, instead of leaving
  the previous frame apparently frozen. After reboot the backlight remains
  hidden through built-in component migration and is revealed only after the
  first complete frame has rendered.
- `firmware/query`: returns running, boot, and next partition metadata on
  `firmware/status`, including the active `transferId`, byte/sequence progress,
  and an echoed optional `requestId` for exact desktop correlation.
- `diagnostics/query`: returns boot/reset history, heap and PSRAM low-water
  marks, SPIFFS usage, task/GPIO/touch/audio health, joystick
  center/current/minimum/maximum ADC samples, current page,
  `sessionQueueCount`, ordered `sessionQueueIds`, `currentSessionId`,
  `retainedSessionCount`, firmware, `buildId`, `gitSha`, `buildDirty`,
  `protocolSchema`, and running partition on
  `diagnostics/status`.
  `memory` additionally separates `freeInternalBytes`, `minimumFreeInternalBytes`,
  `largestInternalBlockBytes`, `freeDmaBytes`, `largestDmaBlockBytes` and
  `largestPsramBlockBytes`: free PSRAM is not interchangeable with internal/DMA RAM.
- `system/reset-inputs`: restores the safe SW1/SW2/SW3/encoder defaults and
  replies on `diagnostics/action`. It does not erase appearance assets.
- `system/reboot`: acknowledges on `diagnostics/action`, then performs a delayed
  software restart so the response can leave the UART first.

Current Session example:

```json
{
  "topic": "session/current",
  "payload": {
    "sessionId": "019f...",
    "title": "修复设备端 Session 名称显示",
    "index": 2,
    "count": 3,
    "sessions": [
      {"id":"019f-a","title":"修复设备端白屏","state":"done","transitionRevision":1740000001000,"terminalRemainingMs":42000},
      {"id":"019f-b","title":"完善会话切换","state":"working","transitionRevision":1740000002000,"terminalRemainingMs":0},
      {"id":"019f-c","title":"验证语音输入","state":"waiting_user","transitionRevision":1740000003000,"terminalRemainingMs":0}
    ],
    "agentId": "codex",
    "activeSessionIds": ["019f-b", "019f-c"],
    "displayEnabled": true,
    "notice": "已切换到下一个会话"
  }
}
```

For the compatibility action IDs `session_previous` and `session_next`, current
firmware resolves the action inside the visible peer page. On `main` it reports
the selected Session metadata so the desktop follows the same card. On
`components` it reports `component_select`, so desktop Session routing does not
change:

```json
{
  "topic": "input/event",
  "payload": {
    "action": "component_select",
    "context": "components",
    "handledLocally": true
  }
}
```

## A/B Firmware Update

The PC serializes firmware updates with other large transfers and binds every
request to the board id that opened the dialog. It parses the ESP-IDF app
descriptor before upload and accepts only `pet_manager_p4_runtime`. It sends
4KB decoded chunks and waits for an ACK whose next sequence and byte count
match that exact chunk. A missing ACK retries the same sequence up to three
times; the board treats the immediately previous sequence and commit as
idempotent retries. The board independently validates total size, chunk
sequence, SHA-256, and project identity before selecting the inactive slot.

```json
{"topic":"firmware/begin","payload":{"transferId":"firmware-42","size":1402704,"sha256":"<64 hex chars>"}}
{"topic":"firmware/chunk","payload":{"transferId":"firmware-42","seq":0,"decodedSize":4096,"data":"<base64>"}}
{"topic":"firmware/commit","payload":{"transferId":"firmware-42"}}
```

Every phase replies with the current byte and sequence counters:

```json
{"topic":"firmware/ack","payload":{"transferId":"firmware-42","phase":"chunk","ok":true,"receivedBytes":4096,"expectedBytes":1402704,"nextSequence":1,"targetPartition":"ota_1"}}
```

After commit, the device reboots into the selected slot. A new image remains
`pending_verify` until repeated LCD render samples stay healthy for 8 seconds;
the desktop reports success only after the same board reconnects with the
requested version, target partition, and `imageState: valid`. If the sustained
health window never completes within 20 seconds, the bootloader rolls back.
An interrupted transfer is aborted after 30 seconds of inactivity and resumes
normal rendering. The board keeps the committed runtime alive for 5 seconds
before restart, leaving enough time for the desktop's 3-second commit timeout
and one idempotent retry to receive the cached commit result.

The A/B application image contains a bounded, compressed bootstrap catalog.
Complete artwork belongs to factory/PC resource packages, not duplicated OTA slots.
`pet_p4_miniapp_sync_builtins` adds missing entries, never downgrades an existing
complete artwork package to a bootstrap, removes retired entries, and restores
the active id (otherwise the first visible entry). The unreleased canonical list
in `factory-config.json` has 26 entries, with `daily-news` fifth and `city-radio`
first. User-created packages remain ahead of firmware defaults; later installs
prepend normally. `two-key-pong` and `bloomfrog_companion` remain available.
Migration reorders catalog records without moving slots or saved data. A build-id
and bundle-checksum marker is written only after successful completion; an
interrupted migration retries on the next boot. Component ids outside the firmware-owned list are preserved.
Use `tools/read_component_inventory.py` only with an idle serial port to compare
the actual installed inventory; a bundled count does not prove installation.

Firmware OTA, asset OTA, and delayed reboot are mutually exclusive. Once an
OTA commit or diagnostic reboot is pending, new asset/native transfers are
rejected until the board restarts. Boot validation samples LCD/render and both
transport RX tasks independently of asset rendering, so a long asset transfer
cannot accidentally starve the health window or mark a bad image healthy.

## Hardware Input Configuration

Inputs are sampled every 5ms. Buttons are active-low with internal pull-ups,
25ms debounce and a 700ms long-press threshold. On current hardware the
joystick X/Y axes use ADC1 GPIO21/GPIO20. The runtime calibrates their neutral
center at boot, derives activation/release hysteresis independently from the
available travel in each direction, resolves diagonals by normalized dominant
axis, and repeats a held direction after a bounded delay. This accepts
limited-travel joystick batches without weakening the neutral dead zone. The
legacy EC11 decoder on GPIO2/GPIO3 remains active, so the same firmware also
supports the previous board revision. Firmware queues input events outside
GPIO and render callbacks, and stores accepted mappings in NVS.

PC to board:

```json
{
  "topic": "input/config",
  "payload": {
    "requestId": "input-42",
    "version": 9,
    "bindings": [
      {"event":"button.sw1.short_press","action":"page_enter","value":""},
      {"event":"button.sw1.long_press","action":"disabled","value":""},
      {"event":"button.sw1.hold","action":"voice_ptt","value":""},
      {"event":"button.sw2.short_press","action":"component_center","value":""},
      {"event":"button.sw2.long_press","action":"disabled","value":""},
      {"event":"button.sw2.hold","action":"disabled","value":""},
      {"event":"button.sw3.short_press","action":"page_back","value":""},
      {"event":"button.sw3.long_press","action":"disabled","value":""},
      {"event":"button.sw3.hold","action":"disabled","value":""},
      {"event":"button.encoder.short_press","action":"page_enter","value":""},
      {"event":"button.encoder.long_press","action":"disabled","value":""},
      {"event":"button.encoder.hold","action":"disabled","value":""},
      {"event":"knob.rotate_ccw","action":"session_previous","value":""},
      {"event":"knob.rotate_cw","action":"session_next","value":""},
      {"event":"joystick.up","action":"session_previous","value":""},
      {"event":"joystick.down","action":"session_next","value":""}
    ]
  }
}
```

Allowed actions are `disabled`, `voice_ptt`, `agent_enter`, `agent_prompt`,
`session_next`, `session_previous`, `session_clear`, `miniapp_screen_tap`,
`miniapp_screen_long_press`, `component_center`, `miniapp_action`,
`page_toggle`, `page_enter`, `page_back`, `page_main`, and `page_app`.
The firmware keeps this full set for stored-config and component compatibility.
Pet Manager's P4 button menu exposes only custom prompt, voice input,
previous/next item, clear sessions, pet/component page toggle, confirm, back/cancel,
and unbound.

SW1 short press and joystick center short press both default to `page_enter`:
they activate the selection and open `app` only from `components`; they do not
open the catalog from `main`. SW3 short press defaults to the global
`page_back` path. Center long press defaults to `disabled`; joystick Up/Down
default to `session_previous`/`session_next`. All remain editable and are
persisted with the rest of the input map. SW2 short press toggles `main` and
`components` in both directions. Other SW short/long gestures
default to `disabled`, except SW1 long
press, whose hidden `.hold` transport defaults to `voice_ptt`. Joystick left
and right deliberately retain `knob.rotate_ccw` / `knob.rotate_cw` event names,
so old component packages continue to work without conversion. Inside the
catalog, previous/next selects the prior/following catalog entry. On `main`, the
same actions select Session bubbles. Neither action changes the two-dot peer
page; SW2 does that. While `app` is open, component bindings—including SW2—retain priority.
New packages may bind `joystick.up` and `joystick.down`.

`page_toggle` switches between `main` and `components`; from `app` it returns to
`components`. `page_main` and `page_app` remain accepted for older persisted
device configs. Component packages no longer own navigation: PC downlink removes
legacy `page_main/page_back` records, and firmware ignores any such records that
remain in an already-installed package. While `app` is open, whichever persisted
global event currently maps to `page_back` is resolved before component gameplay
bindings, so changing the global exit key immediately changes every component.
The default remains SW3 short press; SW1 long-press PTT is unaffected. The two
legacy-named session actions now select content within the current peer page. The two mini-app proxy actions only run while
the app page is open and dispatch its existing `screen.region.tap` or
`screen.region.long_press` binding. Custom values are limited to 159 UTF-8 bytes.
The board replies on `input/config-ack`; legacy config uses `button-config-ack`.
The desktop reads the NVS-backed device map after opening the dashboard or
reconnecting USB. This read waits for any firmware or asset transaction to
finish before using the serial link:

```json
{"topic":"input/config-query","payload":{"requestId":"device-request-42"}}
```

The board correlates the response and returns the complete persisted map. The
desktop compares it with the complete PC mapping; any difference is resolved by
writing the PC mapping to the board. The board response never replaces the PC
cache, and a failed read falls back to the same full PC write:

```json
{
  "topic": "input/config-state",
  "payload": {
    "requestId": "device-request-42",
    "ok": true,
    "boardDeviceId": "p4-<board-id>",
    "runtime": "esp-p4",
    "bindingCount": 16,
    "config": {
      "version": 7,
      "voiceEnabled": false,
      "voiceButton": "",
      "bindings": [
        {"event":"button.sw1.short_press","action":"page_back","value":""}
      ]
    }
  }
}
```

Pet Manager places the P4 device illustration above four matching control
groups and exposes twelve logical gestures: short/long press for SW1, SW2, and
SW3, plus joystick center short/long press and four directions. Left/right keep
the counter-clockwise/clockwise compatibility event names. The internal `.hold`
rows are never shown separately.
A visible long-press row is encoded as two mutually exclusive board bindings:
`.long_press` for one-shot actions and `.hold` for `voice_ptt` start/end.
SW1 long press defaults to `voice_ptt`; the shared switch in Button
Configuration and Voice Assistant decides whether its `.hold` binding is
`voice_ptt` or `disabled`. Turning voice off retains the visible client
selection but sends both SW1 long-press bindings as `disabled`. The complete
ten-row client model therefore remains a fourteen-binding board map. Config
version 3 migrates version-2 values that exactly match either previous default
layout to this map while preserving unrelated custom actions.
While `app` is open, the persisted global `page_back` event is resolved first.
Other events declared in the component's `buttons.json` then resolve as gameplay
or tool actions; outside `app`, or for an undeclared component event, the
persisted device-page mapping remains authoritative. Package-authored system
navigation actions are ignored. A component-declared `.long_press` also
suppresses the matching global `.hold` while that component is open, preventing
an editable PTT binding from firing underneath the component action.

Board to PC:

```json
{
  "topic": "input/event",
  "payload": {
    "version": 3,
    "seq": 17,
    "boardDeviceId": "p4-<board-id>",
    "control": "key.1",
    "gesture": "hold_start",
    "event": "button.sw1.hold",
    "context": "main",
    "action": "voice_ptt",
    "handledLocally": false,
    "dropped": 0
  }
}
```

## Touchscreen Gestures

GT911 is sampled every 15ms without driving reset or interrupt GPIOs. The local
board profile declares separate LCD/touch reset pins; that is not proof that
the installed panel routes both pins. Startup probes both legal addresses up
to four times, with bounded delays, without resetting the shared audio I2C bus.
Physical portrait coordinates are converted into the 640x480 logical UI.
For the installed CST826 panel, physical testing showed the sensor axes are
180 degrees from the LCD native axes: its board-specific mapping is
`uiX=639-rawY, uiY=rawX`. GT911 / FT6336U keep the existing mapping. This is
panel orientation, not a universal CST826 property. The diagnostic page shows
a persistent marker while pressed; health reports raw/mapped coordinates,
queued-pointer age and stale-event counts to separate mapping from latency.
If both GT911 addresses are absent, the product probes `0x15` and `0x38`.
CST826 requires chip type `0x11` at register `0xAA`; FT6336U requires chip ID
`0x64` at register `0xA3`. These enable the bounded six-byte-point report reader;
an unknown responding device is rejected. This path reads touch reports only,
without changing controller tuning registers. The register format and ID follow
[Espressif's FT6336U driver](https://github.com/espressif/esp-iot-solution/blob/master/components/display/lcd_touch/esp_lcd_touch_ft6336u/esp_lcd_touch_ft6336u.c).
The CST826 identity and report layout are documented in
[Adafruit's CST8XX driver](https://github.com/adafruit/Adafruit_CST8XX_Library).
Register `0xA3` returning zero alone does not rule out CST826.
`touchInput.controller` reports the selected controller, not a hardcoded GT911.
Tap and long press route to `screen.region.tap` and
`screen.region.long_press`; four-direction swipes move from main to the component
center, from the component center to main, or from an app to the component center.
Realtime conversation owns its history scrolling. Main-page tap feedback selects a bounded `touch.*` family but cannot
interrupt `waiting_user` or `error`.

Each recognized gesture is emitted on `input/event` with control
`screen.touch`, logical `x`/`y`, `dx`/`dy`, duration, local handling status,
and the cumulative dropped-event count. Diagnostics separately reports
`touchReady` and `touchDroppedEvents`. `runtime.touchHealth` includes initialization
stage/error, selected address, both probe results, sample/contact counts and poll
error counters. Three consecutive read failures report touch unavailable until
a successful poll. Read errors, ambiguous/multiple contacts and queue overflow
cancel a stroke rather than synthesizing a swipe or connecting stale points.

### Native touch diagnostic (not a PetUI package capability)

`diagnostics/touch-test` with `{"action":"start"}` enters a temporary single-fruit
slicing test only when touch is ready and no conversation/transfer is active.
`{"action":"stop"}` returns home. Replies use `diagnostics/action`; the test
expires after 120 seconds. SW1 restarts, with configured global Back/realtime
bindings taking priority. A captured stroke never becomes global swipe navigation.
Points stay local (no per-sample USB traffic); score appears in touch diagnostics.
The native prototype uses swept hit testing, bounded trails and three-miss
settlement. This diagnostic is independent of installed components.

### PetUI touch slicing

`capabilities.widgetScenes` now includes `p4-touch-slice-v1`, with
`widgetPointer: "p4-pointer-v1"`. The desktop permits installation when optional touch support is unavailable;
this does not establish that touch gameplay is usable. Runtime interaction still
requires a ready controller. Package schema, integrity and resource limits apply.
`widgetSliceRevision: 2` adds three selectable modes, radius-based collision,
golden fruit and bounded double-score energy. A package declaring
`scene.revision: 2` requires revision-2 runtime behavior for full gameplay.
The authoring validator checks the declared target revision. Desktop installation
does not turn missing optional capabilities into a readiness guarantee; installed
packages may have reduced or unavailable gameplay on older devices.
Top-level `p4-bounded-runtime-v4` packages can select this bounded scene and
provide four PNG fruit sprites, a 30–120 second duration, 1–5 lives and a
0–100 effect volume. Package scripts and executable assets remain forbidden.
The strict schema and original component generation workflow are documented in
`../pc/skills/petui/references/touch-slice.md`.

The native core owns fixed-size fruit, particle and trail pools, continuous
segment collision, combo scoring, bomb priority and settlement. Touch remains
local rather than streaming through USB. Global exit, conversation and transfer
screens suspend play; resuming requires the pause action. Sounds share the
existing audio task and stop on pause/exit. The PC preview and skill smoke test
run the same C core compiled into a bundled, import-free, 128 KiB WASM module;
no code is loaded from component packages.

For hold bindings, `hold_start` is emitted only after the 700ms threshold;
`hold_end` is emitted on release. Page actions set `handledLocally: true`.

## USB PTT Audio

The default `button.sw1.hold -> voice_ptt` mapping starts board-microphone capture on
`hold_start` and stops it on `hold_end`. Audio is PCM S16LE, 16kHz, mono, in
20ms (640-byte) frames. Capture stops automatically after 30 seconds.

Board to PC stream:

```json
{"topic":"audio/begin","payload":{"sessionId":"p4-audio-123-1","boardDeviceId":"p4-...","sessionQueueEmpty":true,"format":"pcm_s16le","sampleRate":16000,"channels":1,"bitsPerSample":16,"frameMs":20,"transport":"usb-jsonl-pcm-v1"}}
{"topic":"audio/chunk","payload":{"sessionId":"p4-audio-123-1","seq":0,"bytes":640,"checksum":"0123456789abcdef","data":"<base64>"}}
{"topic":"audio/end","payload":{"sessionId":"p4-audio-123-1","reason":"released","chunks":50,"bytes":32000,"durationMs":1000,"checksum":"0123456789abcdef"}}
```

`sessionQueueEmpty` freezes whether the device had zero visible cards at the
instant capture started. When true, the PC still resolves the target from its
independent P4 Session binding; Codex and Claude use their visible composer path
to foreground that Agent's current bound Session before live text updates and
submission. Older firmware that omits the field is treated as `false`.

Each chunk checksum is FNV-1a 64-bit over the decoded frame. The final checksum
is FNV-1a over the complete PCM stream. The PC rejects session, sequence, byte
count, or checksum mismatches. P4 PCM is retained for bounded Windows zh-CN
recognition without opening the PC microphone; legacy boards may still use the
local voice-service relay at `127.0.0.1:50001`. `audio/error` terminates the
current session. `audioCapture.codec` reports `ES7210`, `ES8311`, or
`unavailable` according to the initialized capture path.

## Realtime Conversation Audio

Firmware 0.7.57 implements a realtime persona conversation mode on top of the PTT
stream. The PC owns VAD, ASR, the persona LLM and TTS; the board only captures,
plays, and shows a HUD.

PC to board:

```json
{"topic":"audio/conversation","payload":{"enabled":true,"halfDuplex":true,"sessionId":"rtc-1"}}
{"topic":"audio/play_begin","payload":{"sessionId":"rtc-1-turn","sampleRate":16000,"channels":1,"format":"pcm_s16le"}}
{"topic":"audio/play_chunk","payload":{"sessionId":"rtc-1-turn","seq":0,"bytes":3200,"data":"<base64 PCM S16LE 16 kHz mono>"}}
{"topic":"audio/play_end","payload":{"sessionId":"rtc-1-turn","bytes":64000}}
{"topic":"audio/play_flush","payload":{"sessionId":"rtc-1"}}
{"topic":"ui/conversation","payload":{"sessionId":"rtc-1","state":"listening","name":"小西","text":""}}
```

- `audio/conversation` with `enabled:true` starts continuous microphone capture
  (same `audio/begin` / `audio/chunk` / `audio/end` framing, `audio/begin`
  carries `"conversation":true`, no 30 s cap). With `halfDuplex:true` the board
  keeps reading the microphone but drops frames while the speaker is playing and
  for 250 ms afterwards. `enabled:false` stops capture, flushes playback and
  clears the HUD.
- `audio/play_begin` opens a streamed playback turn; `audio/play_chunk` carries
  up to 4096 decoded bytes per line into a 3 s ring buffer; playback starts once
  200 ms are buffered (or immediately on `play_end`), and `play_flush` drops
  everything for barge-in / session end. State WAV cues yield while a stream is
  active. `audio/status` gains `conversation`, `streamActive` and
  `streamBufferedMs`.
- `audio/diagnostic` reports metadata-only lifecycle/error events (event, sessionId,
  sequence, bytes, bufferedMs, uptimeMs, ok). The PC persists these alongside its
  own realtime diagnostics; no PCM or dialogue text is logged. `audio/status.playbackVolume`
  reports the codec volume readback (100 / 0 dB in 0.7.57, shared by v1 and v3). `audio/status`
  adds `streamUnderruns` and `streamReceivedBytes`.
- `capabilities.features.conversationSubtitles:true` advertises synchronized captions.
  `ui/conversation` accepts `sessionId`, `state`, `name`, `text`, `role` (`user` or
  `pet`) and `final` (boolean). It owns the main-page bubble while active; Agent
  session cards and statistics remain hidden but continue updating in the background.
  The existing card style uses a blue outline. Listening displays live ASR text;
  replies display only the pet's current sentence, never a retained user summary.
  `state:"error"` displays `text` as a failure hint for 8 seconds. The animation
  family follows the state — `listening`
  → `waiting_user`, `thinking` → `working`, `speaking` → `welcome`. `state:"ended"`
  clears it. Host disconnection also clears captions and restores Agent display.
- `audio/play_subtitle` carries `{sessionId,offsetBytes,text}` after `audio/play_begin`
  and before its sentence's PCM. The ID must match the playback turn, and the offset
  is an even unsigned byte count from that turn's beginning (16 kHz mono S16LE).
  The device advances captions using consumed PCM, not elapsed wall time. Up to 12
  pending cues and 8 finalized captions of at most 511 UTF-8 bytes are held in RAM;
  overflow/invalid cues are rejected. `audio/play_flush` discards pending cues.
  Joystick/touch navigation only reviews this bounded history; new captions restore
  the live view. Exiting clears all caption/history text. No dialogue text is logged.
- The `realtime_chat` input action is emitted as a normal `input/event`; the PC
  starts conversations only from `main` (the pet screen). While active, it consumes
  all input from that board; the realtime shortcut or configured back action ends
  the conversation, and other keys cannot submit text or change Agent sessions.

## Protocol ACK/NACK

While USB serial is connected, Pet Manager sends `system/heartbeat` every two
seconds. The board considers the desktop disconnected after six seconds without
any valid host message. This drives the on-device waiting, initialization, and
disconnected status banner (`客户端未连接`); normal pet playback remains
unobstructed once the host is bound and an appearance pack is available.

Supported non-streaming messages that include `payload.requestId` receive a
`protocol/ack` success response. Malformed, unknown, and recognized-but-not-yet-
implemented topics receive an immediate NACK instead of being ignored:

```json
{
  "topic": "protocol/ack",
  "payload": {
    "requestTopic": "control/apply-wifi",
    "requestId": "req-42",
    "ok": false,
    "code": "unsupported_topic",
    "error": "topic is not supported by this firmware"
  }
}
```

Unrecognized topics use `code: "unknown_topic"`; malformed envelopes use
`invalid_json` or `invalid_message`. Asset transfers continue to use the more
specific `asset/ack` contract.

## Speech Payload Shape

The PC can reuse the legacy Linux runtime speech card fields:

```json
{
  "topic": "speech/text",
  "payload": {
    "displayTitle": "pet dev",
    "displayContent": "Building the P4 runtime...",
    "status": "working",
    "statusText": "thinking",
    "tsMs": 1780000000000
  }
}
```

The firmware also accepts `sessionName`/`title` for the title and `text`/`message`
for the body, so older bridge payloads still render.

## Statistics Payload Shape

The PC forwards the active Agent state with token usage and bounded runtime
metrics. When `dailyTokenUsage` is available, it also sends that
current-followed-Agent daily aggregate on `runtime/stats`, independently of the
device bubble's selected Session. The regular state compactor continues to
prefer that aggregate over a single Session's `tokenUsage` for compatibility.
The P4 parses the normalized fields when the message arrives, then renders from
a fixed-size model without retaining an unbounded JSON tree, and replays the
model whenever the Token mini-app is activated. Changing the followed source
clears stale stats before the new source is applied.

```json
{
  "topic": "stats/update",
  "payload": {
    "source": "codex",
    "state": "working",
    "sessionTitle": "pet dev",
    "tokenUsage": {
      "totalTokens": 18432,
      "inputTokens": 13000,
      "outputTokens": 2432,
      "cachedInputTokens": 3000,
      "estimatedCostUsd": 0.0421,
      "modelContextWindow": 128000
    },
    "metrics": {
      "latency": {"turnMs": 12340, "firstTokenMs": 820},
      "toolCalls": 4,
      "toolErrors": 0,
      "waitingUserMs": 0,
      "contextUsagePct": 14.4
    },
    "tsMs": 1780000000000
  }
}
```

## Asset Direction

The Linux runtime consumes MP4/WAV. The P4 runtime consumes preprocessed packs:

- `p4/manifest.json` with format `p4-h264-v1`.
- `p4/families/sha256-<digest>.h264` H.264 Annex-B streams.
- Each family is aspect-fitted into a 640x480 canvas with black letterboxing and at most
  225 frames. Clips up to 15 seconds retain the 15 fps ceiling; longer clips
  lower the sampling rate so those frames span the complete source timeline.
- Every family records `fps` for older-firmware fallback and exact
  `durationMs` timing. Firmware selects frames proportionally across that exact
  duration; `frameDurationMs` remains available as an older-firmware fallback.
- FFmpeg exports yuv420p Baseline H.264 at CRF 27 with no B-frames. The desktop
  and packaging prebuilder then replace x264's SPS with the minimal SPS proven
  against `esp_h264`; device synchronization only validates and transfers the
  finished ready pack and never invokes FFmpeg.
- Current public R178 images use 4 Mbaud UART for both Windows and macOS.
  `asset/raw-chunk` carries a bounded JSON header followed by binary data with
  a per-chunk FNV-1a check. The host selects bounded, paced writes using the
  negotiated capability; a device maximum is not a required host chunk size.
  Exact transaction/path/index receipts are required before advancing.
  A timeout is an unknown mutation result, not proof that the write failed.
  The desktop does not automatically flush/replay that mutation, abort the
  transaction or restart with another chunk size. Read-only identity/status
  reconciliation and a new explicit operation are separate from retrying an
  unknown write. Compatibility probing for older baud rates is not permission
  to resume an unconfirmed transfer. Video data uses the dedicated appearance
  slot; manifest/audio storage and commit rules remain separately checked.
- Older firmware without `transport.rawAssetChunks` continues to receive
  20,478-byte pre-Base64 `asset/chunk` JSON Lines. File checksum commits remain
  authoritative for both transports.
- Native USB vendor bulk can query and activate an exact pack already cached
  on the device. Until native USB reuses the dedicated raw-partition writer, a
  cache-miss pack containing H.264/MJPEG is rejected by the desktop before the
  first full-sync write; USB-UART is the supported cache-miss path. Every
  native mutation is bound to the requested `boardDeviceId` through the
  challenge-response handshake above, including when several matching
  `303A:4040` devices are connected.

Example manifest:

```json
{
  "format": "p4-h264-v1",
  "packId": "<sha256-of-sorted-payload-paths-and-bytes>",
  "codec": "h264",
  "container": "annex-b",
  "width": 640,
  "height": 480,
  "fps": 15,
  "families": [
    {
      "family": "idle.default",
      "path": "p4/families/sha256-<digest>.h264",
      "frames": 225,
      "streamBytes": 738259,
      "fps": 15,
      "frameDurationMs": 68,
      "durationMs": 15130
    }
  ]
}
```

The PC must not treat MP4/WAV OTA as successful on P4. The board accepts only
`p4/...` logical paths and returns explicit error acks for Linux asset paths.

The manifest is parsed into a fixed-capacity catalog only when an asset slot is
loaded or committed. Runtime selection plays `welcome` once, rotates available
`idle.*` and `working*` families only after each selected clip reaches its own
full duration, maps `waiting_user`, `done`, and `error` directly, and prefers
`working.browsing` while speaking when that family exists. Every family switch
starts playback at frame zero; touch and welcome reactions are not clipped by a
fixed five-second lifecycle limit. When Session cards are present, this family
selection uses the current card's lifecycle rather than the aggregate Agent state.
Selected content-addressed H.264 streams are loaded into bounded PSRAM cache
slots and decoded sequentially. A timeline loop or family change reopens the
decoder at frame zero, so lifecycle changes interrupt the old action on the
next render cycle while uninterrupted actions retain their real duration.
YUV conversion always targets the logical 640x480 RGB565 framebuffer before a
separate 270-degree rotation. The former combined direct-native conversion is
not used. Two PSRAM native-output buffers alternate between frames, preventing
PPA writes for the next frame from mutating memory still being copied by the
LCD DMA path after a delayed refresh.
Session-card overlays are likewise cached by queue content and selected index,
with only animated working markers redrawn per frame.
# 公共状态提示音（0.7.63+）

设备能力 `appearance.systemCues: true` 表示默认完成、错误、等待用户音效由固件公共资源提供，不依赖形象内是否存在对应视频。完成音保持四组、每组三声。新形象 manifest 可声明 `systemCues: true`，默认不含 WAV；`families[].audioPath` 配合 `audioSource: "custom"` 表示专属音效，缺失/无效时公共音效兜底。旧 manifest 保持兼容，已知旧默认 WAV 按内容指纹迁移，不按文件名猜测用户音效。

内置形象切换使用现有 `asset/slot-query` 与 `asset/activate`：仅对设备声明 `builtinProtected` 的内置形象请求，选择有效 slot 0，并使用设备返回的 packId 激活；不要求与 PC 内置 packId 相等。槽位无效时停止并报错，不能回退成写入自定义槽位。普通自定义包仍严格校验 packId。
# 原生敲击组件能力（0.7.70-p4）

设备通过 `widgetInstrument: p4-instrument-v1` 声明本地敲击音源支持。受控组件使用 `instrument.source = percussion.wooden-fish`，`instrument.strike` 只由绑定按键触发；`effect_volume` 和 `ambience_volume` 为 0–100 的整数。缺少能力声明时 PC 阻止安装并提示升级，不模拟成功。组件退出淡出，录音、实时对话和媒体流优先；音频不需要新增 USB 流协议。
# PC 扩展媒体布局（p4-media-surface-v1）

设备声明 `widgetMediaNotice:true` 时，支持 `media/notice`：`{source,state,message,requestId}`。
state 只接受 idle/buffering/error，message 最多 240 UTF-8 字节；source 必须匹配当前媒体视图。
仅在媒体未播放时更新提示，不改变音频所有权、播放列表或音量，也不续期媒体看门狗。
回复沿用 media/status；不匹配 source 或非法字段拒绝，正在播放时忽略通知。

`media.source=pc.<component-id>`、`mode=live` 随 media/library、media/begin 和 media/event 标识所有者。设备通用 surface 由包声明 panel/text/meter/ticks/list/transport，最多 20 个节点、640×480、RGB565、无脚本。`media.sync` 通过列表操作请求 PC 同步；歌词与 seek 不适用于 live。全局停止事件带 `global:true`，PC 取消所有扩展的播放/准备，不取消实时语音。PC 配置不下发；契约详见 `pc/skills/petui/references/pc-extensions.md`。
# 媒体绘制性能说明（0.7.81）

`p4-surface-images-v1` 的压缩格式、配额和事务协议不变。渲染器以 PSRAM 缓存首个静态全屏背景，在图片修订改变时失效；颜色预计算与 Q16 增量旋转避免逐像素浮点除法。带旋转节点的声明式媒体界面请求 33 ms 刷新，但实际帧率由设备日志验收，不作为网络音频无缓冲的承诺。
# PetUI 既有游戏精细画面（0.7.88）

能力 `widgetGameSurface=p4-game-surface-v1` 允许维护已有 snake/blocks/flappy
包以及原生 `gomoku` 预设时设置有界 `surface.game_view`。布局、图片、字体仍走 App Surface，
玩法仍走原有 C 核心。无此能力的设备拒绝安装，不静默丢掉画面。
详细契约见 `pc/docs/classic-game-surface.md`。

高清内置资源随 PC/出厂资源事务安装，不占两份 OTA 应用空间。
固件 zlib 启动包解压上限 128 KiB，原生图片启动布局仅补缺失项，
不能覆盖已安装的完整资源包。压缩数据摘要参与升级标记。

# PetUI 原生五子棋预设（gomoku）

`widgetGamePresets` 含 `gomoku` 的固件接受 `game.type="gomoku"`：标准 15×15 棋盘，
玩家执黑先行，设备执白，由同一 C 核心（`pet_p4_game.c`）在玩法 tick 内回应。
旧固件通过能力列表拒绝，不静默降级。

- 固定 action 表：`place|left|right|up|down|undo`，六项全部必填，并须同时出现在
  `transitions` 与 `buttons.json`。`place` 在未开始/已结束时开始或重开一局（与 flappy 的
  `flap` 约定一致），进行中在光标处落子；`left/right/up/down` 移动光标（边界截停，不回绕）；
  `undo` 撤回最近一回合（玩家一子及设备已回应的一子，仅一级）。白方思考期间 `place` 被忽略。
- 胜负：任一方连成 ≥5 子即结束；棋盘满且无五连为和棋。结束后画面冻结，光标隐藏，五连子高亮。
- 分数变量契约：进行中 `score_var` = 盘面手数（黑白棋子总数；奇数表示白方待回应，偶数表示轮到黑方）；
  结束后 `score_var = 结果×1000 + 手数`（1 黑胜 / 2 白胜 / 3 和棋）。dashboard 可用 `show:{var,min,max}`
  显示结果文案，`logic=p4-logic-v1` 的 tick 条件可据奇偶变化触发回应音效。
- 设备 AI：对每个空格在四条直线的 9 格窗口上按双方的小型模式表打分（连五 / 封堵对方四连 /
  活四 / 活三 等），确定性、无递归、无堆分配，每次回应最多 `225×4×9=8100` 次格读取
  （`PET_P4_GAME_GOMOKU_AI_MAX_READS`），在 `tick_ms` 节拍内完成。
- 帧单元格打包：bit0-1 棋子（1 黑 / 2 白），bit2 光标，bit3 最近落子，bit4 五连。
  `surface.game_view` 只绘制棋子、光标、落子点与五连高亮；棋盘线由资源包背景提供，
  因此背景网格须按 `cell=min(w,h)/15` 的整数格对齐视口。PC 预览与自测使用同源 WASM。

# 0.7.91 可选组件能力

- `widgetLifecycle=p4-lifecycle-v1`：原生/场景游戏声明暂停动作与独立最高分变量。
  隐藏时冻结逻辑时钟，恢复不补帧；最高分通过组件自己的存档保存。
- `widgetMediaTouch=p4-media-touch-v1`：媒体 surface 显式声明 input 后，
  可用现有七个受控 media 动作作为节点触屏目标，不接受任意指令。
- `widgetSurfaceAtlas=p4-wide-atlas-v1`：普通图集最大 512×240，宽图使用 P4I2
  的 16 位宽高；仍最多四张图、98,304 个调色板像素和 128 KiB 编译资源。
  字形图集保留 240×240 上限，P4I1/P4J1 与旧组件继续兼容。

安装端必须单独检查这些 capability。声明支持与实机已验收是两回事；
离线测试的能力 JSON 不可替代实际设备握手。
## 0.7.92 开发中的受控输入与字格能力

握手新增 `widgetInput=p4-input-v1` 与 `widgetGlyphCells=p4-glyph-cells-v1`，
安装前由 PC 校验实际握手；离线测试 fixture 不能替代握手。
输入策略详见仓库 `pc/skills/petui/references/input-policy.md`。
宿主应用快照可带 `ui:{mode,actions,context}`，最多 16 个已声明动作、64 个十六进制
上下文字节。`app/event` 的 confirmed/context 只说明设备确认了当时内容；
PC 仍须核对记录版本、设备/实例/轮次和操作状态，不自动批准其他动作。
禁用、等待和超时不产生重试；10 秒没有有效快照禁用宿主动作。
字体可声明 cell，度量仍为 32/em；旧字格解释与 palette/JPEG 总预算不变。
本版本的场景拖动仍是受控网格坐标，不声明连续物理或完整场景存档能力。

## 0.7.101 附加性能诊断（向后兼容）

`audio/status`、`audio/diagnostic` 等会话元数据增加数值字段 `internalFreeBytes`、`internalLargestBytes`、`captureTxLastUs`、`captureTxMaxUs`、`captureStackMinBytes`。发送耗时是同步串口发送耗时，不是 DSP 耗时；最大值在本轮采集开始时清零。不得将这些指标误认为端到端语音延迟或回声消除质量。

diagnostics 的 `runtime` 增加 `renderLastUs`、`renderP95Us`、`renderMaxUs`、`renderDecodeUs`、`renderWindowSamples`、`renderSubmittedFrames`、`renderUnchangedFrames`。P95/max 取最近完成的 100 帧渲染窗口；窗口尚未完成时 samples 为 0，不能解释为零耗时。相同帧省略 LCD 提交不省略游戏逻辑、输入或动画步进。

开发候选的纯数值日志另有 `P4 frame context`：同帧绝对 `start_us/end_us`、
内容/公共覆盖层墙钟和前后台操作首尾观察。`prev_end_us/gap_us` 单独报告两帧之间
的停顿，不把它冒充下一帧 CPU 耗时。phase 的位 16 表示 active，位 8–15 为操作
类型，位 0–7 为阶段；完整操作时间来自 `P4 widget trace`，
`vN=count,sum_wall_us,max_wall_us`，嵌套阶段不可相加。两条 lane 分别为组件命令
与后台存档。日志不包含组件 ID、文件路径或存档正文，不是新的客户端协议命令。
`P4 frame ppa` 记录同帧 scale/convert/rotate API 墙钟、调用至完成回调和回调至
返回时长，以及有效/缺失/迟到/错误标志。SDK 回调观察时刻不等于硬件完成时刻；
缺失和迟到不得解释为零耗时，完成后等待也不能单独证明具体调度原因。

## 0.7.103-p4-r1 资源完整性修复

开发候选的安装优化只直接写入当前目录未引用的备用 widget/buttons 代次，
原代次、存档和线缆协议格式不变；仍须完整读回、校验，再原子提交目录。
旧迁移路径与图片写入不套用该优化。不能把备用文件已存在当作安装已提交。

显式启用的 `debug/transport_audit_*` 数值观察可额外包含 `fnvAfter` 与
`hashAfterAvailable`，表示驱动调用返回后，同一调用者源缓冲的再次采样。
旧报告缺失这些字段时只能视为未观测，不能推定源缓冲稳定或物理接收成功。
前后指纹相同也只是有碰撞可能的关联证据；不因此重发业务操作。

本修复基于 PC 内置 0.7.103，不包含 0.7.108 实验分支。握手增加
`widgetResourceIntegrity: p4-resource-integrity-v1`。`widget/inventory.items`
增加 `resourceState`（ready/bootstrap/damaged/unchecked）和 `imageCount`。
只有 ready 报 `manifestState: valid`；已知无图启动包不再冒充完整资源。
目录文件存在但均无法解析时返回 `recoveryBlocked: true, complete: false`，
禁止自动补装、事务提交和清空旧资源；必须保留现有文件用于显式恢复。

启动保留最新结构有效目录中的全部组件身份、顺序及正常资源；只为损坏项寻找
上一份已提交目录中同 ID、同槽位、校验有效的完整包。不能把未提交的残留图片
或其他组件的备用代次用于恢复。普通切换仅保存当前组件 ID，不覆盖备用目录。

私有验证固件可协商 `widgetPreserveUnchanged=p4-preserve-unchanged-v1`。
私有 `esp32_p4_lookup_parent` 另对已有文件保存受限的查找起点提示；仍调用原 SDK
完整名称/标志/头部遍历器且仅一次，未命中继续完整查找，保留原错误及 errno。
不缓存“不存在”，不改变目录原子 rename、资源校验、存档格式或八描述符池。
它同时为精确文件比较增加数值 OPEN/DATA/CLOSE 分段，不输出文件名、内容或指针；
普通构建不启用。全局查找统计不能冒充某次安装的独占 I/O 计数或物理串口根因证明。
PC 仅对同一连接明确报告该能力、且非资源修复的传输，在 `widget/begin` 添加
布尔 `preserveUnchanged: true`；缺省/false 保留原事务。提交仍完整解析和校验，
仅当前激活且选中的完整组件，文字、按钮与每张 P4S/图片的文件字节及 EOF 全部
一致时，返回本次 `commit` 的 `unchanged: true`，不写新包/目录、不重启程序，
也不切换页面。校验和仅作快速排除，不作相等依据。原生任务绑定作用域绕过产品
HAL 的读取缓存；SDK SPIFFS 自身的文件/索引缓存仍遵循原一致性语义，此路径不是
原始 Flash 介质巡检，不能据此声称检测所有介质位翻转。中途音频预约、权限丢失或
无法建立作用域则拒绝并保留 staging，不重试。
内容不同、缺失或损坏继续原双代事务，内置启动同步与修复永不走此路径。
该能力默认关闭，不能由普通组件自行开启；未收到回执也不能推断曾提交或重发。

私有 `esp32_p4_save_budget_parent` 候选用原生单所有者的 PSRAM 快照减少保存线程
栈占用；普通固件默认关闭。保留原有 4096 字节线程栈、优先级 1、A/B 代次、fsync、
新输入合并、音频互斥和权限撤销；副本、重入或其他任务不能覆盖/释放在途快照。
`diagnostics.runtime.saveWorkerStack` 只返回保存线程在实际保存返回后缓存的数值：
available/taskStackBytes/observations/invalidSamples，available 时附 minimumFreeBytes。
诊断请求不扫描任务栈，不返回地址、句柄、栈内容或用户存档。此观测不是全 SDK
调用链上界或声学验收，仍须在实际保存负载中独立验证栈余量与性能。

`widget/begin` 可带 `repairResources: true`，仅修复已安装且被识别为 bootstrap
的同 ID 组件；damaged 完整包只接受与已提交目录的界面及图片校验值完全一致的恢复源，
不自动降级或升级。须提供完整图片包，固件读取并校验原有按键配置而不覆盖，保留存档。
begin/commit 遇到录音、对话或播放则拒绝；客户端每次连接仅尝试一次，不循环重试。
修复其他组件不切换当前页面。已存在完整图片资源时拒绝已知无图启动包覆盖。
无有效备用包时设备明确提示需要恢复，完整美术仍来自 PC/出厂资源区，不占应用 OTA 两份空间。

## 开发候选宿主输入观察边界

固定与滚动宿主 UI 的指针输入原生捕获同一份组件 ID、资源版本、宿主实例、
变量 epoch 与 context，直到实际命令入队都保留这份观察。命令保留旧 context，
不重新绑定到刚到达的列表；在宿主锁内同时核验观察和动作可用性，过期则消费
这次动作但不发送、不降级为桌面输入、不自动重放。普通点击不设置 confirmed；
确认框仍保留其原 context，由现有 PC 确认/事务协议判断，不绕过用户确认。
这些字段只在原生内存中组合，app/event 的 wire 格式与旧组件数据保持不变。

## 候选有界绘图

### r45 程序原子发布边界的固定向量诊断

静默且没有传输/重启任务时，可显式发送 `debug/program_publication_probe`，payload
必须恰好为空对象，不接受程序、状态、owner 或采样参数。返回
`debug/program_publication_result`，engine 为 `p4-program-publication-probe-v1`。
128次固定私有状态复制采用与真正 PROGRAM factory 相同的原子发布实现，但严格使用
独立 NATIVE_PROBE 租约，绝不打开 PROGRAM 配额。每次只复制<=512字节并让出一次
调度；实时音频预约可撤销它，不录音/播放，不读写用户资源或存档。五秒限流。
元数据包含 samples/minUs/maxUs/sumUs/stateBytes/storage；r46 另提供
criticalSamples/criticalMaxCycles/criticalSumCycles/configuredCpuHz。周期读取位于
锁内、仅本核硬件 cycle counter；host stub 的 criticalSamples=0，不冒充硬件测量。
configuredCpuHz 是编译配置；整次 wall time 仍含锁外调度/等待，不能混作锁内时长。
设备使用 PSRAM 私有缓冲，
host stub 明确标 host-stub。观察值不是最坏执行时间、整套程序宿主或 AEC 验收。

### r44 受限组件程序（实现接入，执行门禁关闭）

`runtime.program` 的严格数据与 sole-writer 契约见
`pc/skills/petui/references/component-program.md`。只有 implementation 与 execution
reviewed 两个独立开关均通过，设备才声明 `widgetProgram=p4-component-program-v1`
并分配 PROGRAM 配额；当前普通/UI 目标均不声明此能力。实现候选中的
`miniapp/state.boundedProgram` 仅返回当前激活的分类状态/耗时/燃料；没有存储格、
事件数据或 owner token。当前激活尚未启动/启动拒绝时不复用上一组件报告。
不新增任意执行命令、不授予网络/外设/音频权限，不改变旧包语义。

### r43 宿主可见集合（独立 opt-in）

UI 候选另声明 `widgetHostCollection=p4-host-collection-v1`；普通目标不声明且
拒绝 `surface.collection_view` 与 `app/snapshot.collectionView`。严格字段、
来源/动作映射和布局见 `pc/skills/petui/references/host-collection.md`。
PC composition v2/v3 保留完整记录，仅发送至多八个可见槽。新字段精确为
engine/source/visible/count/offset/selectionKey/rows，行字段为
present/key/label/detail/index/selected/truncated。空槽没有可用目标。
总 app/snapshot 仍至多 2048 字节；超预算发送固定错误/空动作，保留记录，不重复操作。
设备先私有解析，再按原组件/实例/序号原子发布。一次渲染捕获不授予输入权限；
最终入队另核验来源、可见槽、行存在、动作可用性和原生观察。该路径不开放设备
独立程序、记录编辑、音频或外设；实际 P4 输入/时延仍待验收。

独立 `esp32_p4_canvas_candidate` 报告 `widgetCanvas: p4-canvas-v1`，解释
surface.canvas 的有界线段、矩形、圆和整数变量绑定；参考 bundled PetUI 的
`references/canvas.md`。普通目标不报告此能力且拒绝对应声明。组件包不能上传
WASM、地址、回调或 DMA 配置，绘制仍由现有 surface 生命周期和 framebuffer
拥有者执行。软件 span 预算、目标编译和像素对照不是设备帧耗时或 AEC 证明。

## 候选混合文本表单

同一独立 UI 候选目标另报告 `widgetAppFormV2: p4-app-form-v2`，普通目标
不报告、不接受 v2。v1 仍报告原能力且语义不变。新表单只解释有界数据：
整数/选择器/UTF-8 文本、独立草稿和八个逻辑编辑动作，最多使用原有八个变量。
细节见 `references/app-form-v2.md`；PC 安装在传输前核验精确能力标识，
不能只检查 `widgetAppForm` 或版本号。文本只经明确确认才成为持久化字段；
PC 输入法补全只修改当前草稿，不是额外的 wire 提交命令或自动保存授权。
没有开放设备 OS 输入法、任意 Unicode 字形、程序/外设配额或新增音频路径。

## 只读验收身份

`debugEvidenceIdentity: p4-evidence-identity-v1` 与 `debugBootId` 表示只读诊断
可用。`debug/identity` 请求只接受一个 1–64 字节 ASCII requestId，响应绑定
requestId、boardDeviceId、本次启动的 16 位十六进制 bootId、构建与实际
appElfSha256、协议版本和单调 uptimeUs。启动 nonce 不落盘，不是密钥或设备认证；
网络输入不能设置、重置它，也不能凭该字段获得执行或外设权限。
截图 begin 另带 bootId，旧收集器可以忽略；严格验收则要求截图与前后两次只读
身份处于同一次启动及同一 ELF。源码/构建/截图一致性不替代长帧或语音/AEC 门禁。
详见 `tests/FRAMEWORK_EVIDENCE.md`。

# Renderer snapshot and lifetime diagnostics (unreleased r28)

`diagnostics/status.storage.spiffsMaxFiles` is the compiled mount pool size,
read-only. Ordinary/UI/Windows targets remain eight. Independent private
storage comparison targets use eight versus thirty-one; IDF couples descriptor
name hints and page-cache allocation to this value. This is not a component
limit or user setting, does not authorize more work or format/migrate storage,
and cannot by itself establish improved throughput or safe acoustic behavior.

The r66 experimental component parent defers a new program's first visibility
decision while its install/activation storage publication is still owned. The
new runtime can be visible to the main loop during unlocked cleanup before
the outer command publishes its page; old-page visibility cannot cancel that
not-yet-started lifecycle. Existing live owners keep normal progress/stop
semantics, and a real hide after completion still latches stop. No visibility
recovery, lease recovery or read-only query restarts an old owner.

The private scheduler-observer image adds `debug/sched_summary` with exactly
`generation` and registered `actor` (1..3). It reads a stopped window only and
returns fixed numeric FS wait/hold-before-release/release pairs, totals/maxima,
unpaired/unfinished phases and zero/nonzero SDK priority-return counts. These
aggregates survive raw-ring overflow; they never clear overflow or widen the
600 ms window. OTHER tasks are not paired. Release occurs somewhere between
BEGIN and RETURN; wall intervals are not CPU/flash time, and zero return is not
proof that no mutex or priority operation occurred. Normal/UI builds contain
no observer, these commands or aggregate storage. No paths or TCB addresses
are exposed. SDK and final-link coverage audits remain required.

Candidate r42 hardens the existing native storage command lane used by package
install/remove/cold activation. Task/serial authority also binds the exact live
scope address: copied contexts cannot release physical occupancy, alter durable
phase or mismatch the state mutex. Rebinding a live scope and overlapping lane/
scope storage fail before mutation. A configured task observer returning zero
grants no work; boot's deliberately unconfigured synchronous path is unchanged.
No wire topics, component schemas, saves, quotas or audio algorithms change.
Sanitizer fault cases are not P4 timing or physical Flash/AEC acceptance.

Candidate r41 compiles a default-closed owned collection bridge. It adds no
protocol topics, runtime JSON fields, widgetCollection capability or resource
permission. Native banks own copied strings and bind one receiver; the trusted
UI receiver's expected identity is immutable before first publication. Replaced
data/previous actions invalidate observed input through generation/epoch/revision.
Cached view capture and context recheck allow rasterization outside the model
lock; parent must still own actual buffers/leases and admission. Stop retains
historical selection and never resumes from a later true callback. No record
editing/submission, UI installer/render route, dataset persistence, P4 timing or
voice/AEC acceptance is supplied by this library.

Candidate r40 adds a preparse bound of 32 open JSON object/array containers for
JSONL/native control, native JSON frame metadata and component JSON files.
Scalar leaves and quoted/escaped bracket characters do not count as containers.
Malformed/over-depth input is refused before recursive cJSON parsing and before
transport registration/native abort/enqueue/package publication. Proper JSON
grammar, matching container types, duplicate keys and schema remain separate
parser/validator work. It does not enlarge task stacks, change vendor cJSON or
reset user data. C/Python/JS/serde shared vectors are offline evidence only,
not a complete linked stack graph, runtime watermark or timing/audio acceptance.
Private PC JSON, binary assets and legacy identity text do not use this bound.

Candidate r39 extends the preparse text guard to strict UTF-8 scalars, legal
JSON string escapes and paired UTF-16 escapes. Raw string controls, overlong/
surrogate UTF-8, unpaired escaped surrogates and NUL are refused before transport
registration, native abort/enqueue or component JSON publication. This never
repairs text with replacement characters. Binary chunks and legacy identity
text remain byte-exact on separate paths. The actual parser/schema still runs;
this guard is NOT complete JSON grammar, duplicate-key or nesting validation,
and is not physical transport, font, latency or acoustic acceptance.

Candidate r38 compiles a default-closed cooperative native program dispatcher.
It adds no JSON topics, package declarations, hello capability or nonzero
PROGRAM/PERIPHERAL quota. Its private fixed-address registrations are not wire
tokens. One explicit caller turn offers at most one local timer/bus message and
one pump; it never consumes terminal reports. Group stop, clock regression and
registration reuse keep native lifecycle/receipt semantics. This is not device
scheduler integration or timing/audio acceptance.
Per-registration removal latches stop before cleanup; BUSY never turns into
permission to resume on a later turn. Unknown retirement retains occupancy.

Candidate r36 rejects raw NUL and escaped decoded NUL in JSON control text
before cJSON parsing, topic dispatch or transport registration. The native USB
JSON frame guard runs before abort handling/enqueue; binary FILE_DATA is not
text and remains byte-exact. Component JSON readers reject the same ambiguity
before strlen/checksum/activation; legacy identity/marker text is unchanged.
Literal backslash-u0000 is allowed. Errors do not echo source payloads. This
representation guard is not a general syntax/UTF-8 validator, an authority
grant, or a diagnosis/fix of the outstanding physical serial corruption.

App/media rendering copies its native view, resolved surface, changed image pack,
side scene/art and confirmation overlay under one runtime lock. Host dashboard
text, typed variables and UI availability are captured under one nested host lock.
No decoder, rasterizer or filesystem operation runs under those locks. These
copies are rendering data only; they cannot authorize input or reserve services.
Existing package/save schemas and native input observations are unchanged.

The existing diagnostics runtime object additionally exposes `renderLifetime`:
`samples`, `maxUs`, `maxGapUs`, `overBudget`, `gapOverBudget`, `invalidClock`,
`peakFrame`, `peakPage`, `peakStartUs`, `peakEndUs`, `gapPeakFrame`, `windowResets`,
`completedWindows`, `windowEndFrame`. Counts saturate at UINT32_MAX and live only
for this boot. Frame budget is 66667 us and the gap counter threshold is 100000 us;
these diagnostic thresholds are not hardware acceptance limits. Lifetime peaks
include >2 s samples excluded from the legacy p95/window calculation. The frame
number correlates with numeric operation/PPA trace logs but does not establish
causality, source authenticity, or real-time safety. No audio/network action is
performed by reading diagnostics. Existing window metrics keep their semantics.

# Private query-lock comparison

私有 `PET_P4_DIAGNOSTIC_STORAGE_CACHED=1` 的 `storage.statisticsCached=true` 表示
`totalBytes/usedBytes` 来自最近一次成功的后台只读统计，不是即时空间保证。
`statisticsAvailable`、`statisticsStale`、`statisticsClockInvalid`、`statisticsAgeMs`
和 `statisticsRefreshError` 显式说明快照状态；刷新失败保留旧值，首次无有效值时
available=false。音频保护期间延后刷新，超过三秒或刷新失败标记陈旧。查询不刷新
或调用文件系统；启动时及现有存档线程在锁外刷新。没有新增写入权限、后台任务、
组件命令、自动重试或对存档 fsync 的豁免，普通/UI 默认不启用该私有对照。

`diagnostics/status.lastResetRawReasonCpu0/Cpu1` 是启动时读取的 ROM 数值原因，补充
SDK 将多种 MWDT/RWDT/SUPER_WDT 聚合为 `watchdog` 的局限，不包含寄存器转储。
私有 `PET_P4_RESET_PHASE_TRACE=1` 才增加 `previousResetTrace`：固定244字节 RTC
双槽/启动代号/全构建摘要校验后的历史任务阶段数值；`lastValidOnly=true`、
`authentication=false`。损坏、跨构建、无记录保持 unknown；不得把每核回绕的周期
计数当墙钟或把最后有效阶段当作根因。没有新增记录命令、组件权限、自动重跑或声音。

`PET_P4_QUERY_REPLY_DEFERRED=0` and `PET_P4_QUERY_REPLY_TRACE=0` by default.
The private query-lock pair preserves the original parser, host activity and
stable transport registration. Only `diagnostics/query` can return its existing
owned serialization to the protocol worker; it is sent once on the original
route after releasing the state and diagnostics mutexes. No new transport queue,
copy, task, retry, mutation ACK deferral or audio/media routing is introduced.
The optional `runtime.previousDiagnosticQuery` contains bounded numeric timing
for the preceding completed query, with a saturating sequence and invalid-clock
latch. It is observable only by the same protocol worker, not proof of delivery,
authentication, worst-case timing, or the cause of a historical watchdog reset.

`PET_P4_H264_PREFETCH=0` by default. A private native experiment reuses the
existing asset-preparation worker for at most one owned decoder frame; it does
not advertise a package capability. `runtime.nativePrefetch`, when compiled,
contains numeric job/completion/cancellation, decoded/presented frame counts,
worker/decode wall times, enqueue-to-consumption latency (including target
waiting), and byte-labelled worker high-water observations. These are not peer
receipts, CPU time, acoustic acceptance or a worst-case stack proof. Existing
renderLifetime/max/gap counters retain all real preemption and render work.
No decoder reset/free or encoded-source reuse precedes physical retirement.
The exact native program window excludes a physical codec job without changing
fuel, instruction budgets, lifecycle cancellation or accepted-event ordering.

The separate private `dirty-parent` keeps `PET_P4_APP_DIRTY_CACHE` and
`PET_P4_SPARE_UPDATE` default OFF. No wire/schema or public capability changes.
Exact resolved native app keys may select bounded dirty-region repaint through
the unchanged raster functions and clean-frame cache; all overlapping layers
retain original order and viewport clips. Unknown/dynamic/resource modes keep
full repaint. Common/confirmation overlays are never cached. Empty image packs
still advance revision/metadata without inventing changed pixels.
Non-cacheable app prefixes require their exact captured resource revision, so
visually empty image packs cannot retain a different application's old prefix.
The storage candidate uses r+b/write/flush/final ftruncate only for the already-excluded
unreferenced JSON generation with a successful-write hint. Only missing files
fall back to wb; other open/data/truncate/close/current failures remain failures.
No catalog, private-save, readback, checksum or publication step is bypassed.
Both candidates still require independent source-bound physical acceptance.

Private startup-parent additionally defaults three switches OFF:
`PET_P4_HELLO_ASYNC`, `PET_P4_SPARE_RECREATE`, `PET_P4_APP_PHASE_TRACE`.
Only unsolicited hello announcements use the existing protocol work queue;
the full original payload uses immutable native board identity, coalesces while
queued/in-flight, and transmits outside the main/state mutex. Requested bind
responses, message framing and routes are unchanged. No new task or grant.
The diagnostic I2C sweep runs before revealing frame 1 with identical calls and
timeouts. Optional numeric `app_snapshot_us`, `app_snapshot_wait_us` and
`app_draw_us` append to frame-context logs; timings remain nested wall spans,
not exclusions from lifetime or budget checks. Spare recreation applies ONLY
after the native storage scope rejects every live catalog generation; it
replaces the same already-destructive `fopen(wb)` spare truncation with remove
then write. Catalogs/saves never use it, and complete flash readback, checksums,
validation, atomic catalog publication and original failure handling remain.
These candidates require independent source-bound regression and physical
timing/data-preservation acceptance; they are not released capabilities.

`PET_P4_NATIVE_PANIC_TRACE=0` by default. The private SDK-pinned diagnostic
profile adds `previousNativePanic`: same-build, checksum/commit-validated numeric
PC/return-PC/cause/core records only (144 RTC bytes, no text or stack contents).
Its observation does not authenticate a device, authorize a replay, or change
the original exception handler, watchdog or reboot policy. Ordinary firmware
does not link the observer or publish this optional object.

Private `PET_P4_RESET_PHASE_BENCHMARK=0` by default. When enabled, native boot
measures exactly32 recorder enter/leave pairs, yielding between four-sample
groups. Explicit `diagnostics/query` with `includeRecorderCost:true` may return
`resetRecorderCost` (fixed samples, cycles, wall microseconds, observed clock,
validity). Polling never reruns it. Measurements include preemption, reject
clock discontinuity, and are not CPU time or WCET. The private cache-hit phase
comparison may retain only the caller phase for software cache hits, without
changing native IO/error/checksum/cache-invalidation behavior or fault history.

Private `PET_P4_STORAGE_IO_RETAIN_COMPLETED` optionally advertises
`storageCompletedObservations=p4-storage-completed-v1`.
`debug/storage_io_completed` requires exactly requestId, bootId, owner (1..3),
generation (0 means latest completed for that native caller), and actor (1..4).
Its single `debug/storage_io_result` contains numeric completed-generation
metadata and all three operation counters copied atomically. A later SAVE
window cannot replace a PROTOCOL window. An unavailable exact generation is
an explicit error; callers must not attribute another generation or replay
an install. No paths, files, payloads, credentials or memory addresses appear.
Existing storage observer messages and persisted formats are unchanged.

Private `programParentPeak.queryAtPeak.nativeKind` identifies ordinary dispatch
(0), debug/native handler (1), unsolicited hello (2), or native activation (3).
These are inclusive wall intervals, not exclusive CPU time or causal proof.
The optional native work window serializes only pure compute with non-realtime
drained control/snapshot transmission. It creates no task or protocol authority;
audio/media transmission, original checksums, close ACKs, cancellation, whole
frame gates and aggregate program gates retain their existing contracts.


### 公开程序候选 profile（2026-11-01 工程默认）

`esp32_p4_program_public_candidate`（0.7.103-p4-r137-public）继承普通 evboard 镜像并以显式 profile 表开放设备程序：PROGRAM 配额 {1,4096}，每回合 64 条，PERIPHERAL 仍为 {0,0}，后台仍只由 PC `widget/program-start`，无自启动/重启恢复/持久闹钟/音频撤销后自动恢复/能力混用。hello 的 `widgetProgram*` 字段由同一组开关驱动；普通镜像仍不发布这些能力。该候选不是默认 OTA 镜像，尚无实机回执。


### D 出站通知与数据刷新（开发实现，统一构建/HIL 待验）

`runtime.notify={title,badge_var?}` 声明后，transition 或 tick.then 可使用 `notify:true`。成功执行主分支/完成分支后，当前可见实例排出 `widget/notify`，负载仍为 `componentId,title,badge?`；每组件 30 秒最多一条。隐藏/组件替换丢弃未发送项，失败传输不重放。

`runtime.data.refresh_action="data.refresh"` 对应已声明动作。成功执行后排出 `widget/data/refresh`：`{componentId,source,boardDeviceId}`，同实例 5 秒冷却。PC 验证 board 和已安装声明，worker 经有界 admission 提供 computer.status / todos.upcoming 当前快照或触发 fetch 宿主状态刷新。不会接受任意 URL，fetch 白名单、缓存 TTL 和最短 60 秒请求间隔不变；账号支付数据源不在此路由内。

shell+canvas 组合目前在 Python/Rust/C 一致拒绝；shell 的页 0/4/11 是私有页，省略 page 为共享，legacy page 4 的共享语义保留。

### Read-only miniapp content observation

`miniapp/query` echoes `requestId` in `miniapp/state`, with `widgetId`, `bootId`, `observationAvailable` and (when available) `instanceGeneration` (8 lowercase hex digits, meaningful only paired with bootId). Hello advertises `widgetStateObservation=p4-miniapp-observation-fnv1a32-v1`. Native view and digest input are captured under one model lock; VM capture uses existing owner serialization and per-call PSRAM scratch. Queries do not pump, save, flush, restore or dispatch. `durability` is explicitly `not_observed`. No ACK, digest or checkpointPending=false proves a durable write. Persistence HIL may instead explicitly reboot, require a different bootId, reopen the same fixture and compare its expected readback digests. This has not been device-tested here.

Hashes are FNV-1a32 (offset2166136261, prime16777619), lowercase hex8, non-cryptographic content evidence. Integers encode LE32 (signed values as two's complement); strings are UTF-8 plus NUL. `cellsDigest={cols,rows,bits,count,hash}` hashes LE32 cols/rows/bits followed by row-major uint8 grid values (up to256). `vmDigest={schema,cells,revision,hash}` hashes LE32 schema/cell-count, all committed LE32 cell values, then8 LE32 outputs; revision is metadata excluded from hash. `persistProjectionDigest` hashes LE32 save-version/save-vars-mask, saved state string (empty if no save-version), saved page string (empty unless save-page), then each selected variable in declaration order: name string, type byte(1 integer/2 string), LE32 integer or string value. Instance/revision and unsaved vars are excluded. This digest does not expose or attest private saved bytes.

Public PROGRAM admission remains exactly one physical lease /4096 aggregate fuel with either VM version; public image validation, hello maxFuel and native default PET_PROGRAM_FUEL share4096. Private program-v2 retains16384.

组件 OTA 修复：`pet_p4_component_media.c` 对空媒体 staging 跳过不会增加用量的全设备预算扫描，避免普通计数器提交执行 896 次 SPIFFS 路径查询。仍校验 staging、阻止覆盖待发布事务、清理目标备用代；实际媒体包保留原预算上限与检查。`tests/component_media_budget_test.py` 验证查询数、清理与拒绝边界；离线通过不代表已刷入设备。

图片组件提交（r157 开发修复）：仅在当前存储命令持有未锁住状态通道、且目标 slot/generation 不被当前目录引用时，图片直接写备用代，替代临时文件和重命名；每次最多写 4096 字节并在 VFS 返回后让出一个 RTOS tick。整包物理回读、校验和、解析及 A/B 目录发布顺序保留；旧同步初始化路径仍使用原子文件写入。该更改尚待用户设备验证，未宣称已消除故障。

## Service bridge candidate: explicit pairing and presentation

`PET_SERVICE_BRIDGE_ENABLED` defaults to 0. Only
`esp32_p4_service_bridge_candidate` explicitly enables this path (and its host
collection dependency); a normal image/version or desktop preview is not an
announcement of support. When enabled, HELLO capabilities include
`widgetServiceBridge:"p4-service-bridge-v1"`, `widgetServiceBridgeImages:2` and
`widgetServiceBridgeImageSide:192`. Check each additional surface/input/host
context/collection capability independently. This is a candidate contract, not
a completed P4 build, physical input, audio or HIL acceptance claim.

A bridge package uses `pc/application.json` engine `pc-service-bridge-v1`,
service `bridge`. Scalar-only bridges may reuse the legacy host route and its
existing capabilities without surface.services, host_contract or presentation;
this does not enable new device fields. Dynamic images/timelines/event arguments
and local-media require explicit device `surface.services:"p4-service-bridge-v1"`
with app graphics/variable bindings, host context and input-v1. The device
`host_contract` is a 64-lowercase-hex SHA256 pairing of the host declaration's
schemaVersion/engine/service/actions/content and complete runtime excluding
host_contract, using the shared Python/Rust typed JSON framing, not JSON text
hashing. All numeric zero spellings, including negative zero and floating-point
zero, frame as ASCII `i0;`; false remains `f`. Other finite floats retain the
`d` + big-endian IEEE754 binary64 framing; do not coerce every integral-valued
float to an integer. Python and Rust must agree before binding. Existing explicit
bridge app host requests/replies carry `contract`; a mismatched
device layout cannot execute updated host commands. Hash pairing is not an
authorization grant or authenticated signature.

The existing host context/instance/sequence route remains authoritative.
Snapshots add `presentation:{timelines,images}` only on the explicit bridge.
At most two timelines have positionMs, durationMs (1..86400000), running,
horizonMs (100..10000) and projection percent/position/remaining. Display
extrapolation clamps at horizon and becomes invalid at snapshot age 10000ms;
it never advances an execution clock or proves playback. Images map aliases to
key/revision/width/height (each side 8..192). Neither URLs nor filesystem paths
are sent to the device. Existing app variable and graphics budgets still apply.

`app/image` uses request-correlated `protocol/ack`. A read-only
`operation:"query"` returns code ready/missing; transfer packets carry
componentId, instance, context, alias, key, revision, width, height, offset and
base64 data. The sender uses 1024-byte RGB565 little-endian chunks; the receiver
requires sequential even offsets/lengths, at most 2048 decoded bytes per chunk,
matching active view/reference and a snapshot younger than 10s. Double-buffer
leases keep the visible bank immutable until full completion and release.
Audio requests have priority; unknown ACK is not permission to replay a chunk.
A later read-only status/ready reconciliation is distinct from successful send.

PC bridge plans allow 1..4 providers, <=8 exact int/string variable bindings,
<=16 typed commands with <=4 int/string/bool arguments each, <=1 stable-key
collection (1..8 visible), <=2 timelines and <=2 images; bridge snapshots are
<=2048 bytes and sessions retain <=128 command receipts. Commands require
current observation context and event identity; claim precedes execution,
unknown/error outcomes consume the observation without automatic retry.
Exit/disconnect/disable/update revoke obsolete views. Version retirement cancels
notes/news/audio owners too; at most one notes provider is allowed per bridge.
Device-epoch import requests are rejected; only explicit PC file selection may
provide local paths. Image MIME must match actual PNG/JPEG; original-bit-depth
alpha is checked before thumbnailing, decoding is serialized and cache quotas
(global 32, per owner 2) are checked before decode and insertion. Existing native service
permissions and confirmations remain; unregistered parameter methods, recursive
bridge and autoRead are rejected. Local-media uses explicit desktop import,
not online songs, arbitrary URLs, OAuth, accounts or payments. Full declaration
limits and binding procedure: ../pc/skills/petui/references/service-bridge.md.
Original program750us, frame/gap66667us and SAVE4096/free1024 gates remain.

安装失败隔离（r158 开发）：存储命令释放状态锁后，既有 RX 任务可处理严格白名单的 RAM 心跳、组件清单与缓存诊断；序列化在状态锁内完成，传输在锁外，任何 mutation 仍只走原命令队列。延迟 ACK 按真实任务所有者隔离。仅存储协议任务在原生 HAL 返回、cache/observer 临界区结束后每 10 ms 让出一 tick，使 GC 不持续饿死 idle；不改变错误码、不中断原生 GC、不自动重放。已知失败的当前未发布图片尽力删除其部分分配，旧目录/当前代和存档保留。实际断连、空间与后续同步恢复尚待用户设备验证。

## 设备总音量与数据过期清空

`hello.capabilities.audioMasterVolume = "p4-master-volume-v1"` 表示支持设备扬声器总音量。请求 topic 为 `audio/volume`，payload 包含 `operation:"query"|"set"`、`requestId`、`expectedBoardDeviceId`；仅 set 可包含整数 `volume`（0–100，0 为硬件 DAC 静音）。回执 topic 为 `audio/volume-state`，包含真实设备 `boardDeviceId`、原 `requestId`、`engine`、布尔 `ok`、实际 `volume`（无法确认时 null）、布尔 `persisted`、`savePending`。硬件确认不等于 NVS 保存完成，读取不能重放设置或触发保存。客户端必须绑定 USB 连接代次，结果未知时先只读核对。

`hello.capabilities.widgetDataExpiry = "p4-data-expiry-v1"` 与 `widgetDataSources:8` 声明有界快照能力。`widget/data` 可选严格布尔字段 `clearOnExpire`，默认 false 保持兼容；true 时 TTL 到期或时间回退会清空 view 中的旧行、日期和消息，并禁止选择过期行。PC 的共享城市源 `location.current` 使用此选项；行数、字段长度及现有内存/耗时验收门槛不变。

2026-11-03：P4 CPU暖重启生命周期修复：CORE priority0 在 init_heap100 前清理未拥有的旧 AHB group0，保留固定数值 before/after 快照及 C getter；不新增 wire 字段、不改计数/验收门槛或音频热路径。V1 IDF5.5.1 restart 仅通过固定哈希校验的构建目录副本回移 V3 已有 AHB reset SET/CLEAR；SDK 原件/V3 原生实现不改。仅离线检查不代表启动/OTA/设备验收通过，事故 fault12 及原 fault11 失败保留。

只读早期 DMA 数值证据：`diagnostics/query` 显式携带 `includeBootDma:true` 才附加 `bootDma`，从不可变 C 快照序列化3对 AHB 通道的 before/after 链地址、EOF/state、dw0 原值及 size/length/owner/successEof。查询不读 MMIO、不复位、不触碰 NVS；普通轮询及组件/持久化数据格式不变。dw0 是 CPU 内存视图，不能单凭该值宣称已证明 DMA 根因。


### 可选原生 32 像素字体候选

`PET_FONT_NATIVE_32` 默认关闭，要求同时开启位图与度量表压缩。
开启后，仅对原生字库覆盖的 199 个字符在 2 倍字号下使用 32px/2bpp
字形；其余字符继续使用完整的 21,207 字回退字库。排版宽度仍沿用
16px 字库，未增加串口图像传输或改变组件声明。新度量表采用可逆列编码，
在启动时借用位图缓冲还原；解码或校验失败沿用既有初始化失败闩锁。
参见 `docs/font-native32.md`；模块离线验证不能替代最终 V1/V3 镜像预算
与实机渲染验收。


## Non-destructive appearance audio override (p4-audio-patch-v1)

Requires hello capability `appearanceAudioPatch: "p4-audio-patch-v1"`.
Older firmware must be rejected before any mutation. Never fall back to legacy
`asset/begin`, full transfer, `asset/patch-commit`, or automatic activation.

New JSON topics: `asset/audio-begin`, `asset/audio-chunk`,
`asset/audio-commit`, `asset/audio-abort`. Every request binds `transferId`,
`boardDeviceId`, `bootId`, current active `slot` (0/1), and exact ready `packId`.
The client must independently prove that the selected appearance is that pack;
this API does not switch or discover a different target appearance.

Begin sends a complete desired WAV override snapshot in `files` (0..24 entries),
with `family`, safe logical `p4/*.wav` `path`, integer `size`, and lowercase
16-hex FNV-1a64 `checksum`. `totalBytes` must equal the sum (max 2 MiB).
An explicit empty `files:[]` with `totalBytes:0` commits an empty override snapshot,
restoring original pack WAV/default lookup without deleting original files.
Each file is at most 1 MiB + 4096 bytes. Supported data is unchanged PCM WAV:
mono, 16000 Hz, signed 16-bit. Missing families retain the original pack WAV or
compiled default; original WAVs and all video/manifest/raw files remain intact.

Chunk sends declared `path`, contiguous per-file `index`, exact `offset`, and
base64 `data` (decoded 1..4096 bytes). Do not retry an unknown chunk. Commit
requires every file complete, checks complete-file size/FNV/WAV framing and
readback, then writes the checksummed independent SPIFFS audio-bank index last.
No component/default-pet, raw header, pack marker, manifest, or active slot is
written. The inactive override bank is the only cleanup target. On restart the
highest fully valid index selects a bank, still bound to the exact slot+pack.

Replies use `asset/ack`, with phase `audio-begin`, `audio-chunk`,
`audio-commit`, `audio-abort`; typed `ok`, exact transfer/board/boot/slot/pack.
Positive chunk ACK also binds `path`, request `index`, `nextIndex`,
`receivedBytes`. Positive commit ACK adds `files`, `totalBytes`,
`audioGeneration`. Negative ACKs must not be promoted using echoed fields.
Unknown outcome retains the appearance owner and staging, blocks automatic
abort/replay, and reports `audio_patch_outcome_unknown`. Known abort/idle cleanup
can remove only this unpublished override bank. It never clears an entire slot.

Storage/decoding/JSON/TX run through the existing physical appearance phase and
borrowed storage IO window, outside the runtime-state mutex. Buffers are bounded
PSRAM and streaming 512-byte file scratch; playback resolves an override only
under its existing realtime reservation. DSP/AEC, task priority, clocks, stack,
render and resource gates are unchanged. A successful ACK proves persisted
bytes, not that a cue has already played.

For an active custom slot, the existing nonce/quiescent `asset/slot-query`
may additionally return top-level `videos:[{path,size,checksum}]`. Names/sizes
are captured from the current RAM catalog before borrowing the storage window;
size/FNV are then checked against the exact ready pack's committed raw header.
No video payload is read. Missing, ambiguous, wrong-pack or mutating header
proof is omitted and cannot authorize a custom audio-only pack-ID mismatch.
The original before/after epoch/revision/lease checks still gate the response.

### 全局状态提示音

`globalAudioCues: "p4-global-audio-cues-v1"` 表示支持跨形象共用的提示音设置。
音频专用 `asset/audio-*` 请求增加 `scope: "global"`。请求仍绑定当前设备、启动身份、当前物理形象槽及其 packId，以验证传输权限；持久化身份独立于视频包和形象槽。每阶段必须保持 scope 一致。仅 WAV 数据进入独立音频双银行，提交后对所有形象生效，空集合提交恢复默认提示音。上传、删除和下发均不准备、重传、清空或激活视频。旧 `p4-audio-patch-v1` 保留兼容，新的客户端要求全局能力后才下发全局音效，不回退到完整形象同步。
