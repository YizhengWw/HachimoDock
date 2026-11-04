# PC 驱动的有界可见集合（实验候选）

仅限设备明确声明 `widgetHostCollection=p4-host-collection-v1`。
普通固件默认关闭；不要为了通过校验自行伪造实际设备能力。
本能力不授予任意程序、联网、音频或外设权限，不代表设备能脱离 PC 运行集合业务。

## 声明与来源

`surface.collection_view` 的精确字段为 engine/source/x/y/w/row_height/gap/visible/
page/actions/colors。engine 固定 `p4-host-collection-v1`；source 为 1–16 字节
ASCII 字母数字及 `._-`。画布 640×480，w 至少 128，row_height 48–96，gap 0–16，
visible 1–8，派生整体高度必须在画布内。page 为实际页面索引 0–3 或通用页 4。
actions 为恰好 visible 个唯一的 `host.` 动作，后缀为安全 ASCII，至多 42 字节。
colors 为六个 RGB565 整数：背景、选中背景、标题、说明、边框、选中边框。
数字必须有界且数学上为整数，不能传 bool、字符串或小数。

需要 app surface/graphics、host=true、host_context 与 input v1。每槽展开为三个
标准节点（按钮、标题、说明），全部计入原有 32 节点；不增帧缓冲或原图片预算。
不与游戏/场景/data_view/媒体输入混用；独立滚动区可以相邻，但不能重叠，
普通可操作节点也不能覆盖集合区域。旧包字段和坐标不变。

PC application 必须是 composition v2/v3，source 指向真实 collection 源，
该源 page_size 必须等于 visible。第 i 个 actions 必须映射到 `source:row_i`。
翻页/上下选择使用已有 collection 操作另配实体按键，不创建组件专用源。
集合仍只做导航与选择；记录编辑、事务、确认和稳定 eventId 沿用既有业务服务。

## 可见快照与失败

现有 app/snapshot 另携带 collectionView，精确字段 engine/source/visible/count/
offset/selectionKey/rows。count 0–128，offset 按 visible 对齐且在末页以内。
rows 恰好 visible 项，每项精确字段 present/key/label/detail/index/selected/truncated。
有记录时稳定 key 唯一、非空；三个文本均不超过 63 UTF-8 字节且无控制字符。
index 等于 offset+i，selected 只由 selectionKey 决定。空槽文本为空、index=-1、
三个布尔字段均 false。暂时隐藏的选择 ID 保留，不因分页或筛选清空。
完整记录及未截断原文留在 PC；显示截取由已有 collection 源标注 truncated。

总 app/snapshot 仍最多 2048 字节。整包超预算时只发有界错误和空动作，保留记录，
不裁剪 ID、自动缩页、分片发布、不重发修改。缺失/过期/不匹配快照显示等待状态，
空列表显示空状态，均无行操作。渲染使用一次捕获；最终入队重新核验来源、槽存在、
动作可用性和原生观察。内容变更、退出重入、换序及来回替换取消旧点击。
PC 同样保留批处理中的变化 epoch，不能用相同文字复活旧点击。

共享测试数据：`contracts/host-collection-v1.json`。C/JS/Rust/Python 边界、生产宿主
生命周期、真实 AppSurface 点击和编译预算均须回归；这些不能代替 P4 实机输入、
长帧、串口和声学验收。原生独立集合程序及混合能力调度仍是另外的待完成工作。
