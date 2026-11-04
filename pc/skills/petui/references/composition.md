# 通用绑定与 PC 服务组合 v1

## 有界稳定 ID 集合视图

组合 v2/v3 可声明 `collection` 数据源，字段为 `kind`、`expression`、`view`。
`expression` 使用同一受限表达式求出至多 128 项的数组；依赖只允许 document、
constant 及其确定性 computed。禁止依赖时钟、在途网络或 `/event`，避免同一
输入上下文在按下/松开之间换成另一组行。外部数据须先由明确流程保存到文档，
不能用这个限制的移除冒充网络列表已经完成未知结果与身份保护。

`view` 仅含 `key`、`label`、可选 `detail` 和 `page_size`。前三项为非空、至多
128 UTF-8 字节的 JSON Pointer；page_size 为 1–8。key 必须求得唯一、非空、
无控制字符且不超过 63 UTF-8 字节的稳定 ID，不能用数组位置当 ID。显示字段
最多 512 字节；可见槽投影为最多 63 字节、按字符边界截取，并显式标注
`truncated`，原文与原记录不被裁剪或覆盖。

输出含 `rows`、count/offset/page/pages、selectionKey、selectedLabel、selected
和边界可用性。rows 是恰好 page_size 个有界复用槽；空槽 present=false、文本
为空、动作禁用，而不是别的记录。selected 用稳定 ID 查找并保留完整 label/detail
和原 value；selectedLabel 是给设备短文本绑定的有界版本。筛选暂时隐藏所选项时
selected=null，但不会静默清除保存的 ID。状态读取可夹取显示页，不暗改持久页。

动作：page_next/page_previous、up/down、row_0..row_7、clear_selection。只改变
视图导航与选择，不新增/编辑/删除记录。文档修改仍走既有确认、事务与稳定 eventId；
旧 context、重复事件、缺失行或不兼容视图存档不能改选其他记录或重置原存档。
本地业务流程不可将集合导航作为业务修改步骤。

这是一套 PC 集合视图模型。旧包继续用现有命名变量绑定、宿主上下文与固定布局
模板，仅传送显式绑定的可见标量。UI 候选另可按 [host-collection.md](host-collection.md)
声明有界可见槽，减少多字段列表对八变量配额的挤占；普通固件不开放此能力。
全局 32 节点、原图片、渲染和传输预算不变，不是设备无限 UI 或独立数据应用。
完整原文和用户记录仅留在私有文档/宿主；不得复制进包、封面或社区资源。

共享契约为 `contracts/collection-v1.json`；Rust 真实请求、Python 导入校验、
浏览器 AppSurface 和固件无滚动区指针身份门禁均需要回归。该能力不引入网络、
凭据、录音、播放、设备程序或原生外设权限。

另有默认未开放的原生集合核心与固定 `collection-core` WASM 创作模型，采用同样的
稳定键、显示截取和状态保留语义；`contracts/collection-model-v1.json` 由原生 C、
PC 既有 Rust 服务及创作模型共跑。它还不是新的包能力，不能声明设备已能安装动态
列表，也不能绕过原来的 8 变量/32 节点及宿主上下文限制。完整数据、输入观察身份、
存档和设备生命周期接入仍须分别完成，创作预览通过不代表实机或声学门禁通过。

设备端在同一宿主锁内捕获完整观察并在实际命令入口复核：不是只比较松开时
的标签，也不在发出前改用最新列表的 context。重入、内容版本/变量变化或动作
禁用均取消旧点击，且不自动重放。普通点击的这项防竞态保护不是用户确认授权。

显式 `pc-app-composition-v3` 在 v2 基础上提供公开只读 GET + 本地文档/计数的
持久异步步骤、进度绑定、取消及失败步骤恢复，见 [workflows.md](workflows.md)。
这不是鉴权写入/付费 API 支持；v1/v2 的字段、存档和事务语义保持不变。

## 开发中 v2：受限计算、文档与本地流程

