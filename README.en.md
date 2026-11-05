# HachimoDock

[简体中文](README.md) · [Downloads](https://github.com/YizhengWw/HachimoDock/releases/latest) · [Issues](https://github.com/YizhengWw/HachimoDock/issues)

HachimoDock is a desktop interface for AI Agents. A Windows/macOS manager and an ESP32-P4 display provide task-state animations, conversation bubbles, voice input, custom appearances and interactive components. The installed desktop app is named **HachimiDock**; some developer documentation calls it **Pet Manager**.

## Versions and downloads

| Component | Current release | Platform |
| --- | --- | --- |
| Desktop client | **0.1.199** | Apple silicon macOS / Windows x64 |
| Device firmware | **0.7.103-p4-r179** | Separate ESP32-P4 v1 and v3 images |
| Current source | Client 0.1.199 / firmware R179 version identifiers | `main` may contain post-release changes |

[Download Mac 0.1.199 (Apple silicon)](https://github.com/YizhengWw/HachimoDock/releases/download/v0.1.199/HachimiDock_0.1.199_macOS-arm64.dmg) · [Download Windows 0.1.199 (x64)](https://github.com/YizhengWw/HachimoDock/releases/download/v0.1.199/HachimiDock_0.1.199_Windows-x64-setup.exe)

See [v0.1.199](https://github.com/YizhengWw/HachimoDock/releases/tag/v0.1.199) for all assets and verify downloads against `SHA256SUMS`. The source archive matches that release tag. Version 0.1.199 updates only the desktop client and continues to use R179 firmware; devices already running R179 do not need another firmware update. Intel Mac and Linux installers are not provided.

Complete flashing kits are for first installation or factory recovery. Choose the kit for the actual chip revision: **v1 and v3 are not interchangeable**. A complete flash erases settings, appearances and components. Routine client OTA updates only the application and does not repartition existing devices. See [firmware build and flashing](firmware/BUILD.md).

## Features and compatibility

- Follow supported Agent activity on the device through animations and task bubbles.
- Hold SW1 to record speech; release to append a draft. Confirm separately to send it.
- Long-press SW2 in the pet view to start or end real-time conversation, after configuring speech/chat services.
- Import or generate appearances; compatible firmware supports separate status-audio synchronization.
- Install games, tools and PC-fed components. Optional missing touch capability does not block installation, but touch-dependent gameplay may be unavailable. Schema, integrity and storage checks still apply.
- Adjust device volume and automatic/manual city location below voice input on the device home page. City selection is not GPS positioning and can be disabled.

Integrations include ChatGPT (Codex), Claude, OpenClaw, MiMoCode, WorkBuddy and DeepSeek Harness. Status reporting, dictation, sending and desktop conversation switching are distinct capabilities. WorkBuddy and DeepSeek target conversations must be opened in their own apps; selecting a device bubble does not switch those desktop conversations. MiMoCode voice support is platform-dependent. See the [Agent matrix](pc/docs/agent-integration-audit.md) and [WorkBuddy guide](pc/docs/workbuddy.md).

Unavailable or ambiguous editors stop the operation. Unknown write/send results are not automatically replayed. Detecting an Agent does not prove its editor is ready. Runtime features depend on actual firmware capabilities; host tests and successful compilation do not establish hardware, performance or acoustic acceptance.

Public installers do not supply service API keys. Enter your own credentials in API settings. Online recognition, generation, data providers and Agents may require internet access; PC-fed components require the manager to remain running. User-uploaded materials retain their respective rights.

## Installation and default controls

The macOS release package is Developer ID-signed and Apple-notarized; the Windows installer is not publisher-signed. Verify its source and checksum before following the operating system's security prompts. Do not disable Gatekeeper or remove protections from unknown downloads. On macOS, allow the client under **System Settings → Privacy & Security → Accessibility** for Agent input.

SW1 short press and joystick centre confirm; SW2 short press switches pet/component views; SW3 returns. Joystick directions select sessions/components or perform component actions. These defaults can be changed in the manager. Confirming voice input is separate from approving an Agent's tool permissions.

## Hardware and development

The paired hardware uses ESP32-P4, 32 MiB Flash, 32 MiB PSRAM and a 480×640 MIPI-DSI panel. Components use a rotated 640×480 logical canvas. Connect the display to **DSI**, not CSI, with power disconnected. Consult the [hardware project](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB) before substituting parts.

- [Desktop source and build guide](pc/README.md)
- [Firmware source and build guide](firmware/BUILD.md)
- [PetUI component contracts](pc/skills/petui/SKILL.md)
- [Volume and shared city settings](pc/docs/shared-device-settings.md)

Resources are stored in ordinary Git; Git LFS is not required. Preserve dependency locks and choose the correct target configuration. Published binaries retain their original build contents even when `main` changes.

## Acknowledgements

We thank the following projects and communities for their code, tools and foundational capabilities:

- **[Xiaomi MiLoCo](https://github.com/XiaoMi/xiaomi-miloco) and [MiSans](https://hyperos.mi.com/font/zh/)**: smart-home protocol adaptation references and device display fonts.
- **Espressif and Waveshare**: device development frameworks, board drivers and speech-processing capabilities.
- **Tauri, React, Node.js, FFmpeg and Lucide**: foundations for the desktop application, audiovisual processing and user interface.
- **[Clawd on Desk](https://github.com/rullerzhou-afk/clawd-on-desk)**: portions of the Agent state-monitoring and Hook bridge code.

We also thank everyone who contributed development, testing, feedback and community components. See [Third-Party Notices](THIRD_PARTY_NOTICES.md) for third-party copyrights and licenses.

## License and feedback

**Non-commercial source available.** The [project license](LICENSE) permits use, modification and component creation within its stated scope. Commercial use and independent products require prior written permission. Third-party software and materials retain their [own licenses](THIRD_PARTY_NOTICES.md); earlier releases retain their accompanying terms.

Home-control capabilities integrate [Xiaomi MiLoCo](https://github.com/XiaoMi/xiaomi-miloco), whose relevant works are copyrighted by Xiaomi and governed by the [Xiaomi Miloco License](licenses/Xiaomi-Miloco-LICENSE.md). This project grants no additional commercial, sublicensing or trademark rights.

For project contact, see [作者 / Author](README.md#作者--author). Report bugs through Issues with versions and reproduction steps, excluding credentials, personal data and raw logs. Report security issues privately under [SECURITY.md](SECURITY.md).
