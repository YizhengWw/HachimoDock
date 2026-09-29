# Agent 接入检查（0.1.86）

后续 0.1.88 修复 WorkBuddy 两个遗漏：输入改为进程定向事件，禁止系统级全选/粘贴；气泡不再仅显示状态，而是提取当前提问/回复的受限摘要。原生草稿诊断不发送消息，回复来源与隐私边界见 [WorkBuddy 说明](workbuddy.md)。以下表格保留 0.1.86 当时检查范围，不代表当时已完成这些后续修复。

本次检查覆盖事件接收、设备气泡、语音路由及组件 Skill 安装，属于代码与自动化回归检查，不代表每种 Agent、操作系统和实体设备组合均已真机验收。

| Agent | 状态与气泡 | 按键语音路径 | 本次处理 |
| --- | --- | --- | --- |
| ChatGPT（Codex） | 日志事件及独立会话显示 | macOS / Windows 可见输入框 | 沿用已有路径和回归；统计事件不冒充新回复 |
| Claude | 日志和 Hook；授权请求另走独立通道 | macOS / Windows 可见桌面输入框 | 补授权气泡、部分仅有状态的 Hook 显示；默认不代替用户批准工具操作 |
| MiMoCode | 插件提供标题、内容与状态 | macOS 当前终端光标；Windows 尚未实现 | 保留正常显示链路；非 macOS 在启动 ASR 前明确提示限制，修正旧“不支持任何语音”的笼统文案 |
| OpenClaw | 网关事件 | 已有 Agent Bus 路由 | 补真实会话状态的气泡，心跳不刷新气泡时间，断开连接不残留旧气泡 |
| WorkBuddy | 独立 Hook 和会话状态 | macOS / Windows 当前可见输入框 | 补状态气泡、追加语音草稿、确认发送及 Skill 安装；不读取私有历史数据库 |

语音确认发送不等于代替 Agent 批准工具权限。Claude 的默认权限 Hook 返回空结果，将决定权留给 Claude；显式配置的 allow/deny 策略仍保留。未知策略不再回退为 allow。

协议依据：[Claude 官方 Hook 文档](https://code.claude.com/docs/en/hooks#permissionrequest-decision-control)。只有明确的决定对象才代替用户允许/拒绝请求；非交互场景无法弹出确认时，仍遵循 Claude 自身的权限处理。

WorkBuddy 的设备气泡切换与桌面对话切换是不同能力：历史对话需在 WorkBuddy 手动打开。MiMoCode Windows 的语音尚未实现，不以“已检测到程序”冒充可用；设备自身实时对话不受影响。

新增测试验证 Claude 独立授权事件、Claude/MiMoCode 各活跃状态、OpenClaw 会话事件与心跳/断连、WorkBuddy 生命周期，以及默认授权策略与平台能力限制。macOS WorkBuddy 做过不发送的可恢复草稿 UI 检查；Windows 输入和所有 Agent 的完整设备按键链路仍需真机验证。
