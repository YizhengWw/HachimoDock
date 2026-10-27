# 声明式双端媒体组件 v1

新媒体组件由一个 `.clawpkg` 分发两部分：设备的 `runtime/widget.json` 与 PC 的 `pc/extension.json`。不允许包内脚本、二进制、任意 HTML/CSS 或用户数据。PC 宿主解释 JSON，不运行插件代码。

## 机制与边界

- `media={"source":"pc.<component-id>","mode":"live"}`；组件 ID 最多 44 个 ASCII 字符。仅 v4 tool，不混用 data/scene/game/instrument。真实媒体状态替代 tick/vars。
- 设备必须声明 `widgetMedia=p4-media-v1` 和 `widgetMediaSurface=p4-media-surface-v1`。首次需要升级基础固件，此后能力范围内换布局不改固件。
- PC 扩展默认启用，首次使用与包升级不再要求授权确认；域名、公网音频和本地存储说明以非阻塞方式展示。只接受正式组件库中校验通过且内容哈希匹配的包，不接受调用者自选文件路径。
- PC 的配置文件不下发设备。设备不知道电台目录网址与响应格式，只处理有界媒体操作和绘制节点。
- 收藏、历史、当前条目和音量在宿主应用数据目录的 `component-data/<id>/library.json`；`service-settings.json` 仅保存用户主动的服务开关，不绑定包版本。未设置时默认启用，旧 `grant.json` 不再形成门槛。组件升级保留数据与主动停用的选择；停用取消播放但不删除收藏；删除/升级后运行中实例会被停止。
- 第一版仅支持无需鉴权的 HTTPS GET JSON 搜索。没有任意 JS、POST、自定义请求头、OAuth 或凭据接口。后续新增凭据服务必须由宿主保管，不能写进包；不得宣称本版已经支持。
- 搜索和配置可不接设备；真实播放需要 PC 运行、USB 与相应能力。直播停止后恢复连接当前直播，不提供回放/快进。实时语音优先、全局停止取消所有组件的播放准备和播放；不会开机自动播放。
- 首次打开显示搜索和设备连接就绪情况，不显示授权门槛；默认启用不等于已连接或正在播放。搜索结果可直接播放而不强制收藏。主动停用后的 status 仅返回停用信息，不触发联网；重新开启是服务开关，不是再次授权。
- 宿主系统 DNS 失败或返回 198.18/15 代理虚拟地址时，可通过阿里公共 DNS／Cloudflare 的 HTTPS DNS 获取真实公网地址；始终验证 TLS、解析响应、公网 IP 并钉住连接地址，所有跳转和 HLS 片段重新检查。私网地址不得放行，不能关闭证书校验或直接连接虚拟 IP。数据说明披露解析服务；默认启用不绕过宿主网络策略。解析服务只接收目标域名，不发送搜索词、用户对话或凭据。
- `widgetMediaNotice=true` 的设备支持 `media/notice`，用于展示未启用、空收藏、连接失败等原因。通知仅作用于相同 source 的空闲媒体视图，不启动/刷新/终止音频会话；旧固件仍可播放，详细失败原因在 PC 显示。

## pc/extension.json

最大 16 KiB，所有对象拒绝未知字段。必填字段：

- `schemaVersion:1`、`engine:"pc-declarative-media-v1"`。
- `permissions:{networkHosts:[...],publicAudio:true,storage:true}`：1–4 个精确 DNS 域名，不允许通配符/IP 字面量。搜索每次跳转都检查域名、HTTPS 和解析地址；音频只允许公网 HTTP/HTTPS 80/443，拒绝凭据 URL 和本地地址。解码器只接收管道字节，不获得网络 URL。
- `search:{endpoint,queryParam,params,itemsPath,fields}`：endpoint 是声明域名内不带查询串的 HTTPS URL；queryParam 为关键词参数名；params 为最多 12 个固定字符串参数；itemsPath 为 JSON Pointer，空串表示根数组。
- search 可选 `browseQuery`（非空、最多 30 字/120 UTF-8 字节）：打开组件后用于自动加载设备电台列表，仍复用原受控搜索映射，不自动播放。缓存与收藏分离，设备队列收藏优先、按 ID 去重、最多 20 条；网络失败可在设备重新打开列表重试（15 秒限流），主动停用后不请求。
- `fields` 必需 `id,title,url`，可选 `fallbackUrl,homepage,codec,hls,available`，均为 JSON Pointer。url 为空时使用 fallbackUrl；hls 和 available 识别 true/1。没有表达式、模板执行、正则转换。最多读取前 1000 行，输出 20 个有效且 ID 不重复的条目。
- `panel:{title,description,searchPlaceholder,attribution,attributionUrl,sections}`。sections 为 search/library/history/volume/sleep 的非重复子集；宿主以安全 React 文本与标准控件渲染，不注入 HTML。当前使用标准面板布局，并非任意 PC 应用布局语言。

## 设备 surface

### 自动目录与进入播放（可选 discovery）