`engine:"pc-app-composition-v2"` 必须显式选择，最大 32 KiB；v1 的字段、4 源配额和行为保持兼容。
设备 runtime 另声明 `host_context:"p4-host-context-v1"`，要求实际设备报告 `widgetHostContext` 同值。
设备对普通动作也携带最近快照的 context，只有显式确认才设置 confirmed；旧固件安装前拒绝。
本扩展只在 PC 计算，设备仍接收原有至多 8 个标量，不把表达式或用户记录下发设备。

v2 最多 8 个源。新增 `computed:{kind:"computed",expression:<表达式>}` 和
`document:{kind:"document",initial:<有界 JSON>}` 源；名称和绑定规则沿用 v1。
computed 的源依赖必须是无环图，按依赖求值，不受 JSON 字段顺序影响。
document 输出其真实保存值，支持记录集合；每值至多 8192 字节、8 层、1024 个值节点，
数组至多 128 项、对象至多 32 字段、字符串至多 512 UTF-8 字节、无控制字符。
PC 整数为 JS 安全整数范围 ±(2^53−1)，可保存 epoch 毫秒；投影至设备的整数仍为 ±10⁹。
initial 只放可公开分发的空结构或默认值；真实个人记录经宿主动作写入私有存储，不能把
用户现有笔记、账号、凭据或账本复制进资源包。保存文件不随组件发布或社区上传。

表达式是 JSON 数据，不使用 eval/exec 或脚本。允许标量、`{literal:<有界 JSON>}`、
`{ref:"/sources/<已声明源>/..."}`、`{ref:"/event/..."}`、`{object:{字段:表达式}}`。
运算对象为 `{op, args}`：add/mul/min/max/and/or/concat/coalesce/array 接受 1–16 参数；
sub/div/mod/eq/ne/lt/lte/gt/gte/get 接受 2 参数；if/slice/clamp 为 3；
not/len/str/sum 为 1。filter/map/sort 为 `{op,args:[集合表达式],value:行表达式}`，
行内可引用 `/row/...` 和 `/index`。sort 升序且稳定，仅同类型整数或字符串键。
除法向零取整、余数同被除数符号；不把布尔值当整数、不隐式转换真假。
if/and/or/coalesce 短路；未知运算、除零、溢出拒绝。AST 最深 12、最多 192 节点，
一次求值最多 8192 步，不能借嵌套集合实现无界循环。共享向量为 `contracts/expressions-v1.json`。

计划可新增 `workflows:{流程名:[步骤...]}`，最多 8 条流程，每条 1–8 步；动作目标为 `flow:流程名`。
步骤仅含 target、可选 when/pointer/value。when 是布尔表达式，false 跳过，类型错误使流程失败。
target 可调用本地 counter/timer/ledger，或 document 的 set/append/remove；document 必须声明静态
JSON Pointer，set/append 的 value 是表达式，remove 不接受 value，不能移除根对象。
set 仅替换已有路径，append 仅追加到已有数组；动态更新可用 map/if 构造新集合再 set。
移除、根替换、记录集合/对象整体替换及 reset 流程必须配置 input.confirm，不自动批准确认。
运行时也核对真实目标类型，不能通过变更初始结构绕过已有集合的替换确认。
本地步骤共享候选状态，全部成功后才原子替换；任何一步失败，已有文件不变，不产生半条记录。

每次变更必须提交最新 context 和稳定 eventId；实体事件由宿主生成可信编号。
同编号同动作返回 duplicate、不再次执行；同编号不同内容拒绝。保存最近 128 个回执，
更旧编号仍须通过当前 context，旧内容版本不能重放。PC 不自动重发失败动作。
v2 保存为 `composition-v2-state.json`，总上限 64 KiB；首次读取可导入已有 v1 状态，
原 `composition-state.json` 不覆盖，损坏文件不默认清空。组件升级保留记录。

v2 仍是本地事务流程，尚未开放网络写入、鉴权、朗读、录音、桌面任务，
也不提供后台触发器。v3 的持久异步只开放安全 GET，不是付费任务或外部写入的检查点恢复。
结构/单元测试不代表设备已支持，必须再验收版本上下文、实际输入、显示与重连。

