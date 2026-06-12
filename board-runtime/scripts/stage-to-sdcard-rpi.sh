#!/bin/sh
# Stage the complete Pi board-runtime tree (binaries + Python runtime + widget
# system + systemd units + builtin widgets) onto an SD card so a fresh
# Raspberry Pi can bootstrap by:
#   1. flashing Raspberry Pi OS to its main SD card
#   2. inserting this STAGE card via USB reader (or copying via scp)
#   3. running `sh apply-on-board-rpi.sh` from this card on the Pi
#
# Differences vs the archived T113 scripts/legacy-t113/stage-to-sdcard.sh:
#   - Pi uses systemd, not procd (board-runtime.service + board-widget-runtime.service)
#   - Install path is /opt/board-runtime (not /mnt/UDISK/...)
#   - Includes board-widget-runtime.py + builtin .clawpkg widgets (v2 directory layout)
#   - Verifies fonts-noto-color-emoji + python3-pil are installed
#
# Env:
#   SD=<mount point of SD card>      default: /Volumes/Untitled
#   BUILD_DIR=<built binaries>       default: build-host or build (auto-detect)
#   STAGE_NAME=<dir on card>         default: board-runtime
set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$HERE"

SD="${SD:-/Volumes/Untitled}"
STAGE_NAME="${STAGE_NAME:-board-runtime}"

# Auto-detect build dir — prefer build/, fall back to build-host/
BUILD_DIR=""
for cand in build build-host; do
  if [ -x "$cand/board-server" ]; then
    BUILD_DIR="$cand"
    break
  fi
done
BUILD_DIR="${BUILD_DIR_OVERRIDE:-$BUILD_DIR}"

if [ ! -d "$SD" ]; then
  echo "[stage-rpi] SD card mount not found at $SD" >&2
  echo "  set SD=/path/to/mount if your card is elsewhere" >&2
  exit 1
fi
if [ -z "$BUILD_DIR" ] || [ ! -x "$BUILD_DIR/board-server" ]; then
  echo "[stage-rpi] Pi binaries missing — run on the Pi itself:" >&2
  echo "  cd /opt/board-runtime-src && cmake -S . -B build && cmake --build build" >&2
  echo "  then scp build/board-server etc. to a local build/ dir before staging" >&2
  exit 1
fi

DEST="$SD/$STAGE_NAME"
echo "[stage-rpi] sd=$SD  build=$BUILD_DIR  dest=$DEST"

# Rotate any existing stage dir so we never silently overwrite
if [ -d "$DEST" ]; then
  BAK="$DEST.bak-$(date +%Y%m%d%H%M%S)"
  echo "[stage-rpi] rotating existing stage → $BAK"
  mv "$DEST" "$BAK"
fi
mkdir -p "$DEST"

# ── 1. Shell + Python runtime files ──────────────────────────────────────────
echo "[stage-rpi] copying runtime files"
for f in \
  start-rpi.sh \
  fb-display.sh \
  fb-rawvideo-blit.py \
  fb-stats-renderer.py \
  board-widget-runtime.py \
  board-ap-up.sh \
  board-ap-down.sh \
  board-sta-apply.sh \
  board-network-watchdog.sh \
  board-wifi-scan.sh \
  board-selfcheck.sh \
  board-audio-bridge.sh \
  board-sound.sh \
  fb-device.sh \
  usb-gadget-setup.sh \
  board-runtime-rpi.env \
  board-runtime-rpi.service \
  board-widget-runtime.service \
  unifont-17.0.04.hex \
  unifont-17.0.04.hex.gz; do
  if [ -f "$f" ]; then
    cp "$f" "$DEST/"
  else
    echo "  WARN: missing $f (will skip)" >&2
  fi
done

# ── 2. Compiled binaries (built on/for Pi aarch64) ───────────────────────────
echo "[stage-rpi] copying built binaries from $BUILD_DIR/"
for bin in board-server board-touch-input board-rotary-input fb-speech-overlay board-serial-bridge; do
  if [ -x "$BUILD_DIR/$bin" ]; then
    cp "$BUILD_DIR/$bin" "$DEST/"
  else
    echo "  WARN: missing $BUILD_DIR/$bin" >&2
  fi
done

# ── 3. UI bundle (pairing portal) ────────────────────────────────────────────
[ -d ui ] && cp -R ui "$DEST/"

