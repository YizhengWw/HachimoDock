# 通用服务桥（候选、默认关闭）

本参考对应 `pc-service-bridge-v1` / `p4-service-bridge-v1` 候选代码及
`contracts/service-bridge-v1.json`。协议可表达、离线校验、完整固件编译与实机验收
是不同状态；本参考不证明普通固件已开放或任何提供器已通过 HIL。
`PET_SERVICE_BRIDGE_ENABLED` 缺省为 0，仅独立
`esp32_p4_service_bridge_candidate` 显式开启，并启用其集合依赖。
不由 r157、客户端版本、PC 预览或包内声明推断设备支持。

## 路由与显式协商

需要原生服务状态投影、稳定键列表、类型化命令、显示时间轴、动态封面或显式导入
本地音乐时，走此通用路由，不按组件 ID 添加代码。PC 运行受控原生提供器，设备
绘制有界 surface；包内不放脚本、可执行代码或任意文件路径/URL 执行入口。
公开网络媒体仍见 [pc-extensions.md](pc-extensions.md)，两条路线不能互相推断能力。

纯标量桥可以复用既有 legacy host 路径：不使用动态图片、时间轴、事件参数或
local-media 时，可不声明 `surface.services`，不要求新 `widgetServiceBridge`，但既有
host-context/input/variable-bindings 等依赖及原有配额仍必须满足；此模式不带
`host_contract` 或 presentation，不能借此解锁新增设备字段。

使用动态 image/timeline/event 参数或 local-media 的界面必须明确声明并核验：

- `pc/application.json`：`schemaVersion:1`、`engine:"pc-service-bridge-v1"`、
  `service:"bridge"`、`actions`、恰好一个 `content` 计划；禁止 `autoRead:true` 和 location。
- `runtime/widget.json`：`host:true`、`host_context:"p4-host-context-v1"`、
  `input.engine:"p4-input-v1"`；`tick:[]`，不声明 `persist` 或 `ownership`。
- `surface.engine:"p4-app-surface-v1"`、`graphics:"p4-app-graphics-v1"`、
  `bindings:"p4-variable-bindings-v1"`、`services:"p4-service-bridge-v1"`。
- 读取真实设备能力，要求 `widgetServiceBridge:"p4-service-bridge-v1"` 及所用
  surface/graphics/bindings/host-context/input 能力。使用 `collection_view` 还须独立
  核验集合能力，不能由 bridge 能力推导。动态图片可报告
  `widgetServiceBridgeImages:2`、`widgetServiceBridgeImageSide:192`。
- `host_contract` 必须是当前 PC 声明与完整设备 runtime 配对得到的 64 位小写
  SHA256。旧设备界面不允许调用更新后的宿主动作定义。

`host_contract` 不使用普通 JSON 文本直接散列：取声明的
`schemaVersion/engine/service/actions/content` 与删除 `host_contract` 后的 runtime，
按 Python/Rust 相同的类型化 JSON framing 编码，再作 SHA256；对象键排序，保留
数组顺序与 UTF-8。所有数值零（包括 `0`、`-0`、`0.0`、`-0.0`、零的指数写法）
统一编码为 ASCII `i0;`；布尔 false 仍编码为 `f`，不当作数值零。其余整数保留
整数 framing，其余有限浮点保留 `d` 加大端 IEEE754 binary64，不将所有整数值浮点
强制转成整数。Python/Rust 必须使用同一规范，禁止只修一端后重新绑定；此为配对
契约，本文不代替 B 最终实现/验证回执。修改任一端后重新绑定独立工作包：

```sh
python <skill-dir>/scripts/bind_service_bridge.py <work-component> --write
```

省略 `--write` 只报告配对结果。工具不发布、不联网、不安装，拒绝写正式 library；
摘要不是用户授权、签名身份或 HIL 证据。仍须走原有校验与能力检查流程。

## 有界计划

`content[0]` 必填 `services/bindings/commands`，可选 `collections/timelines/images`；
拒绝未知字段。具体跨语言边界以候选解析器和契约向量为准。

| 项目 | 候选约束 |
| --- | --- |
| 提供器 | 1–4 个别名；`kind:"service"` 引用经过既有校验的原生 declaration/runtime，或 `kind:"local-media"`；禁止递归 bridge、autoRead；最多一个音频提供器、最多一个 notes 提供器（笔记捕获按组件 ID 隔离，不能用多个别名共享草稿） |
| 标量绑定 | 最多 8 个，键集合必须精确等于 runtime.vars；变量仅 int/string；每条 service、JSON Pointer、同类型 fallback，整数 fallback 还须满足 limits |
| 命令 | 最多 16 个 `host.*`，actions 键精确等于 commands；映射 `alias:method`；每动作恰好一条 `{from:"*",on:<action>}` transition |
| 参数 | 每命令最多 4 个，类型 int/string/bool，键及类型精确匹配原生方法；常量、`{event:<key>}` 或 string 的 `{selection:<collection>}`；不执行表达式 |
| 标量范围 | int 为 −10^9…10^9；string 最多 63 UTF-8 字节、无控制字符；JSON Pointer 最多 128 UTF-8 字节 |
| 集合 | 最多 1 个，visible 1–8；复用 component_collection/app_host_collection 的分页、选择和稳定 key，重排后按 key 保持身份，不按行号执行提供器动作 |
| 显示时间轴 | 最多 2 条，horizonMs 100–10000，投影 percent/position/remaining；目标为已绑定 int 变量 |
| 动态图片 | 最多 2 张，尺寸各 8–192；image_bind 引用已声明别名，失败回退节点原静态 image |
| 快照/回执 | bridge 快照最多 2048 字节，超限报错并保留原数据，不静默截断；每会话最多 128 项命令回执 |

