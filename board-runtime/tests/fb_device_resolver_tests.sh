#!/bin/sh
set -eu

ROOT="${TMPDIR:-/tmp}/fb-resolver-test.$$"
SYSFS="$ROOT/sys/class/graphics"
DEV="$ROOT/dev"

cleanup() {
    rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM

mkdir -p "$SYSFS/fb0" "$SYSFS/fb1" "$DEV"
: > "$DEV/fb0"
: > "$DEV/fb1"

printf '%s\n' 'BCM2708 FB' > "$SYSFS/fb0/name"
printf '%s\n' '480,320' > "$SYSFS/fb0/virtual_size"
printf '%s\n' '32' > "$SYSFS/fb0/bits_per_pixel"

printf '%s\n' 'fb_ili9341' > "$SYSFS/fb1/name"
printf '%s\n' '320,240' > "$SYSFS/fb1/virtual_size"
printf '%s\n' '16' > "$SYSFS/fb1/bits_per_pixel"

PET_CLAW_FB_SYSFS_ROOT="$SYSFS"
PET_CLAW_FB_DEV_ROOT="$DEV"
export PET_CLAW_FB_SYSFS_ROOT PET_CLAW_FB_DEV_ROOT

. ./fb-device.sh

assert_eq() {
    expected="$1"
    actual="$2"
    message="$3"
    if [ "$actual" != "$expected" ]; then
        echo "FAIL: $message: expected '$expected', got '$actual'" >&2
        exit 1
    fi
}

assert_eq "$DEV/fb1" "$(fb_resolve_device auto)" "auto prefers fb_ili9341 over HDMI framebuffer"
assert_eq "$DEV/fb0" "$(fb_resolve_device "$DEV/fb0")" "explicit framebuffer override is respected"

rm -f "$DEV/fb0" "$DEV/fb1"
assert_eq "" "$(fb_resolve_device auto || true)" "auto returns empty when no framebuffer exists"
