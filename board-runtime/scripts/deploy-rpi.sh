#!/bin/sh
# Deploy board-runtime to Raspberry Pi.
# Usage: sh scripts/deploy-rpi.sh
#   HOST=user@<pi-ip> sh scripts/deploy-rpi.sh
#   REMOTE_DIR=/opt/board-runtime (default)
set -eu

HOST="${HOST:?ERROR: set HOST=user@<pi-ip> before deploying}"
REMOTE_DIR="${REMOTE_DIR:-/opt/board-runtime}"
SRC_DIR="${SRC_DIR:-/opt/board-runtime-src}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
AP_PACKAGES=""
if [ "${INSTALL_AP_TOOLS:-0}" = "1" ]; then
    AP_PACKAGES="hostapd dnsmasq"
fi

shell_quote() {
    printf "%s" "$1" | sed "s/'/'\\\\''/g"
}

remote() {
    if [ -n "${SUDO_PASSWORD:-}" ]; then
        SUDO_PASSWORD_QUOTED=$(shell_quote "$SUDO_PASSWORD")
        {
            printf '%s\n' 'sudo() { printf "%s\n" "$SUDO_PASSWORD" | command sudo -S -p "" "$@"; }'
            printf '%s\n' "$1"
        } | ssh "$HOST" "SUDO_PASSWORD='$SUDO_PASSWORD_QUOTED' sh -s"
    else
        ssh "$HOST" "$1"
    fi
}

wait_for_apt() {
    remote "for i in \$(seq 1 180); do if command -v fuser >/dev/null 2>&1; then if sudo fuser /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/lib/apt/lists/lock /var/cache/apt/archives/lock >/dev/null 2>&1; then sleep 2; else exit 0; fi; elif pgrep -x apt-get >/dev/null 2>&1 || pgrep -x apt >/dev/null 2>&1 || pgrep -x dpkg >/dev/null 2>&1 || pgrep -x unattended-upgr >/dev/null 2>&1; then sleep 2; else exit 0; fi; done; echo 'apt lock still busy after wait' >&2; exit 1"
}

echo "==> Ensuring remote bootstrap tools"
wait_for_apt
remote "sudo apt-get update -qq && sudo apt-get install -y -qq rsync 2>&1 | tail -3"
remote "sudo mkdir -p '$SRC_DIR' && sudo chown \$(id -un):\$(id -gn) '$SRC_DIR'"

echo "==> Syncing source to $HOST:$SRC_DIR"
rsync -avz --delete \
    --exclude='.git' \
    --exclude='.claude' \
    --exclude='.venv' \
    --exclude='.DS_Store' \
    --exclude='._*' \
    --exclude='__pycache__' \
    --exclude='*/__pycache__' \
    --exclude='build*' \
    --exclude='*.o' \
    "$PROJECT_DIR/" "$HOST:$SRC_DIR/"

echo "==> Installing dependencies"
wait_for_apt
remote "sudo apt-get update -qq && sudo apt-get install -y -qq cmake gcc zlib1g-dev iw python3 python3-pip python3-pil python3-numpy ffmpeg flac alsa-utils fonts-wqy-zenhei fonts-noto-color-emoji $AP_PACKAGES 2>&1 | tail -3"
remote "python3 -m pip install --quiet --break-system-packages --resume-retries 5 --retries 5 --timeout 60 SpeechRecognition || true"

echo "==> Building on Pi"
remote "cd '$SRC_DIR' && mkdir -p build && cd build && cmake .. -DBOARD_RUNTIME_BUILD_TESTS=OFF && make -j\$(nproc)"

echo "==> Deploying to $REMOTE_DIR"
remote "sudo mkdir -p '$REMOTE_DIR'"
# Stop the service before overwriting binaries — running ELFs trigger ETXTBSY on cp.
remote "sudo systemctl stop board-runtime 2>/dev/null || true"
remote "for bin in board-server board-touch-input board-rotary-input fb-speech-overlay board-serial-bridge; do sudo cp '$SRC_DIR/build/'\$bin '$REMOTE_DIR/' || exit 1; done"

# Copy shell scripts and support files
remote "for f in board-ap-up.sh board-ap-down.sh board-sta-apply.sh board-network-watchdog.sh board-wifi-scan.sh board-selfcheck.sh board-audio-bridge.sh board-sound.sh fb-device.sh fb-display.sh fb-rawvideo-blit.py fb-stats-renderer.py board-widget-runtime.py board-voice-ptt.py usb-gadget-setup.sh start-rpi.sh board-runtime-rpi.env board-runtime-rpi.service board-widget-runtime.service; do [ -f '$SRC_DIR/'\$f ] && sudo cp '$SRC_DIR/'\$f '$REMOTE_DIR/'; done"
remote "sudo chmod +x '$REMOTE_DIR'/*.sh '$REMOTE_DIR'/board-server '$REMOTE_DIR'/board-touch-input '$REMOTE_DIR'/board-rotary-input '$REMOTE_DIR'/fb-speech-overlay '$REMOTE_DIR'/board-serial-bridge 2>/dev/null || true"

