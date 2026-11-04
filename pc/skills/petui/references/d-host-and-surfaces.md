# D 能力开发参考与集成边界

本文记录开发中的 D 能力，不代表已经完成统一 Rust 构建或设备 HIL。账号与支付不在范围内。

## PC Fetch

`pc/application.json` 使用 `schemaVersion:1, engine:"pc-fetch-v1", service:"fetch"`，声明 `content`（1–4 个 source）和可选 `actions`。source 包含 `id,url,interval_sec,map`，可选 `method:"GET",headers,ttl_sec,max_bytes`。只接受 HTTPS 443、无账号密码、无 fragment、无凭证 query，响应最多 64 KiB，间隔至少 60 秒。

`map:{vars:{name:"/json/pointer"}}` 映射宿主变量，要求 `host:true`、通用变量绑定、无 tick/persist。每个宿主变量只允许一个来源写入；映射错误不以假数值替代。`map:{rows:{items:"/items",id:"/id",label:"/label",value:"/value"}}` 映射最多 20 行，runtime.data.source 为 `fetch.<id>`。id 截断后去重，文字按 UTF-8 字节边界截断。可选行字段是 value/detail/meta/tone。

包声明主机只代表请求权限。用户在现有 PC 应用面板的“网络来源白名单”表单配置精确主机；授权保存于包外组件状态目录的 `fetch-hosts.json`，空列表禁止联网。每次轮询和缓存输出复核授权，拒绝重定向、系统代理、非公网 DNS 解析，连接固定到已检查地址。没有包内脚本或凭证存储。

Rust `pc_fetch::parse_declaration` 做导入期结构检查；`pc_fetch::host::request` 接入 `component_app::request_validated` 的 fetch 分支。后台 worker 独立于串口 reader；最多 8 个实际 worker，取消但未退出的阻塞请求仍占一个槽位。必须有用户实际写入的 enabled=true 设置和允许主机；默认安装状态不构成后台启用，不扫描目录 autostart。隐藏/切组件/断线仅停止设备实例发布，PC 继续有界轮询与缓存；重新打开绑定新 epoch 并复用缓存。关闭应用面板不终止服务。显式停用、删除状态或包、声明/runtime 变更、主机授权失效或 PC 客户端真正退出才取消任务。阻塞请求取消后最多等待当前 HTTP 请求返回，不会把迟到结果写入新实例。DNS 系统解析本身不保证 15 秒内返回，因此工作线程槽位直到实际退出才释放。

结果提供 variables、data、values、ui。设备变量通过 `publish_fetch` 在实例校验后发送 app/snapshot，数据通过 widget/data；PC 面板轮询同一缓存。成功映射原子替换缓存；失败保留旧缓存，TTL 后不再提供旧变量并把行快照标记过期。手动 refresh 仍遵守网络最小间隔。

## 页面、表单与展示

应用壳 `p4-app-shell-v1` 使用 quota v3 的 12 页。page 0、4、11 都是普通页，省略 page 才是所有页；legacy 包保留 page 4 通用语义。当前拒绝 shell+canvas，直到 canvas 内部页码统一。tab.next/tab.prev 或点击标签会切换根页并清空导航历史。

导航 `app-navigation-v1.json` revision 2 将深度从 8 增至 16；C、WASM bridge、JS 与 Python 同步。能力字符串不区分 revision，旧固件不能据此被宣传支持 16 层。

表单 v3 支持 1–4 个 ≤63 字节文本字段，numeric/latin/pinyin 键盘。预览执行同源 `pet_p4_form_v3.c` WASM，PC 文本输入只替换草稿，确认才提交。仅表单拥有的字段/草稿/键盘字符串按 63 字节约束；其他变量按 quota（v1 8/63、v2 16/63、v3 32/127）验证和保留。C v3 core 的索引空间为 32，实际表单角色最多 15 个。

`data-rows-v1` 支持过滤、选择和 selected-row 文本投影。`longtext-v1` 使用确定性中英文换行与 page_var/count_var；figure 投影为图片+说明。AppSurface 的真实入口已接入这些投影。`notify-v1` 的 USB reader→Tauri component-notify→AppSurface toast 已接入，设备入站 widget/notify 原有泡泡路径保留。固件已解析 runtime.notify 和 transition/tick.then 的 notify:true，成功执行后的通知在现有 host_process 阶段排出；失败分支、不可见实例、重载不发送。出站每组件 30 秒最多一条，传输失败不重放。统一构建及设备 HIL 尚未完成。

## 已接通的 C 钩子与验证边界

