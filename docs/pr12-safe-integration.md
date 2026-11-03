# PR12 safe integration

The integration merge retains original PR head
`85e194d9d970513c6a475a33ce8cabe0c5c72603` as its second-parent ancestry.
It preserves guardian source filtering, assistant line breaks, grayscale canvas
text, native32 glyph ink, the original 21,207-glyph fallback, and factory sounds.
It does not transplant the unrelated dirty firmware/desktop development tree.

The original v1 pixel wire contract is replaced by matched-peer `p4-text-alpha-v2`:

- Native owns desired descriptors by peer/boot/renderer/owner/version, uses exact bounded identities with unique native-issued owner tokens (no hash
  identity), canonicalizes state/speech display roles, checks the expected content asset whitelist, bounds history to12 owners, and passes u64
  connection IDs as decimal strings. Main-window IPC only; stale renderer stop
  is compare-and-clear. No old-window callback clears a new instance.
- JS has one raster/invoke and FIFO whole-asset turns, latest-per-owner coalescing,
  bounded pre-admission busy retry, an absolute owner deadline, lazy pages and
  fallback cooldown. It has no device-resident sent TTL. Explicit, rate-bounded
  device missing notices refresh current assets/pages after eviction.
- One native worker is reserved BEFORE decode/spawn. Each bounded physical frame
  separately holds the existing asset guard, rechecks current peer/boot/content,
  capture/media/update protection at final commit, then releases it before ACK
  wait/token sleep. Unknown delivery is not replayed. The worker uses existing
  paced serial writes; it does not open another port or replace its driver.
- Device stage admission requires matching boot/client/route/owner/version AND an
  expected asset key/style; each text manifest replaces the entire bounded owner
  snapshot across roles, retiring omitted owners immediately; READY precedes chunks. It incrementally checks RLE
  geometry/checksum, ACKs each exact phase/offset, and retires on supersession,
  idle/absolute timeout, protected work or explicit matching abort. Another begin
  cannot overwrite it. Resource limits are8 cache slots/256KiB plus32KiB staging.
- Latest text always wins. Same-frame immutable read pins cover title/current body
  and survive concurrent peer retirement; missing roles fall back together.
  Offscreen items no longer refresh pins. Cache revisions invalidate overlay.

FNVs/nonces are correlation/error checks, not new authentication claims. Pixel
budgets and timeouts are resource limits, not proof of wall-time guarantees.
The base has no later development-tree broker; this change does not import one,
raise its quotas, or claim these fixed buffers grant audio/hardware acceptance.

MiSans16/32 exact data retain public attribution and the unmodified official
Agreement + embedded-font notice, included in Tauri resources and Windows staging.
The font generator defaults to native32-only; rebuilding fallback is explicit and
retains existing codepoints. No OTF or converter is downloaded or executed here.

Review validation must use synthetic HOME/minimal environment and OS sandbox:
external network denied (loopback mocks only), real serial/I/O Kit and actual
credentials/provider paths denied. Focused commands use existing dependencies:

```sh
python -m pytest -p no:cacheprovider firmware/tests/p4_bubble_pixels_test.py \
  firmware/tests/p4_font_test.py firmware/tests/protocol_contract_test.py
node --test pc/src/lib/bubble-pixels.test.js pc/src/lib/p4-session-service.test.js \
  pc/src-tauri/bridge/test/codex-log-monitor.test.js \
  pc/src-tauri/bridge/packages/agent-session-bus/test/codex-adapter.test.js \
  pc/src-tauri/bridge/packages/agent-session-bus/test/server.test.js \
  pc/scripts/misans-license.test.mjs
python pc/scripts/test-bubble-pixels-native.py --deps EXISTING_RELEASE_DEPS \
  --rustc EXISTING_RUSTC --output NEW_PRIVATE_HOST_OUTPUT
```

The small Rust runner compiles the actual controller and extracted actual manager
transfer method against prebuilt rlibs with a fake wire/ACK adapter. It neither
runs Cargo nor proves the full Tauri application type-checks. Full typed Rust
validation belongs to the parent AFTER combining the integration branches; avoid
duplicate heavy compilation. Dual target image/headroom/static RAM/stack and
hardware/audio acceptance are also pending parent scheduling. Original budget
and failed HIL gates are not relaxed, reset, or relabeled passed by these tests.
