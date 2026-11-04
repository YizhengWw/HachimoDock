# PC 托管的“连接后启动”

这是 PC 在线时对已安装程序的一次启动策略。PC 程序必须运行、设备必须连接；它不提供设备断开 PC 后的开机自启，也不安装操作系统登录启动项。启用只保存用户授权，不立即启动。

保持固件公共 **1 个 PROGRAM 租约、aggregate 4096 fuel、750 µs** 及原有资源互斥。策略不创建租约、不改固件预算、不开放 RAM resume、不改变量 owner、不安装/更新组件、不占用音频，也不在音频结束后恢复程序。

## 已实现的文件

- `pc/src-tauri/src/program_autostart.rs`：持久授权、跨进程锁、每 boot 记录、撤销、只提交一次的 Host 接口及 Tauri commands。
- `pc/src-tauri/src/program_autostart/native.rs`：单原生连接 owner、8 项有界邮箱、boot 去重与音频/停止的 admission epoch；RX 只投递，不等待请求或磁盘。
- `pc/src-tauri/src/usb_serial/program_autostart_host.rs`：正式库与 USB 同 guard 下的真实内容/原生 checksum/boot/连接代次核验及一次固定 payload。
- `pc/src/component-center/program-autostart.js` / `.test.js`：纯模块控制器、严格数据验证、状态文案和定向测试。
- `pc/src/component-center/ProgramBackground.jsx` / `program-background.css`：v1/v2 手动入口、PC 托管授权预览/勾选/启用/停用、主动停止的 boot 抑制；不从页面派发启动观察。
- `program-background.js` / `.test.js`、`app_program_background.rs`：共同修复 r158 的 v2 能力协商，Rust 原有命令无需修改 USB 源文件即可通过正确的版本门禁。

## 授权和一次尝试的含义

授权保存 `Target { boardId, componentId, contentHash, checksums }`：

- boardId、componentId 精确区分大小写，不 trim、不按端口或“最近设备”替换。
- contentHash 是本地正式组件真实文件内容的完整 64 位小写 SHA-256；不能使用目录名、mtime、预览 JSON 或现有截断的 16 位 `versionHash`。
- checksums 是设备 `program-inspect` 返回的 `widgetChecksum/buttonsChecksum/spritesChecksum` 三项 8 位小写值，三者同时绑定，不能在启动时换成新 inspect 的值。
- bootId 使用设备已核对的 16 位小写 boot 标识，不使用 PC 进程 ID 或 USB 连接次数。

`component_content_hash(root)` 使用排序后的 UTF-8 相对文件名，逐文件 SHA-256 输入 `name + NUL + bytes + NUL`，完整输出，不截断。遍历最多 256 项、深度 8、文件总计 16 MiB，拒绝符号链接/特殊文件；root 由后端正式库解析，在现有库变更保护下读取，不能接受 UI 传入路径。设备原生 checksum 是 FNV 身份核对，不是设备对完整 SHA-256 的远程密码学证明。Host 必须确认本地版本确实是已同步版本，不能把“本地最新包”和不相干的旧设备内容拼成授权证据；无法证明时返回 `Unavailable`。

后台程序门禁同时要求明确 scalar `widgetProgram=v1|v2`、精确 background-v2、inspect-v1 和整数数量 1..4。`widgetPrograms` 可省略；若存在，只接受非空、无重复、最多两项的已知 v1/v2 字符串数组，且包含 scalar。数组不能充当 scalar，未来未知 scalar 不能借兼容数组开放。组件入口还必须在 scalar/兼容数组中明确协商其自身 engine。

新 autostart 策略额外要求租约数量恰为 **1**，且实际 capabilities 四项同时精确声明整数 `widgetProgramAggregateFuel=4096`、`widgetProgramMaxFuel=4096`、`widgetProgramQuantum=64`、`widgetProgramAggregateQuantum=64`（真实 r158 均有）。任何一项缺失、未知类型、收紧或放宽值均关闭自动策略，不从版本/其他预算推断默认值。手动后台 `supported` 保留原兼容性，不新增这些预算依赖；缺预算的旧固件仍可手动管理，但不能启用自动策略。JS 与 Rust 正例只为明确提供该公共能力的自动策略 fixtures 补齐真实四项，旧手动 fixtures 不补造能力；两端均逐项覆盖缺失/错误值负例。

