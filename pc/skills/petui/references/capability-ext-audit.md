> 最终冻结更新：本线 internal-action / 两阶段事务定向回归 **17 passed in 1.73s**，日志 `/tmp/cext-scope-transaction-tests.log`。7 个私有 fixture 已全部通过完整 Python 包校验，无伪造实体绑定。14 组 `internal-actions-v1.json` 向量由实际 C parser + 完整 Python 包校验覆盖；Rust 真实 `validate_clawpkg_at_path` 回归已编写，rustfmt parser 通过，未运行 Cargo。A 已接管并完成 UTC/hidden checkpoint，B 已完成四个事务调用端与 renderer generation/retire；下面相对时钟与共享入口 TODO 属于前轮历史，不再代表当前阻塞。当前接口与调用顺序以 scratchpad `track-c-A-B-interfaces.md` 及源码为准。

# Track C 收口交接（2026-11-02）

仅记录实现与离线证据，不代表固件已构建、设备准入或 HIL 已通过。五个 Track C flag 仍默认 0，clip 的 worker H.264 `#error` 依赖未放松。本轮没有运行 cargo、PIO、串口、固件打包或 git commit/reset/clean。

## Borrow 生命周期与锁约定

`pet_p4_component_media.c/h` 采用 **3 个固定 bank、8 个 token**，无链表、无动态 retire 队列。sample 与 clip 必须先 pin；旧裸指针接口返回 NULL。unload 立即撤销可见性，活动借用的内存留在固定 bank，最后一个 token 归还后回收。load 在备用 bank 完整校验成功后切换，失败保留旧代；备用 bank 耗尽时返回 false。token 不复用，达到序列上限后拒绝新 pin；visibility generation 到 UINT32_MAX 后拒绝新 load，终止 unload 进入永久 invalid generation 0，避免 ABA。

音频队列和 voice 分别拥有 token：成功入队转移所有权；覆盖旧队列、替换 voice、自然播放结束、停止/录音阻塞时归还。**音频只调用 `pet_cmedia_unpin_deferred()`，最多 8 次 32 位 lock-free CAS，不获取 media_mutex、不 free、不读文件、不创建线程。** store 下一次操作/显式 collect 在锁内回收。`miniapp_ext_process` 与 publish 已在非音频 lane collect。

锁约定：

- 音频持 stream_lock 时不得调用任何会锁 media_mutex 的 store API；只允许 deferred release。
- store 持 media_mutex 时不得调用音频 API、获取 stream_lock 或回调 model/renderer。
- sample pin 在 stream_lock 外完成，`pet_p4_audio_cext_sample_lease` 只在 stream_lock 内转移现有 token。
- ext snapshot/publication lock 保护 generation 校验与音频入队；store 操作返回并释放 media_mutex 后才能获取 stream_lock。模块没有 media_mutex -> stream_lock 的嵌套。
- clip descriptor 和生命周期元数据受现有 `g_asset_prepare_mux` 保护；mux 内不调用 store/broker/codec/free。

sample 接入样例（调用方持组件 snapshot/publication 互斥，而不是 audio/store 锁）：

```c
pet_cmedia_sample_lease_t lease = {0};
if (pet_cmedia_sample_pin_at(index, captured_generation, &lease)) {
    if (!pet_p4_audio_cext_sample_lease(&lease)) {
        // 失败未转移所有权；成功已经清零 lease。
        (void)pet_cmedia_unpin(lease.token);
    }
}
```

现有 miniapp 音频路径已接入：`pet_cext_runtime_t.media_generation` 在 publish 后绑定；`pet_cext_audio_take()` 在现有 runtime lock 内连同音频状态复制 generation，旧 apply 包装器据此调用检查接口。迟到的旧 snapshot 不能把 sample 索引映射到新包。新增 `enabled` 防止无新 sample_pending 的普通更新截断仍在播放的一次性采样。