# Copy UI files (pairing portal)
remote "[ -d '$SRC_DIR/ui' ] && sudo cp -r '$SRC_DIR/ui' '$REMOTE_DIR/'"

# Copy video assets if present
remote "[ -d '$SRC_DIR/assets' ] && sudo cp -r '$SRC_DIR/assets' '$REMOTE_DIR/' || true"
remote "if [ -d '$REMOTE_DIR/assets/pets/terrier/generated-videos' ]; then sudo rm -rf '$REMOTE_DIR/terrier-clips'; sudo mkdir -p '$REMOTE_DIR/terrier-clips'; for src in '$REMOTE_DIR'/assets/pets/terrier/generated-videos/*/*.loop.raw.mp4; do [ -f \"\$src\" ] || continue; name=\$(basename \"\$src\" .loop.raw.mp4); sudo ln -sf \"\$src\" '$REMOTE_DIR/terrier-clips/'\"\$name\".mp4; done; fi"

# Copy built-in component packages used by the desktop client and by direct
# board-side validation. Also refresh already-installed builtins so a board that
# had one active keeps the new passive negative-screen behavior immediately.
remote "[ -d '$SRC_DIR/builtin-clawpkgs' ] && sudo rm -rf '$REMOTE_DIR/builtin-clawpkgs' && sudo cp -r '$SRC_DIR/builtin-clawpkgs' '$REMOTE_DIR/' || true"
remote "if [ -d '$SRC_DIR/builtin-clawpkgs' ]; then sudo mkdir -p '$REMOTE_DIR/widgets'; for d in '$SRC_DIR'/builtin-clawpkgs/*; do [ -d \"\$d\" ] || continue; id=\$(basename \"\$d\"); if [ -d '$REMOTE_DIR/widgets/'\"\$id\" ]; then sudo rm -rf '$REMOTE_DIR/widgets/'\"\$id\"; sudo cp -r \"\$d\" '$REMOTE_DIR/widgets/'\"\$id\"; fi; done; fi"

# Copy font asset used by self-check and fb-speech-overlay.
remote "[ -f '$SRC_DIR/unifont-17.0.04.hex.gz' ] && sudo cp '$SRC_DIR/unifont-17.0.04.hex.gz' '$REMOTE_DIR/' && cd '$REMOTE_DIR' && sudo ln -sf unifont-17.0.04.hex.gz unifont.hex.gz || true"

# Fresh Raspberry Pi OS can already be on Wi-Fi via the image customizer, but
# board-runtime treats a missing/empty network-config.json as "pairing required" and
# immediately falls back to its AP, dropping the SSH LAN connection. Preserve
# the current STA association as a minimal runtime config unless the device has
# a valid explicit pairing config.
remote "needs_config=1; if [ -f '$REMOTE_DIR/network-config.json' ]; then python3 -c 'import json, sys; data=json.load(open(sys.argv[1])); sys.exit(0 if data.get(\"ssid\") else 1)' '$REMOTE_DIR/network-config.json' >/dev/null 2>&1 && needs_config=0 || needs_config=1; fi; if [ \"\$needs_config\" = 1 ]; then ssid=\$(iw dev wlan0 link 2>/dev/null | awk -F': ' '/SSID/ {print \$2; exit}'); if [ -z \"\$ssid\" ] && command -v nmcli >/dev/null 2>&1; then ssid=\$(nmcli -t -f active,ssid dev wifi 2>/dev/null | awk -F: '\$1 == \"yes\" {print \$2; exit}'); fi; mqtt=\$(awk -F= '/^MQTT_BROKER_URL=/ {print \$2; exit}' '$REMOTE_DIR/board-runtime-rpi.env' 2>/dev/null); [ -n \"\$mqtt\" ] || mqtt='mqtt://broker.openclaw.example:1883'; ns='desk'; if [ -n \"\$ssid\" ]; then SSID=\"\$ssid\" MQTT_URL_VALUE=\"\$mqtt\" MQTT_NAMESPACE_VALUE=\"\$ns\" python3 -c 'import json, os; print(json.dumps({\"ssid\": os.environ[\"SSID\"], \"mqttUrl\": os.environ[\"MQTT_URL_VALUE\"], \"mqttNamespace\": os.environ[\"MQTT_NAMESPACE_VALUE\"]}, ensure_ascii=False))' | sudo tee '$REMOTE_DIR/network-config.json' >/dev/null; sudo chmod 600 '$REMOTE_DIR/network-config.json'; fi; fi"

