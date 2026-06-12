#!/bin/sh
# Bring wlan0 up as a SoftAP exposing 192.168.44.1 so a phone or PC can
# connect directly and POST /pairing/apply-config while the board waits for
# a network configuration.  Safe to invoke when already AP-mode (idempotent).
set -u

IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"
AP_IP="${BOARD_RUNTIME_AP_IP:-192.168.44.1}"
AP_NETMASK="${BOARD_RUNTIME_AP_NETMASK:-255.255.255.0}"
AP_CHANNEL="${BOARD_RUNTIME_AP_CHANNEL:-6}"
AP_COUNTRY="${BOARD_RUNTIME_AP_COUNTRY:-CN}"
AP_SSID="${BOARD_RUNTIME_AP_SSID:-claw-pet}"
AP_PSK="${BOARD_RUNTIME_AP_PSK:-88888888}"
DHCP_START="${BOARD_RUNTIME_AP_DHCP_START:-192.168.44.50}"
DHCP_END="${BOARD_RUNTIME_AP_DHCP_END:-192.168.44.150}"
STATE_DIR="${BOARD_RUNTIME_AP_STATE_DIR:-/tmp/board-runtime-ap}"

LOG_TAG="[board-ap-up]"
log() { echo "$LOG_TAG $*"; }

restore_networkmanager() {
    if command -v nmcli >/dev/null 2>&1; then
        nmcli dev set "$IFACE" managed yes >/dev/null 2>&1 || true
        nmcli dev connect "$IFACE" >/dev/null 2>&1 || true
    fi
}

mkdir -p "$STATE_DIR"
HOSTAPD_CONF="$STATE_DIR/hostapd.conf"
HOSTAPD_PID="$STATE_DIR/hostapd.pid"
UDHCPD_CONF="$STATE_DIR/udhcpd.conf"
UDHCPD_PID="$STATE_DIR/udhcpd.pid"
UDHCPD_LEASES="$STATE_DIR/udhcpd.leases"

if ! command -v hostapd >/dev/null 2>&1; then
    log "ERROR: hostapd not found on PATH; cannot start AP"
    exit 1
fi

# Capture a snapshot of nearby Wi-Fi networks while we are still in STA mode;
# once hostapd takes over the radio we can no longer scan.  The portal reads
# this file via GET /wifi/scan so users can pick an SSID from a list instead
# of typing it by hand.
SCAN_SCRIPT="$(dirname "$0")/board-wifi-scan.sh"
if [ -x "$SCAN_SCRIPT" ]; then
    # Cap the entire pre-scan phase so a hung iw/driver never blocks AP startup.
    if command -v timeout >/dev/null 2>&1; then
        timeout 20 sh -c "BOARD_RUNTIME_WLAN_IFACE='$IFACE' sh '$SCAN_SCRIPT' '$STATE_DIR/wifi-scan.json'" \
            >/dev/null 2>&1 || log "wifi pre-scan timed-out or failed (continuing)"
    else
        BOARD_RUNTIME_WLAN_IFACE="$IFACE" sh "$SCAN_SCRIPT" "$STATE_DIR/wifi-scan.json" >/dev/null 2>&1 \
            || log "wifi pre-scan failed (continuing)"
    fi
else
    log "wifi-scan script not found at $SCAN_SCRIPT"
fi

log "stopping STA client on $IFACE"
# Raspberry Pi OS commonly uses NetworkManager. If it keeps managing wlan0,
# it can immediately pull the interface back into STA mode after we switch it
# to AP mode for hostapd.
if command -v nmcli >/dev/null 2>&1; then
    nmcli dev disconnect "$IFACE" >/dev/null 2>&1 || true
    nmcli dev set "$IFACE" managed no >/dev/null 2>&1 || true
fi
# The system init script manages wpa_supplicant via procd — a plain killall would
# be respawned immediately.  Stop the service so procd releases it, then kill any
# leftover processes for good measure.
if [ -x /etc/init.d/wpa_supplicant ]; then
    /etc/init.d/wpa_supplicant stop 2>/dev/null || true
fi
killall wpa_supplicant 2>/dev/null || true
killall udhcpc dhcpcd 2>/dev/null || true
# Stop any previous AP leftovers.
if [ -f "$HOSTAPD_PID" ]; then
    kill "$(cat "$HOSTAPD_PID" 2>/dev/null)" 2>/dev/null || true
    rm -f "$HOSTAPD_PID"
fi
if [ -f "$UDHCPD_PID" ]; then
    kill "$(cat "$UDHCPD_PID" 2>/dev/null)" 2>/dev/null || true
    rm -f "$UDHCPD_PID"
fi
killall hostapd udhcpd 2>/dev/null || true
sleep 0.3

log "reconfiguring $IFACE -> AP ($AP_IP)"
ip link set "$IFACE" down 2>/dev/null || ifconfig "$IFACE" down 2>/dev/null || true
if command -v iw >/dev/null 2>&1; then
    iw dev "$IFACE" set type __ap 2>/dev/null || iw dev "$IFACE" set type ap 2>/dev/null || true
