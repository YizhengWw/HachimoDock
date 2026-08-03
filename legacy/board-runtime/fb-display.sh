#!/bin/sh
# Framebuffer video driver for the board runtime.
# New model: each animation is one complete clip under terrier-clips, with
# optional same-family WAV cues for terminal states.
# When both working.typing and working.thinking are present, the working state
# alternates those two clips one full clip at a time instead of choosing random
# working variants.
# Main-screen subtitle rendering is disabled; .current-speech remains a data
# handoff file for upstream text, pairing hints, and diagnostics.
# The screen pulls .current-state at clip boundaries; .screen-interrupt means
# "stop now and pull the current state again", while .welcome-trigger injects
# a one-shot welcome clip before returning to the current session state.
set -u
export LANG="${LANG:-C.UTF-8}"
export LC_ALL="${LC_ALL:-C.UTF-8}"

DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
RUNTIME_ROOT="${PET_CLAW_RUNTIME_ROOT:-$DIR}"
if [ -f "$DIR/fb-device.sh" ]; then
  . "$DIR/fb-device.sh"
fi
CLIP_MAX_SECONDS="${PET_CLAW_FB_CLIP_MAX_SECONDS:-${PET_CLAW_FB_LOOP_MAX_SECONDS:-30}}"
CLIP_EDGE_TRIM_SECONDS="${PET_CLAW_FB_CLIP_EDGE_TRIM_SECONDS:-0.08}"
FFMPEG_MIN_PLAY_SECONDS="${PET_CLAW_FB_FFMPEG_MIN_PLAY_SECONDS:-8.0}"
FFMPEG_OUTPUT_FPS="${PET_CLAW_FB_FFMPEG_OUTPUT_FPS:-24}"
FB_ROTATE="${PET_CLAW_FB_ROTATE:-0}"
DURATION_FILE="${PET_CLAW_FB_DURATION_FILE:-$RUNTIME_ROOT/terrier-clips-durations.tsv}"
VIDEO_CACHE="${PET_CLAW_FB_VIDEO_CACHE:-/tmp/fb-videos}"
AUDIO_PLAY_DEV="${PET_CLAW_FB_AUDIO_PLAY_DEV:-${AUDIO_BRIDGE_PLAY_DEV:-default}}"
AUDIO_PREPARE_MODE="${PET_CLAW_FB_AUDIO_PREPARE:-auto}"
AUDIO_PLAY_GAIN="${PET_CLAW_FB_AUDIO_GAIN:-0.18}"
AUDIO_PLAY_FADE_IN="${PET_CLAW_FB_AUDIO_FADE_IN:-0.08}"
AUDIO_PLAY_RATE="${PET_CLAW_FB_AUDIO_RATE:-48000}"
AUDIO_PLAY_CHANNELS="${PET_CLAW_FB_AUDIO_CHANNELS:-2}"
AUDIO_PLAY_SAMPLE_FMT="${PET_CLAW_FB_AUDIO_SAMPLE_FMT:-s16}"
FB_REQUEST="${PET_CLAW_FB_DEV:-${FB_DEV:-auto}}"
if command -v fb_resolve_device >/dev/null 2>&1; then
  FB_DEV="$(fb_resolve_device "$FB_REQUEST" 2>/dev/null || true)"
else
  FB_DEV="$FB_REQUEST"
  case "$FB_DEV" in ""|auto|AUTO) FB_DEV="/dev/fb0";; esac
  if [ ! -c "$FB_DEV" ] && [ -c /dev/fb0 ]; then
    echo "[fb] framebuffer device $FB_DEV is not a character device; falling back to /dev/fb0"
    FB_DEV="/dev/fb0"
  fi
fi
FB_REQUIRED=1
case "${1:-}" in
  --self-test|--debug-text|--speech-filter|--duration) FB_REQUIRED=0;;
esac
if [ "$FB_REQUIRED" = "1" ] && { [ -z "$FB_DEV" ] || [ ! -c "$FB_DEV" ]; }; then
  echo "[fb] framebuffer device unavailable (${FB_DEV:-none}); exiting display loop" >&2
  exit 0
fi
export PET_CLAW_FB_DEV="$FB_DEV"
FB_NUM="${FB_DEV##*/fb}"
FB_SIZE=$(cat "/sys/class/graphics/fb${FB_NUM}/virtual_size" 2>/dev/null || echo "480,640")
FB_WIDTH="${FB_SIZE%,*}"
FB_HEIGHT="${FB_SIZE#*,}"
case "$FB_WIDTH" in ""|*[!0-9]*) FB_WIDTH=480;; esac
case "$FB_HEIGHT" in ""|*[!0-9]*) FB_HEIGHT=640;; esac
INDEX_DIR=""
VIDEO_ROOT_DISK=""
VIDEO_ROOT=""
LOCKFILE="$RUNTIME_ROOT/.fb-display.lock"
TPLAYER_FIFO="$RUNTIME_ROOT/.tplayerdemo.fifo"
TPLAYER_LOG="${PET_CLAW_FB_TPLAYER_LOG:-/dev/null}"
TPLAYER_READY_SECONDS="${PET_CLAW_FB_TPLAYER_READY_SECONDS:-0.4}"
DEBUG_OVERLAY_FLAG="$RUNTIME_ROOT/.debug-overlay-enabled"
DEBUG_SCREEN_STATE_PATH="$RUNTIME_ROOT/.debug-screen-state.json"
DEBUG_SPEECH_PATH="$RUNTIME_ROOT/.current-debug-speech"
SPEECH_PATH="$RUNTIME_ROOT/.current-speech"
SPEECH_RENDER_PATH="$RUNTIME_ROOT/.current-speech-render"
SPEECH_HOLD_UNTIL_PATH="$RUNTIME_ROOT/.current-speech-hold-until"
# Volume HUD: board-rotary-input writes .volume-display as "<pct>\n<epoch_ms>".
# volume_is_fresh() gates the HUD on both screens — the negative screen overlays
# it inside fb-stats-renderer; the pet screen pauses its ffmpeg animation and the
# persistent renderer (--serve "hud") draws the same Pillow HUD over a dark frame.
VOLUME_DISPLAY_PATH="$RUNTIME_ROOT/.volume-display"
SCREEN_PAGE_PATH="$RUNTIME_ROOT/.screen-page"
WELCOME_TRIGGER_PATH="$RUNTIME_ROOT/.welcome-trigger"
CLIPS_RELOAD_PATH="$RUNTIME_ROOT/.clips-reload"
STATS_RENDERER="${PET_CLAW_FB_STATS_RENDERER:-$DIR/fb-stats-renderer.py}"
STATS_RENDER_INTERVAL_SECONDS="${PET_CLAW_FB_STATS_RENDER_INTERVAL_SECONDS:-2}"
FONTFILE="${PET_CLAW_FB_FONTFILE:-}"
if [ -z "$FONTFILE" ]; then
  for CANDIDATE in \
    /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc \
    /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
    /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf; do
    if [ -f "$CANDIDATE" ]; then
      FONTFILE="$CANDIDATE"
      break
    fi
  done
fi
STATUS_FONTFILE="${PET_CLAW_FB_STATUS_FONTFILE:-}"
if [ -z "$STATUS_FONTFILE" ]; then
  for CANDIDATE in \
    /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
    "$FONTFILE"; do
    if [ -n "$CANDIDATE" ] && [ -f "$CANDIDATE" ]; then
      STATUS_FONTFILE="$CANDIDATE"
      break
    fi
  done
fi

CURRENT_STATE=""
CURRENT_CLIP=""
CURRENT_LOOP_TARGET=1
CURRENT_LOOP_COUNT=0
CURRENT_SPEECH_MARKER=""
LAST_INTERRUPT_MARKER=""
LAST_TOUCH_MARKER=""
LAST_WELCOME_TRIGGER_MARKER=""
LAST_STATS_RENDER_AT=0
WAS_PAIRING=0
CHECKPOINT_STATE=""
TPLAYER_PID=""
STATE_AUDIO_PID=""
CURRENT_AUDIO_CUE_KEY=""

log() {
  echo "[fb] $*" >&2
}

read_runtime_file() {
  FILE="$1"
  if [ -f "$FILE" ]; then
    tr -d '\r\n' < "$FILE" 2>/dev/null
  fi
}

write_runtime_file() {
  FILE="$1"
  VALUE="$2"
  TMP="$FILE.tmp.$$"
  printf "%s" "$VALUE" > "$TMP" 2>/dev/null && mv "$TMP" "$FILE" 2>/dev/null
}

stop_state_audio() {
  if [ -n "${STATE_AUDIO_PID:-}" ]; then
    kill "$STATE_AUDIO_PID" 2>/dev/null || true
    wait "$STATE_AUDIO_PID" 2>/dev/null || true
    STATE_AUDIO_PID=""
  fi
}

state_audio_cue_path() {
  CLIP="$1"
  CUE="${CLIP%.mp4}.wav"
  [ -f "$CUE" ] || return 1
  echo "$CUE"
}

audio_prepare_enabled() {
  case "$AUDIO_PREPARE_MODE" in
    1|true|TRUE|yes|YES|on|ON|always) return 0;;
    0|false|FALSE|no|NO|off|OFF|never) return 1;;
  esac
  case "$AUDIO_PLAY_DEV" in
    *MAX98357A*) return 0;;
  esac
  return 1
}

