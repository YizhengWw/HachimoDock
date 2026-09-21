# 安全使用与问题反馈 / Security

## 安全使用

- API Key 请填写在客户端“API 配置”中，不要上传到公开 Issue、截图或代码仓库。
- 本机 Agent 连接服务仅用于当前电脑，不要通过代理或端口转发暴露到网络。
- 此服务信任同一系统用户运行的原生程序，不提供进程隔离。运行不可信程序时，请使用独立系统账户。
- 设备通过 USB 接收更新；当前 OTA 校验不等同于签名固件或安全启动验证。请使用可信来源的客户端和固件。

## 反馈安全问题

请通过 GitHub 的私密漏洞报告联系维护者；若该入口不可用，可使用 [项目首页](README.md#作者--author) 的联系邮箱。请提供受影响的客户端及固件版本、操作系统、复现步骤和必要的脱敏日志。

不要在公开 Issue 中发布密钥、私人会话、完整设备配置或漏洞利用细节。优先在最新发布版本上确认问题。

## Safe use

Keep service keys, private conversations and device configuration out of public issues and screenshots. The local Agent service is intended for this computer only: do not expose it through a proxy or port forwarding. Native programs running as the same OS user are trusted; use separate accounts for untrusted software.

USB firmware updates require trusted software and physical device access. OTA integrity checks are not a signed secure-boot guarantee.

## Reporting a vulnerability

Use GitHub private vulnerability reporting, or the maintainer email on the [project homepage](README.md#作者--author) if that option is unavailable. Include affected versions, your operating system, reproduction steps and minimal redacted logs. Do not post exploit details or personal data in a public issue. Please check the latest release first.