fi
ip link set "$IFACE" up 2>/dev/null || ifconfig "$IFACE" up 2>/dev/null || true

# Give the driver a moment to settle after the type switch; on some kernels the
# interface rejects `ip addr add` for a few hundred ms right after the mode
# flip.  Retry silently so boot-time races do not leave wlan0 without an IP.
assign_ap_ip() {
    ip addr flush dev "$IFACE" 2>/dev/null || true
    ATTEMPT=0
    while [ "$ATTEMPT" -lt 10 ]; do
        if ip addr add "$AP_IP/24" dev "$IFACE" 2>/dev/null; then
            return 0
        fi
        if ifconfig "$IFACE" "$AP_IP" netmask "$AP_NETMASK" up 2>/dev/null; then
            return 0
        fi
        ATTEMPT=$((ATTEMPT + 1))
        sleep 0.2
    done
    return 1
}

if ! assign_ap_ip; then
    log "ERROR: failed to assign $AP_IP to $IFACE after 10 attempts"
fi

cat > "$HOSTAPD_CONF" <<EOF
interface=$IFACE
driver=nl80211
ctrl_interface=$STATE_DIR/ctrl
ssid=$AP_SSID
country_code=$AP_COUNTRY
ieee80211d=1
hw_mode=g
channel=$AP_CHANNEL
ieee80211n=1
wmm_enabled=1
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=$AP_PSK
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
EOF

log "starting hostapd ssid='$AP_SSID' channel=$AP_CHANNEL"
hostapd -B -P "$HOSTAPD_PID" "$HOSTAPD_CONF" >"$STATE_DIR/hostapd.log" 2>&1
# hostapd -B exit code is unreliable (VLAN warnings can cause rc=1 even when
# the AP is running fine).  Check whether the process is actually alive instead.
sleep 0.5
if [ -f "$HOSTAPD_PID" ] && kill -0 "$(cat "$HOSTAPD_PID" 2>/dev/null)" 2>/dev/null; then
    log "hostapd running (pid=$(cat "$HOSTAPD_PID"))"
elif pidof hostapd >/dev/null 2>&1; then
    log "hostapd running (pid file missing but process found)"
else
    log "ERROR: hostapd is not running after start, see $STATE_DIR/hostapd.log"
    cat "$STATE_DIR/hostapd.log" 2>/dev/null
    restore_networkmanager
    exit 1
fi

# hostapd may flap the interface during bring-up and occasionally clears the
# address we added above.  Re-assert 192.168.44.1 so dnsmasq and the HTTP
# portal are actually reachable from clients.
if ! ip -4 addr show "$IFACE" | grep -q "inet $AP_IP/"; then
    log "re-asserting $AP_IP on $IFACE after hostapd start"
    assign_ap_ip || true
fi

DNSMASQ_CONF="$STATE_DIR/dnsmasq.conf"
DNSMASQ_PID="$STATE_DIR/dnsmasq.pid"
DNSMASQ_LEASES="$STATE_DIR/dnsmasq.leases"

if command -v dnsmasq >/dev/null 2>&1; then
    : > "$DNSMASQ_LEASES"
    cat > "$DNSMASQ_CONF" <<EOF
interface=$IFACE
bind-interfaces
except-interface=lo
no-resolv
no-hosts
domain-needed
bogus-priv
dhcp-range=$DHCP_START,$DHCP_END,$AP_NETMASK,10m
dhcp-option=3,$AP_IP
dhcp-option=6,$AP_IP
dhcp-leasefile=$DNSMASQ_LEASES
pid-file=$DNSMASQ_PID
# Captive-portal-ish: resolve any hostname to the AP so the built-in portal
# loads even when the phone probes external domains.
address=/#/$AP_IP
EOF
    log "starting dnsmasq ($DHCP_START - $DHCP_END)"
    # A running system dnsmasq instance would conflict; kill any pre-existing
    # ones bound to the same iface first.
    killall dnsmasq 2>/dev/null || true
    sleep 0.2
    dnsmasq --conf-file="$DNSMASQ_CONF" >"$STATE_DIR/dnsmasq.log" 2>&1 \
        || log "dnsmasq start returned non-zero, see $STATE_DIR/dnsmasq.log"
elif command -v udhcpd >/dev/null 2>&1; then
    : > "$UDHCPD_LEASES"
    cat > "$UDHCPD_CONF" <<EOF
start $DHCP_START
end $DHCP_END
interface $IFACE
max_leases 100
lease_file $UDHCPD_LEASES
pidfile $UDHCPD_PID
opt router $AP_IP
opt dns $AP_IP
opt subnet $AP_NETMASK
opt lease 600
EOF
    log "starting udhcpd ($DHCP_START - $DHCP_END)"
    udhcpd "$UDHCPD_CONF" >"$STATE_DIR/udhcpd.log" 2>&1 || log "udhcpd start returned non-zero, continuing"
else
    log "no DHCP server available; clients must configure a static $AP_IP/24 address"
fi

log "AP ready: SSID=$AP_SSID IP=$AP_IP"