play_prepared_wav_file() {
  CUE="$1"
  TMP="/tmp/fb-audio-cue-$(basename "${CUE%.wav}")-$$.wav"
  FILTER="volume=$AUDIO_PLAY_GAIN"
  case "$AUDIO_PLAY_FADE_IN" in
    ""|0|0.0|0.00) ;;
    *) FILTER="$FILTER,afade=t=in:st=0:d=$AUDIO_PLAY_FADE_IN";;
  esac
  if ! command -v ffmpeg >/dev/null 2>&1; then
    return 1
  fi
  if ! ffmpeg -y -v error -i "$CUE" \
      -af "$FILTER" \
      -ar "$AUDIO_PLAY_RATE" \
      -ac "$AUDIO_PLAY_CHANNELS" \
      -sample_fmt "$AUDIO_PLAY_SAMPLE_FMT" \
      "$TMP"; then
    rm -f "$TMP"
    return 1
  fi
  aplay -q -D "$AUDIO_PLAY_DEV" "$TMP"
  RC=$?
  rm -f "$TMP"
  return "$RC"
}

play_wav_file() {
  CUE="$1"
  if command -v aplay >/dev/null 2>&1; then
    if audio_prepare_enabled && play_prepared_wav_file "$CUE"; then
      return 0
    fi
    aplay -q -D "$AUDIO_PLAY_DEV" "$CUE"
    return $?
  fi
  if command -v tinyplay >/dev/null 2>&1; then
    tinyplay "$CUE"
    return $?
  fi
  log "no WAV player available for $(basename "$CUE")"
  return 1
}

play_state_audio_cue() {
  CLIP="$1"
  STATE="$2"
  CUE=$(state_audio_cue_path "$CLIP" "$STATE") || return 0
  KEY="$STATE:$(basename "$CUE")"
  if [ "$KEY" = "$CURRENT_AUDIO_CUE_KEY" ]; then
    return 0
  fi
  CURRENT_AUDIO_CUE_KEY="$KEY"
  stop_state_audio
  (
    play_wav_file "$CUE" >/dev/null 2>&1 || log "state audio cue failed: $(basename "$CUE")"
  ) &
  STATE_AUDIO_PID=$!
}

speech_int() {
  VALUE="$1"
  DEFAULT="$2"
  case "$VALUE" in
    ""|*[!0-9]*) echo "$DEFAULT";;
    *) echo "$VALUE";;
  esac
}

append_status_disc_filter() {
  DISC_CX="$1"
  DISC_CY="$2"
  DISC_R="$3"
  DISC_COLOR="$4"
  DISC_LAYERS=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_LAYERS:-10}" 10)
  if [ "$DISC_LAYERS" -lt 4 ]; then
    DISC_LAYERS=4
  fi

  DISC_I=0
  while [ "$DISC_I" -lt "$DISC_LAYERS" ]; do
    DISC_TOP=$((DISC_CY - DISC_R + DISC_I * DISC_R * 2 / DISC_LAYERS))
    DISC_NEXT=$((DISC_CY - DISC_R + (DISC_I + 1) * DISC_R * 2 / DISC_LAYERS))
    DISC_H=$((DISC_NEXT - DISC_TOP))
    if [ "$DISC_H" -lt 1 ]; then
      DISC_H=1
    fi

    if [ "$DISC_I" -lt $((DISC_LAYERS / 2)) ]; then
      DISC_EDGE=$((DISC_LAYERS / 2 - DISC_I))
    else
      DISC_EDGE=$((DISC_I - DISC_LAYERS / 2 + 1))
    fi
    DISC_INSET=$((DISC_R * DISC_EDGE * DISC_EDGE / (DISC_LAYERS * DISC_LAYERS / 2 + 1)))
    DISC_X=$((DISC_CX - DISC_R + DISC_INSET))
    DISC_W=$((DISC_R * 2 - DISC_INSET * 2))
    if [ "$DISC_W" -lt 1 ]; then
      DISC_X=$((DISC_CX - DISC_R))
      DISC_W=$((DISC_R * 2))
    fi
    DRAW_FILTER="$DRAW_FILTER,drawbox=x=$DISC_X:y=$DISC_TOP:w=$DISC_W:h=$DISC_H:color=$DISC_COLOR:t=fill"
    DISC_I=$((DISC_I + 1))
  done
}

append_speech_status_filter() {
  STATUS_STATE=$(read_runtime_file "$RUNTIME_ROOT/.current-state")
  [ -n "$STATUS_STATE" ] || STATUS_STATE="$CURRENT_STATE"

  STATUS_SIZE=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_SIZE:-18}" 18)
  STATUS_MARGIN_X=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_MARGIN_X:-12}" 12)
  if [ "${BUBBLE_SHORT:-0}" = "1" ]; then
    STATUS_DEFAULT_MARGIN_Y=$(((BUBBLE_H - STATUS_SIZE) / 2))
    if [ "$STATUS_DEFAULT_MARGIN_Y" -lt 0 ]; then
      STATUS_DEFAULT_MARGIN_Y=0
    fi
  else
    STATUS_DEFAULT_MARGIN_Y=7
  fi
  STATUS_MARGIN_Y=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_MARGIN_Y:-$STATUS_DEFAULT_MARGIN_Y}" "$STATUS_DEFAULT_MARGIN_Y")
  STATUS_X=$((BUBBLE_X + BUBBLE_W - STATUS_MARGIN_X - STATUS_SIZE))
  STATUS_Y=$((BUBBLE_Y + STATUS_MARGIN_Y))
  STATUS_CX=$((STATUS_X + STATUS_SIZE / 2))
  STATUS_CY=$((STATUS_Y + STATUS_SIZE / 2))
  STATUS_R=$((STATUS_SIZE / 2))

  case "$STATUS_STATE" in
    attention|done|complete|completed|success)
      append_status_disc_filter "$STATUS_CX" "$STATUS_CY" "$STATUS_R" "${PET_CLAW_FB_SPEECH_STATUS_DONE_COLOR:-0x04b354@1.0}"
      CHECK_SYMBOL="${PET_CLAW_FB_SPEECH_STATUS_CHECK_SYMBOL:-✔}"
      CHECK_SIZE=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_CHECK_SIZE:-13}" 13)
      CHECK_DX=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_CHECK_DX:-0}" 0)
      CHECK_DY=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_CHECK_DY:-0}" 0)
      DRAW_FILTER="$DRAW_FILTER,drawtext=fontfile=$STATUS_FONTFILE:text='$CHECK_SYMBOL':x=$STATUS_CX-(text_w/2)+$CHECK_DX:y=$STATUS_CY-(text_h/2)-$CHECK_DY:fontsize=$CHECK_SIZE:fontcolor=${PET_CLAW_FB_SPEECH_STATUS_CHECK_COLOR:-white}:borderw=${PET_CLAW_FB_SPEECH_STATUS_CHECK_BORDER:-0}:bordercolor=${PET_CLAW_FB_SPEECH_STATUS_CHECK_COLOR:-white}:fix_bounds=1"
      ;;
    error|failed|failure)
      append_status_disc_filter "$STATUS_CX" "$STATUS_CY" "$STATUS_R" "${PET_CLAW_FB_SPEECH_STATUS_ERROR_COLOR:-0xd93025@1.0}"
      CHECK_SIZE=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_ERROR_SIZE:-18}" 18)
      DRAW_FILTER="$DRAW_FILTER,drawtext=fontfile=$STATUS_FONTFILE:text='!':x=$STATUS_CX-(text_w/2):y=$STATUS_CY-(text_h/2):fontsize=$CHECK_SIZE:fontcolor=${PET_CLAW_FB_SPEECH_STATUS_CHECK_COLOR:-white}:fix_bounds=1"
      ;;
    working|active|thinking|tool_running|speaking)
      RING_SIZE=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_RING_SIZE:-20}" 20)
      DOT_RADIUS=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_SPINNER_RADIUS:-6}" 6)
      DOT_FONT_SIZE=$(speech_int "${PET_CLAW_FB_SPEECH_STATUS_SPINNER_DOT_FONT_SIZE:-12}" 12)
      DRAW_FILTER="$DRAW_FILTER,drawtext=fontfile=$STATUS_FONTFILE:text='○':x=$STATUS_CX-(text_w/2):y=$STATUS_CY-(text_h/2):fontsize=$RING_SIZE:fontcolor=${PET_CLAW_FB_SPEECH_STATUS_RING_COLOR:-0x5f6368@1.0}:fix_bounds=1"
      DRAW_FILTER="$DRAW_FILTER,drawtext=fontfile=$STATUS_FONTFILE:text='●':x='$STATUS_CX+cos(t*5.5)*$DOT_RADIUS-(text_w/2)':y='$STATUS_CY+sin(t*5.5)*$DOT_RADIUS-(text_h/2)':fontsize=$DOT_FONT_SIZE:fontcolor=${PET_CLAW_FB_SPEECH_STATUS_SPINNER_COLOR:-0x202124@1.0}:fix_bounds=1"
      ;;
    *)
      return
      ;;
  esac
}

clear_multi_speech_render_files() {
  rm -f "$SPEECH_RENDER_PATH".[0-9]* 2>/dev/null || true
}

