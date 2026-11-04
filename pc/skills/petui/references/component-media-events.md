# 组件音序、媒体与事件

这些是显式 v4 扩展，先从目标设备真实能力选择声明。未报告某项能力时，不能
因为同时支持另一项就生成或同步该扩展。源码实现、包校验、设备播放与生命周期
验收分别报告；本文不是实机通过证明。账号、支付、任意脚本与任意 URL 不在范围内。

## 合成音序

`runtime.sequence` 声明 `engine:"p4-sequence-v1"`，需要 `widgetSequence`
精确匹配。字段为 `engine,bpm,volume,tracks`，可选布尔 `loop`：

- bpm 40–240，整体音量 0–100，1–2 条轨道。
- 每条轨道精确包含 `wave,volume,notes`；wave 为 square/triangle/noise，
  轨道音量 0–100，1–64 个音符。
- 音符是 `[midi,beats]`；midi 为 36–96 或休止符 0，beats 为整数 1–16。
- transition 使用 `sequence:"play"` / `"stop"`，tick 的操作放在 `then`。
  非循环曲目自然结束后不会仅因普通状态刷新自动重播。

这是设备合成的有界旋律，可以作游戏背景音，不是任意音频脚本。固定提示音
`p4-sfx-v1` 继续遵守自身契约；音序不与 instrument/media/local/program 所有者
混用，不以音序声明绕过录音、语音或资源仲裁。共享向量为
`contracts/sequence-v1.json`。

## 短采样

`runtime.samples` 需要 `widgetSamples="p4-samples-v1"`，声明
`engine,volume,items`。items 为 1–4 个唯一 `{id,asset:"assets/<id>.wav"}`，
id 为 1–15 位安全 ASCII 字母、数字、点、短横线或下划线。transition 或
tick.then 用 `sample:"<id>"` 触发，不能引用未声明采样。

PC 在安装事务中将实际源文件预编译为 16 kHz 单声道 PCM；每项编译后最多
64 KiB，合计最多 256 KiB。源读取、格式、输出长度和解码时限仍受真实校验，
超限时拒绝，不截断成假成功。采样与音序一样保留所有者互斥，语音优先。
这组受控 WAV 是普通 PNG-only 包规则的显式例外；其他未引用音频仍拒绝。

## 组件视频与大图

两项都要求 app surface、`graphics:"p4-app-graphics-v1"` 和
`bindings:"p4-variable-bindings-v1"`，并单独核验目标能力。

- `widgetClip="p4-clip-v1"`：恰好一个 `type:"clip"` 节点，asset 指向
  `assets/<id>.mp4`，尺寸只能为 160×120 或 320×240。PC 编译为受控 H.264，
  最多 15 fps、75 帧/5 秒、512 KiB；源文件读取最多 4 MiB。隐藏时暂停，
  返回后继续；切换时必须等待原解码器与 worker 物理退出再释放资源。
- `widgetBigImage="p4-big-image-v1"`：`surface.big_images` 声明 1–2 个
  `{id,asset:"assets/<id>.png",width,height}`，宽 241–1280、高 1–960，
  每张 `ceil(width/240)*ceil(height/240)≤16` 块，编译总预算 1 MiB。
  image 节点用 `big,offset_x_var,offset_y_var` 引用，两项偏移都绑定整数变量；
  显示窗口不得超过原图，不与同一节点的旋转、动作或其他图像源混用。

不得在包里放 URL、播放器程序或 FFmpeg 脚本。实际资源由 PC 编译并随同一次
组件事务安装；文件接收完成不等于校验与目录发布完成。

## 跨组件事件与持久闹钟

`runtime.events` 需要 `widgetEvents="p4-events-v1"`，声明 engine 和至多
8 个唯一 `emit` 名称；名称同样为 1–15 位安全 ASCII。transition / tick.then
使用 `emit:"<name>"`；接收方声明 `on:"event:<name>"`，不能用任意 topic
或不存在的动作代替。订阅和闹钟内部动作由真实声明获得绑定豁免，不伪造实体键。

`runtime.alarms` 额外要求 events 与 persist，最多 4 条。每条包含 id、已声明
action，并且恰好使用 `every_ms` 或 `at_ms_var`；可选 background 和至多
40 UTF-8 字节 text。every_ms 为 60 秒至 7 天；at_ms_var 引用整数变量，
其含义为延迟毫秒数，不能填入 uptime 或 Unix 时间戳冒充绝对截止点。

原生持久化保存 UTC deadline；没有有效校时时等待，不以零时刻触发。后台提醒
仍需 PC 的显式启动期授权，只排有界通知，不运行隐藏 action/program。通知槽
占用时保持 pending，周期追赶为 O(1)，不会循环补发旧周期。取消与删除只影响
对应闹钟；存档入队或 RAM ACK 不等于断电耐久证明，需重启读回验收。

源码与共享向量分别见 `scripts/capability_ext.py`、`contracts/events-v1.json`；
设备生命周期与锁规则见 `capability-ext-audit.md`，验收方案见
`capability-ext-hil.md`。联网数据、页面、文本表单和通知 UI 另见
`d-host-and-surfaces.md`；PC 托管本地媒体播放器见 `service-bridge.md`。
