# 配额档位（p4-quota-v2）

## 显式 v3 候选

`quota: "p4-quota-v3"` 必须协商 `widgetQuota=p4-quota-v3`，或设备
`widgetQuotas` 中明确列出该档位；不能由 v2 推断支持。共享向量见
`contracts/quota-v3.json`。v3 上限：vars/persist.vars 32、states 16、pages 12、
transitions 48（未声明 input 仍为 12）、tick 16、switch 分支 12、surface 节点 64、
每组 local deck 64、字符串变量 127 UTF-8 字节、紧凑 widget JSON 12287 字节。
form/navigation 等专属 profile 的限制仍独立适用。

这只是声明与预算契约。设备解析、绘制、存档和最大负载 HIL 未全部验证前，
不能宣传该档位已完成设备验收，也不能仅更新 PC 校验而绕过能力协商。

`quota: "p4-quota-v2"` 是 v4 包的显式能力，需设备报告 `widgetQuota: "p4-quota-v2"`；
能力报告的 `widgetLimits.v2` 给出 v2 上限，`widgetLimits` 其余字段仍是 v1 数字。
未声明 `quota` 的包保持原契约；旧固件在 PC 安装前被拒绝。不与旧版 `game` 预设混用。

| 项 | v1（默认） | v2 |
|---|---:|---:|
| 变量 `vars` | 8 | 16 |
| 状态 `states` | 6 | 8 |
| 页面 `pages` | 4 | 6 |
| transition（需 `p4-input-v1`；无 input 仍为 12） | 24 | 32 |
| tick | 8 | 12 |
| dashboard `switch_state/switch_page` 分支 | 6 | 8 |
| app surface 节点 | 32 | 48 |
| local deck 每组项数（组数仍为 3） | 16 | 24 |
| `persist.vars` | 8 | 16 |
| `runtime/widget.json` 紧凑字节（无 graphics 的包） | 4095 | 8191 |
| `runtime/widget.json` 紧凑字节（graphics 包） | 8191 | 8191 |

`navigation` 与 `form` profile 仍按各自契约限制 2–4 页，不随 v2 放宽。

不变的项：每条 transition/tick 效果 4 项、按钮 8、图片 4 张 / 98304 像素 / 128 KiB、游戏精灵配额、
scene 实体 12 / 规则 20、`buttons.json` 2047 字节、字符串变量 63 字节、`logic` 的 scene 变量镜像仍只覆盖**前 8 个**变量。

## 成本与验收边界

- 固件数组按 v2 上限分配，运行时结构体位于外部 RAM（`EXT_RAM_BSS_ATTR`），增量约 24 KB
  （vars 8×112 B、transition 8×~420 B、tick 4×~700 B、dashboard 分支 14×2×172 B、节点 16×428 B、
  deck 项 3×8×206 B）；解析时的临时副本同样增长。这是按 `ptype/o` 读到的结构体大小估算，
  **内部堆、帧耗时（48 节点绘制、32 条 transition 匹配）和存档恢复必须在真机测量后才能宣称通过**。
- 保存位图从 `uint8_t` 扩到 `uint16_t`；存档 JSON 格式不变，旧存档按原样读回。
- 验证：`validate_generated_widget.py` 与 Rust 校验按 profile 选表；`contracts/quota-v2.json` 提供
  接受/拒绝向量；`firmware/tests/petui_quota_test.py` 核对固件常量与能力报告。

## 持久化子集的最坏尺寸准入

`persist.vars` 的项数上限不等于所有字符串都能同时存档。声明阶段按选择的子集
计算最坏紧凑 JSON 尺寸，不能用较短的 init 值代替运行时上限：

- 固定信封、状态、版本和打印余量：128 字节；保存 page 再加 64 字节。
- 声明非空 alarms 再预留 268 字节（有界 ASCII 告警状态及字段包装）。
- 每项字段：`变量名字节数 + 4 + 值字节数`；整数值按 11 字节计。
- 字符串值按 `2 + 6 × 最大 UTF-8 字节数` 计，覆盖 JSON 控制字符转义。
  普通 v3 字符串为 127；表单 v2/v3 committed 文本按该字段的 max_bytes。
- v1/v2 总预算 1023 字节；v3 为 2303 字节，均不含终止 NUL。

超预算即拒绝声明；可选择较少字段或降低表单字段的 max_bytes。例如 v3 可选择
32 个普通整数，两个 127 字节字符串也可容纳；三个普通 127 字节字符串会被拒绝，
即使 init 均为空。仅 v3 开放普通 string 子集；旧档位仍限表单 committed string。
草稿与键盘瞬态字段仍受表单所有权规则排除。共享向量：`contracts/store-budget-v1.json`。
