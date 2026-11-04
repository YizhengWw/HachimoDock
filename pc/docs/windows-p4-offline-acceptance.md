# Windows P4 离线检查（尚未实机执行）

本次只有脚本与步骤交付；Windows COM、GUI、麦克风、声学和 v3 均未验收。P4 使用客户端原生 ASR，不要求 `voice-service`、LiveKit 或 8080 端口。

## 搬到 Windows

携带主线程最终提供的 **Windows x64 安装包/完整 portable 目录**，以及本说明和 `pc/scripts/check-windows-p4-offline.ps1`。不要只复制主 exe：Node、FFmpeg、bridge、固件、内置组件等资源须保持原相对目录。无需安装 Node/npm/Python 或拉取依赖。WebView2 缺失时保留报告，另由主线程安排离线安装资源，不自行联网安装。

在 PowerShell 指定实际资源根目录（其中应直接存在 `bridge\runtime\node.exe` 和 `tools\ffmpeg.exe`）。先做软件检查，**不启动客户端或打开 COM**：

```powershell
powershell.exe -NoProfile -File .\check-windows-p4-offline.ps1 `
  -ResourceRoot 'C:\实际安装目录' `
  -ClientExe 'C:\实际安装目录\HachimiDock.exe' `
  -OutputDirectory "$env:USERPROFILE\Desktop\p4-check-01"
```

输出目录必须全新。报告检查 x64 PE、真实 Node 可执行性、审计 FFmpeg SHA/LGPL配置/编码器声明、WebView2存在性和COM枚举；不会执行编码、启动语音、读取凭据或打开麦克风。

## 显式 COM 单次只读检查

先完全退出客户端、串口助手和其他占用者；禁止并行 reader。从首次报告的 `comPorts` 中选实际端口，不猜、不自动换端口：

```powershell
powershell.exe -NoProfile -File .\check-windows-p4-offline.ps1 `
  -ResourceRoot 'C:\实际安装目录' -ProbeCom -Port COM3 `
  -OutputDirectory "$env:USERPROFILE\Desktop\p4-com-check-01"
```

默认4000000波特，最多只读3秒；DTR/RTS关闭，无发送、无reset/flash/安装指令、不保存串口正文。驱动仍可能影响端口状态，所以仅在明确允许时运行。无字节只表示监听期内无数据，不证明设备异常；读到字节也不证明固件身份/协议正确。`finally` 关闭并Dispose，报告单独记录opened/closed/disposed。打开/读取/关闭失败均保留报告，不自动重试或重放。存在失败就停止，先交回目录。

## 后续 GUI/人工验收

确认脚本已退出、`closed=true` 且 `disposed=true` 后，再启动GUI作为唯一reader。检查握手前显示等待设备、确认P4后无legacy缺包提示；手动验证设备麦克风监听/按住录音、草稿/确认、停止及断连恢复。仅使用无隐私短句，不回传录音、识别正文、密钥或配置文件。同步/删除/OTA若结果未知，停止写入，不能重试。

脚本只给有限软件/被动COM证据；`windowsP4VoiceAcceptance`、`windowsGuiAcceptance`、`v3HardwareAcceptance` 保持 `NOT_RUN`，不能据此写“Windows实机通过”。既有 `check-voice-runtime-windows.ps1` 默认也不要求legacy；只有显式 `-CheckLegacyVoice` 才核对可选旧服务。
