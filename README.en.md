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

| Glanceable | Voice-first |
| --- | --- |
| Agent state, progress, pending decisions, and results are translated into pet expressions, animations, colors, and short labels. A quick look tells you what the agent is doing. | No need to switch back to a terminal or chat window first. Speak to the device to talk to the agent, issue commands, capture thoughts, or continue work. |

![Desktop companion][img-intro-1]

## 2. Highlights

### Agent State Following

The desktop app keeps the pet display in sync with the agent in real time. Expressions, animations, colors, captions, and subtle prompts communicate current agent state.

![Agent status demo][img-status-gif]

| Status screen | Prompt screen |
| --- | --- |
| ![Status screen 1][img-status-1] | ![Status screen 2][img-status-2] |

### Idle and Touch Interaction

When the agent is idle, the pet plays on its own with multiple idle states. You can also touch the screen to interact with it.

![Idle interaction][img-idle-gif]

### Voice Interaction With Agents

The device microphone can be used to talk to any agent session, reducing the need to type or switch contexts.

### Custom Pet Avatars

Hachimiao includes a built-in Westie avatar with 16 animated states. You can also upload pet photos, profile images, or original characters to generate new avatars, and import avatars from the local Codex pet library or the pet community.

| Avatar generation | Avatar collection |
| --- | --- |
| ![Custom avatar poster][img-custom-avatar-1] | ![Custom avatar dense poster][img-custom-avatar-2] |

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

### 4.1 Hardware BOM

![Hardware overview][img-bom]

#### Option 1: One-click LCSC Order

Use this when ordering the custom board and electronic materials together. Replace the placeholder with the real LCSC order link before release.

#### Option 2: Buy Parts Manually

Use the table below to check and purchase the complete-device materials item by item.

| Category | Module / Part | Position / Interface | Qty / Unit | Qty / 50 Units | Notes |
| --- | --- | --- | ---: | ---: | --- |
| Dev board | [Raspberry Pi Zero 2 WH][buy-pi] | Insert into U1 | 1 | 50 | Pre-soldered 40-pin header version recommended |
| SD card | [microSD card][buy-sd] | TF card slot | 1 | 50 | 32 GB or above, A1/A2 preferred |
| Display | [2.8-inch 240x320 SPI TFT touch screen][buy-screen] | Insert into U2 | 1 | 50 | 11-pin XPT2046 touch version used as reference; 8-pin non-touch version can be an alternative; choose soldered header and ILI9341 version |
| Knob | [Pushable knob module / EC11 encoder module][buy-knob] | Connect to H2 | 1 | 50 | Confirm shaft length, knob height, installation direction, and mounting method with the enclosure design |
| Microphone module | [INMP441 microphone module][buy-mic] | Connect to H1 | 1 | 50 | External module can be used if SMT microphone cost is too high |
| Speaker | [1224 small cavity speaker, 1.25 connector, adhesive backing][buy-speaker] | Connect to CN7 | 1 | 50 | 1224, 8 ohm, 1-2 W, 1.25P |
| Wires | [Dupont wires][buy-wire] | Buttons / knob / microphone | 15 | 750 | Around 20 cm, female-to-female |
| Screws | [Self-tapping screw M2 * 8 mm][buy-screw] | Screen mounting | 4 | 200 | Used to mount the screen |
| Screws | [Self-tapping screw M2 * 5 mm][buy-screw] | Structure mounting | 8 | 400 | Used to mount other structure parts |
| Enclosure | [3D printed / CNC enclosure][buy-shell] | Whole device structure | 1 | 50 | Enclosure, back cover, and internal brackets |
| Adapter cable | [Micro-USB male to Type-C female cable][buy-micro-usb] | Internal board extension | 1 | 50 | 10 cm; MicroUSB male up-angle to Type-C female straight `[mic2-tpc1]` |
| Adapter cable | [Type-C to Type-A cable][buy-typec-a] | Device to computer | 1 | 50 | USB 2.0 is enough; must be 4-wire or above, not charge-only |
| Custom PCB baseboard | See the project PCB BOM | Interconnects modules and reduces Dupont wiring | - | - | Verify baseboard materials with the PCB project |