append_speech_bubble_background_filter() {
  if [ "$BUBBLE_R" -lt 2 ] || [ "$BUBBLE_H" -le $((BUBBLE_R * 2)) ]; then
    DRAW_FILTER="$DRAW_FILTER,drawbox=x=$BUBBLE_X:y=$BUBBLE_Y:w=$BUBBLE_W:h=$BUBBLE_H:color=$BUBBLE_COLOR:t=fill"
    return
  fi

  LAYERS=$(speech_int "${PET_CLAW_FB_SPEECH_BUBBLE_LAYERS:-8}" 8)
  if [ "$LAYERS" -lt 3 ]; then
    LAYERS=3
  fi
  MID_Y=$((BUBBLE_Y + BUBBLE_R))
  MID_H=$((BUBBLE_H - BUBBLE_R * 2))
  DRAW_FILTER="$DRAW_FILTER,drawbox=x=$BUBBLE_X:y=$MID_Y:w=$BUBBLE_W:h=$MID_H:color=$BUBBLE_COLOR:t=fill"

  I=0
  while [ "$I" -lt "$LAYERS" ]; do
    TOP_Y=$((BUBBLE_Y + I * BUBBLE_R / LAYERS))
    NEXT_Y=$((BUBBLE_Y + (I + 1) * BUBBLE_R / LAYERS))
    BAND_H=$((NEXT_Y - TOP_Y))
    if [ "$BAND_H" -lt 1 ]; then
      BAND_H=1
    fi
    REM=$((LAYERS - I))
    INSET=$((BUBBLE_R * REM * REM / (LAYERS * LAYERS)))
    BAND_X=$((BUBBLE_X + INSET))
    BAND_W=$((BUBBLE_W - INSET * 2))
    if [ "$BAND_W" -lt 1 ]; then
      BAND_X="$BUBBLE_X"
      BAND_W="$BUBBLE_W"
    fi
    BOTTOM_Y=$((BUBBLE_Y + BUBBLE_H - (I + 1) * BUBBLE_R / LAYERS))
    DRAW_FILTER="$DRAW_FILTER,drawbox=x=$BAND_X:y=$TOP_Y:w=$BAND_W:h=$BAND_H:color=$BUBBLE_COLOR:t=fill"
    DRAW_FILTER="$DRAW_FILTER,drawbox=x=$BAND_X:y=$BOTTOM_Y:w=$BAND_W:h=$BAND_H:color=$BUBBLE_COLOR:t=fill"
    I=$((I + 1))
  done
}

append_multi_speech_draw_filter() {
  case "${PET_CLAW_FB_SPEECH_MULTI_BUBBLES:-1}" in
    0|false|FALSE|no|NO) return 1;;
  esac

  FONT_SIZE=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_FONT_SIZE:-${PET_CLAW_FB_SPEECH_FONT_SIZE:-14}}" 14)
  LINE_SPACING=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_LINE_SPACING:-${PET_CLAW_FB_SPEECH_LINE_SPACING:-2}}" 2)
  BUBBLE_COLOR="${PET_CLAW_FB_SPEECH_BUBBLE_COLOR:-white@0.70}"
  FONT_COLOR="${PET_CLAW_FB_SPEECH_FONT_COLOR:-black}"
  WRAP_COLS=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_WRAP_COLS:-30}" 30)
  MAX_LINES=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_MAX_LINES:-2}" 2)
  MAX_BUBBLES=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_MAX_BUBBLES:-4}" 4)
  if [ "$MAX_LINES" -lt 1 ]; then
    MAX_LINES=1
  fi
  if [ "$MAX_LINES" -gt 3 ]; then
    MAX_LINES=3
  fi
  if [ "$MAX_BUBBLES" -lt 2 ]; then
    return 1
  fi
  if [ "$MAX_BUBBLES" -gt 4 ]; then
    MAX_BUBBLES=4
  fi

  clear_multi_speech_render_files
  MULTI_METRICS=$(python3 - "$SPEECH_PATH" "$SPEECH_RENDER_PATH" "$WRAP_COLS" "$MAX_LINES" "$MAX_BUBBLES" <<'PY' 2>/dev/null || true
import sys
import unicodedata

src_path, render_base = sys.argv[1], sys.argv[2]
wrap_cols = max(8, int(sys.argv[3]))
max_lines = max(1, int(sys.argv[4]))
max_bubbles = max(2, int(sys.argv[5]))

try:
    text = open(src_path, "r", encoding="utf-8", errors="ignore").read().strip("\n")
except Exception:
    text = ""

items = [line.strip() for line in text.splitlines() if line.strip()]
if len(items) < 2:
    raise SystemExit(0)
items = items[:max_bubbles]

def width(value):
    total = 0
    for ch in value:
        total += 2 if unicodedata.east_asian_width(ch) in ("F", "W") else 1
    return total

def ellipsize(value, cols):
    suffix = "..."
    chars = list(value)
    while chars and width("".join(chars) + suffix) > cols:
        chars.pop()
    return "".join(chars) + suffix

def wrap_line(value, cols):
    lines = []
    current = ""
    current_width = 0
    for ch in value:
        ch_width = 2 if unicodedata.east_asian_width(ch) in ("F", "W") else 1
        if current and current_width + ch_width > cols:
            lines.append(current.rstrip())
            current = ch
            current_width = ch_width
        else:
            current += ch
            current_width += ch_width
    lines.append(current.rstrip())
    lines = lines or [""]
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = ellipsize(lines[-1], cols)
    return lines

for index, item in enumerate(items):
    lines = wrap_line(item, wrap_cols)
    out_path = f"{render_base}.{index}"
    try:
        with open(out_path, "w", encoding="utf-8") as output:
            output.write("\n".join(lines))
    except Exception:
        continue
    cols = max(width(line) for line in lines) if lines else 1
    print(f"{index}|{len(lines)}|{cols}|{out_path}")
PY
)
  if [ -z "$MULTI_METRICS" ]; then
    return 1
  fi

  BASE_X=$(speech_int "${PET_CLAW_FB_SPEECH_BUBBLE_X:-6}" 6)
  BUBBLE_PAD_X=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_PADDING_X:-12}" 12)
  BUBBLE_PAD_Y=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_PADDING_Y:-7}" 7)
  BUBBLE_R=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_BUBBLE_R:-14}" 14)
  BOTTOM_MARGIN=$(speech_int "${PET_CLAW_FB_SPEECH_BOTTOM_MARGIN:-6}" 6)
  BUBBLE_GAP=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_GAP:-6}" 6)
  COL_PX=$(speech_int "${PET_CLAW_FB_SPEECH_COL_PX:-8}" 8)
  MAX_BUBBLE_W=$((FB_WIDTH - BASE_X * 2))
  if [ "$MAX_BUBBLE_W" -lt 120 ]; then
    MAX_BUBBLE_W="$FB_WIDTH"
    BASE_X=0
  fi
  STACK_Y=$((FB_HEIGHT - BOTTOM_MARGIN))
  DRAWN_COUNT=0

  while IFS="|" read -r ITEM_INDEX ITEM_LINES ITEM_COLS ITEM_PATH; do
    case "$ITEM_INDEX:$ITEM_LINES:$ITEM_COLS" in
      *[!0-9:]*|"") continue;;
    esac
    TEXT_BLOCK_H=$((ITEM_LINES * FONT_SIZE + (ITEM_LINES - 1) * LINE_SPACING))
    BUBBLE_H=$((TEXT_BLOCK_H + BUBBLE_PAD_Y * 2 + 8))
    if [ "$BUBBLE_H" -lt 34 ]; then
      BUBBLE_H=34
    fi
    if [ "$BUBBLE_H" -gt 68 ]; then
      BUBBLE_H=68
    fi
    BUBBLE_W=$((BUBBLE_PAD_X * 2 + ITEM_COLS * COL_PX))
    MIN_W=$(speech_int "${PET_CLAW_FB_SPEECH_MULTI_MIN_W:-116}" 116)
    if [ "$BUBBLE_W" -lt "$MIN_W" ]; then
      BUBBLE_W="$MIN_W"
    fi
    if [ "$BUBBLE_W" -gt "$MAX_BUBBLE_W" ]; then
      BUBBLE_W="$MAX_BUBBLE_W"
    fi
    BUBBLE_X="$BASE_X"
    BUBBLE_Y=$((STACK_Y - BUBBLE_H))
    if [ "$BUBBLE_Y" -lt 0 ]; then
      break
    fi
    STACK_Y=$((BUBBLE_Y - BUBBLE_GAP))
    BUBBLE_SHORT=0
    TEXT_X=$((BUBBLE_X + BUBBLE_PAD_X))
    TEXT_Y=$((BUBBLE_Y + BUBBLE_PAD_Y))
    TEXT_Y_EXPR="$TEXT_Y"
    if [ "$ITEM_LINES" -le 1 ]; then
      BUBBLE_SHORT=1
      TEXT_Y_EXPR="$BUBBLE_Y+($BUBBLE_H-text_h)/2"
    fi
    append_speech_bubble_background_filter
    if [ "$ITEM_INDEX" = "0" ]; then
      append_speech_status_filter
    fi
    DRAW_FILTER="$DRAW_FILTER,drawtext=fontfile=$FONTFILE:textfile=$ITEM_PATH:reload=1:x=$TEXT_X:y=$TEXT_Y_EXPR:fontsize=$FONT_SIZE:fontcolor=$FONT_COLOR:line_spacing=$LINE_SPACING:fix_bounds=1"
    DRAWN_COUNT=$((DRAWN_COUNT + 1))
  done <<EOF
$MULTI_METRICS
EOF

  [ "$DRAWN_COUNT" -gt 1 ]
}