本能力是有界声明式框架，不执行组件内代码，不按组件 ID 选择逻辑。旧 `pc-app-services-*` 包、旧 dashboard 绑定和存档继续兼容。新包必须协商 `widgetVariableBindings=p4-variable-bindings-v1`，缺少能力时阻止同步，不能依靠固件版本号猜测。

PC 同样必须支持 pc-app-composition-v1；只有新固件而旧客户端时不能执行组合服务。作者不传能力文件才使用离线创作默认值，显式传入空能力对象不会自动获得新能力。离线默认值不能作为真实设备支持的证据。

## 设备 UI 与数据

在 `p4-app-surface-v1` + `p4-app-graphics-v1` 的 surface 添加 `bindings:"p4-variable-bindings-v1"`。text/button/meter 节点可用 `var_bind` 指向 `runtime.vars` 中已声明的名字，与 `text`、`bind`、`key_for` 互斥。名字为 1–31 个 ASCII 字母、数字、点、下划线或连字符；不是对象路径。text/button 接受整数或字符串，meter 只接受整数并显示 0–100。

沿用最多 8 个变量、32 个图形节点、4 个页面、6 个状态和 8191 字节紧凑 runtime 上限。不提高图片、触屏、音频配额。整数 ±10⁹，可用 `limits` 缩小范围；字符串最多 63 UTF-8 字节、不能含控制字符。文字、进度、显隐 `show` 和图片分格 `tile` 共用变量语义；触屏命中也检查显隐。坐标、字号、图标、页面和实体键仍遵循原图形与输入契约。

离线组件从状态机变量直接取值。`host:true` 组件的变量来自当前 PC 实例的完整快照，不能添加设备 tick 或设备 persist；PC 保存自己的服务数据。回包必须匹配组件、实例及序号，超过 10 秒没有有效快照时文字显示不可用、动作禁用，不把旧实例数据移植到新组件。

新的 host 组合包将动态文字和进度统一放在 var_bind 节点，dashboard 只放静态兼容文案，不混用另一套 dashboard 表达式。button 应显式选择与填色对比的 foreground，避免同色文字不可见。旧服务包与离线状态机的 dashboard 表达式不受影响。

## pc/application.json

`schemaVersion:1, engine:"pc-app-composition-v1", service:"composition"`，最大 16 KiB。`content` 必须是只有一个计划的数组，计划只有 `sources` 和 `bindings`，所有字段拒绝未知键。

最多 4 个有名字的数据源，每个都必须被绑定或动作引用。名字最多 16 个安全 ASCII 字符。

| kind | 字段与输出 | 操作 |
| --- | --- | --- |
| constant | value 是最多 4096 字节的 JSON；可用 JSON Pointer 投影标量 | 无 |
| clock | 无其他字段；输出 iso/hour/minute/second/year/month/day，使用 PC 本地时区 | 无 |
| counter | initial/min/max/step；step 为 1–10⁹，数值落在 ±10⁹；输出 value | increment/decrement/reset |
| timer | seconds 为 1–86400；输出 seconds/progress/phase，phase 为 paused/running/finished | start/pause/reset |
| ledger | unit 为 day/event、target 为 1–capacity、capacity 为 1–128、label 为 1–24 UTF-8 字节 | append/remove_latest/previous/next |
| json-get | endpoint、hosts、refresh_sec；输出公网 JSON | refresh |

counter/timer 按数据源名字保存在该组件的 `composition-state.json`，原子更新；换包不清除。暂停的计时器不推进，运行中的计时器按 PC 墙钟恢复。存档损坏或类型/范围不兼容时明确报错，不覆盖原数据。最多保留 64 个历史源名；达到配额报错，不静默删除。

ledger 同样保存在上述文件中，只保存真实本地时间戳和当前历史索引。day 按 PC 本地日期去重（重复派发 append 不增计数）；event 允许一天多次，仍由设备事件序号去重，不自动重发操作。容量用尽拒绝新增，不静默丢最早记录；本机时钟早于最新记录时禁止追加，原历史可读。跨日测试以注入时钟的核心测试验证，不修改用户系统时间。

PC 面板和设备事件共用一个短时本地记录事务锁，覆盖读取、变更和原子替换，防止并发覆盖；网络请求只进入有界后台队列，语音、AEC、模型和桌面任务不使用该锁。