> Purchase note: links are provided as reproduction references only. Prices, stock, and after-sales service follow the corresponding platform. Verify models and supply before batch production.

### 4.2 Enclosure and Assembly

The MakerWorld model link should be replaced with the real release page before publishing.

![Assembly overview][img-assembly]

![Assembly detail][img-assembly-detail]

### 4.3 PCB Process Information

![PCB front/back][img-pcb-front]

![PCB render][img-pcb-back]

| Item | Value |
| --- | --- |
| Thickness | 1.6 mm |
| Layers | 2-layer board |
| Size | 71 mm * 30.5 mm |
| Soldering | Audio-related parts such as amplifier and microphone may need a hot plate. Other parts can be soldered with an iron. If voice interaction is not required, a bare board with manually soldered headers can be used without affecting the core product flow. |

## 5. User Guide

For installation, build, configuration, secondary development, and troubleshooting, refer to the project documentation. This section is for users who already have Hachimiao hardware or have assembled the device following the reproduction guide.

![Pet Manager overview][img-manager]

### 5.1 Preparation

- Device-side runtime is installed and can boot normally.
- Pet Manager is built or installed on the PC.
- A usable Wi-Fi network, or access to the device AP provisioning flow.
- At least one local CLI agent: Codex, Claude Code, OpenClaw, etc.

### 5.2 Device Binding

Power on the device and wait for first-start mode. An unbound device enters waiting/provisioning state and displays a connection prompt or the default pet screen. Open Pet Manager and start the binding wizard.

The binding wizard has 4 steps:

1. Choose the connection method: Ethernet or Wi-Fi.
2. Bind the network.
3. Verify communication and confirm that the bridge is online.
4. Confirm the avatar.

Connection methods:

- **Wired binding**: plug in the cable and click detect/bind. No password is required.
- **Wi-Fi provisioning**: temporarily connect the computer to the device hotspot (default SSID: `claw-pet`). Pet Manager sends Wi-Fi, MQTT, desktop device ID, and namespace configuration through the device AP (`192.168.44.1`). The device then returns to the user LAN, and the computer network is restored after provisioning.

### 5.3 Pet Manager Dashboard

The dashboard brings together device connection status, agent channels, avatars, button configuration, and the voice assistant entry.

- **Connection status**: shows desktop device ID, USB and Wi-Fi online status, and serial rescan controls.
- **Channels and avatars**: Claude Code, Codex, and OpenClaw are separate channels. Each channel can bind a different avatar; the device shows the avatar of the active channel.
- **More actions**: send test message, copy desktop device ID, return device to home screen, force avatar sync, and unbind device.

### 5.4 Avatar Gallery and Custom Avatars

The avatar gallery lets you browse built-in and custom avatars. Detail pages can preview every state animation and state sound, and custom WAV files can be uploaded.

The add-avatar flow supports:

- Creating a custom avatar.
- Importing from Codex.
- Importing from the community.

The built-in Westie avatar (`builtin://terrier-clips`) includes 16 animated states:

- **Connection / welcome**: `welcome`
- **Working**: `working.thinking`, `working.typing`, `working.browsing`
- **Results**: `waiting_user`, `done`, `error`
- **Touch feedback**: `touch.lick`, `touch.what`
- **Idle**: `idle.playing`, `idle.wandering`, `idle.begging`, `idle.daydreaming`, `idle.eating`, `idle.reading`, `idle.traveling`

Custom avatar wizard:

1. Upload a reference image (PNG / JPEG / WebP / GIF; GIF uses the first frame).
2. Enter avatar name and personality description.
3. Generate the configuration and sync it to the device.

### 5.5 Widget Center

The widget center manages the device negative screen. Four widgets are built in, and new widgets can be generated with AI.

| Widget | ID | Description |
| --- | --- | --- |
| Token usage | `token-usage` | Pushes real-time coding-agent token usage to the device and converts it into an easy-to-understand lunch-cost equivalent. |
| Slack-off countdown | `slack-off-countdown` | Shows how much time is left before the end of the workday. |
| Pomodoro timer | `tomato-clock` | 25-minute focus + 5-minute break loop. Tap to start/pause; long press to reset. |
| Water reminder | `drink-reminder` | Reminds you to drink water every 45 minutes. Tap to confirm; long press to pause or resume. |

