#!/bin/sh
# Factory-reset the board: wipe network credentials and return to AP pairing mode.
# Safe to call at any time — idempotent.
#
# Usage:
#   sh board-factory-reset.sh           # uses default BOARD_DIR
#   BOARD_DIR=/path sh board-factory-reset.sh
set -u

BOARD_DIR="${BOARD_DIR:-/mnt/UDISK/board-runtime}"
LOG_TAG="[factory-reset]"
log() { echo "$LOG_TAG $*"; }

# 1. Delete network credentials so board-server enters pairing on next start.
for f in network-config.json device-config.json; do
    if [ -f "$BOARD_DIR/$f" ]; then
        rm -f "$BOARD_DIR/$f"
        log "deleted $f"
    fi
done

# 2. Disable system wpa_supplicant service permanently.
#    board-runtime owns wlan0 lifecycle; the system service would steal the
#    interface by auto-associating with a cached SSID.
if [ -f /etc/init.d/wpa_supplicant ]; then
    /etc/init.d/wpa_supplicant stop 2>/dev/null || true
    /etc/init.d/wpa_supplicant disable 2>/dev/null || true
    mv /etc/init.d/wpa_supplicant /etc/init.d/wpa_supplicant.disabled-by-board-runtime
    log "disabled system wpa_supplicant service"
fi
killall wpa_supplicant udhcpc 2>/dev/null || true

# 3. Restart board-runtime — it will detect no network-config and enter AP mode.
if [ -x /etc/init.d/board-runtime ]; then
    /etc/init.d/board-runtime stop 2>/dev/null || true
    sleep 1
    /etc/init.d/board-runtime start
    log "board-runtime restarted"
else
    # Fallback: manually start AP
    if [ -x "$BOARD_DIR/board-ap-up.sh" ]; then
        sh "$BOARD_DIR/board-ap-up.sh"
        log "AP started manually"
    fi
fi

log "done — device is in AP pairing mode"