clip 的一个 token 覆盖整个 decoder 生命周期，不在 draw 返回或单帧 worker 完成时释放。`renderer_ext_clip_retire()` 先请求现有 `h264_prefetch_reset()`；只有它返回 true，且 `decoder_readers == 0`、prefetch 为 EMPTY，才结束 buffer broker lease、清 descriptor 并 unpin。QUEUED/WORKING/CONSUMING、取消尚未物理返回或 decoder cleanup 尚未结束时均保留源指针。固定 descriptor 在此之前不复用。DISCARDED 也进入完整 retire，防止继续使用已重置 decoder 的中间帧。

## 主代理必须接入的共享入口

声明见 `firmware/main/pet_p4_component_media_clients.h`。本轮没有直接修改共享 main/miniapp/renderer、protocol、clawpkg 或公共校验器。

1. **检查 `miniapp_ext_publish()` 的 bool 返回值。** `pet_p4_miniapp_ext.h` 已改为 bool。共享 miniapp 四处调用（内存激活、事务激活、install 的 ext_swap、删除活动组件后的替换）必须在 false 时中止新的 runtime/catalog 发布。不能继续 `g_runtime = *next`。把此检查置于不可回滚的 catalog 提交之前；具体回滚归事务 owner。store 加载失败本身不会停止旧音频或释放旧代。
2. **renderer view snapshot 携带 generation。** 与 runtime/view 同锁复制 `g_runtime.cext.media_generation`，不得在 unlock 后另查当前 store generation 去标注旧 view。每个渲染 snapshot 调用 `renderer_ext_media_snapshot(view.media_generation)`；big 的 meta/tile 读取和 clip pin 均检查这一代。未标注 generation 的 draw 安全拒绝。
3. **renderer 隐藏/退出/切回主页/错误路径持续 retire。** 在 render thread 调用 `renderer_ext_clip_retire()`；false 仅代表 worker/decoder 仍在清理，后续帧继续调用。在 true 前不要复用 clip descriptor，也不要开始新的主页面 decoder 源。不能仅在销毁瞬间调用一次后丢弃重试。
4. **取消后的私有存档 checkpoint 已接入。** `pet_p4_miniapp_alarm_cancel(widget_id, alarm_id)` 对 parked 状态取消后，锁外调用有界私有 checkpoint；`save_app_runtime()` 也轮询重试一个 dirty parked 条目。它只合并已有私有 JSON 的 alarms 成员，保留 version/state/vars，不加载或执行隐藏 runtime/program。每次调用独占 PSRAM workspace；OOM、缓存不可读、输出不足或存储更新失败保留 dirty，不写默认空存档。entry serial/revision 拒绝迟到快照，避免覆盖更新取消或新代组件。bool 取消结果仍是内存接受；checkpoint 入队不等于 A/B worker 已落盘，不能据此回报耐久成功。
5. **UTC 时钟与持久 deadline 已接入。** `miniapp_utc_snapshot(now_ms)` 在现有 clock mux 中一次捕获 valid、UTC ms、monotonic_at_sync、calibration_generation；UTC 独立于 utcOffsetMinutes，`local_wall_seconds()` 继续只供旧本地日期功能。新 alarm-clock API 保存 UTC deadline（`:u`）；重启或时钟异常时进入 WAITING_FOR_CLOCK，不拿 uptime/0 比较。首次尚未校时的延迟保存为 `:w`，在获得有效校准后才开始完整延迟。旧 remaining_ms 文本仍可读，迁移时保持其剩余时长暂停语义。周期校准后 O(1) catch-up，最多一条 pending，不循环补发。


渲染入口示例（共享 owner 负责具体 frame 分支）：

```c
renderer_ext_media_snapshot(view.media_generation); // 值已在 runtime lock 内捕获
if (!view_visible || !view_has_clip || leaving_component) {
    if (!renderer_ext_clip_retire()) {
        // 等待下一帧继续 cleanup；不可开始新的 decoder 源。
        return;
    }
}
```

## 预算与 FFmpeg