全局最多一条授权。开始核验/发送前先持久化 `(boardId, bootId)` 的 `result_unknown_no_replay` 记录；记录键不含 componentId，因此停用、重新启用或换授权组件都不能重试同 boot。每次最多一个物理 start。核验忙/失败、已有任意注册（包括 stopped）、boot 变化、否定/丢失/无效回复均消费本 boot，没有定时重试和补发。`running` 仅表示那次启动曾获确认，当前是否运行仍以后台程序查询为准。

音频抢占/主动停止调用 `suppress_boot`，即使尚无授权也保存本 boot 的禁止自动启动记录；之后启用也不会恢复。已有未知结果不被音频事件改写成“已确认停止”。内容更新、组件删除、换设备撤销授权；恢复旧内容或接回原设备不会恢复授权。

记录最多 256 个 board+boot，**不淘汰**。满后撤销自动启动，继续保留所有锁存；手动启动仍独立可用。本版本没有“清空记录后同 boot 重试”接口。临时文件写入、文件同步、原子替换（Unix 同步父目录）完成后才允许 Host 提交。写入失败将服务阻止，不能继续发送；提交后的保存失败仍保留此前落盘的 unknown。崩溃、PC 重启、页面重挂载不会清掉记录。损坏/过大/初始化后丢失的 journal 不会重建成空授权；第二个进程不能同时持有服务锁。

## API

所有 Tauri command 已在新 Rust 模块定义，均使用 `tauri::State<ProgramAutostartState>`，阻塞工作放入 `spawn_blocking`；主线程只需注册：

| Command | 参数 | 行为 |
| --- | --- | --- |
| `program_autostart_get` | 无 | 读取 Status，不启动 |
| `program_autostart_inspect` | `boardId, componentId` | 返回只读 Target，供用户审阅 |
| `program_autostart_enable` | `target, userConfirmed: true` | 重新核验后保存明确授权，不启动 |
| `program_autostart_disable` | 无 | 撤销策略，保留全部 boot 记录，不暗中停止程序 |
| `program_autostart_observe` | `boardId, bootId` | 精确核验后最多一次尝试 |
| `program_autostart_suppress_boot` | `boardId, bootId` | 主动停止/音频抢占禁止本 boot 自动启动 |
| `program_autostart_component_changed` | `componentId, deleted` | 更新/删除前撤销（失败也不恢复授权） |
| `program_autostart_device_changed` | `boardId` | 不同设备撤销；同设备短暂断线不撤销 |

`Status = { schemaVersion:1, enabled, policy:Target|null, attempts:0..256, code }`。错误只反映固定状态，不带私有路径或设备错误扩展。

`ProgramAutostartState { pub service: Option<Arc<ProgramAutostart>> }` 是始终可 manage 的包装：`ProgramAutostartState::open(directory, Arc<dyn Host>) -> Self` 吸收存储/权限/锁失败，`unavailable() -> Self` 供宿主依赖尚未就绪时使用。`state.service()` 返回服务或固定错误 `program_autostart_unavailable`；UI 仅禁用连接后启动。**不得将可选 policy 错误传播成 Tauri setup Err。**

Rust 服务方法同名（去 command 前缀），另有 `ProgramAutostart::open(directory, Arc<dyn Host>) -> Result<Self,String>`、`status()`、`inspect_target(board, component)`；应用应使用上面的无 Err 包装初始化。`VerifiedProgram::from_device(board, component, fullHash, caps, inventory, inspected)` 验证完整库存和原生 inspect。其 `target()`、`boot_id()`、`has_registration()`、`start_payload()` 供适配器使用。

JS `createProgramAutostartController({invoke,onChange})` 提供 `getState/refresh/prepare({boardId,componentId})/enable(target,{userConfirmed:true})/disable/connected({boardId,bootId})/suppress({boardId,bootId})/dispose`。单 flight，不排队、不自动重发；refresh 只读。`supportsProgramAutostart(caps)` 复用严格版本门禁，并要求 1 租约及上述完整 4096/64 公共预算声明。

## 已接入的原生 hooks

原生集成已写入 `lib.rs`、`usb_serial.rs` 和正式库变更入口；保留现有 startup supervision、语音和 metadata 修改。UI 不承担调度。以下列出具体接线，供统一编译/定向检查复核。

