# PC 持久异步流程 v3

这是 `pc-app-composition-v3` 的显式扩展，不改变 v1/v2 本地事务。
PC 客户端必须支持此引擎；设备只使用既有变量快照、`widgetVariableBindings` 和
`widgetHostContext=p4-host-context-v1`，仍需实际能力协商，不能以版本号代替。
本版不是任意云服务连接器：未实现鉴权写入、OAuth、付费请求、POST、模型调用、
音频、桌面任务或自动触发器，不声明“已经完成外部业务写入”。

原生宿主另有默认关闭的外部结果状态机 `component_external_outcome`，仅用于审核
提交前持久 claim、取消、结果未知及只读核对的通用语义。它没有注册真实连接器、
凭据或组件协议；另有未注册、无应用调用方的原生 Linux/macOS 私有追加日志，
跨进程 CAS、损坏保留和落盘屏障仅在临时目录验证，Windows 尚未实现。
不能把这些故障测试当作可用的鉴权写入、断电安全或授权能力。
现有 v3 GET 的 resume 规则不变，也不能据此重试未来结果未知的付费或写入请求。

## 声明与实际输入

`pc/application.json` 仍为 schemaVersion 1 / service composition，最大 32 KiB。
在一个计划中添加 `asyncWorkflows:{流程名:[步骤...]}`，最多 4 个名字，每条 1–8 步。
每条流程必须有且只有一个 `{"kind":"workflow","workflow":"流程名"}` 数据源；
源名称计入至多 8 个源的原配额。该源输出 phase/progress/step/error，可用普通
var_bind 绑定到文字和进度；phase 为 idle/queued/running/completed/failed/cancelled。
progress 为已完成或跳过步骤的百分比，step 为已完成或跳过步骤数。

必须声明该源的 start/resume/cancel 三个动作，例如 `task:start`，对应真实 host action
和 transition。设备按键、触屏与 PC 面板都走相同请求入口，提交最新 context 和
稳定 eventId。开始不会由 status、enable、组件安装、重连或恢复进程自动触发。
同编号同内容返回 duplicate；不同内容拒绝。最近 128 个动作回执保存在私有存档。

### 公开 GET 步骤

字段级形式：`{"get":{"endpoint":"https://公开域名/path","hosts":["公开域名"],
"pointer":"/结果路径"},"result":"result_name"}`。

只允许无需鉴权的 HTTPS GET、443 端口及 1–4 个精确 DNS 域名；继承公共网络的
DNS 钉住、公网地址检查、证书验证和逐跳主机检查。IP、私网、凭据、常见密钥查询参数、
headers、method、POST 或任何未知字段都拒绝。响应最多 64 KiB；pointer 为至多
128 字节的合法 JSON Pointer，必须指向存在的有界值。投影结果最多 8192 字节，
8 层/1024 节点/128 数组项/32 对象字段/512 字节字符串，规则与 v2 document 一致。
结果名为 1–16 个安全 ASCII 字符，同一流程不能重复。

成功结果持久保存在该 run；后续本地 value/when 表达式以
`{"ref":"/event/results/result_name"}` 读取。原始动作 data 必须是有界对象，
不能设置保留字段 results。错误只提供固定错误码，不把 URL、响应体或底层错误写进 UI。
GET 的完成不表示任何外部写入完成。

### 本地步骤

沿用 `{target,pointer?,value?,when?}`，只开放 counter increment/decrement 和
document append/非根标量 set。禁止递归启动流程、timer/ledger 操作、reset、remove、
根覆盖或集合/对象整体替换；需破坏性操作时使用 v2 风格的已确认本地事务。
运行时再次检查既有存储的真实目标类型，不能修改 initial 来绕过集合覆盖限制。
when 必须求值为布尔值，false 持久记录为 skipped。表达式预算沿用 v2。
本地表达式不会隐式发起网络请求；异步数据应从已完成 GET 的 results 读取。
与普通状态快照及 v2 一致，某个源的旧存档类型/预算不兼容时，该源在表达式上下文中为
不可用的 null，不提前阻断不依赖它的其他步骤；不会清空或替换错误源的数据。
执行到该源的写入步骤时仍严格检查真实目标类型、预算与路径，目标不可用则停止在该步。
表达式自身报错或 when 不是布尔值也会失败，不能把失败默认为 false 或伪造成功。

## 检查点、恢复与并发

每个组件同时最多一个 queued/running 流程。运行中普通本地变更动作不显示并在
请求入口拒绝，避免无声合并用户记录；cancel 保持可用。UI/status 读取继续工作。
既有计时器的自然计时和显示不因此暂停。