- store 3 bank 的 sample/clip/table payload 上限约 2.25 MiB；staging arena 上限约 1.75 MiB，reset 立即释放。加载临时 sample buffer 最多 64 KiB + 8。加上最多 3 bank，store 这一部分峰值约 4.064 MiB；不包含 renderer 既有 framebuffer、tile cache、decoder/pack 静态空间。
- 设备媒体文件预算为 4 MiB。persist 对固定合法命名空间执行最多 896 次 stat，并把现有 final/tmp 和本次 staged bytes 都计入保守峰值。预算不足拒绝写入，不通过删旧包挤空间。slot/generation 有界；load 对两张 big image 合计实施 1 MiB 预算。锁保护 persist/remove 和 tile IO。
- 文件级 rename 失败保留旧文件；不宣称多文件 persist 是原子提交。半成品 inactive generation 的事务清理仍归 catalog/storage owner。
- Python 与 Rust 输入读取最多 4 MiB + 1 byte，拒绝无效路径和越界 symlink。Rust 将已准入的字节写入至多一个临时输入文件，FFmpeg 使用该不可变输入，不追读可能增长的原文件。PNG 先检查尺寸，再使用 image reader 的 1280x960、16 MiB allocation limit。
- FFmpeg 输出只走 pipe，不落地无界输出文件；sample 限 64 KiB + 1 byte 检测，clip 限 512 KiB - 16 + 1 byte 检测，超限失败，不静默截断为合法资源。stderr/ stdin 指向 null，不无界捕获 stderr。
- 单次 native conversion timeout 为 30 秒，错误、超限、超时均 kill + wait。最多一个 native process/stdout reader；若异常继承的 pipe 在 child 退出后仍不关闭，固定 admission 仍被占用，后续任务拒绝，不能累积 reader 线程/队列。FFmpeg `-threads 1`、`-max_alloc 16 MiB` 限制单 allocation，**不是整个进程 RSS 硬上限**。
- Track C 不再调用共享 resolver 的无 timeout `-version`/`-encoders` subprocess。仅选择已知 bundled/env/PATH 路径，在受限 conversion 本身验证可用性。未修改其他业务的共享 resolver。

## 事件与计时语义

`at_ms_var` 仍是延迟毫秒数，不是 uptime 截止点或 Unix epoch，未新增包声明字段。
新私有记录为 `id:<UTC deadline ms>:u[:p][:v<last_var>][:c]`；未校时新建延迟使用
`id:<remaining ms>:w[...]`。旧 `id:remaining_ms[:p][:v<last_var>][:c]` 保持兼容。
未知时钟保留 armed/pending/取消信息，但不能触发 action 或排通知；校准后再判断。
格式校验失败不部分修改；新 reader 两遍扫描，无整份 alarm set 的局部副本。
四条记录的输出仍严格小于 PET_CEV_SAVE_TEXT_MAX=256；只包含安全 ASCII，连同
JSON 字段包装不超过 store-budget-v1 的 268 字节预留，三端预算未放宽。

当前活动组件隐藏与 parked 组件均继续 tick；background 提醒须本 boot PC 授权，只排有界通知，不运行隐藏 action/program。通知槽占满时保留 alarm.pending，空出后补发；明确取消清 armed/pending 和同一 alarm 的待通知，删除清整个订阅/parked/待通知记录。周期 catch-up 为 O(1)，不逐周期追赶。

## 离线证据

轻量测试命令：

```
python -m pytest firmware/tests/component_events_owned_test.py firmware/tests/component_media_owned_test.py firmware/tests/petui_capability_ext_test.py -q
```

最近通过 **33 项**（包含 7 个私有 fixture 的实际 C parser / 素材检查）。日志 `/tmp/cmedia-lease-tests.log`。实际 store 用 pthread mutex 及真实临时媒体文件、ASan/UBSan 运行；跨线程持 sample/clip 后 unload/swap、三 bank 耗尽、八 token 耗尽、double/stale release、旧 snapshot、全部归还后的实际释放均有断言。测试直接编译生产音频 Track C 区域和 renderer clip 片段；在持 media_mutex 时让另一线程停止音频，并在超时约束内 join，覆盖 ABBA 回归。decoder SDK/broker 用 host stub；物理 decoder/HIL 尚未验证。

