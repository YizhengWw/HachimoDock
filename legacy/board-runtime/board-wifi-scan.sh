#!/bin/sh
# Scan nearby Wi-Fi networks via `iw dev <IFACE> scan` and write a JSON file
# that the pairing portal can read to populate its SSID picker.
#
# Designed to be invoked from board-ap-up.sh _before_ the interface is flipped
# into AP mode (scanning is not allowed while acting as an AP).  The output
# file is written atomically.
#
# Usage:  board-wifi-scan.sh [output-path]
set -u

IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"
OUT="${1:-/tmp/board-runtime-ap/wifi-scan.json}"
mkdir -p "$(dirname "$OUT")" 2>/dev/null || true

LOG_TAG="[board-wifi-scan]"
log() { echo "$LOG_TAG $*"; }

if ! command -v iw >/dev/null 2>&1; then
    log "iw not found; writing empty result"
    printf '{"networks":[],"updatedAt":%s,"error":"iw_missing"}\n' "$(date +%s 2>/dev/null || echo 0)" > "$OUT"
    exit 0
fi

# Ensure the interface is up; scanning an admin-down iface fails silently.
ip link set "$IFACE" up 2>/dev/null || ifconfig "$IFACE" up 2>/dev/null || true

TMP="$OUT.tmp.$$"
RAW="$TMP.raw"

# A single scan pass often misses some APs; we keep the latest run but only
# need one here.  If the first pass yields nothing we retry once after a brief
# wait.  Scan itself is synchronous and can take 2-4s on this radio.
# The XR829 driver can occasionally hang on `iw scan`; cap each attempt with a
# timeout so the caller (board-ap-up.sh) is never blocked indefinitely.
SCAN_TIMEOUT=8
do_scan() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "$SCAN_TIMEOUT" iw dev "$IFACE" scan > "$RAW" 2>/dev/null
    else
        iw dev "$IFACE" scan > "$RAW" 2>/dev/null
    fi
}
if ! do_scan; then
    sleep 1
    do_scan || true
fi

# Parse the textual `iw scan` output.  We only emit visible-named APs (hidden
# SSIDs are skipped because the portal cannot meaningfully display them), and
# de-duplicate by SSID keeping the strongest signal seen.
awk '
    function flush(    key) {
        if (cur_ssid != "") {
            key = cur_ssid
            if (!(key in best_sig) || cur_signal > best_sig[key]) {
                best_sig[key] = cur_signal
                best_secure[key] = cur_secure
            }
        }
        cur_ssid = ""
        cur_signal = -200
        cur_secure = "false"
    }

    BEGIN {
        cur_ssid = ""
        cur_signal = -200
        cur_secure = "false"
    }

    /^BSS / { flush() }

    /signal:/ {
        v = $2 + 0
        cur_signal = v
    }

    # iw emits one SSID line per BSS, strictly "    SSID: <name>".  Matching
    # only lines whose first non-whitespace token is exactly "SSID:" avoids
    # false positives from things like "Group cipher: SSID: ..." in the RSN
    # capability block that previously leaked BSSID fragments into the list.
    /^[[:space:]]+SSID:[[:space:]]?/ {
        s = $0
        sub(/^[[:space:]]+SSID:[[:space:]]?/, "", s)
        if (s != "" && s !~ /^\\x00/) {
            gsub(/\\/, "\\\\", s)
            gsub(/"/, "\\\"", s)
            cur_ssid = s
        }
    }

    /^[[:space:]]+RSN:/ { cur_secure = "true" }
    /^[[:space:]]+WPA:/ { cur_secure = "true" }
    /capability:.*Privacy/ { cur_secure = "true" }

    END {
        flush()
        first = 1
        for (k in best_sig) {
            if (!first) { printf "," }
            first = 0
            printf "{\"ssid\":\"%s\",\"signal\":%d,\"secure\":%s}", k, best_sig[k], best_secure[k]
        }
    }
' "$RAW" > "$TMP.items"

TS="$(date +%s 2>/dev/null || echo 0)"
{
    printf '{"updatedAt":%s,"interface":"%s","networks":[' "$TS" "$IFACE"
    cat "$TMP.items"
    printf ']}\n'
} > "$TMP"

mv "$TMP" "$OUT"
rm -f "$TMP.items" "$RAW"

COUNT=$(grep -o '"ssid"' "$OUT" | wc -l)
log "found $COUNT networks on $IFACE"
