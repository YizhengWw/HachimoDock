#!/bin/sh
set -eu

DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$DIR"

echo "uname: $(uname -a)"

for BIN in board-server board-touch-input board-rotary-input fb-speech-overlay; do
  if [ -x "$DIR/$BIN" ]; then
    RUN="$DIR/$BIN"
  elif [ -x "$DIR/build/$BIN" ]; then
    RUN="$DIR/build/$BIN"
  else
    echo "$BIN: missing"
    exit 127
  fi
  echo "$BIN: ok"
  "$RUN" --help >/dev/null
done

if [ -f "$DIR/device-config.json" ]; then
  echo "device-config.json: ok"
fi
