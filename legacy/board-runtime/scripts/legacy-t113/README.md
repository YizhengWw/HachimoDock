# Legacy T113 deployment scripts

This directory archives the **Allwinner T113 armhf** cross-compile + deployment
chain. The active device is now a **Raspberry Pi Zero 2 W (aarch64)** which
builds on-device via `scripts/deploy-rpi.sh`; the T113 board is retired.

Archived 2026-06-01 to keep the main `scripts/` directory focused on the Pi
flow. The history is preserved via `git mv` so blame still works.

## What's here

| File | Role |
|---|---|
| `build-armhf.sh` | Host-side cross-compile to `build-armhf/` using a Docker armhf toolchain |
| `deploy-armhf.sh` | Bundle `build-armhf/` + tree, push to T113 board over SSH |
| `stage-to-sdcard.sh` | Stage tree + armhf binaries onto an SD card for offline bootstrap |
| `serial-deploy-update.py` | Serial-port-only fallback updater for a T113 board with no network |
| `build-armhf/` | Last cross-compiled armhf binaries (2026-05-29 snapshot) |

## If you need to revive T113 support

These scripts were authored assuming they live in `board-runtime/scripts/` with
`build-armhf/` at `board-runtime/build-armhf/`. They use a relative
`HERE="$(... "$(dirname -- "$0")/.." ...)"` idiom that now resolves one level
too shallow. To re-activate cleanly, either:

1. Move scripts and `build-armhf/` back to their original locations, OR
2. Update each script's `HERE` to add one more `/..`, and update
   `BUILD_DIR="${BUILD_DIR:-build-armhf}"` to the new path.

Either way, the T113 hardware-specific paths (`/mnt/UDISK/board-runtime`,
procd instead of systemd) also need to be re-verified — refer to git history
before 2026-06-01 for the last fully-tested wiring.
