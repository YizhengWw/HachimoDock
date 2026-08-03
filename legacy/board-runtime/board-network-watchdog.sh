#!/bin/sh
# Keep wlan0 usable for the board runtime. TinaLinux sometimes comes up with
# Wi-Fi associated but without an IPv4 lease after reboot.
set -u

IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"
INTERVAL="${BOARD_RUNTIME_DHCP_INTERVAL:-20}"
ATTEMPTS="${BOARD_RUNTIME_DHCP_ATTEMPTS:-2}"
TRIES="${BOARD_RUNTIME_DHCP_TRIES:-6}"
TIMEOUT="${BOARD_RUNTIME_DHCP_TIMEOUT:-3}"

has_ipv4() {
  if command -v ip >/dev/null 2>&1; then
    ip -4 addr show "$IFACE" 2>/dev/null | grep -q "inet "
  else
    ifconfig "$IFACE" 2>/dev/null | grep -q "inet "
  fi
}

has_default_route() {
  route -n 2>/dev/null | awk -v iface="$IFACE" '$1 == "0.0.0.0" && $8 == iface { found = 1 } END { exit found ? 0 : 1 }'
}

# Returns 0 when wlan0 is configured as a SoftAP (either because hostapd is
# running, iw reports "type AP", or the marker file from board-ap-up.sh is
# present).  In that case the watchdog MUST stay out of the way -- running
# udhcpc here would kick the interface back through the deconfig handler and
# silently wipe the 192.168.44.1 address, which breaks the pairing portal.
is_ap_mode() {
  if [ -f /tmp/board-runtime-ap/hostapd.pid ]; then
    return 0
  fi
  if command -v iw >/dev/null 2>&1; then
    iw dev "$IFACE" info 2>/dev/null | grep -qi '^[[:space:]]*type AP' && return 0
  fi
  if command -v pidof >/dev/null 2>&1; then
    pidof hostapd >/dev/null 2>&1 && return 0
  fi
  return 1
}

ensure_once() {
  if [ "${BOARD_RUNTIME_DHCP_AUTO:-1}" = "0" ]; then
    return 0
  fi

  if is_ap_mode; then
    return 0
  fi

  # No network config means the device should be in pairing/AP mode, not STA.
  # Do not attempt DHCP or the driver may auto-associate with a cached SSID
  # and steal wlan0 away from the AP that board-server is about to start.
  # Kill any leftover wpa_supplicant/udhcpc from a previous STA session.
  BOARD_DIR="${BOARD_DIR:-/mnt/UDISK/board-runtime}"
  if [ ! -f "$BOARD_DIR/network-config.json" ]; then
    if pidof wpa_supplicant >/dev/null 2>&1 && ! is_ap_mode; then
      echo "[net] pairing mode: killing leftover wpa_supplicant/dhcp"
      killall wpa_supplicant udhcpc dhcpcd 2>/dev/null || true
    fi
    return 0
  fi

  if has_ipv4 && has_default_route; then
    return 0
  fi

  echo "[net] repairing network on $IFACE"
  ifconfig "$IFACE" up 2>/dev/null || true

  # If wpa_supplicant is not running, start it first.  This handles the cold
  # boot case where network-config.json exists but the device never went
  # through AP→STA transition (board-ap-down.sh was never called).
  WPA_CONF="${BOARD_RUNTIME_WPA_CONF:-/etc/wifi/wpa_supplicant.conf}"
  WPA_OVERLAY="${BOARD_RUNTIME_WPA_OVERLAY:-/etc/wifi/wpa_supplicant_overlay.conf}"
  if [ -f "$WPA_CONF" ] && ! pidof wpa_supplicant >/dev/null 2>&1; then
    echo "[net] wpa_supplicant not running, starting it"
    if [ -f "$WPA_OVERLAY" ]; then
      wpa_supplicant -B -i "$IFACE" -Dnl80211 -c "$WPA_CONF" -I "$WPA_OVERLAY" 2>/dev/null || \
      wpa_supplicant -B -i "$IFACE" -Dnl80211 -c "$WPA_CONF" 2>/dev/null || \
      echo "[net] wpa_supplicant failed to start"
    else
      wpa_supplicant -B -i "$IFACE" -Dnl80211 -c "$WPA_CONF" 2>/dev/null || \
      echo "[net] wpa_supplicant failed to start"
    fi
    sleep 3
  fi

  killall -9 udhcpc dhcpcd 2>/dev/null || true

  INDEX=0
  while [ "$INDEX" -lt "$ATTEMPTS" ]; do
    if has_ipv4 && has_default_route; then
      return 0
    fi

    if command -v udhcpc >/dev/null 2>&1; then
      udhcpc -i "$IFACE" -S -t "$TRIES" -T "$TIMEOUT" -n || true
    elif command -v dhcpcd >/dev/null 2>&1; then
      dhcpcd -n "$IFACE" 2>/dev/null || true
    elif command -v dhclient >/dev/null 2>&1; then
      dhclient -1 "$IFACE" 2>/dev/null || true
    fi
    if has_ipv4 && has_default_route; then
      return 0
    fi

    INDEX=$((INDEX + 1))
    sleep 2
  done

  return 1
}

case "${1:-watch}" in
  once)
    ensure_once
    ;;
  watch)
    while true; do
      ensure_once || true
      sleep "$INTERVAL"
    done
    ;;
  *)
    echo "usage: $0 [once|watch]" >&2
    exit 2
    ;;
esac
