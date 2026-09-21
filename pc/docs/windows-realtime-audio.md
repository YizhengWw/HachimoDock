# Windows 实时对话音频

如果同一设备在 macOS 正常，而 Windows 一句话内部频繁断音，应先检查 PC 到设备的音频供流，不应仅据此判断为 ASR 或大模型故障。

## 当前实现

- 使用 `FILE_FLAG_OVERLAPPED` 串口读写，分别持有完成事件，避免等待中的读操作阻塞写入。
- 取消仅针对当前 I/O，并等待完成后释放缓冲区与 OVERLAPPED；串口和事件句柄不继承给子进程。
- 实时音频使用 64 字节分片、400 μs 间隔，不调用同步排空，不暂停麦克风接收，不禁用 AEC/VAD。素材、固件传输和 macOS 分片策略保持独立。
- `playback_tx` 记录写入耗时、字节数和成功状态；`protocol_rejected` 记录允许范围内的指令及拒绝码，不记录声音、对话或 Key。

0.1.78 的实际用户反馈已确认恢复正常播放；0.1.79 保留该实现。交叉编译、离线测试与安装包解包校验不代替所有 Windows 驱动组合的实机验证。

## 验证与排障

完全退出旧客户端后安装最新版本，在原设备上听完整开场白和较长回复，再验证说话打断、继续提问及退出后的形象传输。此 PC 修复沿用 0.7.63-p4 固件。

如仍异常，可导出诊断日志，比较 `playback_tx`、设备 `bufferedMs`、`playback_underrun` 与打断事件。默认日志位于 `%LOCALAPPDATA%\com.petmanager.desktop\logs\realtime-chat.jsonl`；分享前仍请检查是否包含不希望公开的信息。

Windows API 参考：[GetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult)、[CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)。
