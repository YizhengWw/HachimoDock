#!/bin/sh
# Start board-runtime on Raspberry Pi.
# [Input] Runtime env, USB gadget state, WiFi/MQTT config, and board process binaries.
# [Output] Starts board-server/display/input workers with a verified USB or MQTT transport.
# Transport auto-detection waits for late USB gadget enumeration, but only treats
# USB as usable after the UDC reaches configured; stale ttyGS0 nodes fall back to MQTT.
set -u

BOARD_DIR="${BOARD_DIR:-/opt/board-runtime}"
MQTT_BROKER_URL="${MQTT_BROKER_URL:-mqtt://broker.openclaw.example:1883}"
BOARD_RUNTIME_WLAN_IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"
BOARD_USB_ENUM_WAIT_SECONDS="${BOARD_USB_ENUM_WAIT_SECONDS:-30}"

LOG_TAG="[start-rpi]"
log() { echo "$LOG_TAG $*"; }

if [ -f "$BOARD_DIR/fb-device.sh" ]; then
    . "$BOARD_DIR/fb-device.sh"
fi

# Derive device ID from wlan0 MAC
if [ -z "${PET_DEVICE_ID:-}" ]; then
    WLAN_MAC=$(cat "/sys/class/net/${BOARD_RUNTIME_WLAN_IFACE}/address" 2>/dev/null | tr -d ':' | tr 'A-F' 'a-f')
    if [ -n "$WLAN_MAC" ]; then
        PET_DEVICE_ID="board-${WLAN_MAC}"
    else
        PET_DEVICE_ID="board-unknown"
    fi
fi
export PET_DEVICE_ID

# USB gadget setup
if [ -f "$BOARD_DIR/usb-gadget-setup.sh" ]; then
    log "setting up USB gadget"
    sh "$BOARD_DIR/usb-gadget-setup.sh" 2>&1 || true
fi

# Transport auto-detection
BOARD_TRANSPORT="mqtt"
if [ -z "${BOARD_TRANSPORT_FORCE:-}" ]; then
    UDC_STATE=""
    case "$BOARD_USB_ENUM_WAIT_SECONDS" in
        ''|*[!0-9]*) BOARD_USB_ENUM_WAIT_SECONDS=30 ;;
    esac
    # USB enumeration can lag well behind gadget setup on Pi Zero after a
    # reboot. Wait long enough for the UDC to become configured, but do not
    # treat a stale /dev/ttyGS0 character device as a working data path.
    USB_WAIT_REMAINING="$BOARD_USB_ENUM_WAIT_SECONDS"
    while [ "$USB_WAIT_REMAINING" -gt 0 ]; do
        for udc_dir in /sys/class/udc/*; do
            [ -f "$udc_dir/state" ] && UDC_STATE=$(cat "$udc_dir/state" 2>/dev/null) && break
        done
        [ "$UDC_STATE" = "configured" ] && break
        sleep 1
        USB_WAIT_REMAINING=$((USB_WAIT_REMAINING - 1))
    done
    if [ "$UDC_STATE" = "configured" ] && [ -c /dev/ttyGS0 ]; then
        BOARD_TRANSPORT="usb"
    else
        if [ -c /dev/ttyGS0 ]; then
            log "USB serial device exists but host is not configured after ${BOARD_USB_ENUM_WAIT_SECONDS}s (state=${UDC_STATE:-unknown}); using MQTT"
        else
            log "USB gadget not configured after ${BOARD_USB_ENUM_WAIT_SECONDS}s (state=${UDC_STATE:-unknown}); using MQTT"
        fi
    fi
else
    BOARD_TRANSPORT="$BOARD_TRANSPORT_FORCE"
    if [ "$BOARD_TRANSPORT" = "usb" ]; then
        UDC_STATE=""
        case "$BOARD_USB_ENUM_WAIT_SECONDS" in
            ''|*[!0-9]*) BOARD_USB_ENUM_WAIT_SECONDS=30 ;;
        esac
        USB_WAIT_REMAINING="$BOARD_USB_ENUM_WAIT_SECONDS"
        while [ "$USB_WAIT_REMAINING" -gt 0 ] && [ ! -c /dev/ttyGS0 ]; do
            for udc_dir in /sys/class/udc/*; do
                [ -f "$udc_dir/state" ] && UDC_STATE=$(cat "$udc_dir/state" 2>/dev/null) && break
            done
            sleep 1
            USB_WAIT_REMAINING=$((USB_WAIT_REMAINING - 1))
        done
        if [ ! -c /dev/ttyGS0 ]; then
            log "USB transport forced but /dev/ttyGS0 is not ready after ${BOARD_USB_ENUM_WAIT_SECONDS}s (state=${UDC_STATE:-unknown})"
        fi
    fi
fi
export BOARD_TRANSPORT
log "transport mode: $BOARD_TRANSPORT"

# Resolve framebuffer before any self-check or display process so the chosen
# PET_CLAW_FB_DEV is inherited consistently by board-server, fb-display, and
# overlays. PET_CLAW_FB_DEV=auto means "prefer the SPI LCD by sysfs identity".
FB_REQUEST="${PET_CLAW_FB_DEV:-auto}"
if command -v fb_resolve_device >/dev/null 2>&1; then
    FB_DEV="$(fb_resolve_device "$FB_REQUEST" 2>/dev/null || true)"
else
    FB_DEV="$FB_REQUEST"
    case "$FB_DEV" in ""|auto|AUTO) FB_DEV=/dev/fb0;; esac
    if [ ! -c "$FB_DEV" ] && [ -c /dev/fb0 ]; then
        log "framebuffer device $FB_DEV is not a character device; falling back to /dev/fb0"
        FB_DEV=/dev/fb0
    fi
fi
FB_AVAILABLE=0
if [ -n "$FB_DEV" ] && [ -c "$FB_DEV" ]; then
    FB_AVAILABLE=1
    FB_NUM="${FB_DEV##*/fb}"
    FB_NAME=$(cat "/sys/class/graphics/fb${FB_NUM}/name" 2>/dev/null || echo "?")
    FB_SIZE=$(cat "/sys/class/graphics/fb${FB_NUM}/virtual_size" 2>/dev/null || echo "?")
    FB_BPP=$(cat "/sys/class/graphics/fb${FB_NUM}/bits_per_pixel" 2>/dev/null || echo 0)
    log "framebuffer selected: $FB_DEV name=$FB_NAME size=$FB_SIZE bpp=$FB_BPP request=$FB_REQUEST"
