<div align="center">

# Hachimiao

[简体中文](README.md) | English

A dedicated companion display for CLI agents: always present on your desk, physically visible, and able to turn agent state into expressive pet behavior.

<p>
  <img src="https://img.shields.io/badge/license-GPL--3.0--only-blue" alt="License: GPL-3.0-only" />
  <img src="https://img.shields.io/badge/desktop-Tauri%202%20%2B%20React-24292f" alt="Desktop: Tauri 2 + React" />
  <img src="https://img.shields.io/badge/hardware-Raspberry%20Pi%20Zero%202%20WH-2ea44f" alt="Hardware: Raspberry Pi Zero 2 WH" />
  <img src="https://img.shields.io/badge/version-0.1.0-orange" alt="Version: 0.1.0" />
</p>

<img src="assets/image_01.png" alt="Hachimiao cover" />

</div>

## Table of Contents

- [1. Overview](#1-overview)
- [2. Highlights](#2-highlights)
- [3. Software Development](#3-software-development)
- [4. Hardware Reproduction](#4-hardware-reproduction)
- [5. User Guide](#5-user-guide)
- [6. Maintenance](#6-maintenance)

## 1. Overview

Hachimiao is a dedicated small display for agents. It turns CLI agents running on your PC, such as Codex, Claude Code, and OpenClaw, into a visible and touchable desktop companion.

When the agent thinks, the pet thinks with it. When the agent calls tools, the pet starts working. When a task finishes, it celebrates; when something fails, it reacts. Hachimiao is not just another virtual assistant window. It gives agent activity a persistent, glanceable, physical presence.

| Product Photo | System Overview |
| --- | --- |
| ![Desktop companion][img-intro-1] | ![System overview][img-intro-2] |

| Glanceable | Voice-first |
| --- | --- |
| Agent state, progress, pending decisions, and results are translated into pet expressions, animations, colors, and short labels. A quick look tells you what the agent is doing. | No need to switch back to a terminal or chat window first. Speak to the device to talk to the agent, issue commands, capture thoughts, or continue work. |

## 2. Highlights

### Agent State Following

The desktop app keeps the pet display in sync with the agent in real time. Expressions, animations, colors, captions, and subtle prompts communicate current agent state.

![Agent status demo][img-status-gif]

| Status screen | Prompt screen |
| --- | --- |
| ![Status screen 1][img-status-1] | ![Status screen 2][img-status-2] |

### Idle and Touch Interaction

When the agent is idle, the pet plays on its own with multiple idle states. You can also touch the screen to interact with it.

<p align="center">
  <img src="assets/readme/idle-touch-feedback.gif" alt="Idle and touch feedback" />
</p>

### Voice Interaction With Agents

The device microphone can be used to talk to any agent session, reducing the need to type or switch contexts.

### Custom Pet Avatars

Hachimiao includes a built-in Westie avatar with 16 animated states. You can also upload pet photos, profile images, or original characters to generate new avatars, and import avatars from the local Codex pet library or the pet community.

| Avatar generation | Avatar collection |
| --- | --- |
| <img src="assets/readme/custom-avatar-poster.webp" alt="Avatar generation" /> | <img src="assets/readme/custom-avatar-collection.webp" alt="Avatar collection" /> |

### Custom Widgets

Built-in widgets include a slack-off countdown, Pomodoro timer, water reminder, and token usage display. New widgets can be generated from a natural-language prompt and sent to the device.

| Widget center | Widget preview |
| --- | --- |
| ![Component center][img-components-1] | ![Component preview][img-components-2] |

## 3. Software Development

Hachimiao consists of a PC-side manager, a device runtime, and an agent integration layer:

- **Pet Manager desktop app**: built with Tauri 2, React, and Vite. It handles device binding, agent state following, avatar management, the widget center, button configuration, voice entry, and connection diagnostics.
- **Device runtime**: runs on Raspberry Pi hardware and handles screen rendering, touch/rotary/button input, state reception, widget runtime, and local resource management.
- **Agent integration layer**: reads session state, tool calls, pending confirmations, token usage, and captions from local CLI agents, then sends normalized state to the device over USB serial or MQTT.

```text
CLI Agent
  |  Codex / Claude Code / OpenClaw session state
  v
Pet Manager desktop app
  |  device binding, appearance, component, button config
  |  USB serial or MQTT
  v
Board runtime on Raspberry Pi
  |  state files, widget runtime, touch/rotary/button input
  v
Hachimiao hardware display
```

### 3.1 Quick Start

> These commands are for developers. If you only want to reproduce the hardware and use a packaged app, jump to [5. User Guide](#5-user-guide).

```bash
git clone <your-repo-url>
cd claw-pet-manager/ref
npm install
npm run dev
```

Common scripts:

| Command | Description |
| --- | --- |
| `npm run dev` | Start the Tauri desktop app in development mode |
| `npm run dev:web` | Start the Vite web preview only |
| `npm run build` | Build the Tauri desktop app package |
| `npm run build:web` | Build web static assets |
| `npm run pack-builtins` | Package built-in `.clawpkg` widgets |

### 3.2 Development Environment

- Node.js and npm.
- Rust and the Tauri 2 build toolchain.
- At least one local CLI agent, such as Codex, Claude Code, or OpenClaw.
- USB serial access or a LAN environment with an MQTT broker.
- Raspberry Pi Zero 2 WH runtime environment for the device side.

### 3.3 Repository Layout

```text
claw-pet-manager/
  ref/                  Pet Manager desktop app (Tauri 2 + React + Vite)
    src/                React UI: device wizard, dashboard, avatar gallery, widget center, voice config
    src-tauri/          Tauri + Rust backend: local files, bridge, USB/MQTT dispatch, widget install
    builtin-clawpkgs/   Built-in negative-screen widget source
  board-runtime/        Raspberry Pi device runtime (C / shell / Python)
    src/                Device services such as board-server
    builtin-clawpkgs/   Built-in widgets on the device side
    ui/ assets/         Provisioning pages, fonts, and device assets
  scripts/              Repository-level scripts, such as built-in .clawpkg packaging
  skills/               Agent skills used by the widget center
  docs/                 Architecture, voice pipeline, packaging, and development docs
```

### 3.4 Open Specifications

The project plans to maintain four open specifications:

1. **Agent state protocol**: how third-party agents send working state, captions, tokens, tool calls, and pending confirmations to the pet display.
2. **Device communication / SDK**: state dispatch over USB serial line protocol or MQTT, with support for multiple hardware targets, languages, and agent integrations.
3. **Avatar / animation format**: reusable community pet resources compatible with the Codex Pet ecosystem.
4. **`.clawpkg` widget format**: a package format for generating, installing, sharing, and reusing negative-screen widgets.

A `.clawpkg` package is expected to contain:

```text
component.json        Component metadata (id / name / version / author / description)
buttons.json          Button action bindings
negative-screen.json  Negative-screen display configuration
runtime/widget.json   Declarative state machine (vars / states / transitions / tick / dashboard)
share.json            Sharing / export metadata
assets/               Icons and other assets
```

## 4. Hardware Reproduction

Hardware reproduction materials have been split into a dedicated document so the BOM, enclosure, assembly images, and PCB process notes can be maintained cleanly.

- [Open hardware reproduction guide](docs/hardware-reproduction.en.md)
- [OSHWHub hardware project](https://oshwhub.com/eda_lfilxkob/project_ukwrttbk)
- Covers: complete-device BOM, purchase notes, enclosure and assembly, and PCB process information.

## 5. User Guide

The user guide has been split into a dedicated document and now includes the full software screenshots from the Word document.

- [Open user guide](docs/user-guide.en.md)
- Covers: device binding, Pet Manager dashboard, avatar gallery, custom avatars, widget center, and AI widget generation flow.

## 6. Maintenance

### 6.1 Contributing

GitHub issues, discussions, and pull requests are welcome. Suggested contribution areas:

- Fix Pet Manager or board runtime issues.
- Adapt new screens, main boards, enclosures, or hardware forms.
- Create pet resources, animation assets, and caption styles.
- Develop new `.clawpkg` negative-screen widgets.
- Improve assembly guides, flashing guides, and troubleshooting docs.
- Improve the agent state protocol and third-party agent integrations.

### 6.2 License

The README badge marks the software code as `GPL-3.0-only`. Before a formal release, the repository should include a complete root-level `LICENSE` file and separate notices for hardware designs, enclosure files, official pet assets, and third-party resources.

- Software code License: `GPL-3.0-only`
- Hardware design: follow the OSHWHub project page and the engineering files published with the repository.
- 3D enclosure files: follow the license notice shipped with the model files.
- Official pet assets: intended for this project's examples and demos; keep source attribution before reuse or redistribution.
- Third-party resources: document external models, assets, widgets, or libraries in their files or a future `THIRD_PARTY_NOTICES.md`.

### 6.3 Security Issues

If you discover a security issue, please do not disclose sensitive details publicly. For now, contact the maintainers through the community group first. If you must use a GitHub issue, describe the impact without posting sensitive reproduction details. If GitHub Security Advisory or a dedicated security email is enabled later, follow the latest repository instructions.

### 6.4 Project Links

- GitHub: <https://github.com/Skylerww/Hachimiao>
- OSHWHub hardware project: <https://oshwhub.com/eda_lfilxkob/project_ukwrttbk>
- Community group: scan the QR code below.

<p align="center">
  <img src="assets/community-qr.png" alt="Hachimiao community QR code" width="220" />
</p>

### 6.5 Acknowledgements

- Thanks to the JLC / LCSC ecosystem, OpenClaw, MiMo, Petdex / Codex Pet, related open-source projects, and community contributors.

<!-- Images -->

[img-intro-1]: assets/image_02.jpeg
[img-intro-2]: assets/readme/intro-system-overview.webp
[img-status-gif]: assets/image_04.gif
[img-status-1]: assets/readme/status-screen-1.webp
[img-status-2]: assets/readme/status-screen-2.webp
[img-components-1]: assets/image_10.jpeg
[img-components-2]: assets/readme/component-preview.webp

<!-- Purchase links -->

