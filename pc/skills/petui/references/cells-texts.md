# 有界 cells / texts 候选

`cells.engine=p4-cells-v1`：cols/rows 各 1–16、总数 ≤256、bits 为 2 或 4。
init 为每行 cols 个十六进制数字，值必须符合 bits。缺省全零。
读取越界返回 -1；普通 cell 写越界无效果，值钳位；cellrun 取穿过指定格子的
横、纵及两条对角线最长连续长度。每个分支最多四项 cells 操作。
先执行 set/inc，再按顺序执行 cells 操作；cellcount/cellrun 结果受目标变量 limits 限制。
cell 条件使用 `{cell:{x,y},op,value}`，需要 logic；else 可以只包含 cells。
共享向量：`contracts/cells-v1.json`。

`texts` 为 1–64 项字符串，每项 ≤120 UTF-8 字节，不含控制字符、NUL 或孤立代理项；
总池按条目间一个 NUL 计算，≤4096 字节。`text_at` 引用整数变量，越界返回空串。
surface text_at 需要 graphics + variable bindings，且不能同时声明其他文本来源。
共享向量：`contracts/texts-v1.json`。

分别协商 widgetCells/widgetTexts 的精确引擎。预览数据行位于 state.cells，
程序提交后通过 gridSnapshot() 更新；程序拥有网格时 transition/tick 不得写它。
这些离线契约不能代替 renderer/设备存档接入和 HIL，不得据此声称已完成设备支持。
