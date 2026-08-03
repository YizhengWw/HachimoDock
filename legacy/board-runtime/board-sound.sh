#!/bin/sh
# Board-side short sound effects.
# Usage: sh board-sound.sh task_done [runtime-root]
set -eu

ACTION="${1:-task_done}"
ROOT="${2:-/opt/board-runtime}"
DEV="${PET_TASK_DONE_SOUND_DEV:-plughw:0,0}"
VOLUME="${PET_TASK_DONE_SOUND_VOLUME:-0.18}"
CACHE_DIR="${PET_TASK_DONE_SOUND_CACHE_DIR:-/tmp/board-runtime-sounds}"
VARIANTS="${PET_TASK_DONE_SOUND_VARIANTS:-sparkle,rise,bell,pop,soft}"

log() {
  printf '[board-sound] %s\n' "$*" >&2
}

case "$ACTION" in
  task_done) ;;
  *) log "unknown action: $ACTION"; exit 2 ;;
esac

if ! command -v aplay >/dev/null 2>&1; then
  log "aplay not found"
  exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
  log "python3 not found"
  exit 0
fi

mkdir -p "$CACHE_DIR" 2>/dev/null || true

TASK_DONE_WAV="$(python3 - "$CACHE_DIR" "$VOLUME" "$VARIANTS" <<'PY'
import math
import os
import random
import struct
import sys
import tempfile
import wave

cache_dir = sys.argv[1]
try:
    volume = float(sys.argv[2])
except Exception:
    volume = 0.18
volume = max(0.02, min(volume, 0.8))
variant_text = sys.argv[3] if len(sys.argv) > 3 else ""

ALL_VARIANTS = ("sparkle", "rise", "bell", "pop", "soft")
requested = [
    item.strip().lower()
    for item in variant_text.replace(";", ",").split(",")
    if item.strip()
]
variants = [item for item in requested if item in ALL_VARIANTS]
if not variants or "random" in requested:
    variants = list(ALL_VARIANTS)
variant = random.SystemRandom().choice(variants)

rate = 48000
amp = int(2147483647 * volume)
path = os.path.join(cache_dir, f"task-done-{variant}-s32.wav")

def sample_frame(value):
    value = max(-2147483648, min(2147483647, int(value)))
    return struct.pack("<ii", value, value)

def write_silence(wav, seconds):
    frames = int(rate * seconds)
    zero = sample_frame(0)
    for _ in range(frames):
        wav.writeframes(zero)

def write_tone(wav, freqs, seconds, gain=1.0, fade_in=0.012, fade_out=0.035):
    if isinstance(freqs, (int, float)):
        freqs = (float(freqs),)
    else:
        freqs = tuple(float(freq) for freq in freqs)
    frames = int(rate * seconds)
    scale = amp * gain / max(1, len(freqs))
    for index in range(frames):
        t = index / rate
        fade = min(1.0, t / fade_in if fade_in > 0 else 1.0)
        fade = min(fade, (seconds - t) / fade_out if fade_out > 0 else 1.0)
        value = 0.0
        for freq in freqs:
            value += math.sin(2 * math.pi * freq * t)
            value += 0.18 * math.sin(2 * math.pi * freq * 2.0 * t)
        wav.writeframes(sample_frame(scale * fade * value))

def render_sparkle(wav):
    for freq, seconds in ((660.0, 0.12), (880.0, 0.12), (1320.0, 0.20)):
        write_tone(wav, freq, seconds, gain=0.92)
        write_silence(wav, 0.024)

def render_rise(wav):
    for freq in (523.25, 659.25, 783.99, 1046.50):
        write_tone(wav, freq, 0.085, gain=0.78, fade_out=0.025)
        write_silence(wav, 0.014)
    write_tone(wav, (1046.50, 1318.51), 0.18, gain=0.65)

def render_bell(wav):
    write_tone(wav, (783.99, 987.77, 1174.66), 0.20, gain=0.72, fade_out=0.08)
    write_silence(wav, 0.035)
    write_tone(wav, (987.77, 1318.51), 0.26, gain=0.62, fade_out=0.12)

def render_pop(wav):
    write_tone(wav, 1174.66, 0.055, gain=0.82, fade_out=0.018)
    write_silence(wav, 0.028)
    write_tone(wav, 1567.98, 0.060, gain=0.76, fade_out=0.018)
    write_silence(wav, 0.040)
    write_tone(wav, 1046.50, 0.13, gain=0.66)

def render_soft(wav):
    write_tone(wav, (587.33, 739.99), 0.16, gain=0.56, fade_out=0.065)
    write_silence(wav, 0.026)
    write_tone(wav, (739.99, 987.77), 0.22, gain=0.52, fade_out=0.10)

renderers = {
    "sparkle": render_sparkle,
    "rise": render_rise,
    "bell": render_bell,
    "pop": render_pop,
    "soft": render_soft,
}

def render(target_path):
    fd, tmp_path = tempfile.mkstemp(
        prefix=f".task-done-{variant}-",
        suffix=".wav",
        dir=cache_dir,
    )
    os.close(fd)
    try:
        with wave.open(tmp_path, "wb") as wav:
            wav.setnchannels(2)
            wav.setsampwidth(4)
            wav.setframerate(rate)
            renderers[variant](wav)
        os.replace(tmp_path, target_path)
    finally:
        try:
            os.unlink(tmp_path)
        except FileNotFoundError:
            pass

if not os.path.exists(path) or os.path.getsize(path) <= 0:
    render(path)

print(path)
PY
)"

aplay -q -D "$DEV" "$TASK_DONE_WAV" >/dev/null 2>&1 || {
  log "play failed dev=$DEV"
  exit 0
}
