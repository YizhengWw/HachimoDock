#!/bin/sh
# Package the board-runtime tree + cross-compiled armhf binaries and push them
# to the device over ssh using a tar pipe (rsync is not available on Tina).
set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$HERE"

HOST="${HOST:?ERROR: set HOST=user@<device-ip> before deploying}"
REMOTE_DIR="${REMOTE_DIR:-/mnt/UDISK/board-runtime}"
BUILD_DIR="${BUILD_DIR:-build-armhf}"

echo "[deploy] ensuring remote dir $REMOTE_DIR on $HOST"
ssh -o ConnectTimeout=10 "$HOST" "mkdir -p '$REMOTE_DIR'"

# Build package in a staging dir so we only ship what's needed.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "[deploy] staging files"
mkdir -p "$STAGE"
# Core runtime scripts + configs + UI + assets.
# NOTE: device-config.json and network-config.json are device-specific
# (written during pairing) and must NOT be overwritten by deploy.
cp -R \
  README.md \
  start.sh \
  check-runtime.sh \
  fb-display.sh \
  fb-stats-renderer.py \
  board-runtime.init \
  board-network-watchdog.sh \
  board-ap-up.sh \
  board-ap-down.sh \
  board-sta-apply.sh \
  board-wifi-scan.sh \
  board-selfcheck.sh \
  board-audio-bridge.sh \
  board-sound.sh \
  usb-gadget-setup.sh \
  install.sh \
  manifest.json \
  assets \
  ui \
  "$STAGE/"

if [ -d terrier-clips ]; then
  cp -R terrier-clips "$STAGE/"
elif [ -d ../terrier-clips ]; then
  cp -R ../terrier-clips "$STAGE/"
fi

# Unifont (required by fb-speech-overlay for Chinese text rendering).
for f in unifont-*.hex.gz unifont.hex.gz unifont.hex; do
  [ -f "$f" ] && cp "$f" "$STAGE/"
done

# Cross-compiled armhf binaries.
cp "$BUILD_DIR/board-server" "$STAGE/board-server"
cp "$BUILD_DIR/board-touch-input" "$STAGE/board-touch-input"
cp "$BUILD_DIR/board-rotary-input" "$STAGE/board-rotary-input"
cp "$BUILD_DIR/fb-speech-overlay" "$STAGE/fb-speech-overlay"

chmod +x \
  "$STAGE/start.sh" "$STAGE/check-runtime.sh" "$STAGE/fb-display.sh" \
  "$STAGE/fb-stats-renderer.py" \
  "$STAGE/board-runtime.init" "$STAGE/board-network-watchdog.sh" \
  "$STAGE/board-ap-up.sh" "$STAGE/board-ap-down.sh" "$STAGE/board-sta-apply.sh" \
  "$STAGE/board-wifi-scan.sh" "$STAGE/board-selfcheck.sh" \
  "$STAGE/board-audio-bridge.sh" "$STAGE/board-sound.sh" "$STAGE/install.sh" \
  "$STAGE/board-server" "$STAGE/board-touch-input" "$STAGE/board-rotary-input" "$STAGE/fb-speech-overlay"

echo "[deploy] transferring via tar-over-ssh"
(cd "$STAGE" && tar -cf - .) \
  | ssh -o ConnectTimeout=15 "$HOST" "cd '$REMOTE_DIR' && tar -xf -"

echo "[deploy] disabling stock lv_examples demo"
ssh "$HOST" "if [ -f /etc/init.d/S99swupdate_autorun ]; then sed -i 's|^ */usr/bin/lv_examples|#/usr/bin/lv_examples|' /etc/init.d/S99swupdate_autorun; fi; killall lv_examples 2>/dev/null || true"

echo "[deploy] installing init script"
ssh "$HOST" "cp '$REMOTE_DIR/board-runtime.init' /etc/init.d/board-runtime && chmod +x /etc/init.d/board-runtime && /etc/init.d/board-runtime enable 2>/dev/null || true"

echo "[deploy] done. Verifying remote layout:"
ssh "$HOST" "ls -la '$REMOTE_DIR' | head -40 && echo --- && file '$REMOTE_DIR/board-server' 2>/dev/null || true"