包内 `discovery` 声明 `endpoints`（1–3 个受控 HTTPS 镜像）、`locationEndpoint`、`cityField/countryField` JSON Pointer、两字母 `country`、最多 80 个 `aliases`、`stationRegion/popularity/votes` 字段映射、`nationalCount`（1–100）、`localSupplement`（1–30）、`refreshSec`（3600–604800）和 `autoPlayOnEnter`。所有域名仍必须在 permissions 内；itemsPath 必须是根数组。目录使用 GET 的 limit/offset 分页、search.queryParam 和 state 过滤，固定过滤条件在 search.params 中。不执行包内代码。

宿主按热度/投票排序、同 URL/同名去重，匹配本地名称或城市 region；有本地台时保留全部收录台并补充全国台，否则回退全国列表。目录可用不等于每个音频源实时可播；首次自动收听遇到坏源至多尝试 4 台，此后提示失败，不无限重试。地理服务仅估算出口 IP 城市，不保存 IP；未定位、海外或未收录城市回退全国，不冒称完整广播覆盖或真实收听率。

新能力 `widgetMediaQueueMax=128`、`widgetMediaLifecycle=p4-media-lifecycle-v1` 才支持完整自动目录。自动目录不混入旧收藏，旧收藏数据保留。超过 128 条返回明确错误并保留缓存，不能静默截断后声称全部。旧包仍用 20 条收藏优先队列。媒体主视图、列表、触屏和 PC 预览均使用同一队列，更新按 key 保留选择、不打断音频。

`media/event` 的 `visibility` 含 source、boot、epoch、visible。设备进入/离开增加 epoch，处于页面时每 2 秒刷新；宿主按线序消费、忽略过期 epoch，只有当前可见且尚未消费的进入事件允许延迟自动播放。停止、退出（尚未开始的准备）、实时录音和停用取消待播；不因列表刷新重新播放。PC 面板 status、预览、仅安装包不启动播放；退出后已经播放的音频保留原后台策略，全局停止仍有效。刷新失败保留已有缓存和当前流。

`runtime.surface={background:<RGB565>,elements:[...]}`，最多 20 个节点，仍计入 widget.json 紧凑 JSON 的 4095 字节总限额。

每个节点必需 `type,x,y,w,h,color`；坐标与尺寸完全落在 640×480 内，color/background 是 0–65535 整数。可选 `page`（0主页面、1列表、2通用，默认2）、`scale`（1–3）、`radius`（0–24 且不超过半宽/高）。

- panel：圆角填色面板。
- text：字面量 text（最多120 UTF-8字节）或 bind，二选一；高度至少32×scale。绑定 title/subtitle/status/message/volume/index/count/level，不读取任意对象路径。
- meter：bind 仅 volume/level，0–100；level 为真实 PCM 峰值，不是频谱，PC 未收到时保持0。
- ticks：按当前队列索引显示刻度，不冒充 FM 频率。
- list：显示当前选择页的队列，高度至少48，每行48。
- transport：随真实播放状态显示开始/停止图形；PC 预览可调用播放操作，设备仍由 buttons 映射触发。

动作沿用 media.toggle/queue/previous/next/up/down/select，不接受 lyrics。源 ID 随设备事件返回，宿主不按页面标题或默认服务猜测归属。

## 验收

先运行标准校验器与媒体模拟，再原子发布。另需测试两个不同布局、未知字段/越界/跨组件 source 拒绝、隔离存储与升级保留、默认启用/主动停用/重新开启、超时/切台/语音打断、联网解码和真实 USB 播放。浏览器模拟不联网或播放时必须明确标注；模拟与编译不能代替真机验收。

## 通用切图与旋转（p4-surface-images-v1）

媒体 surface 可声明 images（1–4 项），每项为 id、asset、frame_width、frame_height、frames=1、fps=1。id 为 1–15 位小写字母/数字/_/-；asset 为 assets/ 下安全 PNG，不含 ..。尺寸各 8–240，合计最多 98304 像素，单 PNG ≤128 KiB。image 节点使用 image 引用 ID，可选 rotate_ms=8000–60000；旋转节点必须正方形且不超过 320×320，素材也必须正方形。

需要 widgetSurfaceImages=p4-surface-images-v1。旧设备必须阻止同步并提示升级。图片与其他节点同样计入 20 节点、4095 字节限制，不放宽 scene 的 4096 像素配额。PNG 必须实际存在、尺寸一致；不允许额外文件或脚本。允许纯 UTF-8 授权文件 LICENSE.txt、THIRD_PARTY_NOTICES.txt 及 licenses/ 下安全文件名的 .txt/.md；社区发布必须包含 LICENSE.txt。声明随 PC 资源包保留，不下发到设备的有界运行文件槽。

宿主以确定性的 256 色 RGBA 调色板和 RLE 编译 P4I1，单图≤100000字节、全部图片≤128 KiB。设备分块确认、校验后原子提交；双代存储也保存压缩数据。旋转复用单张图，仅 playing 推进时间，缓冲/停止/失败冻结，切台不重置角度。PC 预览使用源 PNG，设备使用有损调色板，需额外核对压缩误差与真机画质；编译和浏览器预览不等于真机帧率验收。
