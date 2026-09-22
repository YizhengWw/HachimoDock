# 双向语音实现与验证边界

实时对话采用 USB 上下行并发音频，并非 WebRTC 协议；不需要额外的 RTC 服务端。

## 音频链路

设备麦克风和实际播放参考 → ESP-SR AEC、降噪、VAD → USB → PC 流式 ASR → 对话大模型 → TTS → USB → 设备扬声器。

- 固件同时声明 `audioAec`、`audioVad`、`audioFullDuplex`，且音频帧带有效 AEC/VAD 标记时启用双向收音。旧固件使用半双工兼容路径。
- 使用 ESP-SR 2.5.3 的独立 AEC/NS/WebRTC VAD API；未引入完整 AFE 工厂、VADNet 或唤醒词模型。当前 AEC 使用 `AEC_MODE_VOIP_LOW_COST`。
- v1/v3 共用 16 kHz、单声道 PCM16、20 ms USB 音频帧。DSP 块长独立适配，AEC 工作区和参考缓冲使用 PSRAM。
- PC 保留 600 ms 句首缓存，区分聆听与回复阶段的触发门槛。用户语音触发后取消当前 LLM/TTS/播放，递增播放代次，避免旧音频和字幕重新出现。
- AEC 标记失效、队列满或持续音频故障会明确停止会话，不静默丢弃新语音。诊断日志记录数量、阶段及耗时，不记录 PCM、对话正文或凭据。
- 打断不会撤回已经执行的家居操作，也不会自动重试；尚未完整播报的待确认任务失效。

实现参考 DeskBot V2 的音频参考拓扑和取消边界，按 ESP-IDF、ES7210/ES8311 和 Rust 适配；不引入其 Python/Arduino 运行时或服务配置。

## 官方方案对照

[ESP-SR VAD 文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/vadnet/README.html)说明句首缓存的重要性。本项目用 PC 有界预录补偿判定延迟，不声称接入其 VADNet。

[ESP-SR AEC 文档](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/acoustic_echo_cancellation/README.html)提供 FD 模式等选择。切换模式需要同时比较回声误触发、近端识别和实时负载，不能仅凭模式名称判断效果。

## 验证边界

0.1.82：回复期间 VAD 只触发候选，ASR 识别至少两个有效字符或单字“停”后才取消回复；等待确认时继续播报，拒绝空转写候选。600 ms 预录与同一识别流继续使用，减少句首丢失。该策略不能证明声音一定来自用户：其他音箱播放和残留回声仍可能产生转写，间歇性误打断尚未完成定位。本次外发构建未新增打断算法改动，不将重新打包描述成问题已经修复。

已有主机侧回归及部分实机播放、识别、打断测试；这些不等于所有 v1/v3 板型、外壳与声学环境均已完成验收。轻声、远场、持续双讲及长时间压力仍需在实际安装位置验证。

建议验收：最大音量播放时不自打断；开场白/思考/播放阶段均可打断；近远距离短句完整识别；连续打断十次无旧音频复活；断线后可恢复；退出后恢复 Agent 跟随与按键语音。日志解读见 [实时对话](realtime-chat.md) 和 [Windows 音频](windows-realtime-audio.md)。
