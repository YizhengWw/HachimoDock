# User Guide

[Back to README](../README.en.md)

For installation, build, configuration, secondary development, and troubleshooting, refer to the project documentation. This page is for users who already have Hachimiao hardware or have assembled the device following the reproduction guide.

## Preparation

- Device-side runtime is installed and can boot normally.
- Pet Manager is built or installed on the PC.
- A usable Wi-Fi network, or access to the device AP provisioning flow.
- At least one local CLI agent: Codex, Claude Code, OpenClaw, etc.

## Device Binding

Power on the device and wait for first-start mode. An unbound device enters waiting/provisioning state and displays a connection prompt or the default pet screen. Open Pet Manager and start the binding wizard.

The binding wizard has 4 steps:

1. Choose the connection method: Ethernet or Wi-Fi.
2. Bind the network.
3. Verify communication and confirm that the bridge is online.
4. Confirm the avatar.

Connection methods:

- **Wired binding**: plug in the cable and click detect/bind. No password is required.
- **Wi-Fi provisioning**: temporarily connect the computer to the device hotspot (default SSID: `claw-pet`). Pet Manager sends Wi-Fi, MQTT, desktop device ID, and namespace configuration through the device AP (`192.168.44.1`). The device then returns to the user LAN, and the computer network is restored after provisioning.

![Device binding](../assets/image_19.png)

## Pet Manager Dashboard

The dashboard brings together device connection status, agent channels, avatars, button configuration, and the voice assistant entry.

- **Connection status**: shows desktop device ID, USB and Wi-Fi online status, and serial rescan controls.
- **Channels and avatars**: Claude Code, Codex, and OpenClaw are separate channels. Each channel can bind a different avatar; the device shows the avatar of the active channel.
- **More actions**: send test message, copy desktop device ID, return device to home screen, force avatar sync, and unbind device.

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
| Token usage | `token-usage` | Pushes real-time coding-agent token usage to the device and converts it into an easy-to-understand lunch-cost equivalent. |
| Slack-off countdown | `slack-off-countdown` | Shows how much time is left before the end of the workday. |
| Pomodoro timer | `tomato-clock` | 25-minute focus + 5-minute break loop. Tap to start/pause; long press to reset. |
| Water reminder | `drink-reminder` | Reminds you to drink water every 45 minutes. Tap to confirm; long press to pause or resume. |

Click a widget to preview its description and button mappings. If the device is offline, install over USB or wait until the device is online.

![Widget center](../assets/image_23.png)

Creating a widget has 3 steps:

1. **Install skill**: install `petAgent-ui-generator` into the detected coding agent. The app scans `~/.claude/`, `~/.codex/`, `~/.openclaw/`, `~/.gemini/`, and `~/.cursor/`.
2. **Describe and generate**: describe the widget purpose, displayed numbers/states, and click/long-press behavior in natural language. The skill invokes the agent to generate the widget.
3. **Auto refresh / manual import**: after generation, the widget center refreshes automatically. You can also drag in a `.clawpkg` directory or zip file manually.

![Widget generation](../assets/image_24.png)