# True while the rotary volume handler wrote .volume-display ("<pct>\n<epoch_ms>")
# within VOLUME_HUD_WINDOW_MS. Drives the volume HUD on both screens: the
# negative screen overlays it inside fb-stats-renderer; the pet screen pauses
# its ffmpeg animation and lets the persistent renderer draw the same HUD.
VOLUME_HUD_WINDOW_MS="${PET_CLAW_FB_VOLUME_HUD_WINDOW_MS:-1000}"
volume_is_fresh() {
  [ -s "$VOLUME_DISPLAY_PATH" ] || return 1
  VTS=$(sed -n '2p' "$VOLUME_DISPLAY_PATH" 2>/dev/null | tr -dc '0-9')
  [ -n "$VTS" ] || return 1
  NOW_MS=$(date +%s%3N 2>/dev/null)
  case "$NOW_MS" in ""|*[!0-9]*) NOW_MS=$(( $(date +%s) * 1000 ));; esac
  AGE=$(( NOW_MS - VTS ))
  [ "$AGE" -ge 0 ] && [ "$AGE" -le "$VOLUME_HUD_WINDOW_MS" ]
}

# Persistent fb-stats-renderer (--serve): rendering the negative screen / volume
# HUD by spawning a fresh python+PIL+numpy process per frame cost ~1.5s; a long-
# lived process amortizes that startup so each frame is ~250ms. fb-display feeds
# it commands ("render [path]" / "hud") over a FIFO and holds a writer FD open so
# the daemon never sees EOF. Falls back to per-frame spawns if it can't start.
SERVE_FIFO="$RUNTIME_ROOT/.fb-serve-cmd"
SERVE_PID=""
start_stats_serve() {
  command -v python3 >/dev/null 2>&1 || return 1
  [ -f "$STATS_RENDERER" ] || return 1
  rm -f "$SERVE_FIFO" 2>/dev/null
  mkfifo "$SERVE_FIFO" 2>/dev/null || return 1
  # No --width/--height: let the renderer read the actual framebuffer size from
  # --fb (matches the prior one-shot behavior; avoids a stale FB_WIDTH mismatch).
  python3 "$STATS_RENDERER" "$RUNTIME_ROOT" --serve \
    --fb "$FB_DEV" --rotate "$FB_ROTATE" \
    < "$SERVE_FIFO" >/dev/null 2>&1 &
  SERVE_PID=$!
  exec 8>"$SERVE_FIFO" || { SERVE_PID=""; return 1; }
  log "stats serve started pid=$SERVE_PID"
  return 0
}
serve_alive() {
  [ -n "$SERVE_PID" ] && kill -0 "$SERVE_PID" 2>/dev/null
}
serve_cmd() {  # $1 = "render [path]" | "hud"
  serve_alive || return 1
  printf '%s\n' "$1" >&8 2>/dev/null || return 1
  return 0
}
stop_stats_serve() {
  serve_alive && { serve_cmd "quit" 2>/dev/null; kill "$SERVE_PID" 2>/dev/null; }
  exec 8>&- 2>/dev/null || true
  rm -f "$SERVE_FIFO" 2>/dev/null || true
  SERVE_PID=""
}

append_speech_draw_filter() {
  clear_multi_speech_render_files
}

now_ms() {
  SEC=$(date +%s 2>/dev/null || echo 0)
  case "$SEC" in
    ""|*[!0-9]*) SEC=0;;
  esac
  echo $((SEC * 1000))
}

speech_marker() {
  if [ ! -f "$SPEECH_PATH" ]; then
    echo "missing"
    return
  fi
  if stat -c '%y:%s' "$SPEECH_PATH" >/dev/null 2>&1; then
    stat -c '%y:%s' "$SPEECH_PATH" 2>/dev/null
  else
    ls -l "$SPEECH_PATH" 2>/dev/null
  fi
}

json_escape() {
  printf "%s" "${1:-}" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

debug_overlay_enabled() {
  case "${PET_CLAW_FB_DEBUG_OVERLAY:-}" in
    1|true|TRUE|yes|YES) return 0;;
  esac
  FLAG=$(read_runtime_file "$DEBUG_OVERLAY_FLAG")
  case "$FLAG" in
    ""|0|false|FALSE|no|NO) return 1;;
    *) return 0;;
  esac
}

clip_base_name() {
  FILE="$1"
  if [ -n "$FILE" ]; then
    basename "$FILE"
  else
    echo "-"
  fi
}

clip_reason_name() {
  NAME=$(clip_base_name "$1")
  echo "${NAME%.mp4}"
}

fb_device_number() {
  case "${FB_DEV:-}" in
    */fb*) echo "${FB_DEV##*/fb}";;
    *) echo "0";;
  esac
}

fb_bits_per_pixel() {
  FB_NUM=$(fb_device_number)
  cat "/sys/class/graphics/fb${FB_NUM}/bits_per_pixel" 2>/dev/null || echo 16
}

# rgb565le for SPI LCD (fb1); bgra for HDMI (fb0).
fb_rawvideo_pixel_format() {
  case "$(fb_bits_per_pixel)" in
    16) echo "rgb565le";;
    32) echo "bgra";;
    *) echo "rgb565le";;
  esac
}

# Nearest-neighbor keeps sprite pixels crisp; bilinear smears alpha halos.
fb_video_scale_filter() {
  FB_W="${PET_CLAW_FB_WIDTH:-$FB_WIDTH}"
  FB_H="${PET_CLAW_FB_HEIGHT:-$FB_HEIGHT}"
  echo "scale=${FB_W}:${FB_H}:flags=neighbor:force_original_aspect_ratio=decrease,pad=${FB_W}:${FB_H}:(ow-iw)/2:(oh-ih)/2:black"
}

fb_video_rotation_filter() {
  case "${FB_ROTATE:-0}" in
    180|-180) echo ",hflip,vflip";;
    *) echo "";;
  esac
}

state_speech_text() {
  STATE="$1"
  REASON="$2"
  case "$REASON" in
    idle|idle.*) echo "休息中"; return;;
    working.*|tool_running.*) echo "努力工作中"; return;;
    touch.lick) echo "贴一下你"; return;;
    touch.what) echo "咦，是你呀？"; return;;
  esac
  case "$STATE" in
    working) echo "努力工作中";;
    done) echo "搞定啦！";;
    waiting_user) echo "在等你的回答";;
    error) echo "出错了，卡了卡了";;
    idle) echo "休息中";;
    touch) echo "咦，是你呀？";;
    *) echo "";;
  esac
}

state_speech_suppressed() {
  HOLD_UNTIL=$(read_runtime_file "$SPEECH_HOLD_UNTIL_PATH")
  case "$HOLD_UNTIL" in
    ""|*[!0-9]*) ;;
    *)
      NOW=$(now_ms)
      if [ "$HOLD_UNTIL" -gt "$NOW" ] 2>/dev/null; then
        return 0
      fi
      ;;
  esac
  EVENT=$(read_runtime_file "$RUNTIME_ROOT/.current-event")
  case "$EVENT" in
    PairingReady|"") return 1;;
    Pairing*) return 0;;
    *) return 1;;
  esac
}

write_state_speech_for_display() {
  if state_speech_suppressed; then
    return
  fi
  REASON=$(clip_reason_name "$CURRENT_CLIP")
  TEXT=$(state_speech_text "$CURRENT_STATE" "$REASON")
  write_runtime_file "$SPEECH_PATH" "$TEXT"
}

seconds_to_ticks() {
  VALUE="$1"
  TICKS=$(awk -v s="$VALUE" 'BEGIN { if (s <= 0) print 1; else printf "%d\n", int(s * 5 + 0.999); }' 2>/dev/null)
  case "$TICKS" in
    ""|*[!0-9]*) echo 25;;
    0) echo 1;;
    *) echo "$TICKS";;
  esac
}

current_loop_display() {
  LOOP=$((CURRENT_LOOP_COUNT + 1))
  if [ "$LOOP" -lt 1 ] 2>/dev/null; then
    LOOP=1
  fi
  echo "$LOOP"
}

clip_duration_seconds() {
  FILE="$1"
  NAME=$(basename "$FILE")
  case "${PET_CLAW_FB_FAKE_DURATION_SECONDS:-}" in
    ""|*[!0-9.]*) ;;
    *) echo "$PET_CLAW_FB_FAKE_DURATION_SECONDS"; return;;
  esac

  if [ -f "$DURATION_FILE" ]; then
    while read -r KEY VALUE _rest; do
      if [ "$KEY" = "$NAME" ]; then
        case "$VALUE" in
          ""|*[!0-9.]*) ;;
          *) echo "$VALUE"; return;;
        esac
      fi
    done < "$DURATION_FILE"
  fi

  if command -v python3 >/dev/null 2>&1; then
    DURATION=$(python3 - "$FILE" <<'PY' 2>/dev/null
import os
import struct
import sys

path = sys.argv[1]

def read_u32(data):
    return struct.unpack(">I", data)[0]

def read_u64(data):
    return struct.unpack(">Q", data)[0]

def find_mvhd(handle, end):
    while handle.tell() + 8 <= end:
        start = handle.tell()
        header = handle.read(8)
        if len(header) < 8:
            return None
        size = read_u32(header[:4])
        box_type = header[4:8]
        header_size = 8
        if size == 1:
            large = handle.read(8)
            if len(large) < 8:
                return None
            size = read_u64(large)
            header_size = 16
        elif size == 0:
            size = end - start
        if size < header_size:
            return None
        payload_start = start + header_size
        payload_end = start + size
        if payload_end > end:
            return None
        if box_type == b"mvhd":
            handle.seek(payload_start)
            version_flags = handle.read(4)
            if len(version_flags) < 4:
                return None
            version = version_flags[0]
            if version == 1:
                payload = handle.read(28)
                if len(payload) < 28:
                    return None
                timescale = read_u32(payload[16:20])
                duration = read_u64(payload[20:28])
            else:
                payload = handle.read(16)
                if len(payload) < 16:
                    return None
                timescale = read_u32(payload[8:12])
                duration = read_u32(payload[12:16])
            if timescale > 0 and duration > 0:
                return duration / timescale
            return None
        if box_type in (b"moov",):
            handle.seek(payload_start)
            found = find_mvhd(handle, payload_end)
            if found:
                return found
        handle.seek(payload_end)
    return None

with open(path, "rb") as fh:
    duration = find_mvhd(fh, os.path.getsize(path))
if duration:
    print(f"{duration:.3f}")
PY
)
    case "$DURATION" in
      ""|*[!0-9.]*) ;;
      *) echo "$DURATION"; return;;
    esac
  fi

  echo "$CLIP_MAX_SECONDS"
}

