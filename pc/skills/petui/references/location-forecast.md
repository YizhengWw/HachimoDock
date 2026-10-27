# 资源包驱动的位置预报

`pc/application.json` 使用 `engine:"pc-app-services-v2"`、`service:"weather"`，
新增 `location` 对象。设备仍使用既有 `host:true` 和 `p4-app-surface-v1`，
本扩展不改变固件协议。旧客户端不支持 v2，必须明确拒绝，不能忽略配置假装运行；
初次使用需支持 v2 的 Pet Manager，此后适配器、城市列表和文案随包更新。

## 使用闭环

打开后读本机保存城市；无城市才用 IP 城市估算。手动选中即保存，优先级高于迟到
自动结果。自动失败可直接手动选择，不必等待天气请求。PC/设备共用同一状态服务，
设备定期查询，配置变更不需要重新同步包。没有有效预报时保留城市、显示原因；
缓存过期不伪装实时数据。主键更新、副键切本机待办、详情动作查询来源。
不新增退出动作、不自动录音；城市与缓存不放进分发包。

设备选城包可声明 `choose_city/previous/next`，并同时保留 `refresh/todo`。
点击城市入口进入选择模式，左右浏览候选，主动作确认保存，副动作取消；
浏览不改已有城市，轮询不重置候选，手动确认优先于迟到的 IP 结果。
该模式复用既有宿主快照与触屏 action，不需要增加固件接口。
包内补充 `cityButton/emptyCityButton/pickerNote/pickerConfirm/pickerCancel/refreshButton/todoButton`
七项文案；标题城市始终独立显示，包括请求失败和待办视图。

## 有界声明

- `location.schemaVersion:1`；`cities` 为 1–64 个 `{city,lat,lon}`，名称唯一、
  最多 60 UTF-8 字节、无控制字符，纬度 ±90、经度 ±180，数字必须有限。
- `geo` 含 `request,fields,require,aliases,cooldownSec`。fields 精确为
  city/lat/lon 三个 JSON Pointer；require 为最多 4 条 Pointer 到布尔值/短字符串
  的成功条件；aliases 最多 64 项城市名翻译。cooldownSec 为 86400–604800，
  成功、失败均退避，不反复按键绕过。只保留城市和两位小数坐标，不保存原始 IP 响应。
- `forecast` 含 `request,rows,time,value,conditions,labels,fallbackLabel,refreshSec,maxAgeSec`。
  rows 指向最多 500 条记录的数组；time 为 RFC3339；value 为摄氏温度（−100…70）；
  conditions 为 1–4 个按顺序取值的 Pointer；labels 为最多 32 条
  `{contains,label}`，按顺序匹配。不是通用算术、脚本或正则引擎。
  refreshSec 为 1800–86400；maxAgeSec 为 3600–86400。按响应缓存头延长等待，
  超时保留有效缓存，拒绝过期及异常时钟数据。只缓存映射后的时刻、温度与条件码。
- 两种 request 都只含 `endpoint,params`。endpoint 为不带查询串、片段或凭据的
  公网 HTTPS DNS 地址（443）；params 最多 12 项固定字符串，预报请求额外支持
  `$lat` / `$lon` 两个完整值绑定，必须同时声明。不支持用户秘密、任意变量、
  自定义请求头、POST、脚本。DNS 地址检查并钉住连接；禁止私网/本地域名/IP
  字面量/重定向，保持 TLS 校验。原始响应分别最多 16 KiB / 512 KiB。
- `links` 最多 4 项 `{label,url}` 公网 HTTPS 来源/许可链接。
- `text` 为校验器列明的完整键集，涵盖 PC 控件、自动/手动状态、预报显示、
  空态/失败/来源/待办文案。每项最多 600 UTF-8 字节；仅允许
  `{city}`、`{time}`、`{value}`、`{condition}`、`{mode}` 一次替换。
  不递归解释响应中的花括号，不支持 HTML、表达式和代码；下发时仍按仪表槽位裁剪。
- 顶层 application.json 仍最大 16 KiB，拒绝未知字段。v1 旧包使用宿主兼容
  配置；v2 必须携带 location，不能靠兼容配置代替真正的包内适配器。

宿主负责安全联网、限流、时效校验、隔离存储、城市选择状态及 USB 同步。
API 地址、返回字段路径、常用城市、条件翻译、显示格式、服务说明属于包。
存储按组件 ID 而非包哈希保存；更新适配器废弃旧来源缓存，不清空手动城市。
新 API 必须有真实网络验收及适用条款，不能仅换 URL 宣称支持所有服务。

## 验收

标准结构/自测后，还要验证更换字段映射和城市/文案只改包即生效、更新仍保留城市、
手动城市不被迟到自动结果覆盖、断网不假装未选城、主动停用不联网、私网和未知字段
被拒绝。最后独立验证真实 IP 查询、真实预报及设备收到城市状态。
模拟 IPC 与包契约通过不等于网络和实机通过。