Creating a widget has 3 steps:

1. **Install skill**: install `petAgent-ui-generator` into the detected coding agent. The app scans `~/.claude/`, `~/.codex/`, `~/.openclaw/`, `~/.gemini/`, and `~/.cursor/`.
2. **Describe and generate**: describe the widget purpose, displayed numbers/states, and click/long-press behavior in natural language. The skill invokes the agent to generate the widget.
3. **Auto refresh / manual import**: after generation, the widget center refreshes automatically. You can also drag in a `.clawpkg` directory or zip file manually.

## 6. Maintenance

### 6.1 Contributing

Issues, discussions, and pull requests are welcome. Suggested contribution areas:

- Fix Pet Manager or board runtime issues.
- Adapt new screens, main boards, enclosures, or hardware forms.
- Create pet resources, animation assets, and caption styles.
- Develop new `.clawpkg` negative-screen widgets.
- Improve assembly guides, flashing guides, and troubleshooting docs.
- Improve the agent state protocol and third-party agent integrations.

### 6.2 License

This project currently uses `GPL-3.0-only` for software code. Hardware designs, 3D structure files, official pet assets, and third-party resources may need separate notices as the project is organized for release.

- Software code License: `GPL-3.0-only`
- Hardware design License: `[TBD, for example CERN-OHL-S-2.0 / CERN-OHL-P-2.0]`
- 3D structure License: `[TBD, for example CC BY-SA 4.0]`
- Official pet assets License: `[TBD]`
- Third-party notices: `THIRD_PARTY_NOTICES.md`

### 6.3 Security Issues

If you discover a security issue, please do not disclose sensitive details publicly. Contact the maintainers through the security channel once it is available, and we will confirm and handle the issue as soon as possible.

### 6.4 Contact

- Maintainer: `[TBD]`
- Collaborator: `[TBD]`
- GitHub: `[TBD]`
- LCSC / OSHWHub project page: `[TBD]`
- Community / chat group: `[TBD]`
- Security contact: `[TBD]`

### More Information

- Project homepage, community group, model downloads, and resource links should be replaced with real links before release.
- Thanks to the JLC / LCSC ecosystem, OpenClaw, MiMo, Petdex / Codex Pet, related open-source projects, and community contributors.

<!-- Images -->

[img-intro-1]: assets/image_02.jpeg
[img-status-gif]: assets/image_04.gif
[img-status-1]: assets/image_05.png
[img-status-2]: assets/image_06.png
[img-idle-gif]: assets/image_07.gif
[img-custom-avatar-1]: assets/petclaw-custom-crowd-product-poster.png
[img-custom-avatar-2]: assets/petclaw-custom-crowd-product-poster-white-dense.png
[img-components-1]: assets/image_10.jpeg
[img-components-2]: assets/image_11.png
[img-bom]: assets/image_12.png
[img-assembly]: assets/1aa4e315-fe70-44ae-853e-b72996ee1aae.png
[img-assembly-detail]: assets/image_15.jpeg
[img-pcb-front]: assets/image_16.png
[img-pcb-back]: assets/image_17.png
[img-manager]: assets/image_18.png

<!-- Purchase links -->

[buy-pi]: https://item.taobao.com/item.htm?id=693613248231
[buy-sd]: https://detail.tmall.com/item.htm?id=848065818893
[buy-screen]: https://item.taobao.com/item.htm?id=526024381409
[buy-knob]: https://e.tb.cn/h.iForjxnIRX1llEz
[buy-mic]: https://e.tb.cn/h.izOC4n5sjGeIoAm
[buy-speaker]: https://e.tb.cn/h.ixBS9SgI6gFXrIx
[buy-wire]: https://so.szlcsc.com/global.html
[buy-screw]: https://item.taobao.com/item.htm?id=39761471376
[buy-shell]: https://www.jlc-3dp.cn/
[buy-micro-usb]: https://detail.tmall.com/item.htm?id=867489662609
[buy-typec-a]: https://item.taobao.com/item.htm?id=726410843702
