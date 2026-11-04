# 本地随身听：候选交付与机制

目标闭环：PC明确选择本地文件导入私有曲库；设备/PC选择稳定歌曲key后显式播放，显示真实回执进度、封面和歌词；暂停/停止/退出/断线/语音打断终止旧播放，不自动恢复。空库提示先导入，失败保持旧曲库，不自动重放。

新建包 `local-music-player`，不是旧全局 audio.player 的改名版本；只使用注册的 `kind:local-media` 通用提供器。没有账号、付费、搜索、在线URL、包内脚本或组件ID分支。保留旧全局 AVAILABLE=false。

- SW1播放当前列表选中项，SW2播放/暂停；摇杆上下改变稳定键选择，左右以5为步长调节音量，中按切换播放模式。全局退出不在包中绑定。
- 队列由host collection分页；下一首/上一首/停止/精确进度/确认移除在PC通用服务面板或支持触控的候选设备操作。当前touchReady=false仍有核心实体键闭环，不冒充触屏可用。
- title/artist/lyric/mode/status由服务标量投影；progress由真实position/duration短暂外推；没有可靠时长就不提供seek。
- 静态唱片PNG是真实fallback，动态封面只接收本地sidecar解码结果的有界handle；不是预置假专辑。原曲标题/私有路径不进包，展示字段遵循63字节上限。
- 视觉为clean/ocean：深靛背景、暖金唱片、蓝青重点、圆角队列和清晰文字层级。主体原生PNG，所有按钮/文字动态绘制，不把控制烘焙进背景。

状态：SOURCE_CANDIDATE / NOT_VALIDATED。本包要求真实 `widgetServiceBridge=p4-service-bridge-v1`、host collection与其他声明能力；现有r158缺少bridge能力，禁止发布或同步到该设备。候选expected能力仅供后续离线验证，不能替代真实协商。统一编译、专用测试、声学、退出/重入及资源释放验收由主线程安排；此处不宣称通过。

## 共享入口接入（主线程合入）

`ComponentAppPanel.jsx` 引入 `./ServiceBridgeControls.jsx`；用新控件替换原有两块 `service==="bridge"` 的导入按钮和原始 host.* 参数表单：

```jsx
<ServiceBridgeControls component={component} data={data} enabled={enabled}
  busy={busy} onCommand={command} onError={setError}/>
```

保留 AppSurface、现有请求身份/事件ID、轮询、context确认、图片handle加载与 detach，不能由新面板直接invoke绕过这些流程。该组件无组件ID特例。

公共校验器 `validate_generated_widget.py::validate_buttons` 应将已验证 `surface.collection_view.actions` 算作真实生成的行按钮；当前只看 elements 会误报集合行未绑定。固件/Rust同样须保留真实集合作用域，不能泛化豁免所有host动作。此共享入口由主线程处理。

r159 public待验收构建依赖：新增 `PET_SERVICE_BRIDGE_ENABLED=1`，保留已开启 `PET_HOST_COLLECTION_ENABLED=1`；无需 variable-owner、RAM resume、PERIPHERAL权限。evboard默认0。包的host_contract在最终runtime/PC声明冻结后需再绑定；当前包源码已绑定。

专用回归已编写但未执行：native实际逐会话锁争用/设备旧会话退役、provider方法及删除确认。资产退休epoch防止解码跨退出重新插回缓存；全局缓存32项、每组件2图，配额保持。源码修改后需要主线程安排统一验证，不能引用旧候选报告。

## 候选产物

`pc/candidate-clawpkgs/local-music-player/` 为可审源码，旁边的 `local-music-player.clawpkg` 是同字节ZIP候选。`local-music-player.candidate.json` 记录完整文件SHA及contract；expectedCapabilities明确是待验证预期，不是r158设备报告。包未发布到正式library、未安装设备、未执行候选自测。真实完整包validator回归已写入native测试，统一验证时须执行而不是只校验Plan。

## 统一验证待执行清单

1. 源绑定候选编译/原预算/真实hello，保持public待验收与evboard默认区别。
2. 原生实际package校验、Python候选expected校验、结构/输入回归；缺bridge真实能力的r158必须明确拒绝。
3. 0/1/20曲目、中文长标题、同名LRC/PNG/JPG、损坏库/写盘失败、重复文件、不存在路径、空导入；未点击播放前无音频。
4. 播放/暂停/继续/停止/选曲/音量/模式/seek/删除确认及拒绝；未知ACK只查状态、不自动重放。
5. 播放中退出/断连/进入语音/更新/停用；旧view与旧contract不得恢复播放。资源归还、原有音频门槛与实际声学结果独立记录。
6. PC控件使用同一command事件ID/context路径，实体键实际控制且退出服从全局设置；touchReady=false不改为true、不要求触控主流程。

