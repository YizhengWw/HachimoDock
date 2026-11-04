# 条件逻辑（p4-logic-v1）

`logic: "p4-logic-v1"` 是 v4 包的显式能力，给 transition、tick 和通用 scene 规则加上有界的
条件判断、else 分支和变量间增量。设备必须报告 `widgetLogic: "p4-logic-v1"`；旧固件在
PC 安装前被拒绝，不会静默忽略条件。未声明 `logic` 的包保持原契约：transition 没有
`when/else`，tick 只接受 `{var, lte}`，`inc` 只接受整数字面量。

不与 `local`、`media`、`instrument`、`host`、`program` 混用；可与原生预设 `game`（含 `gomoku`）并用，预设不写变量，`when` 条件与 tick 照常生效。它不是脚本、
表达式求值器或算术引擎：没有乘除、字符串比较、嵌套或 `or`。

## 条件

```json
{"var": "fish", "op": "gt", "value": 0}
{"var": "score", "op": "gte", "value": {"var": "goal"}}
{"all": [{"var": "lives", "op": "gt", "value": 0}, {"var": "ammo", "op": "gt", "value": 0}]}
{"var": "left", "lte": 0}
```

- `var` 必须是已声明的 `int` 变量；`op` 为 `lte|lt|gte|gt|eq|ne`；`value` 为 ±10⁹ 内整数或
  `{"var": "另一个 int"}`。
- `all` 最多 2 条，不能嵌套，不与其他键并列。没有 `any/not`。
- `{var, lte}` 是旧 tick 写法，等价于 `op: "lte"`；未声明 `logic` 的包只能用这一种。
- 条件读取当前变量值；`set/inc` 的 `{var}` 源读取本次效果前的快照（与原 `set {var}` 一致）。

## transition

```json
{"from": "answer", "on": "grade.again", "when": {"var": "retries", "op": "lt", "value": 8},
 "to": "ask", "page": "next", "inc": {"again": 1, "retries": 1},
 "else": {"to": "ask", "page": "next", "inc": {"again": 1, "remaining": -1}}}
```

- `when` 为假且没有 `else`：这条 transition 不执行，继续匹配后面的 transition（与不可用的
  导航规则一样）。两条互补条件的 transition 可以表达 if/else。
- `else` 只接受 `to/page/set/inc`，必须和 `when` 同时出现，并且至少改变状态、页面或变量。
  `else` 不执行导航栈操作（`push/back/replace/reset`），`page` 只能是页面 ID、`next`、`prev`。
- 主分支与 `else` 的 `set+inc` **合计不超过 4 项**：两者共用同一块固件效果数组，内存不增长。
- 与 scene 同用时，无论走哪个分支，scene 的同名 action 规则都会派发；规则自己再用 `when` 守卫。
- `inc` 可以写 `{"var": "preset"}`，把另一个 int 变量加到目标上，仍受 `limits` 与 ±10⁹ 夹限。

## tick

tick 的 `when` 使用同一条件语法（含 `op/value/all`），`then` 不变。tick 没有 `else`：
每步都执行的效果写在 tick 顶层即可。

## scene

```json
{"on": "fire", "when": {"var": "ammo", "op": "gt", "value": 0},
 "do": [{"op": "place", "entity": "bullet", "source": "player", "dy": -1}, {"op": "var", "name": "ammo", "inc": -1}]}
```

- 规则级 `when` 只接受一条条件（不接受 `all`），变量必须在前 8 个声明中。
- `{"op": "var", "name": "...", "set": n}` / `"inc": n`，`n` 为 -10000..10000，只写 int 变量；计入每条
  规则 4 个操作的配额。`restart` 不会重置这些变量。
- 变量通过固件的有界镜像进出引擎：动作派发前写入，派发后读回并按 `limits` 夹限；PC 预览
  和自测用同一份 C 核心（WASM）与同一份 Python 模型。
- 规则按声明顺序求值，前一条规则对变量的修改对后一条立即可见。要表达"生命耗尽才结束"，
  把 `when lives lte 1 → stop` 写在 `when lives gt 1 → lives-1` 前面。

## 校验与验收

- `validate_generated_widget.py` 检查 profile、能力、条件 schema、else 约束与合计效果数。
- `smoke_test_widget_game.py` 的应用/场景模拟都执行条件；一个永远为假的 `when` 会让动作在可达
  状态中没有反馈，自测会指出来。
- 共享向量：`contracts/logic-v1.json`，由 Python、JS（预览与技能镜像）、C（比较表）和 Rust
  拒绝用例共同执行。结构与模拟通过不等于实机通过：设备解析、按键、存档与帧耗时仍需另外验收。
