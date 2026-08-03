#!/bin/sh
set -eu

INSTALL_DIR="${1:-/mnt/UDISK/pet-claw-board-runtime}"
mkdir -p "$INSTALL_DIR"
cp -R . "$INSTALL_DIR"
if [ -d terrier-clips ]; then
  cp -R terrier-clips "$INSTALL_DIR/"
elif [ -d ../terrier-clips ]; then
  cp -R ../terrier-clips "$INSTALL_DIR/"
fi
for BIN in board-server board-touch-input board-rotary-input fb-speech-overlay; do
  if [ -x "$BIN" ]; then
    cp "$BIN" "$INSTALL_DIR/$BIN"
  elif [ -x "build/$BIN" ]; then
    cp "build/$BIN" "$INSTALL_DIR/$BIN"
  fi
done
chmod +x "$INSTALL_DIR/start.sh"
for BIN in board-server board-touch-input board-rotary-input fb-speech-overlay check-runtime.sh fb-display.sh fb-stats-renderer.py board-network-watchdog.sh board-runtime.init board-ap-up.sh board-ap-down.sh board-selfcheck.sh board-wifi-scan.sh board-sta-apply.sh board-audio-bridge.sh board-sound.sh; do
  if [ -f "$INSTALL_DIR/$BIN" ]; then
    chmod +x "$INSTALL_DIR/$BIN"
  fi
done
echo "installed board-runtime to $INSTALL_DIR"
echo "installed; runtime connects to mqtt://broker.openclaw.example:1883 by default."
echo "override with: PET_CLAW_MQTT_URL='mqtt://host:1883' $INSTALL_DIR/start.sh"
