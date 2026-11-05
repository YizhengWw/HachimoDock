# M5Stack StopWatch v1.0 — upstream board adaptation

Status: experimental; display, touch, both keys and vibration verified on one
physical StopWatch v1.0. See the [hardware acceptance record](m5stack-stopwatch-hil-2026-11-05.md).
This is an additional HachimoDock board target, not a replacement UI stack.
The original P4 environments, dispatcher, renderer and release images remain
separate. Never flash a P4 image onto StopWatch.

## Implemented scope

- ESP32-S3, 16 MiB Flash, 8 MiB OPI PSRAM.
- ESP-IDF esp_lcd CO5300 QSPI driver and 466×466 RGB565 framebuffer.
- Existing LVGL 9.4 font data and TJpgDec; no LVGL widget tree, Arduino,
  M5GFX, Mooncake or smooth_ui_toolkit runtime.
- Existing west-highland-terrier source videos, converted to bounded 240×240,
  8 FPS factory MJPEG frames. The generated embedded index is not a USB format.
- HachimoDock hello/ack, state/*, speech/text, session/current, input/event,
  protocol/ack, heartbeat and basic diagnostics.
- Eight bounded session cards; one visible card; revision-aware terminal TTL,
  stable ordering, Agent isolation and exact-ID previous/next selection.
- Physical keys, horizontal swipe, brightness presets and 120 ms vibration.
- Scoped on-board diagnostics, RGB/border and touch-grid patterns, input counters
  and motor/display/power readback. Diagnostic display leases expire after 60 s.
- Desktop S3 identification and the existing channel/session follow workflow.
  Select the built-in westie in the desktop channel card; fixed-device follow
  skips P4 appearance mutation and preserves board/connection/boot checks.

Audio, configurable button mappings, custom appearance transfer, PetUI
components, screenshots, sleep and in-application OTA are not implemented.
The device advertises disabled capabilities; the desktop hides unsupported
voice/button controls. The ordinary P4 component/OTA routes reject S3.
Application/resource A/B partitions reserve space for later work and do not
constitute an implemented OTA or resource transaction.

## Source boundaries

| Location | Responsibility |
| --- | --- |
| firmware/CMakeLists.txt | Explicit board selection and isolated component graph |
| firmware/profiles/m5stack_stopwatch_v1 | S3 defaults and 16 MiB partition table |
| firmware/targets/stopwatch/components/board_stopwatch | I2C/PM1/IOE1, CO5300, CST820B and keys; excluded from P4 discovery |
| firmware/targets/stopwatch/main | Single-owner state loop, USB RX task, input task and round renderer |
| firmware/main/pet_device_sessions.* | Hardware-independent bounded JSON session model |
| firmware/main/pet_device_lifecycle.h | Shared P4/S3 lifecycle vocabulary |
| firmware/main/pet_p4_input_core.* | Existing physical button decoder, reused unchanged |
| firmware/main/pet_p4_font_cn.c | Existing MiSans glyphs, reused with original license |
| firmware/tools/build_stopwatch_assets.py | Build-time conversion of existing factory videos |
| pc/src/lib/device-capabilities.js | Explicit session/fixed-appearance capabilities |

The P4 dispatcher retains its extended behavior. Migrating its session state to
the bounded device module is a separate refactor, not a prerequisite for this
board. New boards do not imply automatic compatibility with P4-only components.

## Build

FFmpeg must be installed and on PATH. The first configuration generates about
350 KiB of factory animation into the build directory; it is not committed.
Use a separate build directory and sdkconfig for every target. Do not configure
P4 and S3 simultaneously in one checkout: managed_components is shared.

Native ESP-IDF 5.5.x (verified locally with 5.5.5; PlatformIO/CI use 5.5.4):

~~~sh
cd firmware
. "$IDF_PATH/export.sh"
idf.py -B build/stopwatch \
  -DHACHIMO_BOARD=m5stack_stopwatch_v1 \
  -DSDKCONFIG=build/stopwatch/sdkconfig build
~~~

PlatformIO Core 6.1.19 / Python 3.10+ uses the same pioarduino platform as the
existing P4-v3 environment, with a separate S3 board and no P4 post-build scripts:

~~~sh
cd firmware
python -m platformio run -e m5stack_stopwatch_v1
~~~

The application must fit into 0x300000 bytes. Default native output is
firmware/build/stopwatch/hachimo_stopwatch.bin. The initial native build was
approximately 1.4 MiB; actual size is checked by ESP-IDF on every build.
The S3 lockfile is dependencies.stopwatch.lock; do not replace the P4 lockfiles.

## Backup and flashing

Back up the current 16 MiB factory flash with esptool before the first install.
Use the installed esptool version's read-flash/read_flash command; keep that
backup outside the repository. StopWatch v1.0 must not use a P4 flashing bundle.

Connect a data-capable USB-C cable. Hold the power key for about two seconds
until the green LED lights to enter download mode. Check the detected chip and
Flash size, then flash using the selected build directory:

~~~sh
cd firmware
idf.py -B build/stopwatch -p PORT flash
~~~

Replace PORT with the enumerated USB port. The application image alone is not
a full first-install image: the matching bootloader, partition table and OTA
metadata must also be written. idf.py uses the generated flash arguments.
First installation changes the layout and may overwrite existing settings.

The USB serial/JTAG port is used for protocol data; ordinary firmware logs use
UART0, not the protocol stream. Close serial monitors before opening Pet Manager.

## Board-specific notes

- Official v1.0 warning: some stickers mark an external pin BAT although its
  actual function is 5V IN. Never connect a lithium cell to that pin. v1.0.1
  changed that pin; this target is for v1.0.
- System I2C uses SDA47/SCL48. IOE1 is probed at 0x4F, then 0x6F; the address
  does not identify a hardware revision. PM1 is 0x6E and CST820B is 0x15.
- The display uses CLK40, CS39, D0/1/2/3 = 41/42/46/45, a conservative 40 MHz
  QSPI clock and horizontal gap 6 from the manufacturer's window configuration.
- Key A/yellow is GPIO2, key B/blue GPIO1. Short press: next/previous session.
  Long press: brightness 160/64. Power key reset/download/shutdown behavior is
  left under hardware control; charging settings are preserved.
- Touch coordinates and display window/gap require physical calibration.
- A display DMA timeout latches display failure so a possibly in-flight stripe
  is never reused. USB/state processing continues; reboot to recover display.
- This version stays awake while running and does not claim battery life.

Manufacturer references: [hardware documentation](https://docs.m5stack.com/zh_CN/core/StopWatch),
[official demo](https://github.com/m5stack/M5StopWatch-UserDemo),
[M5IOE1](https://github.com/m5stack/M5IOE1),
[M5PM1](https://github.com/m5stack/M5PM1).

## Verification

~~~sh
python -m pytest firmware/tests/test_stopwatch_sessions.py -q
python firmware/tests/protocol_contract_test.py
cd pc
npm ci
npm test
npm run build:web
~~~

For a source-only Rust check, without downloading release bundle resources:

~~~sh
cd pc/src-tauri
TAURI_CONFIG='{"bundle":{"resources":[]}}' cargo check --lib --locked
TAURI_CONFIG='{"bundle":{"resources":[]}}' cargo test --lib --locked stopwatch_hello
~~~

That override is only a compilation/test configuration, not a distributable
desktop package. Release installers still require the normal bundled resources.

The production renderer also runs on the host, using the resolved LVGL/font/
JPEG implementation and factory pack. This is a framebuffer preview, not a
photograph or a claim that panel timings/touch have been verified:

~~~sh
python firmware/tools/preview_stopwatch.py --output artifacts/stopwatch/preview.png
~~~

Before declaring hardware support verified, record:

1. Recovery from original firmware and matching first flash.
2. RGB color order, full circular border, offset and all six factory clips.
3. Chinese title/content clipping and all eight session positions.
4. Both buttons, long holds, swipe axes and physical press latency while animating.
5. Codex/Claude channel selection, Agent switches, terminal TTL and exact-ID selection.
6. USB replug, board restart, desktop restart and host sleep/wake on macOS/Windows.
7. One vibration per lifecycle transition; motor off after 120 ms.
8. Battery reading, USB power transitions and 24-hour heap/stability observations.

No physical board was connected during initial source/build verification.
Subsequent on-board verification and fixes are recorded separately below.

## Initial verification record — 2026-11-05

The following record predates connecting the physical board; see the subsequent
0.1.1 on-board record for the four completed peripheral checks.

- StopWatch native ESP-IDF 5.5.5 build: passed.
- StopWatch PlatformIO / ESP-IDF 5.5.4 build and merged factory image: passed.
- Existing P4-v3 PlatformIO build and image-budget gates: passed.
- P4-v1 was not rebuilt; existing P4 protocol source-contract checks passed.
- Desktop Node tests: 1817 passed, 1 skipped; production web build/CSP: passed.
- Rust library compile and seven hello/identity-related tests: passed with
  release bundle resources excluded for source-only verification.
- Bounded session C tests passed with UBSan on macOS. Linux CI additionally
  enables ASan. The production renderer's native framebuffer preview passed.
- Source publication check: passed. P4 dependency locks/configs are unchanged.
- Physical panel, touch, motor, USB reconnect and battery/stability tests remain
  pending. GitHub-hosted CI has not been run from this local checkout.

## On-board verification — 0.1.1

Display, touch, short/long presses and three bounded vibration pulses passed on
a physical v1.0. The initial black screen, intermittent touch identification and
fresh-session timeout were corrected during testing. See the acceptance record
for evidence and the remaining untested functions.

The opt-in console requires pyserial and an exact device ID read from hello:

~~~sh
python firmware/tools/stopwatch_hil.py \
  --port PORT --expected-id DEVICE_ID --duration 600 --pattern pet \
  --output artifacts/stopwatch/hil/session.jsonl \
  --control artifacts/stopwatch/hil/control.json
~~~

Replace the control JSON atomically to request a pattern:
{"pattern":"colorbars"}, {"pattern":"touch"} or {"pattern":"pet"}.
{"vibrate":120} requests one pulse; firmware only accepts integer durations
20–300 ms, an exact boardDeviceId and a bounded requestId.
Existing actuator commands are not replayed when the console restarts.
The console records synthetic sessions, device replies and input events locally,
then clears its test cards and restores the pet view when stopped.

On this board native USB watchdog reset successfully returned to application
mode after flashing. If an RTS reset leaves download mode active, use the
installed esptool's --after watchdog-reset option. The supported normal
application image address is 0x10000; do not write an app image at 0x0.