# Install systemd services. The main unit is installed as board-runtime.service;
# the widget unit orders itself after that installed name.
remote "sudo cp '$REMOTE_DIR/board-runtime-rpi.service' /etc/systemd/system/board-runtime.service && [ -f '$REMOTE_DIR/board-widget-runtime.service' ] && sudo cp '$REMOTE_DIR/board-widget-runtime.service' /etc/systemd/system/board-widget-runtime.service && sudo systemctl daemon-reload"

# Disable getty on ttyGS0 — it conflicts with board-server's USB serial
remote "sudo systemctl mask serial-getty@ttyGS0.service 2>/dev/null || true"

# Relax the systemd hardware watchdog. Raspberry Pi OS ships
# /usr/lib/systemd/system.conf.d/40-rpi-enable-watchdog.conf with
# RuntimeWatchdogSec=1m, which false-fires on this CPU-saturated Zero 2 W
# (ffmpeg pet animation) and hard-reboots the board mid button_config / widget
# OTA. systemd merges system.conf drop-ins by filename across all dirs, so a
# 99- drop-in sorts after 40- and wins. 10min still recovers a genuine multi-
# minute hang but never trips on normal load/push latency. Clean up earlier
# ineffective drop-ins (10- lost to 40-; the short-lived fully-off 99-disable).
remote "sudo mkdir -p /etc/systemd/system.conf.d; printf '[Manager]\nRuntimeWatchdogSec=10min\n' | sudo tee /etc/systemd/system.conf.d/99-watchdog.conf >/dev/null; sudo rm -f /etc/systemd/system.conf.d/10-watchdog.conf /etc/systemd/system.conf.d/99-disable-watchdog.conf; sudo systemctl daemon-reload"

# Enable services now that binaries/scripts/UI/assets and unit files are in
# place. Start/restart happens after boot overlays are written, so the runtime
# does not briefly launch in the wrong transport mode during first deployment.
remote "sudo systemctl enable board-runtime board-widget-runtime.service >/dev/null"

LCD_OVERLAY='dtoverlay=fbtft,spi0-0,rpi-display,reset_pin=27,dc_pin=22,led_pin=12,speed=32000000,rotate=270,fps=60'
TOUCH_OVERLAY='dtoverlay=ads7846,cs=1,penirq=5,penirq_pull=2,speed=2000000,xohms=150,swapxy=1'

# Configure USB gadget (dwc2 peripheral mode) if not already set
remote "sudo sed -i 's/^otg_mode=1/# disabled for USB gadget: otg_mode=1/' /boot/firmware/config.txt"
remote "sudo sed -i '/^dtoverlay=dwc2/d' /boot/firmware/config.txt; if grep -q '^\[all\]' /boot/firmware/config.txt 2>/dev/null; then sudo sed -i '/^\[all\]/a dtoverlay=dwc2,dr_mode=peripheral' /boot/firmware/config.txt; else printf '\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n' | sudo tee -a /boot/firmware/config.txt >/dev/null; fi"
remote "grep -q 'modules-load=dwc2' /boot/firmware/cmdline.txt 2>/dev/null || sudo sed -i 's/rootwait/rootwait modules-load=dwc2,g_serial/' /boot/firmware/cmdline.txt"
# Enable Raspberry Pi headphone/PWM playback as a fallback, then enable the
# combined I2S VoiceHAT card. It exposes MAX98357A playback and ADAU7002
# capture on one ALSA card, avoiding the /sound conflict from the standalone
# max98357a and adau7002-simple overlays.
remote "if grep -q 'snd_bcm2835.enable_headphones=0' /boot/firmware/cmdline.txt 2>/dev/null; then sudo sed -i 's/snd_bcm2835.enable_headphones=0/snd_bcm2835.enable_headphones=1/' /boot/firmware/cmdline.txt; elif ! grep -q 'snd_bcm2835.enable_headphones=1' /boot/firmware/cmdline.txt 2>/dev/null; then sudo sed -i 's/rootwait/snd_bcm2835.enable_headphones=1 rootwait/' /boot/firmware/cmdline.txt; fi"
remote "sudo sed -i 's/^dtoverlay=max98357a/# combined voicehat disabled separate speaker: dtoverlay=max98357a/' /boot/firmware/config.txt; sudo sed -i 's/^dtoverlay=adau7002-simple/# combined voicehat disabled separate mic: dtoverlay=adau7002-simple/' /boot/firmware/config.txt; grep -q '^dtparam=i2s=on' /boot/firmware/config.txt || echo 'dtparam=i2s=on' | sudo tee -a /boot/firmware/config.txt >/dev/null; grep -q '^dtoverlay=googlevoicehat-soundcard' /boot/firmware/config.txt || printf '\n# Combined I2S speaker + microphone soundcard\ndtoverlay=googlevoicehat-soundcard\n' | sudo tee -a /boot/firmware/config.txt >/dev/null"
# asound.conf with a softvol "Master" stage on playback: the VoiceHAT MAX98357A
# has no hardware mixer, so this software control is the system volume the rotary
# knob drives (board_rotary_input → amixer -D default sset Master). Capture is
# left direct. The Master control is created lazily on first default-PCM open,
# so start-rpi.sh primes it at boot.
remote "printf '%s\n' 'pcm.!default {' '    type asym' '    playback.pcm \"pet_softvol\"' '    capture.pcm  \"plughw:CARD=sndrpigooglevoi,DEV=0\"' '}' 'pcm.pet_softvol {' '    type softvol' '    slave.pcm   \"plughw:CARD=sndrpigooglevoi,DEV=0\"' '    control { name \"Master\"; card sndrpigooglevoi }' '    min_dB -51.0' '    max_dB   0.0' '    resolution 100' '}' 'ctl.!default { type hw; card sndrpigooglevoi }' | sudo tee /etc/asound.conf >/dev/null"