PC新控件专用参数回归在 `pc/src/component-center/service-bridge-controls-model.test.js`，覆盖原生命令无额外路径参数、常量/选择引用不变成事件参数、整数字段空白/非整数/越界、UTF-8字节限制及bool false。未执行。

队列采用2行、每行72px，14条命令，7个标量、1条时间轴与1张动态图；保留2048字节原快照门槛及最终envelope检查，没有加大任何图形/内存/帧预算。

## 本轮 LocalPlayer 收口

- `music_player.rs`本地LRC/封面/曲库从打开的regular-file描述符读取，硬限额后再解析；Unix额外NOFOLLOW/NONBLOCK，拒绝符号链接与特殊文件，避免stat后文件增长造成无界读取。
- 每个播放器最多缓存一个最多256行的LRC，未修改时轮询复用；首条时间戳前显示空歌词。封面原文件限制真实PNG/JPEG并在原位深检查不透明，失败沿静态fallback。
- volume/mode/remove在operation等待后、临时文件同步后/原子落盘前再次核验view；最终RAM发布以data→view顺序复核。reader的view锁不跨文件或USB调用。若持久化已完成而随后view退出，明确返回只读确认/不重放，不能虚报回滚。
- 损坏、非regular、悬空符号链接曲库拒绝加载并保留，不自行清空。唯一key、20首上限、私有路径映射及原音频文件保留。
- 新增未执行测试：真实operation锁等待时view更换、临时文件完成后的view撤销、保存失败保持RAM、LRC缓存/时间戳/大小/文件类型，以及16-bit微小alpha拒绝。此改动不修改录音/AEC/音频前端或PCM热循环；legacy AVAILABLE仍为false。

共享PC控件入口和Python精确collection行可达校验已由主线程完成；此文原接入片段保留作接口说明，不代表还有待办。专属代码待统一验证，禁止以旧报告冒称本轮通过。

## 冻结后的定向验证命令（本线未执行）

工作目录由 `HACHIMODOCK_ROOT` 指向本地检出目录。Rust 验证须统一协调，并将已有缓存路径填入 `HACHIMODOCK_CARGO_CACHE` 与 `HACHIMODOCK_CARGO_TARGET`；不安装或升级依赖：

```sh
cd "$HACHIMODOCK_ROOT/pc/src-tauri"
# 使用现有稳定版 Rust 工具链，确保 cargo 已在 PATH 中
export CARGO_HOME="${HACHIMODOCK_CARGO_CACHE:?set the existing Cargo cache}"
export CARGO_TARGET_DIR="${HACHIMODOCK_CARGO_TARGET:?set the shared Cargo target}"
cargo test --offline --lib service_bridge::
cargo test --offline --lib music_player::tests::local_
```

同一源码不变时第二个命令复用同一lib测试产物，不另开target或并行Cargo。

```sh
cd "$HACHIMODOCK_ROOT"
node --test pc/src/component-center/service-bridge-controls-model.test.js
python3 pc/skills/petui/scripts/validate_generated_widget.py pc/candidate-clawpkgs/local-music-player --capabilities pc/candidate-clawpkgs/local-music-player.expected-capabilities.json --json
```

expected-capabilities文件来自已归档且fw明确为r158的实际hello，仅补三项bridge-only预期字段，来源/原始capture SHA记录在candidate manifest。该命令仅候选expected校验，不是硬件协商通过。真实HIL/声学/当前public准入仍需r159同源hello及主线程统一执行。

快照默认树不含 `pc/candidate-clawpkgs`；统一source binding必须额外包含候选目录、ZIP、candidate manifest及expected-capabilities文件。不要在HIL后改变包/flags而沿用旧source receipt。

## visualSprite最小修复与重新冻结

negative-screen/runtime的dashboard均补契约安全图标 `visualSprite:"music"`。实际surface fallback仍为已创作的不透明176×176 `assets/vinyl.png`，声明id=vinyl且image节点真实引用，动态alias=cover；没有造假sprite、未改组件ID或放宽validator。runtime变更已重绑host_contract，ZIP和candidate manifest逐文件SHA已同步。

授权最小validate已退出0（候选expected能力，非设备证据）。smoke退出1：`bounded_app.py:116–123` 的host-contract分支已计入实体按钮和surface.elements动作，但遗漏已验证collection_view.actions，因此误报缺少host.row0/host.row1。没有改冻结共享代码，没有给行操作造假实体绑定，也未削减功能来绕过检查。完整新日志和handoff在 `OUT/r159-fix-all-ojrsrcfy/music-fallback-fix-kv1qzz1m/`，原失败日志未覆盖；整体仍不得发布/宣称通过。