else
    FB_DEV=""
    FB_NAME="none"
    FB_SIZE="?"
    FB_BPP=0
    log "no framebuffer character device available; display disabled (request=$FB_REQUEST)"
fi
export PET_CLAW_FB_DEV="$FB_DEV"

# Keep the MAX98357A speaker amp enabled across cue playback. Letting the
# overlay toggle SD_MODE for every short WAV creates an audible startup pop.
SPEAKER_AMP_GPIO="${PET_CLAW_SPEAKER_AMP_GPIO:-${PET_CLAW_AUDIO_AMP_GPIO:-}}"
if [ -n "$SPEAKER_AMP_GPIO" ]; then
    if command -v pinctrl >/dev/null 2>&1; then
        pinctrl set "$SPEAKER_AMP_GPIO" op dh 2>/dev/null || true
    elif command -v raspi-gpio >/dev/null 2>&1; then
        raspi-gpio set "$SPEAKER_AMP_GPIO" op dh 2>/dev/null || true
    fi
fi

# NOTE: Do NOT kill wpa_supplicant on Pi — the system manages WiFi via
# NetworkManager/wpa_supplicant. Killing it drops the network connection.
# (The kill was originally for T113 TinaLinux where the runtime manages WiFi.)

# Self-check
if [ -x "$BOARD_DIR/board-selfcheck.sh" ]; then
    BOARD_DIR="$BOARD_DIR" BOARD_RUNTIME_WLAN_IFACE="$BOARD_RUNTIME_WLAN_IFACE" \
        sh "$BOARD_DIR/board-selfcheck.sh" "$BOARD_DIR" 2>&1 || true
fi

# Common env vars for board-server
export MQTT_URL="$MQTT_BROKER_URL"
export PET_CLAW_MQTT_URL="$MQTT_BROKER_URL"
export PET_SCREEN_NAME="OpenClaw Board Runtime"
export PET_CLAW_DEVICE_ID="$PET_DEVICE_ID"
export PET_CLAW_MQTT_NAMESPACE=desk
export BOARD_RUNTIME_HOST=0.0.0.0

# Export all BOARD_RUNTIME_* vars
export BOARD_DIR MQTT_BROKER_URL

PIDS=""
cleanup() {
    log "shutting down..."
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null || true
    done
    wait
    log "all processes stopped"
}
trap cleanup INT TERM

# Start board-server
log "starting board-server"
"$BOARD_DIR/board-server" "$BOARD_DIR" &
PIDS="$PIDS $!"