# Configure ILI9341 SPI display + ADS7846/XPT2046 touch overlays.
# Pin map (BCM): CLK=11 MOSI=10 MISO=9 RES=27 DC=22 BLK=12 LCD_CS=8 (CE0)
#                T_CS=7 (CE1) PEN/IRQ=5
# - fbtft 'rpi-display' preset drives ILI9341 320x240, rotate=270 → fb1
# - ads7846 separate overlay so PEN matches GPIO5 (mhs35's bundled ads7846
#   hardcodes PEN=GPIO17 and breaks touch on this panel)
BOOT_OVERLAY_CHANGED=$(remote "
  changed=0
  if grep -q '^dtoverlay=mhs35' /boot/firmware/config.txt 2>/dev/null; then
    sudo sed -i 's/^dtoverlay=mhs35/# legacy display disabled: dtoverlay=mhs35/' /boot/firmware/config.txt
    changed=1
  fi
  before=\$(sha256sum /boot/firmware/config.txt 2>/dev/null | awk '{print \$1}')
  sudo sed -i '/^dtparam=spi=on$/d; /^dtoverlay=fbtft,/d; /^dtoverlay=ads7846,/d; /^dtparam=i2s=on$/d; /^dtoverlay=googlevoicehat-soundcard$/d' /boot/firmware/config.txt
  if grep -q '^\[all\]' /boot/firmware/config.txt 2>/dev/null; then
    sudo sed -i '/^\[all\]/a dtoverlay=googlevoicehat-soundcard' /boot/firmware/config.txt
    sudo sed -i '/^\[all\]/a dtparam=i2s=on' /boot/firmware/config.txt
    sudo sed -i '/^\[all\]/a $TOUCH_OVERLAY' /boot/firmware/config.txt
    sudo sed -i '/^\[all\]/a $LCD_OVERLAY' /boot/firmware/config.txt
    sudo sed -i '/^\[all\]/a dtparam=spi=on' /boot/firmware/config.txt
  else
    printf '\n[all]\ndtparam=spi=on\n$LCD_OVERLAY\n$TOUCH_OVERLAY\ndtparam=i2s=on\ndtoverlay=googlevoicehat-soundcard\n' | sudo tee -a /boot/firmware/config.txt >/dev/null
  fi
  after=\$(sha256sum /boot/firmware/config.txt 2>/dev/null | awk '{print \$1}')
  [ \"\$before\" = \"\$after\" ] || changed=1
  echo \$changed
")

echo "==> Deployment complete."
if [ "${BOOT_OVERLAY_CHANGED:-0}" = "1" ]; then
    echo
    echo "    !! Boot overlays were updated for USB, display/touch, and audio."
    echo "    !! Reboot the Pi to activate them:"
    echo "       ssh $HOST 'sudo reboot'"
    echo
fi
remote "sudo systemctl restart board-runtime board-widget-runtime.service"
echo "    board-runtime has been restarted automatically."
echo "    Logs:    ssh $HOST 'sudo journalctl -u board-runtime -f'"
echo "    Status:  ssh $HOST 'sudo systemctl status board-runtime'"
echo "    Enable:  ssh $HOST 'sudo systemctl enable board-runtime'   # auto-start on boot"
