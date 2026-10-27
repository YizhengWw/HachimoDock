# 包内黄历数据（pc-app-calendar-v1）

`pc/application.json`：schemaVersion=1，engine=`pc-app-calendar-v1`，
service=`calendar`，actions 将四个 host 动作映射到 today/details/previous/next。
不声明 autoRead=true，不联网、不定位、不录音。依赖配套 PC 宿主；旧客户端不应接受未知引擎。

content 最多 8000 条连续日期记录，按日期升序；每条仅有：

- date：严格 YYYY-MM-DD，真实公历日期。
- lunar、ganzhi：非空，分别最多 60 UTF-8 字节。
- term：可空，最多 30 UTF-8 字节。
- festivals、yi、ji：字符串数组，各最多 32 项，每项非空且最多 90 UTF-8 字节。

禁止控制字符、重复或缺失日期。完整表仅留在 PC，设备接收当前页的有界快照。
宿主按 Asia/Shanghai 计算“今天”，跨零点自动更新；浏览别的日期时保持所选日期，
按 today 或真正重新进入组件回到今天。超出数据范围明确显示无数据，不猜测或循环回旧年份。
首页左右切日；详情页左右翻页；SW1 今天、SW2 详情/返回，触屏共享同一动作。
首尾禁用越界操作，详情必须可完整阅读，不能截掉长宜忌列表。

数据生成需保留历法库许可和出处，并核对跨年、闰月与节气锚点。
宜忌属于传统民俗，不预测吉凶，不代替医疗、投资、法律决定；节日不等于法定假期。
不得把自动生成的历书当作天文台认证或官方放假安排。
