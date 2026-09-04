# HachimoDock

[简体中文 · Product gallery](README.md) · [Downloads](https://github.com/YizhengWw/HachimoDock/releases)

HachimoDock turns Agent activity into a desktop companion: pet animations, conversation bubbles, voice dictation and interactive components on an ESP32-P4 display. Pet Manager supports macOS and Windows and integrates with agents including ChatGPT (Codex), Claude, OpenClaw and MiMoCode.

## Source and builds

- [`pc/`](pc/README.md): desktop client 0.1.54, local Agent bridge, component-generation Skill and built-in resources.
- [`firmware/`](firmware/BUILD.md): ESP32-P4 runtime 0.7.49-p4, board drivers, tests and complete factory-image tools.
- Install Git LFS and run `git lfs pull` after cloning. Build dependencies are described in each directory.
- For a ready-to-use app or a complete device flashing kit, visit [Downloads](https://github.com/YizhengWw/HachimoDock/releases/latest).
- To use speech recognition or appearance generation, enter your service keys in Pet Manager’s API settings.

## Device and voice use

Default short presses: SW1 confirms, SW2 switches the pet/component-center view, SW3 returns. Long-press SW1 records speech; releasing writes a draft, and Confirm sends it. Permissions are required to control the chosen Agent's input field. If the Agent is already open, dictation uses its current conversation; otherwise Pet Manager opens the selected Agent and uses the device-selected conversation when available.

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
