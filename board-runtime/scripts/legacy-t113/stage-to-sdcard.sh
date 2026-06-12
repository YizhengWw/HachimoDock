#!/bin/sh
# Package the board-runtime tree + cross-compiled armhf binaries onto a
# removable card (e.g. the SD card pulled off the device).
# After the card is plugged back into the board, running `apply-on-board.sh`
# from the card will copy everything into /mnt/UDISK/board-runtime.
set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$HERE"

SD="${SD:-/Volumes/Lenovo}"
BUILD_DIR="${BUILD_DIR:-build-armhf}"
STAGE_NAME="${STAGE_NAME:-board-runtime}"

if [ ! -d "$SD" ]; then
  echo "sdcard mount not found at $SD" >&2
  exit 1
fi

if [ ! -x "$BUILD_DIR/board-server" ]; then
  echo "armhf binaries missing, run scripts/build-armhf.sh first" >&2
  exit 1
fi

DEST="$SD/$STAGE_NAME"

echo "[stage] backing up existing $DEST -> $DEST.bak-$(date +%Y%m%d%H%M%S) (if any)"
if [ -d "$DEST" ]; then
  mv "$DEST" "$DEST.bak-$(date +%Y%m%d%H%M%S)"
fi

echo "[stage] staging to $DEST"
mkdir -p "$DEST"

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
  board-factory-reset.sh \
  board-audio-bridge.sh \
  board-sound.sh \
  install.sh \
  manifest.json \
  assets \
  ui \
  "$DEST/"

if [ -d terrier-clips ]; then
  cp -R terrier-clips "$DEST/"
elif [ -d ../terrier-clips ]; then
  cp -R ../terrier-clips "$DEST/"
fi

# Unifont (required by fb-speech-overlay for Chinese text rendering).
for f in unifont-*.hex.gz unifont.hex.gz unifont.hex; do
  [ -f "$f" ] && cp "$f" "$DEST/"
done

cp "$BUILD_DIR/board-server" "$DEST/board-server"
cp "$BUILD_DIR/board-touch-input" "$DEST/board-touch-input"
cp "$BUILD_DIR/board-rotary-input" "$DEST/board-rotary-input"
cp "$BUILD_DIR/fb-speech-overlay" "$DEST/fb-speech-overlay"

chmod +x \
  "$DEST/start.sh" "$DEST/check-runtime.sh" "$DEST/fb-display.sh" \
  "$DEST/fb-stats-renderer.py" \
  "$DEST/board-runtime.init" "$DEST/board-network-watchdog.sh" \
  "$DEST/board-ap-up.sh" "$DEST/board-ap-down.sh" "$DEST/board-sta-apply.sh" \
  "$DEST/board-wifi-scan.sh" "$DEST/board-selfcheck.sh" \
  "$DEST/board-factory-reset.sh" "$DEST/board-audio-bridge.sh" \
  "$DEST/install.sh" \
  "$DEST/board-server" "$DEST/board-touch-input" "$DEST/board-rotary-input" "$DEST/fb-speech-overlay"

cat > "$SD/apply-on-board.sh" <<'APPLY'
#!/bin/sh
# Run this script on the board AFTER inserting the SD card.
# It copies the new runtime from the SD card into /mnt/UDISK/board-runtime
# and restarts services.
set -eu

find_sdcard() {
  for p in /mnt/extsd /mnt/sdcard /mnt/sd /mnt/mmcblk0p1 /mnt/sda1 /mnt/mmcblk1p1 /mnt/SDCARD; do
    if [ -d "$p/board-runtime" ] && [ -f "$p/board-runtime/start.sh" ]; then
      echo "$p"
      return 0
    fi
  done
  mount | awk '/mmcblk|sd[a-z]/ {print $3}' | while read -r m; do
    [ -d "$m/board-runtime" ] && [ -f "$m/board-runtime/start.sh" ] && echo "$m" && break
  done
}

