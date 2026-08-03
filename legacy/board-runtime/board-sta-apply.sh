#!/bin/sh
# Apply a user-supplied Wi-Fi credential end-to-end:
#   1. Read the new SSID/PSK from a credentials file written atomically by
#      board-server in response to POST /pairing/apply-config.
#   2. Rewrite wpa_supplicant.conf so the STA stack will associate with the
#      requested network instead of whatever firmware default shipped.
#   3. Tear down the SoftAP and bring STA back up (delegates to
#      board-ap-down.sh which also relaunches wpa_supplicant + udhcpc).
#   4. Verify we actually obtained an IPv4 lease within a timeout; publish
#      the outcome to $STATE_DIR/last-attempt.json so the portal can show
#      success / failure.
#   5. On failure, fall back to AP mode (via board-server's /pairing/ap-mode
#      endpoint so pairing state stays consistent) and leave the user another
#      chance to retry.
#
# Expected inputs:
#   $STATE_DIR/sta-apply.creds  -- a two-line KEY=value file with SSID=... and
#                                  PSK=... (written by board-server just
#                                  before spawning this script).
#   BOARD_RUNTIME_*             -- tunables, see defaults below.
set -u

STATE_DIR="${BOARD_RUNTIME_AP_STATE_DIR:-/tmp/board-runtime-ap}"
IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"
WPA_CONF="${BOARD_RUNTIME_WPA_CONF:-/etc/wifi/wpa_supplicant.conf}"
AP_UP="${BOARD_RUNTIME_AP_UP_CMD:-}"
AP_DOWN="${BOARD_RUNTIME_AP_DOWN_CMD:-}"
ADMIN_TOKEN="${BOARD_RUNTIME_ADMIN_TOKEN:-${PET_CLAW_ADMIN_TOKEN:-}}"
HTTP_PORT="${BOARD_RUNTIME_PORT:-80}"
VERIFY_TIMEOUT="${BOARD_RUNTIME_STA_VERIFY_TIMEOUT:-25}"
CREDS="$STATE_DIR/sta-apply.creds"

mkdir -p "$STATE_DIR"
LAST="$STATE_DIR/last-attempt.json"

LOG_TAG="[board-sta-apply]"
log() { echo "$LOG_TAG $*"; }

json_escape() {
    # Minimal JSON string escaping: backslash, double-quote, newline.
    printf '%s' "$1" | awk '
        BEGIN { RS="\n"; ORS="" }
        {
            gsub(/\\/, "\\\\")
            gsub(/"/, "\\\"")
            if (NR > 1) printf "\\n"
            printf "%s", $0
        }'
}

emit() {
    # emit <ok: true|false> <ssid> <error> <ip>
    ok="$1"; ssid="$2"; err="$3"; ip="$4"
    # BusyBox `date` on this firmware does not support %N; approximate atMs as
    # seconds * 1000 so the portal at least sees a monotonic timestamp.
    now_s=$(date +%s 2>/dev/null || echo 0)
    now_ms="${now_s}000"
    {
        printf '{"ok":%s,"ssid":"' "$ok"
        json_escape "$ssid"
        printf '","error":"'
        json_escape "$err"
        printf '","ip":"'
        json_escape "$ip"
        printf '","atMs":%s}\n' "$now_ms"
    } > "$LAST.tmp"
    mv "$LAST.tmp" "$LAST"
}

if [ ! -f "$CREDS" ]; then
    log "no credentials file at $CREDS"
    emit false "" "missing_creds" ""
    exit 1
fi

# `head` isn't guaranteed on this BusyBox build; use awk with NR gate to pull
# the first SSID=/PSK= line in a portable way.  The creds file is produced by
# board-server with a known two-line layout, but we accept either order.
SSID=$(awk 'sub(/^SSID=/,"") && !seen_ssid{print; seen_ssid=1}' "$CREDS")
PSK=$(awk 'sub(/^PSK=/,"") && !seen_psk{print; seen_psk=1}' "$CREDS")
# Creds file is short-lived and contains the plaintext psk; wipe as soon as
# we've read it so nothing else can stumble onto the password on disk.
rm -f "$CREDS"

if [ -z "$SSID" ]; then
    emit false "" "missing_ssid" ""
    exit 1
fi

# Publish "pending" state immediately so the portal sees something other
# than an earlier attempt.
emit false "$SSID" "pending" ""

