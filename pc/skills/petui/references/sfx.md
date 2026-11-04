# 组件提示音（`sfx: p4-sfx-v1`）

让有界组件在按键、结算和场景事件上发出**短提示音**。它是六个固定合成预设的命名别名，
不是采样播放、背景音乐、语音或任意音频引擎。

## 声明

```json
"sfx": {"engine": "p4-sfx-v1", "volume": 60,
        "cues": [{"id": "meow", "preset": "chirp"}, {"id": "munch", "preset": "tap"}]}
```

- 需要顶层 `engine: "p4-bounded-runtime-v4"`，设备能力 `widgetSfx = "p4-sfx-v1"`（`widgetLimits.v2.maxSfxCues = 8`）。
- `volume` 0-100；`cues` 1-8 个，`id` 为 1-15 位 `A-Za-z0-9_.-` 且唯一；`preset` 只能是：

| 预设 | 听感 | 时长 | 适合 |
| --- | --- | --- | --- |
| `tap` | 短促轻敲 | 60 ms | 按键、放置、吃掉 |
| `beep` | 干净单音 | 120 ms | 确认、命中、计时到 |
| `chirp` | 上扬啾鸣 | 180 ms | 奖励、拾取、宠物叫 |
| `low` | 低沉短鸣 | 300 ms | 失败、扣命、拒绝 |
| `water` | 滤波噪声 | 220 ms | 水、风、翻牌 |
| `page` | 纸张噪声 | 80 ms | 翻页、切换场景 |

- 不与 `instrument`（木鱼）、`media`（播放器）、`local`、`program` 混用；它们各自拥有扬声器路径。

## 触发点

- transition：`{"from": "*", "on": "cat.pet", "to": "petted", "sfx": "meow"}`。transition 执行时排队一次，
  `when/else` 两个分支共用同一个 cue（想按分支区分就拆成两条 transition）。
- `tick[].then.sfx`：条件成立、`then` 触发那一次排队。
- scene 规则：`{"op": "sfx", "cue": "shot"}`。只排队，不改变实体，**不算**自测要求的可见反馈。

## 设备行为与边界

- 组件可见且设备不在录音/实时对话/素材传输/OTA/音乐播放时才会出声；其中任一进行中，排队的 cue 直接丢弃，
  不补播。音效不接入 AEC/音频前端。
- 同时最多 2 个声音，第 3 个替换最早的；每个 cue 不超过 300 ms；排队用位掩码，同一帧内重复触发同一 cue 只响一次。
- 退出组件、切到非组件页面后立即静音并清空队列。
- PC 预览不发声，只在场景核心暴露 `takeSfx()` 供测试与调试；真实听感以设备为准。

## 校验

- `validate_generated_widget.py` 与 Rust `validate_p4_widget_sfx` 共享 `contracts/sfx-v1.json` 向量：
  未声明 profile 却引用 cue、未知 cue、未知预设、超过 8 个 cue、重复 id、volume 越界、与 instrument 混用均拒绝。
- 固件 `pet_p4_sfx_core.c` 是纯算法，`firmware/tests/petui_sfx_test.py` 以 ctypes 直接驱动生产代码：
  预设名与校验器一致、cue 必衰减到静音、volume 0 静音、双声道混音不溢出。
- 实机音量、听感与"对话时不出声"需在设备上人工验收；离线测试不替代。