clip_playback_seconds() {
  FILE="$1"
  DURATION=$(clip_duration_seconds "$FILE")
  TARGET=$(awk -v d="$DURATION" -v max="$CLIP_MAX_SECONDS" -v trim="$CLIP_EDGE_TRIM_SECONDS" '
    BEGIN {
      if (d <= 0) d = max;
      if (trim < 0) trim = 0;
      if (d <= 1.5) target = d;
      else target = d - trim;
      if (target < 0.2) target = d;
      if (max > 0 && target > max) target = max;
      if (target <= 0) target = (max > 0 ? max : 5);
      printf "%.3f\n", target;
    }
  ' 2>/dev/null)
  case "$TARGET" in
    ""|*[!0-9.]*) echo "$CLIP_MAX_SECONDS";;
    *) echo "$TARGET";;
  esac
}

ffmpeg_playback_seconds() {
  VALUE="$1"
  awk -v s="$VALUE" -v min="$FFMPEG_MIN_PLAY_SECONDS" '
    BEGIN {
      if (s <= 0) s = min;
      if (min > 0 && s < min) s = min;
      printf "%.3f\n", s;
    }
  ' 2>/dev/null
}

build_debug_overlay_text() {
  SESSION_STATE=$(get_current_top_state)
  SESSION_EVENT=$(read_runtime_file "$RUNTIME_ROOT/.current-event")
  [ -n "$SESSION_EVENT" ] || SESSION_EVENT="-"
  CLIP_NAME=$(clip_base_name "$CURRENT_CLIP")
  LOOP=$(current_loop_display)
  printf "screen state=%s clip=%s loop=%s/%s\nsession state=%s event=%s" \
    "${CURRENT_STATE:-idle}" \
    "$CLIP_NAME" \
    "$LOOP" \
    "${CURRENT_LOOP_TARGET:-1}" \
    "$SESSION_STATE" \
    "$SESSION_EVENT"
}

write_screen_debug() {
  PHASE="${1:-play}"
  SESSION_STATE=$(get_current_top_state)
  SESSION_EVENT=$(read_runtime_file "$RUNTIME_ROOT/.current-event")
  CLIP_NAME=$(clip_base_name "$CURRENT_CLIP")
  LOOP=$(current_loop_display)
  UPDATED_AT=$(now_ms)
  JSON="{\"desiredState\":\"$(json_escape "$SESSION_STATE")\",\"displayedState\":\"$(json_escape "${CURRENT_STATE:-idle}")\",\"phase\":\"$(json_escape "$PHASE")\",\"currentLoopFamily\":\"$(json_escape "$CLIP_NAME")\",\"loopRepeatCount\":$LOOP,\"loopRepeatTarget\":${CURRENT_LOOP_TARGET:-1},\"sessionEvent\":\"$(json_escape "$SESSION_EVENT")\",\"updatedAtMs\":$UPDATED_AT}"
  write_runtime_file "$DEBUG_SCREEN_STATE_PATH" "$JSON"
  if debug_overlay_enabled; then
    write_runtime_file "$DEBUG_SPEECH_PATH" "$(build_debug_overlay_text)"
  else
    write_runtime_file "$DEBUG_SPEECH_PATH" ""
  fi
}

rand_n() {
  N="$1"
  if [ "$N" -le 1 ] 2>/dev/null; then
    echo 0
    return
  fi
  R=""
  if command -v hexdump >/dev/null 2>&1; then
    R=$(hexdump -n 2 -e '1/2 "%u"' /dev/urandom 2>/dev/null | tr -d ' ' | tr -d '\n')
  fi
  case "$R" in
    ""|*[!0-9]*)
      if command -v od >/dev/null 2>&1; then
        R=$(od -An -N2 -tu2 /dev/urandom 2>/dev/null | tr -d ' ' | tr -d '\n')
      fi
      ;;
  esac
  case "$R" in
    ""|*[!0-9]*) R=$(($(date +%s 2>/dev/null || echo 0) + $$));;
  esac
  echo $((R % N))
}

canonical_state() {
  RAW="${1:-}"
  case "$RAW" in
    welcome) echo "welcome";;
    idle|"") echo "idle";;
    working|active|thinking|tool_running|speaking) echo "working";;
    waiting_user|notification) echo "waiting_user";;
    done) echo "done";;
    error) echo "error";;
    touch|touch.*) echo "idle";;
    *) echo "idle";;
  esac
}

is_clip_state() {
  case "$1" in
    welcome|idle|working|waiting_user|done|error|touch) return 0;;
    *) return 1;;
  esac
}

touch_allowed_for_state() {
  case "$(canonical_state "$1")" in
    waiting_user|error) echo "no";;
    *) echo "yes";;
  esac
}

state_max_loops() {
  STATE="$1"
  case "$STATE" in
    idle) DEFAULT=5;;
    working) DEFAULT=5;;
    *) DEFAULT=1;;
  esac
  KEY=$(echo "$STATE" | tr 'abcdefghijklmnopqrstuvwxyz' 'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
  eval "VALUE=\${PET_CLAW_FB_${KEY}_MAX_LOOPS:-}"
  case "${VALUE:-}" in
    ""|*[!0-9]*) VALUE="$DEFAULT";;
  esac
  if [ "$VALUE" -lt 1 ] 2>/dev/null; then
    VALUE=1
  fi
  echo "$VALUE"
}

pick_loop_target() {
  MAX=$(state_max_loops "$1")
  if [ "$MAX" -le 1 ] 2>/dev/null; then
    echo 1
  else
    echo $((1 + $(rand_n "$MAX")))
  fi
}

find_clip_root() {
  if [ -n "${PET_CLAW_CLIPS_DIR:-}" ]; then
    echo "$PET_CLAW_CLIPS_DIR"
    return
  fi
  for candidate in "$DIR/terrier-clips" "$DIR/../terrier-clips"; do
    if [ -d "$candidate" ]; then
      echo "$candidate"
      return
    fi
  done
  echo "$DIR/terrier-clips"
}

clip_state_from_name() {
  NAME="$1"
  BASE=${NAME%.mp4}
  echo "${BASE%%.*}"
}

prepare_index_dir() {
  if [ -n "$INDEX_DIR" ] && [ -d "$INDEX_DIR" ]; then
    rm -rf "$INDEX_DIR"
  fi
  INDEX_DIR="${TMPDIR:-/tmp}/fb-display-index.$$"
  rm -rf "$INDEX_DIR"
  if ! mkdir -p "$INDEX_DIR" 2>/dev/null; then
    INDEX_DIR="$RUNTIME_ROOT/.fb-display-index.$$"
    rm -rf "$INDEX_DIR"
    if ! mkdir -p "$INDEX_DIR" 2>/dev/null; then
      log "cannot create clip index directory"
      exit 1
    fi
  fi
  for state in welcome idle working waiting_user done error touch; do
    : > "$INDEX_DIR/$state.list"
  done
}