1. **应用初始化/注册**：`lib.rs` 的 `mod app_program_background` 附近添加 `mod program_autostart`。`run()` 创建 USB manager 后，为正式 app-data 目录构造 `ProgramAutostartState::open(dir, host)` 并始终 `.manage(state)`，在 `generate_handler!` 注册上述 8 commands。app-data 路径/Host 不可用或 composer-diagnostic 模式用 `ProgramAutostartState::unavailable()`；不能让 setup Err、expect/panic 或白屏，也不能删除旧 journal 重试初始化。
2. **USB Host 适配器**：由 `usb_serial/program_autostart_host.rs` 实现，全部 read/write 绑定真实 `connection_id`，最终 send 在 serial connection mutex 下再次核对 admission epoch。正式库 resolver 使用现有 mutation guard、最多 32 个版本且不触发懒导入，guard 跨越重验和提交；忙即拒绝，避免库锁/策略锁互等。
3. **应用级连接 owner**：在 `usb_connect` 成功及 `connect_for_auto` 成功分支（刷新 binding、验证真实 hello 后）调用 `device_changed(realBoardId)`；这个撤销必须覆盖任何新设备，包括不支持后台程序的设备。待完整库存和原生 boot 查询就绪后，支持能力的同一连接调用 `observe(realBoardId, realBootId)`。已在线的 PC 应在服务初始化后做一次同样的观察。不要从 USB 读线程同步等待自己的 request/reply；交给现有阻塞 worker。新 boot/同 boot 重连都可以送达，journal 是最终幂等门禁。
4. **更新/删除 owner**：在原生 `install_clawpkg_over_usb`、`remove_widget_from_device`、`delete_component_from_library` 和实际本地发布/更新入口，在变更之前调用 `component_changed(id, deleted)`。在拿 USB/库 mutation guard **之前**撤销；撤销失败不得继续依赖旧授权自动启动。恢复组件不恢复授权。D 的 ComponentCenter 删除/sync 可提前 invoke 同名撤销 command，但原生入口仍必须覆盖其他调用方。
5. **主动停止/音频抢占 owner**：`usb_stop_background_program` 提交 observed boot+registration 的停止前，以及音频资源抢占/显式前台程序停止事件，对已验证 board+boot 调用 `suppress_boot`。实时音频线程不得等待磁盘，交给现有控制 owner 顺序处理；不能把音频结束、资源释放或录音停止事件接成 observe/resume。所有 guard/callback 的锁序都要避免回调本服务时仍持有 USB/库锁。

UI 已挂在现有 Provider/Start/Status 中，无需新增 ComponentCenter 容器。Provider 的 mount/刷新/enable 均只读或保存授权，**不调用 observe/start**；调度由上述 native 连接 owner 派发，因此页面关闭/隐藏不影响托管。当前 UI 的停止先请求 boot 抑制，再保留原 observed-owner stop。其他设备/删除/音频路径仍必须接全局 hooks。

正式库 `publish_source`、内置发布、删除、恢复共用 mutation guard，并在真正变更前撤销；USB installer/remover 在拿 transfer guard 前撤销。停止入口同步抑制 observed boot；音频发送与 RX begin/media 动作先使 in-flight epoch 失效，由单 worker 保存抑制。已核对的 boot 按 board+真实连接代次缓存；未知 boot 才做一次原生 query。邮箱溢出或无法安全保存时关闭自动准入，不自动重试。

`UsbConnectionStatus.connectionId` 是真实非零 u64 的十进制 **字符串**，断连/未知为空；`bootId` 仅来自真实 hello `capabilities.debugBootId` 的 16 位小写 hex，未知/断连为空。缓存和 serde 兼容旧快照，不构造虚假代次。`media/status` 是唯一允许缺省 board 字段的响应；仍要求匹配本次 requestId、发送前后同一真实 generation 和 ok=true，若带 board 则必须精确匹配，其他响应严格要求 board。`ended` 仍持 MUSIC lease，组件同步及自动启动均阻断。

## Host 适配器的提交边界

```rust
trait Host: Send + Sync {
    fn inspect(&self, board: &str, component: &str)
        -> Result<VerifiedProgram, CheckFailure>;
    fn start_once(&self, expected: &VerifiedProgram)
        -> Result<app_program_background::Outcome, CheckFailure>;
}
```

`inspect` 只读，使用真实连接的 `runtime=esp-p4`、严格 capabilities、完整 `ok/complete/recoveryBlocked=false` 的库存、原生 program-inspect 和新计算的正式内容 hash，生成 `VerifiedProgram`。超时/不完整库存不是“组件删除”；仅完整证据才返回 `ComponentDeleted/ContentChanged/DeviceChanged`，否则 `Unavailable`。

`start_once` 必须遵循此顺序，不能仅依靠前面的 inspect：

