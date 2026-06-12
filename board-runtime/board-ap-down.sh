#!/bin/sh
# Tear down the SoftAP and return wlan0 to managed STA mode so the normal
# network services (wpa_supplicant / udhcpc / netifd) can resume.
set -u

IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"
STATE_DIR="${BOARD_RUNTIME_AP_STATE_DIR:-/tmp/board-runtime-ap}"

LOG_TAG="[board-ap-down]"
log() { echo "$LOG_TAG $*"; }

for pidfile in "$STATE_DIR/dnsmasq.pid" "$STATE_DIR/udhcpd.pid" "$STATE_DIR/hostapd.pid"; do
    [ -f "$pidfile" ] || continue
    pid=$(cat "$pidfile" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        sleep 0.3
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
done
killall hostapd 2>/dev/null || true
# Our dnsmasq used a dedicated pidfile; don't kill arbitrary dnsmasqs unless
# they were ours (system dnsmasq may serve other interfaces).
killall udhcpd 2>/dev/null || true

log "switching $IFACE back to managed mode"
ip link set "$IFACE" down 2>/dev/null || ifconfig "$IFACE" down 2>/dev/null || true
if command -v iw >/dev/null 2>&1; then
    iw dev "$IFACE" set type managed 2>/dev/null || true
fi
ip addr flush dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up 2>/dev/null || ifconfig "$IFACE" up 2>/dev/null || true
if command -v nmcli >/dev/null 2>&1; then
    nmcli dev set "$IFACE" managed yes >/dev/null 2>&1 || true
    nmcli dev connect "$IFACE" >/dev/null 2>&1 || true
fi

# Re-start the normal STA stack.  On Allwinner Tina the "wifi"/netifd flow
# does not auto-reassociate after we yank the iface out, so we always relaunch
# wpa_supplicant + udhcpc directly from the on-disk config.
WPA_CONF="${BOARD_RUNTIME_WPA_CONF:-/etc/wifi/wpa_supplicant.conf}"
WPA_OVERLAY="${BOARD_RUNTIME_WPA_OVERLAY:-/etc/wifi/wpa_supplicant_overlay.conf}"

killall wpa_supplicant 2>/dev/null || true
sleep 0.3

if [ -f "$WPA_CONF" ]; then
    log "restarting wpa_supplicant on $IFACE"
    if [ -f "$WPA_OVERLAY" ]; then
        wpa_supplicant -B -i "$IFACE" -Dnl80211 -c "$WPA_CONF" -I "$WPA_OVERLAY" \
            >"$STATE_DIR/wpa_supplicant.log" 2>&1 || \
        wpa_supplicant -B -i "$IFACE" -Dnl80211 -c "$WPA_CONF" \
            >"$STATE_DIR/wpa_supplicant.log" 2>&1 || \
        log "wpa_supplicant failed to start"
    else
        wpa_supplicant -B -i "$IFACE" -Dnl80211 -c "$WPA_CONF" \
            >"$STATE_DIR/wpa_supplicant.log" 2>&1 || \
        log "wpa_supplicant failed to start"
    fi
    # Let association complete before asking for a lease.
    sleep 3
    killall udhcpc dhcpcd 2>/dev/null || true
    sleep 0.2
    log "requesting DHCP lease on $IFACE"
    if command -v udhcpc >/dev/null 2>&1; then
        udhcpc -i "$IFACE" -S -t 6 -T 3 -n >"$STATE_DIR/dhcp.log" 2>&1 || \
            log "udhcpc failed (see $STATE_DIR/dhcp.log)"
    elif command -v dhcpcd >/dev/null 2>&1; then
        dhcpcd -n "$IFACE" >"$STATE_DIR/dhcp.log" 2>&1 || \
            log "dhcpcd failed (see $STATE_DIR/dhcp.log)"
    elif command -v dhclient >/dev/null 2>&1; then
        dhclient -1 "$IFACE" >"$STATE_DIR/dhcp.log" 2>&1 || \
            log "dhclient failed (see $STATE_DIR/dhcp.log)"
    else
        log "no DHCP client available"
    fi
elif [ -x /etc/init.d/network ]; then
    log "reloading /etc/init.d/network (no wpa_supplicant.conf found)"
    /etc/init.d/network reload >/dev/null 2>&1 || /etc/init.d/network restart >/dev/null 2>&1 || true
elif command -v systemctl >/dev/null 2>&1; then
    log "restarting networking via systemd"
    systemctl restart dhcpcd 2>/dev/null || systemctl restart networking 2>/dev/null || true
fi

log "AP down, STA stack restarted"
