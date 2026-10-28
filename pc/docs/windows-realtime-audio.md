# Windows 实时音频与诊断

Windows 串口采用原生重叠 I/O，并复用 serialport 的配置边界。读写操作分别持有完成事件；取消只针对所属请求，待真正结束后再释放缓冲区，不把句柄继承给子进程。

音频发送保留有界分片，不用同步排空阻塞收音，也不禁用设备 AEC、VAD 或插话处理。配置、传输及设备状态分别记录错误，不能由一条通用报错直接推断为网络或驱动原因。

## 排查

1. 使用匹配的公开客户端和设备固件，不先清空用户数据。
2. 区分串口打开/初始化失败、控制协议错误、音频供流不足和识别服务错误。
3. 听取完整开场白和连续回复，检查播放中插话、停声、后续识别及退出后的状态。
4. 提供脱敏日志中的阶段、错误码、写入耗时和设备缓冲状态；不要公开声音、对话正文、Key 或本机用户目录。

macOS 构建、交叉编译或主机回归不能代替 Windows 上实际驱动及硬件组合的验证。部分电脑上的 COM 口问题仍需按实际证据排查，不在此声称已经解决。

Windows I/O 生命周期参考：[GetOverlappedResult](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult)、[CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)。
