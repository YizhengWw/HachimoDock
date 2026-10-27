# 离线内容池与日期工具

`local.engine="p4-local-tools-v1"` 是 0.7.102 新增的通用离线能力。
必须核验设备 `widgetLocalTools`；不能用 bounded-v4 版本代替能力协商。
仅用于 tool + app surface + input-v1，不与 host、media、data、scene、game、instrument、pages 或 tick 混用。
运行时与其他图形包共用 8191 字节/32 节点/128 KiB 编译图片预算。

`local` 精确字段：engine、kind、groups、actions、reveal_ms、idle、busy、hint。
kind 为 deck 或 countdown；揭晓时间 300–2000ms；三条状态文案各 1–144 UTF-8 字节。
groups 为 1–3 组，每组 title 为 1–30 字节。

## 内容池

deck 每组包含 items（2–16 项），每项精确为 title（1–60 字节）及 note（1–144 字节）。
同组标题唯一。内容为原创或取得授权，不冒充占卜、医疗、金融或法律建议。
actions 是逻辑动作到 draw/exclude/reset/close/previous/next 的映射，不得重复操作。
draw 必需；多组须有 previous+next；exclude 必须同时提供 reset，防止排空后无路可退。
揭晓期间重复抽取被阻止，close 可取消。候选数大于一时不连续重复，单项仍可再次选中。
exclude 排除当前结果并重新抽取；全部排空显示 empty，不偷偷清除排除记录。
reset 恢复当前分类，建议通过 input.confirm 二次确认。

## 日期工具

countdown 每组精确为 title、kind。kind 为 weekdays/weekend/date。
weekdays：周一至周五当天目标时间，过点显示到站；周末指向下周一。不计算法定节假日。
weekend：本周六配置时间，周日仍显示到站；周一切到下个周六。
date：2020–2099 年自定义日时。零日期表示尚未设置，过去日期显示到站。
六个动作 previous/next/edit/cancel/up/down 必须齐全。
普通状态左右换目标，edit 开始编辑；编辑时左右选年/月/日/时/分，上下修改。
周内及周末只编辑时/分。再次 edit 保存；cancel 丢弃草稿。闰年与月末自动夹限。
所有动作复用物理按键和 >=44×44 触屏目标；摇杆 up/down 可 repeat。

设备通过既有 system/heartbeat 的 clock 对象接收 unixSeconds、utcOffsetMinutes。
PC 仅对支持本能力的设备发送。本机时间错误会影响倒计时，不宣称是互联网标准时间。
校时后用单调时钟维持，持续供电可离线；完全断电/重启后明确显示等待校时，不能持久化一个假的当前时间。
不保存时区规则，因此离线跨夏令时需要重新连接 PC 校时。

## 固定存档和表面

使用七个 int 变量 local0…local6；local0 是组序号。
每组依次用两个变量：deck 为排除位图和上次结果索引（初值 0/-1），countdown 为 civil-day 和当日分钟。
civil-day 是从 1970-01-01 起的公历天数；分钟范围 0–1439。
状态固定按序 ready、busy、result、editing、empty、offline；初始 ready。
persist 为 version:1、vars 按序包含七个变量、所有 states 恢复映射到 ready，不保存编辑草稿/半次抽取。
每个 transition 精确为 `{from:"*",on:<逻辑动作>}`，不自行添加 set/inc/to/page。
引擎填充 eyebrow/headline/note/metricValue/metricLabel/metricUnit/badge/progress，表面使用 bind 读取。
原始文章不宜超过配额；大量长文请用知识库宿主，不把此能力扩成脚本运行器。

发布前执行结构校验及 smoke_test_widget_game.py；还需 C 核心消毒器测试、PC/固件一致性和设备字宽检查。
PC 模拟不写用户存档，不代表实机验收。存档沿用现有通用存档，不随包上传。
