#!/bin/sh
# Reset a board to AP (pairing) mode via serial.
# Usage: sh scripts/reset-to-ap.sh

DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
exec python3 "$DIR/scripts/serial-exec.py" "rm -f /mnt/UDISK/board-runtime/network-config.json && /etc/init.d/board-runtime restart"