流程和本地模型同存于 `composition-v3-state.json`，总上限 128 KiB；最多保留
4 个历史流程名、64 个历史模型名，达到配额拒绝，不清空旧记录。首次读取可导入 v2
（没有 v2 才导入 v1）；原文件完全保留。损坏、不兼容或超限文件明确报错，不默认清空。
私有存储、动作数据和网络结果不得复制进组件包或上传社区。

本地一步的修改与 completed 检查点是一次原子文件替换，崩溃后不会出现只有计数
而没有回执的半步。每个 GET 在发出前保存 running，成功后保存有界结果和 completed；
最后一步（包含条件为 false 的 skipped 步骤）与整条 run 的 completed 状态同次保存，
不再依赖第二次终态写入；避免已经全部成功却在重启后被误报失败。
在途 GET 的结果未落盘时会被视为 interrupted，不推断它已完成。由于仅是公开只读 GET，
用户可明确恢复该失败步骤；未来外部写入不能复用这个假设。
当前复用宿主的临时文件 sync + rename，保证模型与检查点同属一个旧/新快照；
没有新增父目录持久化屏障或硬件断电测试，因此不承诺异常断电后最后一次已应答写入必然保留。

重启后首次读取把仍有未完成步骤、且无活跃 worker 的 queued/running run 标为 failed/interrupted，
不会自动网络重试。resume 仅接受同一声明版本的失败流程，保留原输入、全部成功结果
及完成/跳过检查点，仅从首个未完成步骤继续；最多 8 次运行尝试。完成流程不能 resume。
声明改变会停止旧 run 并标为 definition_changed，不能用新步骤解释旧检查点。
需要另开一次运行时明确 cancel 旧失败任务后再 start；这是新任务，不是恢复。
旧版若已保存全部 completed/skipped 检查点、但只来不及保存最终 phase，则在同一声明版本、
结果与检查点一致的前提下只补写 completed；同时兼容旧恢复逻辑对此产生的 interrupted 标记。
这不执行任何步骤、不新增网络请求；缺失结果、真正失败、取消或变更声明均不能借此转为成功。

cancel 或手动停用会撤销任务的执行资格；已完成本地效果保留，不宣称回滚整条异步流程。
在途 GET 可能继续至超时，但其结果及后续本地步骤被丢弃。重新启用不会重启已取消任务。
所有异步流程与原 json-get 共用 2 个后台 worker、8 个等待槽；网络 IO 不持有本地
记录事务锁。队列满时保留 failed/queue_full，用户可稍后明确 resume。
真实 GET 入口对同组件的同一固定 endpoint+hosts 实施 15 秒单调时钟节流，
最多保留 32 条近期准入记录；超频/准入表满时记录 failed/rate_limited，不睡眠排队重试。
用户等待窗口后明确 resume；测试注入 IO 不访问真实提供商，也不会等待该窗口。

## 验证边界

共享声明与拒绝向量：`contracts/composition-v3.json`，Rust 与 Python 都验证。
Rust 使用临时目录与注入 GET 结果测试真实 request/status、持久检查点、失败后恢复、
完成步骤不重演、重复编号/变更内容拒绝、取消/停用的晚结果隔离、队列满和迁移保留。
`pc/scripts/test-component-workflows-offline.mjs` 可用已缓存的 Cargo 依赖运行小型离线 harness：
直接引用组合/表达式/记录/生命周期模块，提取实际原子保存实现，真实 HTTP 入口强制失败。
覆盖最后一步完成后的旧存档恢复、源错误隔离，以及总容量不足时撤销已排队但未提交的任务；
证据目录和运行标签由调用方指定，不读取用户组件存档。它不启动完整桌面客户端。
这些测试没有调用真实云提供商，不证明外部鉴权、设备 USB、实际屏幕或声学链路已验收。

### 无网络联调夹具

`contracts/workflow-local-v3.json` 包含独立合成 manifest/buttons/runtime/declaration 和
预期标量，不是正式发布包。可在临时目录把这些字段映射为 component.json、buttons.json、
runtime/widget.json、pc/application.json、share.json；negative-screen.json 取 runtime.dashboard。
只在用户明确要求连接验收时由有权限的主流程安装，不自动发布到用户组件库。

默认应显示 idle、0/0；真实 Start 动作经过后台 worker 后显示 completed、进度 100、
计数 1 / 保存行数 1；第二次明确 Start 为 2/2。重新读取或同 eventId 重放不能增计数。
每次本地执行很快，UI 不保证可见 queued/running 的过渡帧，不加假延时冒充进度。
Cancel/Resume 只在有效运行/失败状态允许；在途 GET 取消及失败恢复由注入 IO 测试覆盖。
此夹具声明的网络权限为空、没有 GET；正常操作不需要网络、Key、麦克风、扬声器或桌面任务。