# ── 4. Builtin v2 widgets — ship the 4 reference widgets so a fresh device
#       has them available out of the box. Source priority:
#         1. REPO_BUILTINS  (repo-tracked: ./builtin-clawpkgs/<id>/)
#         2. USER_BUILTINS  (developer override: ~/.openclaw/builtin-clawpkgs/<id>/)
#       Per-widget: repo wins if present, else fall through to user-local.
#       This lets the repo carry the canonical set so any developer can stage
#       a card without first installing the skill locally, while still allowing
#       a single widget to be overridden via the user-local dir for testing.
mkdir -p "$DEST/builtin-clawpkgs"
REPO_BUILTINS="${REPO_BUILTINS:-$HERE/builtin-clawpkgs}"
USER_BUILTINS="${USER_BUILTINS:-$HOME/.openclaw/builtin-clawpkgs}"
for w in drink-reminder tomato-clock slack-off-countdown token-usage; do
  src=""
  if [ -d "$REPO_BUILTINS/$w" ]; then
    src="$REPO_BUILTINS/$w"
  elif [ -d "$USER_BUILTINS/$w" ]; then
    src="$USER_BUILTINS/$w"
  fi
  if [ -n "$src" ]; then
    cp -R "$src" "$DEST/builtin-clawpkgs/"
    echo "  builtin: $w  ← $src"
  else
    echo "  WARN: builtin $w not found in either $REPO_BUILTINS or $USER_BUILTINS" >&2
  fi
done

