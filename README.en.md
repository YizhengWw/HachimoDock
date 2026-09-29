# HachimoDock

[简体中文 · Product gallery](README.md) · [Downloads](https://github.com/YizhengWw/HachimoDock/releases)

HachimoDock turns Agent activity into a desktop companion: pet animations, conversation bubbles, voice dictation and interactive components on an ESP32-P4 display. Pet Manager supports macOS and Windows and integrates with agents including ChatGPT (Codex), Claude, OpenClaw, MiMoCode and WorkBuddy.

Home-control capabilities are provided by [**Xiaomi MiLoCo**](https://github.com/XiaoMi/xiaomi-miloco). The relevant MiLoCo licensed works are copyrighted by Xiaomi and subject to the [Xiaomi Miloco License](licenses/Xiaomi-Miloco-LICENSE.md); copyright notices, disclaimers and the license are retained with the relevant implementation. Non-commercial use must still comply with the license's permitted scope. This project grants no additional commercial, sublicensing or trademark rights. See [Third-Party Notices](THIRD_PARTY_NOTICES.md) for details.

## Source and builds

- [`pc/`](pc/README.md): desktop client 0.1.101, local Agent bridge, component-generation Skill and built-in resources.
- [`firmware/`](firmware/BUILD.md): ESP32-P4 runtime 0.7.72-p4, board drivers, tests and complete factory-image tools.
- Install Git LFS and run `git lfs pull` after cloning. Build dependencies are described in each directory.
- For a ready-to-use app or a complete device flashing kit, visit [Downloads](https://github.com/YizhengWw/HachimoDock/releases/latest).
- To use speech recognition or appearance generation, enter your service keys in Pet Manager’s API settings.

Choose the complete v1 or v3 flashing kit for your actual chip revision; they are not interchangeable. Contact customer support if unsure. Windows and macOS use the same firmware for each chip family. The scripts require Python 3.10+ and install esptool **5.4.0**. They check the chip before erasing; a complete flash clears settings, appearances and components. Connect the display to **DSI**, not CSI, with power disconnected. The v3 24 MHz display fix restored pet animation on a physical board; input, audio and large-file transfers still require release-level hardware validation. See the [flashing guide](firmware/BUILD.md) for tool versions and instructions.

## Device and voice use

Release 0.1.101 improves the component center and device interaction. Default visible tools are Stocks, Wooden Fish, Upcoming Todos, and Computer Status. Third-party components and services retain their respective licenses and conditions.

Configure speech and chat-model credentials, then long-press SW2 in the pet view to start or end real-time conversation. Compatible firmware supports listening during playback and interruption. Pair your home account on the Smart Home page for natural-language control of authorized devices. The built-in stock watchlist defaults to Xiaomi and Alibaba; manage it in component details. Quotes are refreshed by the PC every two seconds and forwarded over USB, may be delayed, and do not enable trading.

Public installers contain no API keys, enterprise CA certificates or fixed proxy addresses. For image moderation failures, the client explains the rejected stage and offers manual MP4 import using material you are authorized to use.

Default short presses: SW1 confirms, SW2 switches the pet/component-center view, SW3 returns. Long-press SW1 records speech; releasing writes a draft, and Confirm sends it. Permissions are required to control the chosen Agent's input field. Dictation targets and desktop session switching depend on each Agent's integration. For WorkBuddy, manually open the target conversation and keep its input visible; changing the device bubble does not switch the desktop conversation. See [WorkBuddy](pc/docs/workbuddy.md) and [Agent integration](pc/docs/agent-integration-audit.md) for platform limits.

Component behavior and physical bindings can be adjusted in Pet Manager. Use the bundled `petui` Skill inside an Agent to create and iterate on non-commercial components, then synchronize them to the device.

## macOS installation

If a downloaded app will not open, after verifying its origin and checksum run:

```sh
xattr -cr "/Applications/Pet Manager.app"
```

Enable Pet Manager in **System Settings → Privacy & Security → Accessibility** when prompted to allow Agent input and control. See each release for complete firmware flashing instructions.

## License and notices

**Non-commercial source available.** The [HachimoDock Project License](LICENSE) permits non-commercial use, project modifications and component creation within its scope. Commercial use, paid services, commercial integration and independent apps/services/products require prior written permission. Project branding is not licensed for misleading promotion or endorsement. Third-party code and materials retain their [own licenses](THIRD_PARTY_NOTICES.md); earlier releases retain their accompanying licenses.

For authorization, use the maintainer contact published in the [Chinese README](README.md#作者--author). Report security issues privately as described in [SECURITY.md](SECURITY.md).
