# 有界 App 滚动视口

用于带贴图的卡片、表单式操作、记录历史等应用排版，复用原有图形节点和输入。不是 HTML/WebView，也不是任意手机 App 的兼容层。

## 声明与能力

必须核验真实设备 `widgetAppViewport=p4-app-viewport-v1`，并同时具备应用图形、命名变量绑定及 p4-input-v1。仅升级固件版本字符串不代表支持。

在 surface 上声明一个 `viewport`，精确字段为：

```json
{"engine":"p4-app-viewport-v1","x":16,"y":64,"w":608,"h":352,"content_height":720,"offset_var":"scroll"}
```

区域完全位于 640×480 内，宽高至少 64；content_height 在 h–2048 之间。offset_var 必须是已声明的整数变量。绘制与点击共用 offset 的夹取范围 0–(content_height-h)。host 组件可用组合服务 counter 改变 offset；离线组件用普通状态机变量。

内容节点声明 `in_view:true`，x/y 相对内容左上角，w/h 必须有界并完全位于内容内；h 最多 480。未声明 in_view 的节点坐标仍是屏幕坐标，可做固定标题、底栏和导航。最多一个视口、32 个节点、8 个变量，原有图片数量、像素及编译资源配额不变。视口内不接受 JPEG 背景或 floating motion，不与 scene/game/data/media/instrument profile 混用。

## 交互

向上滑动声明为增大 scroll，向下滑动为减小 scroll；上下摇杆复用同一动作，可 opt-in repeat。PC 预览的滚轮派发这些声明动作，150ms 合并一次，不读取或执行包内脚本。v1 为离散步进滚动，不提供惯性、弹性或任意连续拖动。

绘制与命中都裁切到视口；移出区域的按钮不能被点击。按下后 offset、页面、状态或宿主上下文变化会取消原点击，不将松开事件派发给另一个位置的按钮。确认弹窗在屏幕坐标层显示，不受内容裁切影响。无触控硬件必须保留实体键路径；触控是否可用以实际 ready 为准。

## 数据与验证

打卡和按次记录使用 composition 的 ledger；日期与历史必须来自真实保存的时间戳，不能画出假进度。长历史采用有界上一条/下一条展示，不将无上限数据复制进 runtime。断连显示服务不可用，但保存文件不删除。

共享投影向量 `contracts/app-viewport-v1.json` 在 C、Python、JS 执行；Rust/Python/C 分别拒绝越界、未知字段、错误类型。需另外验证真实绘制与点击、存档和恢复。浏览器/编译通过不等于设备实体按键或触控通过；也不能据此声称语音/AEC 声学验收完成。

## 多个独立视口（显式 opt-in）

新增 `surface.viewports` 需要独立能力
`widgetAppViewports="p4-app-viewports-v1"`，以及原有应用图形、变量绑定与
`p4-input-v1`。只有旧 `widgetAppViewport` 或同一固件版本号不算支持。
旧 `surface.viewport` / `in_view:true` 保持原有单视口语义；两种声明不能混用。

```json
{"viewports":{"engine":"p4-app-viewports-v1","items":[
  {"id":"left","x":16,"y":64,"w":288,"h":320,"content_height":720,"offset_var":"left_scroll"},
  {"id":"right","x":336,"y":64,"w":288,"h":320,"content_height":640,"offset_var":"right_scroll"}
]}}
```

- `viewports` 仅接受 `engine/items`；`items` 为 1–3 个对象，字段精确如上。
  `id` 为 1–15 个 ASCII 字母、数字、`_`、`-`、`.`，不可重复。
  每个 `offset_var` 必须引用不同的已声明整数变量。区域与内容高度沿用 v1 界限。
- 视口屏幕矩形不能重叠（边缘相接可以），即使其内容节点分别用于不同页面也不例外。
  不支持嵌套、横向滚动或视口自身的条件显隐；节点仍可按 page/state/show/host_mask 过滤。
- 内容节点用 `view:"left"` 选择区域，不能再声明 `in_view`。坐标相对所选内容区，
  所有节点必须完全位于该区宽度和内容高度内，单节点高度仍不超过 480。
  未声明 `view` 的固定节点继续使用屏幕坐标；绘制顺序与有动作节点命中优先级仍按节点数组。
- 每个区域独立夹取 offset，绘制和命中使用同一裁切框。任一 offset、页面、状态或
  宿主内容上下文改变时取消待完成点击；确认层仍在屏幕坐标中。
- 本地包通过已有 transition 更新各 offset；宿主包通过受控 composition counter
  与变量快照更新，断连时动作按原有宿主规则禁用。核心操作保留实体输入路径。
  多视口没有隐式焦点或按位置猜测滚动动作；本版不将浏览器滚轮映射到多视口。
  已声明的全局 swipe 仍派发同一动作，不能声称它会自动选择手指所在的区域。
- 全部视口共用原有 **32 个节点、8 个变量、8191 字节 widget JSON**，不增加任何
  图片/字体/编译字节配额或帧缓冲。仍拒绝 scene/game/data/media/instrument 混用，
  内容中仍拒绝 JPEG 背景与 floating motion。

`contracts/app-viewports-v1.json` 提供 C/Python/JS 的独立投影和裁切向量，
以及 C/Python/Rust 共同执行的拒绝用例。这里只新增有界布局原语，不代表已实现
动态列表数据绑定、表单编辑、惯性滚动或任意手机应用。真机触控、显示及性能仍需
独立验收；结构与模拟测试不得当作语音/AEC 或真机验收证明。

## 本地浏览器集成回归

`tests/manual/framework-ui.html` 使用共享表单、导航和视口契约派生的合成数据，
加载真实 `AppSurface` / `ComponentAppPreview`，不连接客户端服务或读取用户存档。
可在 `pc` 目录启动仅绑定 `127.0.0.1:4198` 的 Vite，再从仓库根目录执行：

```sh
node pc/scripts/test-framework-ui-offline.mjs /absolute/evidence/directory run
```

已安装的 Chromium 可通过 `PET_TEST_BROWSER` 指定；脚本只接受本机回环页面，
拦截外部 HTTP/WebSocket，记录绘制裁切像素、真实指针事件、表单和导航结果以及
合成页面截图。不依赖 Tauri IPC，不录音、播放或访问模型。

预览模型还保留瞬时点击代号：同一 React 帧内先滚动/换页再回到原位，也必须取消
原点击；仅时钟推进或无贴图组件的空资源默认值不得误取消点击。代号不属于组件
变量、协议能力或存档，不改变已有数据格式。此项浏览器验收仍不能替代设备验收。