# Pre-create the ALSA softvol "Master" volume control. softvol controls are
# instantiated lazily on first open of the default PCM, so without this the
# rotary volume knob's `amixer sset Master` would no-op until the first cue
# plays. A short silent raw playthrough opens the chain; then restore the
# persisted level (falling back to 60%). Backgrounded + silent — no audio.
if command -v amixer >/dev/null 2>&1 && command -v aplay >/dev/null 2>&1; then
    (
        dd if=/dev/zero bs=16000 count=1 2>/dev/null | aplay -q -t raw -f S16_LE -r 48000 -c 2 -D default >/dev/null 2>&1 || true
        alsactl restore >/dev/null 2>&1 || amixer -D default sset Master 60% >/dev/null 2>&1 || true
    ) &
fi

# board-serial-bridge is NOT needed: board-server already integrates USB
# serial transport via runtime_usb_serial.c. Running both would cause
# /dev/ttyGS0 conflicts.

# Start fb-display
rm -f "$BOARD_DIR/.fb-display.lock"
# Preserve the selected UI page across runtime restarts; initialize only when
# the file is missing or has an invalid value.
SCREEN_PAGE="$(cat "$BOARD_DIR/.screen-page" 2>/dev/null || true)"
case "$SCREEN_PAGE" in
    main|stats) ;;
    *) echo main > "$BOARD_DIR/.screen-page" 2>/dev/null || true ;;
esac
rm -f "$BOARD_DIR/.screen-interrupt" "$BOARD_DIR/.touch-request" "$BOARD_DIR/.voice-button-state" 2>/dev/null || true
if [ "$FB_AVAILABLE" = "1" ] && [ -f "$BOARD_DIR/fb-display.sh" ]; then
    log "starting fb-display"
    sh "$BOARD_DIR/fb-display.sh" &
    PIDS="$PIDS $!"
else
    log "skipping fb-display (no framebuffer)"
fi

# Start fb-speech-overlay (only if framebuffer exists and is 32bpp)
if [ -x "$BOARD_DIR/fb-speech-overlay" ] && [ -c "$FB_DEV" ] && [ "$FB_BPP" = "32" ]; then
    log "starting fb-speech-overlay"
    PET_CLAW_FB_SPEECH_OVERLAY=1 PET_CLAW_FB_SPEECH_HOLD_SECONDS=10 PET_CLAW_FB_SPEECH_SCALE=2.0 \
        "$BOARD_DIR/fb-speech-overlay" "$BOARD_DIR" &
    PIDS="$PIDS $!"
else
    log "skipping fb-speech-overlay ($FB_DEV bpp=$FB_BPP, need 32)"
fi

# Start board-touch-input
if [ -x "$BOARD_DIR/board-touch-input" ]; then
    log "starting board-touch-input"
    "$BOARD_DIR/board-touch-input" "$BOARD_DIR" &
    PIDS="$PIDS $!"
fi

# Start GPIO rotary/button input. This is independent from the touch device:
# top button and encoder rotation switch .screen-page; encoder long-press resets pairing.
if [ "${PET_ROTARY_INPUT_ENABLED:-1}" != "0" ] && [ -x "$BOARD_DIR/board-rotary-input" ]; then
    log "starting board-rotary-input"
    "$BOARD_DIR/board-rotary-input" "$BOARD_DIR" &
    PIDS="$PIDS $!"
fi

# Start push-to-talk voice bridge (GPIO17 hold-to-talk -> STT -> /input/action).
if [ "${PET_VOICE_PTT_ENABLED:-1}" != "0" ] && [ -f "$BOARD_DIR/board-voice-ptt.py" ]; then
    log "starting board-voice-ptt"
    /usr/bin/python3 "$BOARD_DIR/board-voice-ptt.py" "$BOARD_DIR" &
    PIDS="$PIDS $!"
fi

# Start network watchdog (only in MQTT mode on TinaLinux where the runtime
# manages WiFi. On Pi/systemd the watchdog kills wpa_supplicant which drops
# the system-managed WiFi connection.)
if [ "$BOARD_TRANSPORT" != "usb" ] && [ -f "$BOARD_DIR/board-network-watchdog.sh" ] && ! command -v nmcli >/dev/null 2>&1; then
    log "starting network watchdog"
    sh "$BOARD_DIR/board-network-watchdog.sh" once >/dev/null 2>&1 &
    sh "$BOARD_DIR/board-network-watchdog.sh" watch &
    PIDS="$PIDS $!"
else
    log "skipping network watchdog (system-managed WiFi or USB mode)"
fi

log "all processes started, waiting..."
wait