log "writing wpa_supplicant config for SSID='$SSID'"
TMP="$STATE_DIR/wpa_supplicant.conf.tmp"
# wpa_passphrase produces a pre-hashed PMK block that is safer and more
# forgiving of shell-special SSID/PSK characters.  Fall back to a manually
# constructed block if the binary is not installed.
NETWORK_BLOCK=""
if command -v wpa_passphrase >/dev/null 2>&1 && [ -n "$PSK" ]; then
    NETWORK_BLOCK="$(wpa_passphrase "$SSID" "$PSK" 2>/dev/null | sed -n '/^network={/,/^}/p')"
fi
if [ -z "$NETWORK_BLOCK" ]; then
    # Escape backslashes and double quotes in plaintext values.
    esc_conf() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
    if [ -n "$PSK" ]; then
        NETWORK_BLOCK=$(printf 'network={\n\tssid="%s"\n\tpsk="%s"\n}\n' \
            "$(esc_conf "$SSID")" "$(esc_conf "$PSK")")
    else
        NETWORK_BLOCK=$(printf 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}\n' \
            "$(esc_conf "$SSID")")
    fi
fi

cat > "$TMP" <<EOF
ctrl_interface=/var/run/wpa_supplicant
update_config=1

$NETWORK_BLOCK
EOF

# One-time backup so a user can dig out their old Wi-Fi if something goes
# catastrophically wrong with the portal flow.
if [ -f "$WPA_CONF" ] && [ ! -f "$WPA_CONF.bak" ]; then
    cp "$WPA_CONF" "$WPA_CONF.bak" 2>/dev/null || true
fi
mv "$TMP" "$WPA_CONF"
sync

log "tearing down AP and starting STA"
if [ -n "$AP_DOWN" ]; then
    sh -c "$AP_DOWN" >"$STATE_DIR/ap-down-apply.log" 2>&1 || true
else
    log "no AP_DOWN command configured"
fi

# board-ap-down.sh already restarts wpa_supplicant + udhcpc.  Poll wlan0 for
# a real IPv4 (anything other than a 169.254 link-local auto-address).
T=0
IP=""
while [ "$T" -lt "$VERIFY_TIMEOUT" ]; do
    CURRENT=$(ip -4 addr show "$IFACE" 2>/dev/null | awk '/inet /{print $2; exit}' | cut -d/ -f1)
    case "$CURRENT" in
        ""|169.254.*) : ;;
        *) IP="$CURRENT"; break ;;
    esac
    sleep 1
    T=$((T + 1))
done

if [ -n "$IP" ]; then
    log "STA associated, ip=$IP"
    emit true "$SSID" "" "$IP"
    exit 0
fi

# Diagnose the failure so the portal can tell the user whether they typed
# the wrong password, picked a missing SSID, or ran into DHCP trouble.
STATE=""
if command -v wpa_cli >/dev/null 2>&1; then
    STATE=$(wpa_cli -i "$IFACE" status 2>/dev/null | awk -F= '/^wpa_state=/{print $2}')
fi
case "$STATE" in
    ""|INACTIVE|SCANNING|DISCONNECTED) ERR="ssid_not_found" ;;
    4WAY_HANDSHAKE|ASSOCIATING|ASSOCIATED) ERR="wrong_password_or_assoc" ;;
    COMPLETED) ERR="no_dhcp_lease" ;;
    *) ERR="sta_timeout_$STATE" ;;
esac
log "STA verification failed (wpa_state=$STATE, err=$ERR); restoring AP"
emit false "$SSID" "$ERR" ""

# Ask board-server to re-enter AP mode so its pairing state stays in sync.
# Fall back to raw AP_UP if the HTTP route is unavailable (server crashed,
# or this script is being run standalone).
set -- -s --max-time 3 -X POST -H "Content-Type: application/json"
if [ -n "$ADMIN_TOKEN" ]; then
    set -- "$@" -H "X-Board-Token: $ADMIN_TOKEN"
fi
set -- "$@" -d '{"on":true}' "http://127.0.0.1:${HTTP_PORT}/pairing/ap-mode"
if ! curl "$@" >/dev/null 2>&1; then
    if [ -n "$AP_UP" ]; then
        sh -c "$AP_UP" >"$STATE_DIR/ap-up-fallback.log" 2>&1 || log "AP re-entry failed"
    fi
fi
exit 0
