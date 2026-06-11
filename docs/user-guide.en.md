# User Guide

[Back to README](../README.en.md)

For installation, build, configuration, secondary development, and troubleshooting, refer to the project documentation. This page is for users who already have Hachimiao hardware or have assembled the device following the reproduction guide.

## Preparation

- The device-side runtime is installed and can boot normally.
- Pet Manager is built or installed on the PC.
- A usable Wi-Fi network, or access to the device AP provisioning flow.
- At least one local CLI agent: Codex, Claude Code, OpenClaw, etc.

## Device Binding

Power on the device and wait for first-start mode. An unbound device enters waiting/provisioning state and displays a connection prompt or the default pet screen. Open Pet Manager and start the binding wizard.

The binding wizard has 4 steps:

1. Choose the connection method: wired direct connection or Wi-Fi.
2. Bind the network.
3. Verify communication and confirm that the bridge is online.
4. Confirm the avatar.

Connection methods:

- **Wired direct binding**: connect the device to the computer with a data cable and click detect/bind. Pet Manager scans the USB serial device, reads the board ID, and saves the local binding. No Wi-Fi password is required.
- **Wi-Fi provisioning**: temporarily connect the computer to the device hotspot (default SSID: `claw-pet`, default password: `88888888`). Pet Manager sends Wi-Fi, MQTT, desktop device ID, and namespace configuration through the device AP (`192.168.44.1`). The device then returns to the user LAN, and the computer network is restored after provisioning.

![Device binding](../assets/image_19.png)

## Pet Manager Dashboard

The dashboard brings together device connection status, agent channels, avatars, button configuration, and the voice assistant entry.

- **Connection status**: shows desktop device ID, USB and Wi-Fi online status, and serial rescan controls.
- **Channels and avatars**: Claude Code, Codex, and OpenClaw are fixed channels, and the page only shows agents detected on the current computer. Each channel can save its own avatar; the device follows one agent's live state at a time.
- **More actions**: send test message, copy desktop device ID, return device to home screen, force avatar sync, configure Wi-Fi over USB, and unbind device.

![Pet Manager dashboard](../assets/image_20.png)

## Avatar Gallery and Custom Avatars

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

![Avatar gallery](../assets/image_21.png)

![Custom avatar wizard](../assets/image_22.png)

## Widget Center

The widget center manages the device negative screen. Four widgets are built in, and new widgets can be generated with AI.

| Widget | ID | Description |
| --- | --- | --- |
| Token usage | `token-usage` | Pushes real-time coding-agent token usage to the device and refreshes automatically when the negative screen is open. |
| Slack-off countdown | `slack-off-countdown` | Shows how much time is left before the end of the workday. Tap to switch display; long press to reset the countdown. |
| Pomodoro timer | `tomato-clock` | 25-minute focus + 5-minute break loop. Tap to pause/continue; long press to reset. |
| Water reminder | `drink-reminder` | Uses a default 60-minute interval to remind you to drink water. Tap to confirm; long press to pause or resume. |

Click a widget to preview its description and button mappings. Installation prefers the online device path; if the device is offline or unreachable, Pet Manager prompts you to connect it with a USB data cable before pushing.

![Widget center](../assets/image_23.png)

Creating a widget has 3 steps:

1. **Install skill**: install `petAgent-ui-generator` into the detected coding agent. The app scans `~/.claude/`, `~/.codex/`, `~/.openclaw/`, `~/.gemini/`, and `~/.cursor/`.
2. **Describe and generate**: describe the widget purpose, the default scene after switching to the negative screen, displayed numbers/states, and what tap/long-press should do. The skill invokes the currently followed agent to generate the widget.
3. **Auto refresh / manual import**: after generation, the widget center refreshes automatically and shows the new draft. If it does not appear, drag in the `.clawpkg` directory or zip file, or choose the file manually.

![Widget generation](../assets/image_24.png)