事件测试编译实际 `_ext.inc` 中的事件/闹钟路径，覆盖隐藏到期、未授权待通知、授权后补发、parked 计时、取消与删除；无隐藏 action dispatch。

Rust 仅完成 rustfmt parser 检查，**未运行 cargo**。待主代理统一执行 `component_capability_ext::tests`：包含 bounded read/WAV 结构、native stdout overflow/stderr/timeout/失败退出、shared vectors、完整 host package 的真实 `clawpkg::validate_clawpkg_at_path` 回归。完整包按 sample-only、clip-only、sample+clip 三种组合构造，合法引用应通过，加入有效但未引用的 WAV 后必须以额外文件拒绝。MP4 fixture 为自产 160x120、15 fps、单帧黑色视频，路径 `pc/src-tauri/src/component_capability_ext/fixtures/one-frame.mp4`，由 FFmpeg lavfi + libx264 生成，仅供离线测试，不涉及真实设备或用户数据。

Track C 五处 `#ifndef` 声明已把行尾注释移至前行；严格默认 0 声明 regex 和 clip `#error` 保留。此前 parent suite 剩余失败是范围外的 `PET_P4_CANVAS_V2` 依赖检测；不通过改测试门禁绕过。

私有 fixture、动作/期望和源码观察字段见 `capability-ext-hil.md`。5 包完整 Python 校验通过，event-recv/alarm 两包被公共物理绑定校验阻塞；没有伪造绑定。`pet_cmedia_stats()` 为只读 main-lane C API（会 collect deferred），尚无 wire endpoint。

## 本工作线改动文件

- 固件 store：`firmware/main/pet_p4_component_media.c`、`.h`，新 adapter 声明 `pet_p4_component_media_clients.h`。
- 固件声明/内核：`pet_p4_component_ext.c/.h`（snapshot generation/enabled 与有界容器）、`pet_p4_component_events.c/.h`（恢复/取消/计时）、`pet_p4_sfx_core.c`（采样释放/边界）、`pet_p4_component_features.h`（仅五处注释位置）。
- 调用端：`pet_p4_miniapp_ext.inc/.h`、`pet_p4_renderer_ext.inc`；`pet_p4_audio.c` 仅新增 Track C 区域，voice/AEC/frontend/优先级路径未动。另修复 non-loop sequence 被持续 requested 自动重启的问题，host adapter 测试覆盖自然结束后不重播。
- PC：`pc/src-tauri/src/component_capability_ext.rs` 与其 `component_capability_ext/fixtures/one-frame.mp4`；`pc/skills/petui/scripts/capability_ext.py`。
- 测试：`firmware/tests/petui_capability_ext_test.py`、`component_media_owned_test.py`、`component_events_owned_test.py`、`prepare_track_c_private_fixtures.py`、`fixtures/track_c_private/`。
- 文档：本文件与 `capability-ext-hil.md`。

最终 host 记录：`/tmp/cmedia-lease-tests.log`，**33 passed in 3.63s**；rustfmt parser 检查通过，Rust 编译/测试仍由主代理统一执行。本清单不把工作区其他已有半成品归为本线修改。


UTC/hidden checkpoint 新增离线证据：`firmware/tests/p4_alarm_utc_test.py` 抽取实际 clock accessor、ext process、cancel 与 checkpoint，结合生产 alarm core，覆盖时区切换不改 UTC、未知时钟/回拨、重启过期 pending、旧 remaining 兼容、周期 catch-up、坏格式原子拒绝、四条最长序列化、PSRAM OOM/存储拒绝重试、保留 vars/state 和取消后恢复不重响，且没有隐藏 action/program dispatch。与旧事件/能力和存档预算定向组合为 45 passed；不是设备/HIL 证据。