# ── 5. Permissions ───────────────────────────────────────────────────────────
chmod +x "$DEST"/*.sh "$DEST"/*.py "$DEST"/board-server \
         "$DEST"/board-touch-input "$DEST"/board-rotary-input \
         "$DEST"/fb-speech-overlay "$DEST"/board-serial-bridge 2>/dev/null || true

# ── 6. Apply-on-board script (runs ON the Pi after card insert) ──────────────
APPLY="$SD/apply-on-board-rpi.sh"
cat > "$APPLY" <<'APPLY_SH'
#!/bin/bash
# Bootstrap a fresh Raspberry Pi from this SD card. Idempotent — safe to re-run.
set -euo pipefail

# Locate the source folder (this script + the board-runtime/ dir are siblings
# on the SD card; the card may be mounted at /media/<user>/<label> by Pi OS).
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/board-runtime"
DST="/opt/board-runtime"
BAK="/opt/board-runtime.bak-$(date +%Y%m%d%H%M%S)"

if [ ! -d "$SRC" ]; then
  echo "Could not find $SRC — is this the right card?" >&2; exit 1
fi

echo "[apply-rpi] src=$SRC  dst=$DST"

# ── 1. system deps ───────────────────────────────────────────────────────────
echo "[apply-rpi] checking apt packages"
sudo apt-get update -qq || true
sudo apt-get install -y cmake gcc zlib1g-dev hostapd dnsmasq iw python3 python3-pip python3-pil python3-numpy ffmpeg flac alsa-utils fonts-wqy-zenhei fonts-noto-color-emoji 2>&1 | tail -3 || true
python3 -m pip install --quiet --break-system-packages SpeechRecognition || true

# ── 2. rotate existing install ───────────────────────────────────────────────
if [ -d "$DST" ]; then
  echo "[apply-rpi] backing up current → $BAK"
  sudo systemctl stop board-runtime board-widget-runtime 2>/dev/null || true
  # preserve device-specific files across updates
  for f in device-config.json network-config.json; do
    [ -f "$DST/$f" ] && sudo cp "$DST/$f" "/tmp/.pet-cfg-preserve-$f"
  done
  # preserve pet animation directory (managed separately from deployments)
  for d in .desktop-pet-current .desktop-pet-previous; do
    [ -d "$DST/$d" ] && sudo cp -a "$DST/$d" "/tmp/.pet-preserve-$d"
  done
  # preserve widget runtime state
  for f in "$DST"/.widget-state-*.json "$DST"/.active-widget "$DST"/.widget-display; do
    [ -f "$f" ] && sudo cp "$f" "/tmp/.pet-preserve-$(basename "$f")"
  done
  sudo mv "$DST" "$BAK"
fi

# ── 3. install new tree ──────────────────────────────────────────────────────
sudo mkdir -p "$DST"
sudo cp -R "$SRC"/. "$DST/"
# restore preserved configs
for f in device-config.json network-config.json; do
  [ -f "/tmp/.pet-cfg-preserve-$f" ] && sudo mv "/tmp/.pet-cfg-preserve-$f" "$DST/$f"
done
# restore pet animation directories
for d in .desktop-pet-current .desktop-pet-previous; do
  [ -d "/tmp/.pet-preserve-$d" ] && sudo mv "/tmp/.pet-preserve-$d" "$DST/$d"
done
# restore widget runtime state
for f in /tmp/.pet-preserve-.*; do
  [ -f "$f" ] && sudo mv "$f" "$DST/$(basename "${f#/tmp/.pet-preserve-}")"
done
# rebuild terrier-clips symlink pointing at current pet
if [ -d "$DST/.desktop-pet-current/videos" ]; then
  sudo ln -sf "$DST/.desktop-pet-current/videos" "$DST/terrier-clips"
fi
# create unifont symlink if compressed font was staged
if [ -f "$DST/unifont-17.0.04.hex.gz" ] && [ ! -L "$DST/unifont.hex.gz" ]; then
  sudo ln -sf unifont-17.0.04.hex.gz "$DST/unifont.hex.gz"
fi

# widgets/ root for runtime-installed widgets (separate from builtin-clawpkgs/)
sudo mkdir -p "$DST/widgets"

# ── 4. systemd units ─────────────────────────────────────────────────────────
for unit in board-runtime-rpi.service board-widget-runtime.service; do
  if [ -f "$DST/$unit" ]; then
    # board-runtime-rpi.service installs as board-runtime.service for parity
    target="$unit"
    [ "$unit" = "board-runtime-rpi.service" ] && target="board-runtime.service"
    sudo cp "$DST/$unit" "/etc/systemd/system/$target"
  fi
done
sudo systemctl daemon-reload

# Match the SSH deployment watchdog policy. The directory is absent on some
# fresh Raspberry Pi OS images.
sudo mkdir -p /etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=10min\n' | sudo tee /etc/systemd/system.conf.d/99-watchdog.conf >/dev/null
sudo rm -f /etc/systemd/system.conf.d/10-watchdog.conf /etc/systemd/system.conf.d/99-disable-watchdog.conf
sudo systemctl daemon-reload

# USB gadget serial. Raspberry Pi Imager may emit otg_mode=1, which forces the
# controller into host mode and leaves g_serial without an available UDC.
sudo sed -i 's/^otg_mode=1/# disabled for USB gadget: otg_mode=1/' /boot/firmware/config.txt
sudo sed -i '/^dtoverlay=dwc2/d' /boot/firmware/config.txt
if grep -q '^\[all\]' /boot/firmware/config.txt 2>/dev/null; then
  sudo sed -i '/^\[all\]/a dtoverlay=dwc2,dr_mode=peripheral' /boot/firmware/config.txt
else
  printf '\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n' | sudo tee -a /boot/firmware/config.txt >/dev/null
fi
grep -q 'modules-load=dwc2' /boot/firmware/cmdline.txt 2>/dev/null || sudo sed -i 's/rootwait/rootwait modules-load=dwc2,g_serial/' /boot/firmware/cmdline.txt

# Enable Raspberry Pi headphone/PWM playback as a fallback, then enable the
# combined I2S VoiceHAT card. It exposes MAX98357A playback and ADAU7002
# capture on one ALSA card, avoiding the /sound conflict from the standalone
# max98357a and adau7002-simple overlays.
if grep -q 'snd_bcm2835.enable_headphones=0' /boot/firmware/cmdline.txt 2>/dev/null; then
  sudo sed -i 's/snd_bcm2835.enable_headphones=0/snd_bcm2835.enable_headphones=1/' /boot/firmware/cmdline.txt
elif ! grep -q 'snd_bcm2835.enable_headphones=1' /boot/firmware/cmdline.txt 2>/dev/null; then
  sudo sed -i 's/rootwait/snd_bcm2835.enable_headphones=1 rootwait/' /boot/firmware/cmdline.txt
fi
sudo sed -i 's/^dtoverlay=max98357a/# combined voicehat disabled separate speaker: dtoverlay=max98357a/' /boot/firmware/config.txt
sudo sed -i 's/^dtoverlay=adau7002-simple/# combined voicehat disabled separate mic: dtoverlay=adau7002-simple/' /boot/firmware/config.txt
grep -q '^dtparam=i2s=on' /boot/firmware/config.txt || echo 'dtparam=i2s=on' | sudo tee -a /boot/firmware/config.txt >/dev/null
grep -q '^dtoverlay=googlevoicehat-soundcard' /boot/firmware/config.txt || printf '\n# Combined I2S speaker + microphone soundcard\ndtoverlay=googlevoicehat-soundcard\n' | sudo tee -a /boot/firmware/config.txt >/dev/null

# Keep all active board overlays under [all]; Raspberry Pi Imager often leaves
# model-specific sections above it, and lines appended in the wrong section do
# not apply on Pi Zero.
LCD_OVERLAY='dtoverlay=fbtft,spi0-0,rpi-display,reset_pin=27,dc_pin=22,led_pin=12,speed=32000000,rotate=270,fps=60'
TOUCH_OVERLAY='dtoverlay=ads7846,cs=1,penirq=5,penirq_pull=2,speed=2000000,xohms=150,swapxy=1'
sudo sed -i 's/^dtoverlay=mhs35/# legacy display disabled: dtoverlay=mhs35/' /boot/firmware/config.txt
sudo sed -i '/^dtparam=spi=on$/d; /^dtoverlay=fbtft,/d; /^dtoverlay=ads7846,/d; /^dtparam=i2s=on$/d; /^dtoverlay=googlevoicehat-soundcard$/d' /boot/firmware/config.txt
if grep -q '^\[all\]' /boot/firmware/config.txt 2>/dev/null; then
  sudo sed -i '/^\[all\]/a dtoverlay=googlevoicehat-soundcard' /boot/firmware/config.txt
  sudo sed -i '/^\[all\]/a dtparam=i2s=on' /boot/firmware/config.txt
  sudo sed -i "/^\[all\]/a $TOUCH_OVERLAY" /boot/firmware/config.txt
  sudo sed -i "/^\[all\]/a $LCD_OVERLAY" /boot/firmware/config.txt
  sudo sed -i '/^\[all\]/a dtparam=spi=on' /boot/firmware/config.txt
else
  printf '\n[all]\ndtparam=spi=on\n%s\n%s\ndtparam=i2s=on\ndtoverlay=googlevoicehat-soundcard\n' "$LCD_OVERLAY" "$TOUCH_OVERLAY" | sudo tee -a /boot/firmware/config.txt >/dev/null
fi
printf '%s\n' \
  'pcm.!default {' \
  '    type asym' \
  '    playback.pcm "pet_softvol"' \
  '    capture.pcm  "plughw:CARD=sndrpigooglevoi,DEV=0"' \
  '}' \
  'pcm.pet_softvol {' \
  '    type softvol' \
  '    slave.pcm   "plughw:CARD=sndrpigooglevoi,DEV=0"' \
  '    control { name "Master"; card sndrpigooglevoi }' \
  '    min_dB -51.0' \
  '    max_dB   0.0' \
  '    resolution 100' \
  '}' \
  'ctl.!default { type hw; card sndrpigooglevoi }' | sudo tee /etc/asound.conf >/dev/null

# ── 5. enable services ──────────────────────────────────────────────────────
sudo systemctl enable --now board-runtime board-widget-runtime 2>&1 | tail -3
sleep 2
echo "[apply-rpi] status:"
systemctl is-active board-runtime board-widget-runtime
echo ""
echo "[apply-rpi] ✓ done. screen should come up within ~5s."
echo "             check journal:  sudo journalctl -u board-runtime -n 20"
APPLY_SH
chmod +x "$APPLY"

# ── 7. summary ───────────────────────────────────────────────────────────────
echo ""
echo "[stage-rpi] ✓ staging complete"
echo "  card: $SD"
echo "  apply-on-board script: $APPLY"
echo ""
echo "On the target Pi after inserting the card:"
echo "  cd <card-mount>"
echo "  sudo sh apply-on-board-rpi.sh"
echo ""
echo "Bootstrap will:"
echo "  - apt install python3-pil + fonts-noto-color-emoji"
echo "  - copy $STAGE_NAME/ → /opt/board-runtime/"
echo "  - install + enable board-runtime.service + board-widget-runtime.service"