旧服务未注册带参数方法时，其 `parameters` 只能声明空参数表，不能由包声称已支持
参数。标量错误使用字段级 fallback 并返回 bindingErrors，不把某个服务失败变成所有
服务成功或整体伪数据。统一 phase 为 idle/preparing/ready/running/paused/completed/
interrupted/unavailable/failed/cancelled；状态投影不是提供器执行回执。

meter 的 `value_param` 仅在此显式服务桥下有效，要求 `host.*` action 和对应 int
事件参数；`value_min/value_max` 默认 0/100，必须为合法递增整数。PC 预览有拖动/
键盘入口，设备输入仍需独立实机验收；这不代表实现了 P4 任意自由文本表单。

## 生命周期、时间轴与图片

会话包含随机 nonce、generation、提供器 observation 和列表选择，产生 `ui.context`。
动作须携带当前 context、事件 ID 及受控 data，执行前认领事件；重复完成事件不再次
执行，失败或未知结果消费旧观察，不能自动重发。组件更新、退出、断连、停用撤销
旧视图；版本退役同时 cancel notes、disable news、stop audio 并释放本地播放器/
图片资源，不能只清桥回执而让旧任务继续。状态读取不能被当成播放、录音、任务派发或未知结果恢复。Session 注册表只持有有界成员操作；提供器调用由各自 Session 锁串行，不再持有全局注册表锁。退出原子撤销旧会话；正在执行的旧调用返回后不发布快照，不自动重放。不同会话不因同一全局锁互相等待，但同一提供器内部仍遵守其原有并发策略。

时间轴只从真实确认的 position/duration/running 短暂外推，最多外推 horizon，快照
年龄达到 10 秒即失效；duration 不超过 86400000ms。它是显示优化，不是执行时钟、
自动 seek 或播放成功证据。

图片仅接受已注册提供器返回的有界 PNG/JPEG data URI，由 PC 解码和缓存，设备只收
key/revision/width/height 与 RGB565 数据，不接文件路径或远端 URL。输入 data URI
最多 3 MiB、解码前字节最多 2 MiB，源尺寸最多 4096×4096、解码分配上限 64 MiB，
MIME 必须与真实 PNG/JPEG 格式匹配，不能由 MIME 启用其他解码器。按原图原位深
检查 alpha（含 8/16 位及浮点 alpha），在缩略前拒绝任何非全不透明像素，防止
缩略/降位深掩盖半透明。解码串行且不持缓存锁解码，解码前检查全局 32 项/每 owner
2 项缓存配额，插入时再检查；缩略到最多 192×192。设备双缓冲借用/释放，
只在完整传输后切换，音频传输优先；既有图形/包体预算不放宽。

## 本地媒体、权限与验收边界

`kind:"local-media"` 复用原播放器核心，但旧全局 `music_player::AVAILABLE=false`
保持；不重新开放旧在线歌曲服务。只能由用户在 PC 明确选择本地文件导入，包、设备
事件和定时器不能提交文件系统路径；带 `_deviceEpoch` 的 import 请求明确拒绝，
不能冒用 PC 文件选择入口。队列、同名 .lrc/.png/.jpg 侧文件与私有存储由
宿主管理，保存原子化，损坏库保留。

方法为 toggle/pause/resume/stop/previous/next，以及 play/remove(key)、
seek(positionMs)、seek_percent(percent)、volume(volume)、mode(mode)；remove 必须
有显式确认。真实播放需要真实设备音频能力与当前活动视图，PC 预览不自动播放；
语音打断复用既有仲裁。local-media 声明 networkHosts=[]、microphone=false、
speechProvider=false，不等于有在线音源、录音、OAuth、账号或支付支持。

其他原生提供器继续遵守各自权限、系统配置、主动停用及破坏性操作确认；桥只汇总
权限，不发放新权限，也不将包声明当成用户已授予域名。账号支付排除。
尚待逐提供器状态/异步生命周期、真实手势与截图、退出/断连竞争、图片 USB 传输、
音频/语音抢占、Windows、声学和固定门槛 HIL；离线 core/JS/编译记录不能代替这些
证据。program750µs、frame/gap66667µs、SAVE4096/free1024 等门槛保持不变。

本地媒体另提供无参数 `volume_up` / `volume_down`（每次5、夹到0–100）及 `cycle_mode`（sequence/single/loop/shuffle）；这些方法注册于通用local-media provider，不能由包自行注册方法。删除仍须显式确认。示例候选包与统一验收清单见 `pc/docs/local-music-player-candidate.md`；没有真实候选协商不得同步。
