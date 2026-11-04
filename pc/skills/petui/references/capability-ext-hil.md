# Track C 私有一轮新能力验收

本轮仅准备源目录与离线验证；**未安装、未打包、未跑设备、未取得 HIL 通过证据**。不得以旧 1200-query 或仅 hello/query 成功代替下面的真实输入、声音、画面与释放证据。

源目录：`firmware/tests/fixtures/track_c_private/`。`manifest.json` 给出每个目录、素材来源/SHA-256 和完整 Python 校验结果。生成器 `firmware/tests/prepare_track_c_private_fixtures.py` 只从 contract 与现有 assets 生成独立私有目录，不修改正式 library，不启动任何连接；为防覆盖，目标已存在会拒绝重新生成。

所有 fixture 是工具包，采用契约默认画布、原生文字和最小计数显示；无新增美术。只使用 `SW1/SW2` 和摇杆；全局退出不在包中绑定。计数表示收到的命令/动作，不能冒充声卡或 decoder 的实际完成状态。互斥的媒体能力分包；事件发送/接收要同时保留在设备目录中。

## 统一构建前硬前提

- 完成 audit 文档列出的 checked publish、view generation 与持续 clip retire hook。
- 实际候选 hello 必须协商相关 capability。不能把本地 `DEFAULT_CAPABILITIES` 当作实体设备报告。
- 当前 7 包全部通过完整 Python 校验。C / Python / Rust 按已注册 subscriber / 精确 alarm target 作用域豁免内部动作；普通动作仍要求物理或既有 touch 路径。14 组共享正负例与真实包回归已添加，未造假绑定或放宽能力门禁。Rust 完整包执行由主代理统一完成。
- 完成候选专属资源观察接口后才能运行释放验收。下面字段是源码 API/私有结构名，**不是已有 wire 字段**；字段缺失记 BLOCKED，不以固定 sleep 或 query 成功补齐。

## 一轮动作与期望

| 顺序/源目录后缀 | 动作 | 新能力验收期望 | 退出/切换释放检查 |
|---|---|---|---|
| 1 `sequence` | 打开先静置；SW1 `start`；待至少 5 秒；SW2 `stop`；再 start | 打开不自动响。120 BPM 的契约三音/休止共 8 拍，约 4 秒后自然停止，不因 requested 仍为 true 自动重播；stop 即停止，stop 后可再 start | sequence state inactive；队列/voice token 为零；不抢录音/AEC 前端；退出后不再发声 |
| 2 `sample` | SW1 `pet` 一次，再短间隔按 3 次；播放未完立刻全局退出；重进后静置 | 复用 `pc/public/instruments/wooden-fish/knock.wav`，1 秒、16 kHz mono。每次命令计数增加；实际输出一次性 PCM；快速连按最多 2 queued + 2 voices，不无界排队。重进不自动播旧 sample | 自然结束/队列覆盖/退出均归还各自 token；deferred 回收增加；退役 bank 最终清零 |
| 3 `clip` | 进入 `show`；观察实际动态视频；SW2 hide；快速 SW1 show；decoder 工作时切至 sample/big | 复用真实动态 `touch.lick.mp4`，由正常 PC 编译阶段压为 320×240/15 fps/≤5 秒；不能用单帧黑屏证明。show/hide 计数变化；帧顺序依赖 predecessor；hide/切换不读已释放源 | QUEUED/WORKING/CONSUMING 或 decoder cleanup 未完时 source pin 仍有效。`renderer_ext_clip_retire` false 不释放、不改 descriptor；物理完成后 true，readers=0、phase=EMPTY，clip pin/broker buffer lease 归还 |
| 4 `big` | 初始看左上；SW2 `bottom`；摇杆右/左/上/下；SW1 origin | 复用现有 640×480 PNG，320×240 可视窗口，6 个 tile。显示内容随 offset 改变，最大实际偏移 (320,240)，负值/超大值被夹住；跨 tile 边界无缺块/越界 | 每帧至多一个缺失 tile load；generation 改变不能取新包数据填旧 node；退出后 table_alloc_bytes 回到基线 |
| 5 `event-recv` + `event-send` | 两者完成安装后先打开 recv；SW1 clear；切 send；连续发送 10 次；返回 recv | 只由 send 的 SW1 发 `done`。recv 未绑定假物理 event。队列上限 8，孤立测试时 delivered 8、dropped 2；返回后 rx 增加 8。需记录实际 emitted/delivered/dropped 与 rx，不以 UI 计数单独证明总线 | 删除 recv 清订阅/队列；再次发无旧订阅 delivery；不触发隐藏程序执行 |
| 6 `alarm` | SW1 set（60000 ms）；切到 big 等待；分别测试本 boot 通知策略 false/true；回到 alarm；SW2 cancel | 一次性计时不是 epoch deadline。隐藏仍到期；未授权保留 pending，授权后补发一次通知，无隐藏 action/program 执行。可见时才走 ring transition 增加计数。cancel 后不再次到期 | 记录 armed/pending/last_var/deadline 与通知归属 id；明确取消清同一 alarm 待通知；删除清 parked/notice/subscription |
| 7 `alarm` reboot/负向 | arm 后约 20 秒 checkpoint；重启未校时恢复；再次观察同一 delay var；取消 checkpoint 后再重启 | UTC 路径由 A 完成：先在有效 UTC 下 arm/checkpoint，再重启未校时应保留 waiting pending，不误触发；再次校准后按原 UTC deadline 比较，到期只通知一次，未到期不能重置为新 60 秒。取消 checkpoint 后重启不得复活。须由统一实机轮次补证，不能把 A 的 host 结果称为 HIL 通过 | 记录 UTC snapshot valid/calibration generation、utc_deadline_ms/waiting_for_clock 和 hidden checkpoint 的 durable 状态；不得把 boot uptime 当 epoch；A 的定向证据归 A，不计入本线结果 |
| 8 合并切换 | sequence→sample（播放中）→clip（worker 工作中）→big→event/alarm→主页，重复有界轮数；最后等待物理 cleanup | 无 stale snapshot 触发新包 sample，无旧 clip 源提前 free，无 ABBA 卡死；bank/token 数不随轮数增长 | 回到主页且 cleanup 完成后，store 的 live_banks/live_tokens/bank_pins 和三类 allocation 都归零；broker 回到进入测试前基线；不以全局 heap 瞬时差为唯一证明 |