scan_clips() {
  ROOT="$1"
  prepare_index_dir
  for file in "$ROOT"/*.mp4; do
    [ -f "$file" ] || continue
    NAME=$(basename "$file")
    STATE=$(clip_state_from_name "$NAME")
    if is_clip_state "$STATE"; then
      echo "$file" >> "$INDEX_DIR/$STATE.list"
    else
      log "ignoring unknown clip state: $NAME"
    fi
  done
}

clip_count() {
  STATE="$1"
  FILE="$INDEX_DIR/$STATE.list"
  if [ ! -f "$FILE" ]; then
    echo 0
    return
  fi
  wc -l < "$FILE" | tr -d ' '
}

clip_for_exact_name() {
  STATE="$1"
  NAME="$2"
  FILE="$INDEX_DIR/$STATE.list"
  if [ ! -f "$FILE" ]; then
    return 1
  fi
  while IFS= read -r CLIP_PATH; do
    if [ "$(basename "$CLIP_PATH")" = "$NAME" ]; then
      echo "$CLIP_PATH"
      return 0
    fi
  done < "$FILE"
  return 1
}

working_alternation_available() {
  clip_for_exact_name "working" "working.typing.mp4" >/dev/null 2>&1 &&
    clip_for_exact_name "working" "working.thinking.mp4" >/dev/null 2>&1
}

working_sequence_file() {
  if [ -n "${INDEX_DIR:-}" ]; then
    echo "$INDEX_DIR/working-sequence-next"
    return
  fi
  echo "$RUNTIME_ROOT/.fb-working-sequence-next"
}

working_sequence_next_variant() {
  NEXT=$(read_runtime_file "$(working_sequence_file)")
  case "$NEXT" in
    thinking) echo "thinking";;
    *) echo "typing";;
  esac
}

set_working_sequence_next_variant() {
  write_runtime_file "$(working_sequence_file)" "$1"
}

pick_working_sequence_clip() {
  NEXT_VARIANT=$(working_sequence_next_variant)
  if [ "$NEXT_VARIANT" = "thinking" ]; then
    CLIP_PATH=$(clip_for_exact_name "working" "working.thinking.mp4" || true)
    set_working_sequence_next_variant "typing"
  else
    CLIP_PATH=$(clip_for_exact_name "working" "working.typing.mp4" || true)
    set_working_sequence_next_variant "thinking"
  fi
  if [ -n "${CLIP_PATH:-}" ]; then
    echo "$CLIP_PATH"
    return
  fi
  pick_random_clip_for_state "working"
}

pick_random_clip_for_state() {
  STATE="$1"
  COUNT=$(clip_count "$STATE")
  if [ "$COUNT" -le 0 ] 2>/dev/null && [ "$STATE" != "idle" ]; then
    log "no clip for state=$STATE, falling back to idle"
    STATE="idle"
    COUNT=$(clip_count "$STATE")
  fi
  if [ "$COUNT" -le 0 ] 2>/dev/null; then
    echo ""
    return
  fi
  INDEX=$((1 + $(rand_n "$COUNT")))
  sed -n "${INDEX}p" "$INDEX_DIR/$STATE.list"
}

pick_clip_for_state() {
  case "$1" in
    touch|touch.*) STATE="touch";;
    *) STATE=$(canonical_state "$1");;
  esac
  if [ "$STATE" = "working" ] && working_alternation_available; then
    pick_working_sequence_clip
    return
  fi
  pick_random_clip_for_state "$STATE"
}

pick_loop_target_for_state() {
  STATE="$1"
  if [ "$STATE" = "working" ] && working_alternation_available; then
    echo 1
    return
  fi
  pick_loop_target "$STATE"
}

get_current_top_state() {
  STATE=$(read_runtime_file "$RUNTIME_ROOT/.current-state")
  canonical_state "$STATE"
}

resolve_checkpoint_state() {
  STATE=$(get_current_top_state)
  EVENT=$(read_runtime_file "$RUNTIME_ROOT/.current-event")
  WELCOME_MARKER=$(read_runtime_file "$WELCOME_TRIGGER_PATH")
  if [ -n "$WELCOME_MARKER" ] && [ "$WELCOME_MARKER" != "$LAST_WELCOME_TRIGGER_MARKER" ]; then
    LAST_WELCOME_TRIGGER_MARKER="$WELCOME_MARKER"
    CHECKPOINT_STATE="welcome"
    return
  fi
  case "$EVENT" in
    PairingWaiting|PairingApMode|Pairing*)
      if [ "$EVENT" != "PairingReady" ]; then
        WAS_PAIRING=1
      fi
      ;;
  esac
  if [ "$EVENT" = "PairingReady" ] && [ "$WAS_PAIRING" = "1" ]; then
    WAS_PAIRING=0
    CHECKPOINT_STATE="welcome"
    return
  fi
  CHECKPOINT_STATE="$STATE"
}

interrupt_changed() {
  MARKER=$(read_runtime_file "$RUNTIME_ROOT/.screen-interrupt")
  if [ -n "$MARKER" ] && [ "$MARKER" != "$LAST_INTERRUPT_MARKER" ]; then
    LAST_INTERRUPT_MARKER="$MARKER"
    return 0
  fi
  return 1
}

speech_changed() {
  NEXT_SPEECH_MARKER=$(speech_marker)
  [ -n "$CURRENT_SPEECH_MARKER" ] || return 1
  [ "$NEXT_SPEECH_MARKER" != "$CURRENT_SPEECH_MARKER" ]
}

clips_reload_changed() {
  MARKER=$(read_runtime_file "$CLIPS_RELOAD_PATH")
  if [ -n "$MARKER" ] && [ "$MARKER" != "$LAST_CLIPS_RELOAD_MARKER" ]; then
    LAST_CLIPS_RELOAD_MARKER="$MARKER"
    return 0
  fi
  return 1
}

touch_changed() {
  MARKER=$(read_runtime_file "$RUNTIME_ROOT/.touch-request")
  if [ -n "$MARKER" ] && [ "$MARKER" != "$LAST_TOUCH_MARKER" ]; then
    LAST_TOUCH_MARKER="$MARKER"
    return 0
  fi
  return 1
}

current_screen_page() {
  PAGE=$(read_runtime_file "$SCREEN_PAGE_PATH")
  case "$PAGE" in
    stats) echo "stats";;
    *) echo "main";;
  esac
}

screen_page_is_stats() {
  [ "$(current_screen_page)" = "stats" ]
}

render_stats_page() {
  # Prefer the persistent serve daemon (async, ~250ms/frame). The main loop's
  # 0.5s sleep paces it, so the old per-render throttle is gone — keeping the
  # negative screen (and its volume HUD) responsive. Fall back to a one-shot
  # spawn only if the daemon isn't running.
  if serve_cmd "render"; then
    return
  fi
  if [ ! -f "$STATS_RENDERER" ]; then
    log "stats renderer missing: $STATS_RENDERER"
    return
  fi
  command -v python3 >/dev/null 2>&1 || return
  python3 "$STATS_RENDERER" "$RUNTIME_ROOT" --fb "$FB_DEV" --rotate "$FB_ROTATE" >/dev/null 2>&1 || true
}

kill_all_players() {
  stop_tplayer
  killall tplayerdemo 2>/dev/null || true
  killall -9 tplayerdemo 2>/dev/null || true
}

stop_tplayer() {
  if [ -n "${TPLAYER_PID:-}" ]; then
    (printf "quit\n" >&3) 2>/dev/null || true
    sleep 0.2
    kill "$TPLAYER_PID" 2>/dev/null || true
    kill -9 "$TPLAYER_PID" 2>/dev/null || true
    wait "$TPLAYER_PID" 2>/dev/null || true
    TPLAYER_PID=""
  fi
  exec 3>&- || true
  rm -f "$TPLAYER_FIFO"
}

kill_player_pid() {
  PID="$1"
  if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null || true
    kill -9 "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
}

create_tplayer_fifo() {
  rm -f "$TPLAYER_FIFO"
  if command -v mkfifo >/dev/null 2>&1; then
    mkfifo "$TPLAYER_FIFO" 2>/dev/null && return 0
  fi
  mknod "$TPLAYER_FIFO" p 2>/dev/null
}

start_tplayer() {
  if [ -n "${TPLAYER_PID:-}" ] && kill -0 "$TPLAYER_PID" 2>/dev/null; then
    return 0
  fi
  if ! create_tplayer_fifo; then
    log "failed to create tplayer fifo: $TPLAYER_FIFO"
    return 1
  fi
  : > "$TPLAYER_LOG" 2>/dev/null || true
  tplayerdemo <> "$TPLAYER_FIFO" >> "$TPLAYER_LOG" 2>&1 &
  TPLAYER_PID=$!
  exec 3<>"$TPLAYER_FIFO"
  sleep "$TPLAYER_READY_SECONDS"
}

send_tplayer_command() {
  COMMAND="$1"
  if ! (printf "%s\n" "$COMMAND" >&3) 2>/dev/null; then
    log "tplayer command failed, restarting player"
    stop_tplayer
    start_tplayer || return 1
    (printf "%s\n" "$COMMAND" >&3) 2>/dev/null || return 1
  fi
}

play_clip_once() {
  PLAY_FILE="$1"
  PLAY_LABEL="$2"
  if [ ! -f "$PLAY_FILE" ]; then
    log "missing clip: $PLAY_FILE"
    sleep 1
    return 1
  fi
  PLAY_SECONDS=$(clip_playback_seconds "$PLAY_FILE")
  if ! command -v tplayerdemo >/dev/null 2>&1; then
    PLAY_SECONDS=$(ffmpeg_playback_seconds "$PLAY_SECONDS")
  fi
  TARGET_TICKS=$(seconds_to_ticks "$PLAY_SECONDS")

  write_state_speech_for_display
  play_state_audio_cue "$PLAY_FILE" "$CURRENT_STATE"
  CURRENT_SPEECH_MARKER=$(speech_marker)
  write_screen_debug "$PLAY_LABEL"
  log "play state=$PLAY_LABEL seconds=$PLAY_SECONDS clip=$(basename "$PLAY_FILE")"
  if ! command -v tplayerdemo >/dev/null 2>&1; then
    if command -v ffmpeg >/dev/null 2>&1 && [ -c $FB_DEV ]; then
      # round=near honors sprite hold PTS; neighbor scaling avoids soft halos.
      PIX_FMT=$(fb_rawvideo_pixel_format)
      DRAW_FILTER="setpts=PTS-STARTPTS,fps=$FFMPEG_OUTPUT_FPS:round=near,$(fb_video_scale_filter)"
      append_speech_draw_filter
      BLIT_ROTATE="$FB_ROTATE"
      ROTATE_FILTER=$(fb_video_rotation_filter)
      if [ -n "$ROTATE_FILTER" ]; then
        DRAW_FILTER="$DRAW_FILTER$ROTATE_FILTER"
        BLIT_ROTATE=0
      fi
      DRAW_FILTER="$DRAW_FILTER,format=$PIX_FMT"
      FB_W="${PET_CLAW_FB_WIDTH:-$FB_WIDTH}"
      FB_H="${PET_CLAW_FB_HEIGHT:-$FB_HEIGHT}"
      (
        ffmpeg -hide_banner -loglevel quiet -nostdin -stream_loop -1 -i "$PLAY_FILE" \
          -vf "$DRAW_FILTER" \
          -f rawvideo - | python3 "$DIR/fb-rawvideo-blit.py" "$FB_DEV" "$FB_W" "$FB_H" "$FFMPEG_OUTPUT_FPS" "$BLIT_ROTATE"
      ) &
      PLAYER_PID=$!
      STOP_PLAYER=0
      TICKS=0
      while kill -0 "$PLAYER_PID" 2>/dev/null; do
        if volume_is_fresh; then
          pkill -P "$PLAYER_PID" 2>/dev/null || true
          kill "$PLAYER_PID" 2>/dev/null || true
          wait "$PLAYER_PID" 2>/dev/null || true
          log "volume HUD requested during $PLAY_LABEL; releasing $FB_DEV"
          return 124
        fi
        if screen_page_is_stats; then
          pkill -P "$PLAYER_PID" 2>/dev/null || true
          kill "$PLAYER_PID" 2>/dev/null || true
          wait "$PLAYER_PID" 2>/dev/null || true
          log "stats page activated during $PLAY_LABEL; releasing $FB_DEV"
          return 125
        fi
        if clips_reload_changed; then
          pkill -P "$PLAYER_PID" 2>/dev/null || true
          kill "$PLAYER_PID" 2>/dev/null || true
          wait "$PLAYER_PID" 2>/dev/null || true
          log "clip reload received during $PLAY_LABEL"
          return 127
        fi
        if interrupt_changed; then
          pkill -P "$PLAYER_PID" 2>/dev/null || true
          kill "$PLAYER_PID" 2>/dev/null || true
          wait "$PLAYER_PID" 2>/dev/null || true
          log "interrupt received during $PLAY_LABEL"
          return 125
        fi
        if touch_changed; then
          if [ "$(touch_allowed_for_state "$(get_current_top_state)")" = "yes" ]; then
            pkill -P "$PLAYER_PID" 2>/dev/null || true
            kill "$PLAYER_PID" 2>/dev/null || true
            wait "$PLAYER_PID" 2>/dev/null || true
            log "touch request received during $PLAY_LABEL"
            return 126
          fi
        fi
        if speech_changed; then
          pkill -P "$PLAYER_PID" 2>/dev/null || true
          kill "$PLAYER_PID" 2>/dev/null || true
          wait "$PLAYER_PID" 2>/dev/null || true
          log "speech changed during $PLAY_LABEL"
          return 125
        fi
        NEXT_TOP_STATE=$(get_current_top_state)
        if [ "$NEXT_TOP_STATE" != "$CURRENT_STATE" ]; then
          pkill -P "$PLAYER_PID" 2>/dev/null || true
          kill "$PLAYER_PID" 2>/dev/null || true
          wait "$PLAYER_PID" 2>/dev/null || true
          log "state changed during $PLAY_LABEL: $CURRENT_STATE -> $NEXT_TOP_STATE"
          return 125
        fi
        sleep 0.2
        TICKS=$((TICKS + 1))
        if [ "$TARGET_TICKS" -gt 0 ] 2>/dev/null && [ "$TICKS" -ge "$TARGET_TICKS" ]; then
          STOP_PLAYER=1
          break
        fi
      done
      if [ "$STOP_PLAYER" = "1" ]; then
        pkill -P "$PLAYER_PID" 2>/dev/null || true
        kill "$PLAYER_PID" 2>/dev/null || true
      fi
      wait "$PLAYER_PID" 2>/dev/null || true
      return 0
    fi
    log "no framebuffer video player available, cannot play: $PLAY_FILE"
    sleep 1
    return 1
  fi
  start_tplayer || return 1
  send_tplayer_command "set loop:0" || return 1
  send_tplayer_command "play url:$PLAY_FILE" || return 1
  TICKS=0
  while [ "$TARGET_TICKS" -le 0 ] || [ "$TICKS" -lt "$TARGET_TICKS" ]; do
    if screen_page_is_stats; then
      log "stats page activated during $PLAY_LABEL; releasing $FB_DEV to overlay"
      return 125
    fi
    if [ -n "${TPLAYER_PID:-}" ] && ! kill -0 "$TPLAYER_PID" 2>/dev/null; then
      log "tplayer exited during $PLAY_LABEL: $PLAY_FILE"
      TPLAYER_PID=""
      return 126
    fi
    if clips_reload_changed; then
      log "clip reload received during $PLAY_LABEL"
      return 127
    fi
    if interrupt_changed; then
      if screen_page_is_stats; then
        log "interrupt ignored on stats page during $PLAY_LABEL"
      else
        log "interrupt received during $PLAY_LABEL"
        return 125
      fi
    fi
    if touch_changed; then
      if screen_page_is_stats; then
        log "touch consumed on stats page during $PLAY_LABEL (no clip)"
      elif [ "$(touch_allowed_for_state "$(get_current_top_state)")" = "yes" ]; then
        log "touch request received during $PLAY_LABEL"
        return 126
      else
        log "touch ignored while state=$(get_current_top_state)"
      fi
    fi
    if speech_changed; then
      log "speech changed during $PLAY_LABEL"
      return 125
    fi
    sleep 0.2
    TICKS=$((TICKS + 1))
  done
  return 0
}

enter_state() {
  TARGET=$(canonical_state "$1")
  CLIP=$(pick_clip_for_state "$TARGET")
  if [ -z "$CLIP" ]; then
    log "no playable clip for $TARGET; retrying after sleep"
    sleep 1
    TARGET="idle"
    CLIP=$(pick_clip_for_state "$TARGET")
  fi
  CURRENT_STATE="$TARGET"
  CURRENT_CLIP="$CLIP"
  CURRENT_LOOP_TARGET=$(pick_loop_target_for_state "$CURRENT_STATE")
  CURRENT_LOOP_COUNT=0
  CURRENT_AUDIO_CUE_KEY=""
  stop_state_audio
  log "enter state=$CURRENT_STATE target_loops=$CURRENT_LOOP_TARGET clip=$(basename "${CURRENT_CLIP:-none}")"
}

advance_same_state() {
  COUNT=$(clip_count "$CURRENT_STATE")
  if [ "$COUNT" -le 1 ] 2>/dev/null; then
    return
  fi
  CURRENT_LOOP_COUNT=$((CURRENT_LOOP_COUNT + 1))
  if [ "$CURRENT_LOOP_COUNT" -lt "$CURRENT_LOOP_TARGET" ]; then
    return
  fi
  CURRENT_CLIP=$(pick_clip_for_state "$CURRENT_STATE")
  CURRENT_LOOP_TARGET=$(pick_loop_target_for_state "$CURRENT_STATE")
  CURRENT_LOOP_COUNT=0
  log "variant next state=$CURRENT_STATE target_loops=$CURRENT_LOOP_TARGET clip=$(basename "$CURRENT_CLIP")"
}

play_touch_sequence() {
  TOP_STATE=$(get_current_top_state)
  if [ "$(touch_allowed_for_state "$TOP_STATE")" != "yes" ]; then
    log "touch ignored before playback while state=$TOP_STATE"
    return
  fi
  TOUCH_CLIP=$(pick_clip_for_state "touch")
  if [ -z "$TOUCH_CLIP" ]; then
    log "touch requested but no touch clips are available"
    return
  fi
  PREV_STATE="$CURRENT_STATE"
  PREV_CLIP="$CURRENT_CLIP"
  PREV_LOOP_TARGET="$CURRENT_LOOP_TARGET"
  PREV_LOOP_COUNT="$CURRENT_LOOP_COUNT"
  CURRENT_STATE="touch"
  CURRENT_CLIP="$TOUCH_CLIP"
  CURRENT_LOOP_TARGET=1
  CURRENT_LOOP_COUNT=0
  play_clip_once "$TOUCH_CLIP" "touch"
  RT=$?
  CURRENT_STATE="$PREV_STATE"
  CURRENT_CLIP="$PREV_CLIP"
  CURRENT_LOOP_TARGET="$PREV_LOOP_TARGET"
  CURRENT_LOOP_COUNT="$PREV_LOOP_COUNT"
  if [ "$RT" -eq 125 ]; then
    return 125
  fi
  return 0
}

cache_clip_file() {
  SRC="$1"
  DEST="$2"
  if command -v ffmpeg >/dev/null 2>&1; then
    # Scale to the panel only — minterpolate blends discrete sprite frames and
    # makes codex-import clips look like they are stuttering between holds.
    CACHE_FILTER="$(fb_video_scale_filter),format=yuv420p"
    if ffmpeg -hide_banner -loglevel error -nostdin -y -i "$SRC" \
      -vf "$CACHE_FILTER" \
      -an -c:v libx264 -preset ultrafast -tune zerolatency -crf 18 -movflags +faststart "$DEST"; then
      return 0
    fi
    log "smooth cache failed for $(basename "$SRC"); using original clip"
  fi
  cp "$SRC" "$DEST" 2>/dev/null || true
}

prepare_video_root() {
  VIDEO_ROOT_DISK=$(find_clip_root)
  if [ ! -d "$VIDEO_ROOT_DISK" ]; then
    log "clip directory not found: $VIDEO_ROOT_DISK"
  fi
  if [ "${PET_CLAW_FB_DISABLE_CACHE:-0}" = "1" ]; then
    VIDEO_ROOT="$VIDEO_ROOT_DISK"
    return
  fi
  rm -rf "$VIDEO_CACHE"
  mkdir -p "$VIDEO_CACHE"
  if [ -d "$VIDEO_ROOT_DISK" ]; then
    for SRC in "$VIDEO_ROOT_DISK"/*.mp4; do
      [ -e "$SRC" ] || continue
      DEST="$VIDEO_CACHE/$(basename "$SRC")"
      cache_clip_file "$SRC" "$DEST"
    done
    for SRC in "$VIDEO_ROOT_DISK"/*.wav; do
      [ -e "$SRC" ] || continue
      cp "$SRC" "$VIDEO_CACHE/$(basename "$SRC")" 2>/dev/null || true
    done
  fi
  echo "$VIDEO_ROOT_DISK" > "$VIDEO_CACHE/.source"
  touch "$VIDEO_CACHE/.cached"
  VIDEO_ROOT="$VIDEO_CACHE"
  log "cache ready: $VIDEO_ROOT"
}

reload_clips() {
  log "clip reload requested"
  kill_all_players
  prepare_video_root
  scan_clips "$VIDEO_ROOT"
}

init_display() {
  if ! command -v tplayerdemo >/dev/null 2>&1; then
    return
  fi
  log "initializing persistent display engine"
  start_tplayer || true
}

self_test() {
  VIDEO_ROOT="$1"
  scan_clips "$VIDEO_ROOT"
  for state in welcome idle working waiting_user done error touch; do
    echo "state $state count=$(clip_count "$state") max=$(state_max_loops "$state")"
  done
  echo "canonical tool_running=$(canonical_state tool_running)"
  echo "canonical notification=$(canonical_state notification)"
  echo "canonical touch.lick=$(canonical_state touch.lick)"
  echo "speech idle.playing=$(state_speech_text idle idle.playing)"
  echo "rand sample=$(rand_n 3)"
  WORKING_PICK=$(pick_clip_for_state "working")
  echo "pick working name=$(basename "$WORKING_PICK")"
  WORKING_PICK_NEXT=$(pick_clip_for_state "working")
  echo "pick working next name=$(basename "$WORKING_PICK_NEXT")"
  TOUCH_PICK=$(pick_clip_for_state "touch")
  echo "pick touch name=$(basename "$TOUCH_PICK")"
  echo "pick touch state=$(clip_state_from_name "$(basename "$TOUCH_PICK")")"
  echo "touch_allowed waiting_user=$(touch_allowed_for_state waiting_user)"
  echo "touch_allowed error=$(touch_allowed_for_state error)"
  echo "touch_allowed working=$(touch_allowed_for_state working)"
}

cleanup() {
  rm -f "$LOCKFILE"
  if [ -n "$INDEX_DIR" ] && [ -d "$INDEX_DIR" ]; then
    rm -rf "$INDEX_DIR"
  fi
  kill_all_players
  stop_state_audio
  stop_stats_serve
}

if [ "${1:-}" = "--self-test" ]; then
  self_test "${2:-$(find_clip_root)}"
  exit 0
fi

if [ "${1:-}" = "--debug-text" ]; then
  RUNTIME_ROOT="${2:-$RUNTIME_ROOT}"
  DEBUG_OVERLAY_FLAG="$RUNTIME_ROOT/.debug-overlay-enabled"
  DEBUG_SCREEN_STATE_PATH="$RUNTIME_ROOT/.debug-screen-state.json"
  DEBUG_SPEECH_PATH="$RUNTIME_ROOT/.current-debug-speech"
  CURRENT_STATE="${3:-idle}"
  CURRENT_CLIP="${4:-}"
  CURRENT_LOOP_COUNT=$((${5:-1} - 1))
  CURRENT_LOOP_TARGET="${6:-1}"
  build_debug_overlay_text
  exit 0
fi

if [ "${1:-}" = "--speech-filter" ]; then
  RUNTIME_ROOT="${2:-$RUNTIME_ROOT}"
  CURRENT_STATE="${3:-working}"
  FB_WIDTH="${4:-$FB_WIDTH}"
  FB_HEIGHT="${5:-$FB_HEIGHT}"
  SPEECH_PATH="$RUNTIME_ROOT/.current-speech"
  SPEECH_RENDER_PATH="$RUNTIME_ROOT/.current-speech-render"
  SPEECH_HOLD_UNTIL_PATH="$RUNTIME_ROOT/.current-speech-hold-until"
  DRAW_FILTER="base"
  append_speech_draw_filter
  printf "%s\n" "$DRAW_FILTER"
  exit 0
fi

if [ "${1:-}" = "--duration" ]; then
  clip_playback_seconds "${2:-}"
  exit 0
fi

if [ -f "$LOCKFILE" ]; then
  OLD_PID=$(cat "$LOCKFILE" 2>/dev/null || true)
  if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
    log "another instance is running (pid=$OLD_PID), exiting"
    exit 1
  fi
fi
echo $$ > "$LOCKFILE"
trap "cleanup; exit 0" INT TERM EXIT

killall lv_examples 2>/dev/null || true
kill_all_players
prepare_video_root
scan_clips "$VIDEO_ROOT"
init_display
dd if=/dev/zero of="$FB_DEV" bs=4096 count=512 2>/dev/null || true

LAST_INTERRUPT_MARKER=$(read_runtime_file "$RUNTIME_ROOT/.screen-interrupt")
LAST_TOUCH_MARKER=$(read_runtime_file "$RUNTIME_ROOT/.touch-request")
LAST_WELCOME_TRIGGER_MARKER=$(read_runtime_file "$WELCOME_TRIGGER_PATH")
LAST_CLIPS_RELOAD_MARKER=$(read_runtime_file "$CLIPS_RELOAD_PATH")
log "starting display driver (pid=$$ clips=$VIDEO_ROOT)"
log "$FB_DEV info: bpp=$(cat /sys/class/graphics/fb${FB_NUM}/bits_per_pixel 2>/dev/null) size=$(cat /sys/class/graphics/fb${FB_NUM}/virtual_size 2>/dev/null) stride=$(cat /sys/class/graphics/fb${FB_NUM}/stride 2>/dev/null)"

resolve_checkpoint_state
enter_state "$CHECKPOINT_STATE"

# Persistent stats/HUD renderer (amortizes python+PIL+numpy startup). Falls back
# to per-frame spawns inside render_stats_page if it can't start.
start_stats_serve || log "stats serve unavailable; per-frame spawn fallback"

while true; do
  # Pet-screen volume HUD: when the rotary handler just wrote .volume-display and
  # we're on the pet animation, pause it and let the serve daemon draw the same
  # Pillow volume HUD as the negative screen, until the write goes stale.
  if volume_is_fresh && ! screen_page_is_stats; then
    kill_all_players
    while volume_is_fresh && ! screen_page_is_stats; do
      serve_cmd "hud" || break
      sleep 0.25
    done
    resolve_checkpoint_state
    enter_state "$CHECKPOINT_STATE"
    continue
  fi

  if screen_page_is_stats; then
    if [ -n "${TPLAYER_PID:-}" ] && kill -0 "$TPLAYER_PID" 2>/dev/null; then
      log "screen-page=stats; stopping tplayer to release $FB_DEV to overlay"
      stop_tplayer
    fi
    render_stats_page
    # 消化潜在的 .touch-request / .screen-interrupt 触发，避免切回 main 后被旧事件打断
    touch_changed >/dev/null 2>&1 || true
    interrupt_changed >/dev/null 2>&1 || true
    sleep 0.5
    continue
  fi

  if [ -z "$CURRENT_CLIP" ]; then
    resolve_checkpoint_state
    enter_state "$CHECKPOINT_STATE"
  fi

  if clips_reload_changed; then
    reload_clips
    resolve_checkpoint_state
    enter_state "$CHECKPOINT_STATE"
    continue
  fi

  if touch_changed; then
    if screen_page_is_stats; then
      log "touch consumed on stats page (no clip switch)"
    elif [ "$(touch_allowed_for_state "$(get_current_top_state)")" = "yes" ]; then
      play_touch_sequence
      resolve_checkpoint_state
      enter_state "$CHECKPOINT_STATE"
      continue
    else
      log "touch ignored before clip while state=$(get_current_top_state)"
    fi
  fi

  play_clip_once "$CURRENT_CLIP" "$CURRENT_STATE"
  RESULT=$?

  if [ "$RESULT" -eq 124 ]; then
    # volume HUD requested mid-clip — loop back; the top-of-loop block draws it.
    continue
  fi

  if [ "$RESULT" -eq 125 ]; then
    resolve_checkpoint_state
    enter_state "$CHECKPOINT_STATE"
    continue
  fi

  if [ "$RESULT" -eq 126 ]; then
    play_touch_sequence
    resolve_checkpoint_state
    enter_state "$CHECKPOINT_STATE"
    continue
  fi

  if [ "$RESULT" -eq 127 ]; then
    reload_clips
    resolve_checkpoint_state
    enter_state "$CHECKPOINT_STATE"
    continue
  fi

  if screen_page_is_stats; then
    # 统计页激活时桌宠 idle 循环不响应 .current-state 变化，让用户专注阅读统计文本
    if [ "$CURRENT_STATE" != "idle" ]; then
      enter_state "idle"
    else
      advance_same_state
    fi
    continue
  fi

  resolve_checkpoint_state
  NEXT_STATE="$CHECKPOINT_STATE"
  if [ "$NEXT_STATE" != "$CURRENT_STATE" ]; then
    enter_state "$NEXT_STATE"
  else
    advance_same_state
  fi
done