1. `pet_p4_miniapp.c::form3_values/form3_write` 只处理表单 committed/draft 和七个控制/键盘角色。其余 127 字节字符串不会经过 64 字节中间值。`restore_app_runtime_buffer` 的字符串长度按当前 quota 验证，只有实际表单角色附加字段 max_bytes / 键盘限制；按目标 sizeof 拷贝，保留 PSRAM 分配 wrapper。
2. 五处 form3 调用端使用递归模型锁保护的 EXT_RAM_BSS workspace，包含未发布 candidate 的 restore 初始化。不会在调用端再叠加 2176 字节 values 数组。native O2 `pet_form3_apply` 栈帧 2448 字节，设备架构及嵌套调用的最终栈余量仍须按原门禁验证。
3. C `pet_surface_parse` 的 shell 校验明确拒绝 canvas，与 Python/Rust 一致；只修改 shell 校验行，未修改 B 的 canvas/renderer 实现。真实 parser 测试覆盖 shell 页 0/4/11/省略，并证明同一有效 canvas 在 legacy surface 可通过、与 shell 组合被拒。
4. 新 `pet_p4_miniapp_d.inc` 负责通知配置/规则校验、实例内待发通知和 refresh。transition 的 false guard / else 不触发主分支通知；tick 仅完成 then 后触发。通知入队后快照 badge，后续变量变化不改已排队负载。host_process 在模型锁下排出当前实例，隐藏时丢弃，替换 runtime 自动丢弃，不跨重连重放。
5. `data.refresh` 在声明动作成功执行后排队 `widget/data/refresh`，5 秒冷却。PC reader 只做有界 admission；worker 校验设备 board、已安装组件 runtime 的 source/refresh_action 后，发送 computer.status / todos.upcoming 当前快照，或调用现有 fetch 宿主生命周期。账号类数据源不在此路由内。fetch 的最短联网间隔与用户主机白名单仍生效。
6. `pet_app_host_emit` 仅允许上述两个固定出站 topic，使用既有 ordered line transport，不借用 HOST_VIEW 授权任意 IPC。payload 由发送函数复制；refresh 增加当前 boardDeviceId，通知保持既有精确负载。断线/分配失败不重试。
7. 这批改动通过实际 C parser、恢复/写回、执行钩子、drain 和真实 transport sink 的 ASan/UBSan 定向测试；PC 新 refresh worker 的 Rust 类型检查与真实 USB/HIL 仍由主代理统一验证。没有运行 Cargo、PIO 或全套测试，没有调整任何栈、优先级或门禁。

## 定向证据

- `firmware/tests/petui_d_modules_test.py`：C/Python 长文和拼音候选、C v3 parser 的 quota 组合、白名单/指针/空态回归。
- `pc/src/component-center/d-surface.test.js`：真实 app runtime→C/WASM 表单、页 0/4/11/省略、标签、过滤行、长文、额外变量保留。
- `pc/scripts/test-d-surface-offline.mjs`：四份契约通过真实 AppSurface React 服务端渲染。SSR 会报告原有 useLayoutEffect 无服务器效果警告；它不验证浏览器事件和硬件操作。
- `build-form-v3-runtime.mjs --check` 与 `build-navigation-runtime.mjs --check` 检查同源制品及镜像一致。合成 workflow/form-schema Rust runner 已更新以保持 composition 真分支和 v3 子模块可编译，需由主代理统一 cargo 执行。

新增 `firmware/tests/petui_d_entrypoints_test.py` 验证上述生产入口；`petui_app_host_test.py` 执行真实 D emitter，验证固定 topic、board 绑定、借用负载不变和断线不重放。

旧 form v1/v2 的实际 core 上限为 PET_FORM_VARS=8，quota v3 不扩展旧 core。C/Python/JS 声明保留该上限，Rust v1 显式锁定 8；form2_values/form2_write/form_write 对绕过导入的超限 runtime 也失败关闭，避免 OOB。新增 C/Python/Rust 边界用例覆盖 8 正例及 9/16/17/32 拒绝例，Rust 由统一构建执行。

`pc_fetch_lifecycle.rs` 是生产 worker 与离线假 transport 测试共同执行的生命周期控制器。定向 rustc --test（不使用 Cargo/lockfile）通过四项：hidden→继续抓取缓存→reopen复用、disable中途取消→旧结果不发布新服务、删除/声明变化/授权撤销/退出丢弃在途结果、真实8槽worker预算。该证据不等于真实HTTPS网络或完整Tauri类型检查。