ledger 输出 count/target/progress（0–100）/progress_label、span_days/span_label、distinct_days/today、date_range、entry_label、position。持续天数从首条记录到今天计算，不冒充连续打卡天数。previous/next 只移动历史索引；remove_latest 只删除最新记录，必须声明 input.confirm 并提交匹配的宿主上下文，过期确认不执行。无记录不能删除。缩小容量或改变 unit 导致既有记录不兼容时保留原文件并报错。存档总上限 16 KiB，超限在替换前拒绝，不留下无法读取的新文件。共享声明/拒绝向量见 contracts/ledger-v1.json。

单个源的存档与新配置不兼容时，只将该源标为不可用，其他源继续工作；保留原记录并只开放该源明确声明的 reset。仅用户实际触发 reset 后重置该源，不清除其他源。整个存档文件损坏时停止写入，不能通过兜底默认值覆盖原文件。

`bindings` 必须精确覆盖 runtime 的变量。每项仅有 source、pointer、fallback。pointer 是最多 128 字节的合法 JSON Pointer，空串表示根值；不执行表达式。fallback 必须符合目标变量的类型与 limits。数值可投影为字符串，其他类型不隐式转换；缺失、越界、非标量返回 fallback，并在 bindingErrors 返回变量名，不暴露完整响应或 URL 查询内容。

动作表例如 `"host.increment":"count:increment"`、`"host.start":"focus:start"`，数量遵循原输入协议（旧输入 8，p4-input-v1 为 16）。每项必须恰好对应一个 `{"from":"*","on":"host.increment"}` transition。按键、触屏和 PC 面板派发同一个动作。可以声明原有 input.confirm，确认上下文过期时不执行动作；通用服务不是绕过系统权限或破坏性操作确认的入口。

## 网络、调度与隔离

宿主原生同步调用另使用固定 16 层资源上下文；这不是包字段、权限或可配置配额。
乱序清理父调用不改变仍在运行的子调用身份，已退役父身份不会被子清理恢复。
超过深度时拒绝存储，不借用外层或匿名身份继续；仍执行中的物理工作占用不释放。
正常清理后可以显式建立新上下文，不恢复已取消任务或重发未知结果。该原生保护
不代表鉴权连接器、设备后台任务或音频服务已开放。

json-get 只允许无需鉴权的 HTTPS GET、443 端口、1–4 个精确 DNS 域名，不允许 IP 字面量、通配符、用户凭据、常见密钥查询参数或自定义请求头。复用宿主的公网 IP 检查、DNS 钉住、TLS 校验和逐跳域名检查；不能访问本机/私网、关闭证书验证或执行脚本。响应最多 64 KiB；刷新间隔 30 秒至 24 小时，手动刷新至少间隔 15 秒。

网络在最多 2 个后台工作线程执行，等待队列 8 项、缓存最多 32 项；不阻塞设备事件或串行调用麦克风/AEC。每个源的请求合并；失败保留上次成功 JSON，只在下一刷新窗口补请求。source 状态为 preparing/ready/error/busy。组件停用清除待用缓存，已在途 GET 可能完成但旧代结果被丢弃，不重新启动组件。实例身份、变量类型及存档目录均由宿主校验，包不能指定任意文件路径。

网络限流使用进程单调时钟，不因 PC 墙钟调整而突然重复请求或长时间停更；跨重启计时器才使用可持久化的墙钟。

## 验证与边界

共享契约 contracts/composition-v1.json 同时供 Rust 与 Python 校验器回归。JS 预览与 Python 模拟器共享状态机向量；还必须验证设备解析、实际绘制/输入、断开重连、存档保留、性能及语音/AEC。编译或模拟通过不能称为实机通过。

本版不是任意原生插件：没有任意脚本、动态 HTML、任意 POST/OAuth、跨组件文件读取或无上限列表。音频、语音、媒体与敏感操作仍用已有受控宿主服务；不得给组合包添加偷偷录音、自动支付或桌面任务执行的能力。超出原语和资源上限的需求应明确指出缺口，不能用组件专用分支冒充泛化支持。