当前 one-shot 样例没有周期取消按钮，不伪造未实现的 alarm_cancel runtime op。周期 catch-up/取消已有 host core 回归；需要专门物理周期验收时，应另建纯 periodic 契约候选并接明确取消入口，不能拿 one-shot 验收冒充。

## 资源观察表（由主代理接入私有候选接口）

`pet_cmedia_stats()` 已在 owned store 实现，main lane 调用同时 collect deferred releases：

- `generation`、`tokens_issued`、`tokens_reclaimed`、`deferred_reclaimed`。
- `live_banks <= 3`、`pinned_banks <= 3`、`live_tokens <= 8`、`bank_pins == live_tokens`（snapshot collect 后）。
- `sample_alloc_bytes`、`clip_alloc_bytes`、`table_alloc_bytes`，不包含 decoder/framebuffer/staging。
- 全部物理 reader 结束并退出后：`tokens_issued == tokens_reclaimed`，上述 resident bytes、live_tokens、live_banks 为 0。

renderer 需要同一观察点：`g_h264_prefetch_lifetime.phase/cancelled`、`g_clip_entry.decoder_readers`、`g_clip_lease.token/generation`、`g_clip_entry.buffer_lease` 的 broker current/finish 状态，以及 retire 返回值。不要在 worker 仍占有时强制读/改 descriptor。

音频 Track C 需要在已有 stream lock 所有者下观察：`g_cext_sample_queued`、`g_cext_voice_tokens[]`、sampler.voice active、sequencer playing/started；只读记录不增加录音/AEC 路径等待。

事件/闹钟记录 bus emitted/delivered/dropped、目标 queue.count/dropped；alarm 的 widget/id、armed/pending/last_var/deadline_ms、通知策略与 notice_pending/归属。源码私有字段尚未公开时必须明确标记未观测。

## 证据文件要求

每轮至少记录：实际候选固件 identity/flags、真实 hello、fixture 源与素材 hash、输入时序、独立声学/视频证据、切换前/中/后的资源快照、cleanup 完成点。事件/闹钟必须含 sender→bus→receiver 或 arm→due→notification 的关联记录。

`manifest.json` 的 `deviceEvidence=false` 在离线交付阶段保持不变。通过 host parser/结构校验不修改它。失败、BLOCKED 和未覆盖项分别保留，不能统一写成全部支持。