SRC_ROOT=$(find_sdcard)
if [ -z "$SRC_ROOT" ]; then
  echo "Could not find board-runtime/ on any mounted sd/mmc volume" >&2
  echo "Mounted volumes:" >&2
  mount | grep -E "mmc|sd[a-z]" >&2 || true
  exit 1
fi

SRC="$SRC_ROOT/board-runtime"
DST=/mnt/UDISK/board-runtime
BAK=/mnt/UDISK/board-runtime.bak-$(date +%Y%m%d%H%M%S)

echo "[apply] source: $SRC"
echo "[apply] dest:   $DST"

echo "[apply] stopping services"
/etc/init.d/board-runtime stop 2>/dev/null || true
sleep 1
killall fb-display.sh board-server board-touch-input board-rotary-input fb-speech-overlay tplayerdemo 2>/dev/null || true
sleep 1

if [ -d "$DST" ]; then
  echo "[apply] backing up current -> $BAK"
  # Preserve device-specific config files across updates
  for _cfg in device-config.json network-config.json; do
    if [ -f "$DST/$_cfg" ]; then
      cp "$DST/$_cfg" "/tmp/_board_cfg_$_cfg"
      echo "[apply] preserved $_cfg"
    fi
  done
  mv "$DST" "$BAK"
fi

mkdir -p "$DST"
echo "[apply] copying (this may take a bit on ubifs)"
cp -R "$SRC"/. "$DST"/

# Restore preserved config files
for _cfg in device-config.json network-config.json; do
  if [ -f "/tmp/_board_cfg_$_cfg" ]; then
    cp "/tmp/_board_cfg_$_cfg" "$DST/$_cfg"
    rm -f "/tmp/_board_cfg_$_cfg"
    echo "[apply] restored $_cfg"
  fi
done
chmod +x "$DST/start.sh" "$DST/check-runtime.sh" "$DST/fb-display.sh" \
         "$DST/fb-stats-renderer.py" \
         "$DST/board-runtime.init" "$DST/board-network-watchdog.sh" \
         "$DST/board-ap-up.sh" "$DST/board-ap-down.sh" "$DST/board-sta-apply.sh" \
         "$DST/board-wifi-scan.sh" "$DST/board-selfcheck.sh" \
         "$DST/board-audio-bridge.sh" "$DST/board-sound.sh" "$DST/install.sh" \
         "$DST/board-server" "$DST/board-touch-input" "$DST/board-rotary-input" "$DST/fb-speech-overlay" 2>/dev/null || true

echo "[apply] running check-runtime"
"$DST/check-runtime.sh" || true

echo "[apply] disabling stock lv_examples demo"
if [ -f /etc/init.d/S99swupdate_autorun ]; then
    sed -i 's|^ */usr/bin/lv_examples|#/usr/bin/lv_examples|' /etc/init.d/S99swupdate_autorun
fi
killall lv_examples 2>/dev/null || true

echo "[apply] installing init script"
cp "$DST/board-runtime.init" /etc/init.d/board-runtime
chmod +x /etc/init.d/board-runtime

# The init script defaults to BOARD_DIR=/mnt/UDISK/board-runtime so no env
# override is strictly needed.

/etc/init.d/board-runtime enable 2>/dev/null || true
echo "[apply] starting services"
/etc/init.d/board-runtime start

sleep 2
echo "[apply] running processes:"
ps w | grep -E "board|fb-" | grep -v grep || true
echo "[apply] listening ports:"
cat /proc/net/tcp | awk 'NR==1 || $4=="0A"' | sed -n '1,10p'

echo "[apply] done"
APPLY
chmod +x "$SD/apply-on-board.sh"

echo "[stage] computing size"
du -sh "$DEST" "$SD/apply-on-board.sh"

echo "[stage] final listing:"
ls -la "$SD"
echo "[stage] done. Eject the card, insert it back into the board, then run:"
echo "         sh /<sdcard-mount>/apply-on-board.sh"
echo "       (apply-on-board.sh will auto-detect the sd mount path.)"
