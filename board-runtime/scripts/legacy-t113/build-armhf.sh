#!/bin/sh
# Cross-compile board-runtime for ZQBoard T113 (arm-linux-musleabihf, Cortex-A7).
# Uses zig as the C cross compiler + its bundled musl libc.
# zlib is statically compiled from source so that the final binaries have no
# non-libc dynamic dependencies.
set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$HERE"

ZIG="${ZIG:-zig}"
ZLIB_DIR="${ZLIB_DIR:-/usr/local/src/zlib}"
TARGET="${TARGET:-arm-linux-musleabihf}"
CPU="${CPU:-cortex_a7}"
OUT="${OUT:-build-armhf}"

if [ ! -x "$ZIG" ]; then
  echo "zig not found at $ZIG" >&2
  exit 1
fi
if [ ! -d "$ZLIB_DIR" ]; then
  echo "zlib source not found at $ZLIB_DIR" >&2
  exit 1
fi

mkdir -p "$OUT"

COMMON_SRCS="
  src/runtime_common.c
  src/runtime_json.c
  src/runtime_protocol.c
  src/runtime_session_state.c
  src/runtime_stats.c
  src/runtime_mqtt.c
  src/runtime_usb_serial.c
  src/runtime_pairing.c
  src/runtime_debug.c
  src/screen_page.c
  src/rotary_decoder.c
  src/touch_gesture.c
  src/voice_button.c
  src/runtime_wifi.c
"

ZLIB_SRCS="
  $ZLIB_DIR/adler32.c
  $ZLIB_DIR/compress.c
  $ZLIB_DIR/crc32.c
  $ZLIB_DIR/deflate.c
  $ZLIB_DIR/gzclose.c
  $ZLIB_DIR/gzlib.c
  $ZLIB_DIR/gzread.c
  $ZLIB_DIR/gzwrite.c
  $ZLIB_DIR/infback.c
  $ZLIB_DIR/inffast.c
  $ZLIB_DIR/inflate.c
  $ZLIB_DIR/inftrees.c
  $ZLIB_DIR/trees.c
  $ZLIB_DIR/uncompr.c
  $ZLIB_DIR/zutil.c
"

CFLAGS="
  -O2 -fno-stack-protector
  -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
  -DHAVE_UNISTD_H -DZ_HAVE_UNISTD_H
  -Isrc -I$ZLIB_DIR
  -std=gnu11 -Wall -Wextra -Wno-pedantic
  -Wno-unused-parameter -Wno-unused-function
  -Wno-implicit-function-declaration
  -Wno-deprecated-non-prototype
"

build_one() {
  target_name="$1"
  main_src="$2"
  echo "[build-armhf] building $target_name"
  # shellcheck disable=SC2086
  "$ZIG" cc \
    -target "$TARGET" -mcpu="$CPU" \
    $CFLAGS \
    -Wl,-s \
    -o "$OUT/$target_name" \
    $main_src $COMMON_SRCS $ZLIB_SRCS \
    -lm
}

build_one board-server src/board_server.c
build_one board-touch-input src/board_touch_input.c
build_one board-rotary-input src/board_rotary_input.c
build_one fb-speech-overlay src/fb_speech_overlay.c

ls -lh "$OUT"/board-server "$OUT"/board-touch-input "$OUT"/board-rotary-input "$OUT"/fb-speech-overlay
file "$OUT"/board-server
echo "[build-armhf] done"