1. 使用现有 transfer/连接/资源互斥保护，忙就拒绝，绝不创建并行 PROGRAM 通道或重试队列。
2. 在保护下重新解析正式组件、新计算 hash、重读设备库存及 inspect；与 `expected.target()` **全部字段**和 `expected.boot_id()` 精确比较，检查 `fresh.has_registration()==false`，保留音频及 foreground PROGRAM 的原生准入门禁。
3. 固定当前 connection generation，使用 **`expected.start_payload()`**，仅一次发送 `widget/program-start`；固件仍核对 boot 和三项 installed checksum，继续由既有单 PROGRAM/4096 fuel/750 µs/音频互斥决定准入。
4. 使用同一份已匹配 `Installed` 调用 `Outcome::started` 校验回复；发送后任何丢失、否定或无效回复都返回 `Ok(Outcome::unknown())`。`CheckFailure` 只用于明确未发送的前置拒绝。

**不能调用现有 `start_background_program(board,id)` 来替代步骤 2/3**：它会重新选择当前内容，可能启动用户未授权的新版本。也不能在持有 transfer guard 时调用会再次拿同一 guard 的 `query_widget_inventory/query_background_programs/start_background_program`；需要复用 USB 模块内原有受连接约束的私有 send helpers。Host 不得回调持有其锁的策略服务。若尚不能提供这个原子适配器，应保持新功能不可用，不应临时用 board+id 启动绕过。

## 验证记录

公共预算准入窄修后，仅运行 `node --test pc/src/component-center/program-autostart.test.js`，**20/20 通过**；四项预算逐项缺失/错误值及旧手动兼容性均覆盖。新增 Rust 测试 `program_autostart::tests::public_autostart_requires_exact_4096_fuel_and_64_quantum_capabilities`，留待父线程统一执行。下述 27/27 为预算窄修前的集成验证记录。

已执行纯 Node 定向测试，**27/27 通过**（包括内存 JSX 转换和 React SSR，无 GUI/浏览器/USB）：

```sh
node --test pc/src/component-center/program-background.test.js pc/src/component-center/program-autostart.test.js
```

另用 `OUT/r159-fix-all-ojrsrcfy/r158-actual-capabilities.json` 实际能力 JSON 只读断言 v1/v2 后台门禁和单租约 autostart 均通过。

Rust 模块内提供策略/落盘顺序/重启/并发/撤销/抢占/满账本/损坏记录/hash/可用性包装/精确公共预算的 17 项测试，共享 Rust 后台模块也新增严格 v2 协商测试。**本阶段未运行 Rust 构建或测试**。未触碰串口、GUI、PIO、全回归、依赖或 commit/reset。后续 Rust 验证须先统一协调，并使用 `CARGO_TARGET_DIR="$HACHIMODOCK_CARGO_TARGET"`（预先设为已有共享编译缓存），不得重建默认 target。

原生集成新增的精确测试名（未执行；交父线程 focused）：

```text
usb_serial::program_autostart_host::tests::host_exact_content_fixed_payload_and_correlated_ack_hold_library_guard
usb_serial::program_autostart_host::tests::host_content_checksum_mismatch_never_selects_replacement_or_sends_start
usb_serial::program_autostart_host::tests::host_changed_local_bytes_boot_generation_or_preempted_epoch_cannot_submit
usb_serial::program_autostart_host::tests::host_library_guard_conflict_fails_without_usb_work
usb_serial::program_autostart_host::tests::media_reply_may_omit_board_only_with_exact_request_and_generation
usb_serial::program_autostart_host::tests::empty_and_native_sprite_checksums_use_firmware_pack_layout
program_autostart::native::tests::preemption_invalidates_inflight_epoch_without_automatic_readmission
program_autostart::native::tests::bounded_mailbox_never_waits_or_discards_overflow_as_a_new_permission
usb_serial::tests::status_returns_the_last_snapshot_while_the_writer_lock_is_busy
usb_serial::tests::component_sync_ended_music_retains_lease_until_owner_releases_it
```

Host 内存 writer 使用真实 request waiters，先送错误 requestId 再送正确 ACK；验证真实 >2^53 connectionId、SHA/原生 checksum、固定 payload 恰发一次、提交时 library guard 仍持有，以及内容/boot/generation/epoch 变化均无 start。另已挂载 D 的 `#[cfg(test)] mod component_sync_safety_tests;`（10 项），原 background_controller 三项保留。命令 allowlist 保留 `frontend_startup_generation` / `frontend_startup_ready` 并增加 8 项 autostart。
