#!/bin/sh
# Board runtime self-check.  Verifies the critical hardware / filesystem
# preconditions for the pairing flow.  Writes a user-facing hint to
# .current-speech on failure so the screen reflects the actual problem
# instead of silently hanging.
#
# Exit code: 0 = all good, non-zero = at least one check failed.
set -u

BOARD_DIR="${BOARD_DIR:-${1:-/mnt/UDISK/board-runtime}}"
IFACE="${BOARD_RUNTIME_WLAN_IFACE:-wlan0}"

LOG_TAG="[board-selfcheck]"
log() { echo "$LOG_TAG $*"; }

if [ -f "$BOARD_DIR/fb-device.sh" ]; then
    . "$BOARD_DIR/fb-device.sh"
fi

FAILED=0
REASONS=""

fail() {
    FAILED=$((FAILED + 1))
    REASONS="${REASONS:+$REASONS; }$1"
    log "FAIL: $1"
}

ok() {
    log "ok: $1"
}

# 1. Runtime directory readable and writable.
if [ ! -d "$BOARD_DIR" ]; then
    fail "runtime dir missing: $BOARD_DIR"
else
    if ! ( : > "$BOARD_DIR/.selfcheck.tmp" ) 2>/dev/null; then
        fail "runtime dir not writable: $BOARD_DIR"
    else
        rm -f "$BOARD_DIR/.selfcheck.tmp"
        ok "runtime dir writable"
    fi
fi

# 2. Critical binaries present and executable.
for bin in board-server board-touch-input fb-speech-overlay; do
    path="$BOARD_DIR/$bin"
    if [ ! -x "$path" ]; then
        fail "binary missing or not executable: $bin"
    else
        ok "$bin present"
    fi
done
if [ -x "$BOARD_DIR/board-rotary-input" ]; then
    ok "board-rotary-input present"
else
    log "warn: board-rotary-input not present, GPIO rotary navigation disabled"
fi

# 3. Framebuffer.
FB_DEV="${PET_CLAW_FB_DEV:-auto}"
if command -v fb_resolve_device >/dev/null 2>&1; then
    FB_DEV="$(fb_resolve_device "$FB_DEV")"
fi
case "$FB_DEV" in ""|auto|AUTO) FB_DEV=/dev/fb0;; esac
FB_NUM="${FB_DEV##*/fb}"
if [ ! -c "$FB_DEV" ]; then
    fail "$FB_DEV not present"
else
    FB_NAME=$(cat "/sys/class/graphics/fb${FB_NUM}/name" 2>/dev/null || echo "?")
    FB_SIZE=$(cat "/sys/class/graphics/fb${FB_NUM}/virtual_size" 2>/dev/null || echo "?")
    BPP=$(cat "/sys/class/graphics/fb${FB_NUM}/bits_per_pixel" 2>/dev/null || echo "?")
    ok "$FB_DEV present name=$FB_NAME size=$FB_SIZE bpp=$BPP"
fi

# 4. Font asset for on-screen hints.
if [ ! -s "$BOARD_DIR/unifont.hex.gz" ] && [ ! -s "$BOARD_DIR/unifont.hex" ]; then
    fail "unifont hex font missing in $BOARD_DIR"
else
    ok "unifont font available"
fi

# 5. Wireless interface.
if [ ! -d "/sys/class/net/$IFACE" ]; then
    fail "wireless iface missing: $IFACE"
else
    ok "wireless iface: $IFACE"
fi

# 6. Video player for idle animation (soft failure; pairing still works).
if ! command -v tplayerdemo >/dev/null 2>&1; then
    log "warn: tplayerdemo not in PATH, idle animations will be skipped"
fi

if [ "$FAILED" -gt 0 ]; then
    MSG="设备自检失败：$REASONS。请联系维护人员。"
    log "$FAILED check(s) failed: $REASONS"
    # Surface on the screen so the user does not see a blank device.
    if [ -d "$BOARD_DIR" ] && ( : > "$BOARD_DIR/.current-speech.tmp" ) 2>/dev/null; then
        printf '%s' "$MSG" > "$BOARD_DIR/.current-speech.tmp"
        mv "$BOARD_DIR/.current-speech.tmp" "$BOARD_DIR/.current-speech"
        echo "waiting_user" > "$BOARD_DIR/.current-state" 2>/dev/null || true
        echo "SelfCheckFailed" > "$BOARD_DIR/.current-event" 2>/dev/null || true
    fi
    exit 1
fi

log "all checks passed"
exit 0
