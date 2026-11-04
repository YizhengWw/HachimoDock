# Debug screenshot integrity

The collector also rejects zero/out-of-range dimensions before accepting any
frame, and rejects encoded chunks over 4096 bytes before decoding. Saved extra
metadata cannot overwrite a verdict, errors, sizes or checksums; a collision
fails the report. Push and pull both retain bounded numeric/fixed error metadata,
not arbitrary wire text. Begin/end report fields are explicitly projected.
Boot/source correlation and its limits are described in [FRAMEWORK_EVIDENCE.md](FRAMEWORK_EVIDENCE.md).

The optional `screenshotTransfer=pull-fnv1a32-v1` path keeps one broker-owned
immutable frame. The PC requests only one chunk at a time, validates it before
acceptance, and may reread the same block within a bounded retry budget. It never
recaptures automatically. Begin retries with the same route/requestId return the
same snapshot; close, audio revocation or expiry releases buffers before leases.
Expiry runs at protocol-worker safe boundaries (3 s idle / 30 s total), never in
a timer callback that could free memory still used by a driver. Underlying
synchronous driver operations still need their own completion; this is not a
hard-real-time deadline or proof of the cause of prior wire corruption.

This is a screenshot-only diagnostic path. Normal `audio/` and `media/` frames
keep their existing queued, no-drain transmission; no microphone or playback is
started by screenshot tests. Active audio rejects/interruption-aborts capture.

`debug/screenshot_begin` advertises `integrity: "chunk-fnv1a32-v1"`, `totalBytes`
and `chunkBytes`. Each chunk includes its decoded `rawSize` and lowercase
eight-digit FNV-1a checksum over **raw bytes** (offset basis 2166136261, multiplier
16777619, wrapping u32). The existing end checksum remains FNV-1a over little-endian
RGB565 **pixel values**, so the per-chunk and whole-frame algorithms are distinct.
`debug/screenshot_end` also includes the complete raw `totalBytes`.

The screenshot sender checks every partial driver write and drains once only
after a complete line is queued. Zero/error writes, encoding failure, lock timeout
or chunk drain failure stop the capture; no end marker is emitted after those
failures. An error line is best effort, prefixed by a newline to separate a
partially written frame. `sentBytes` counts only fully completed chunks, not a
partially queued failing chunk. A disconnected or audio-busy transport may prevent
even the error line, so missing-end/timeout is a failure too.

The final suffix-to-drain boundary rechecks the real broker lease as well as
sampled audio flags. A newly reserved audio request can revoke the screenshot
before its public active flag changes; this skips a further drain wait while
retaining the buffers until the physical write returns. It cannot interrupt a
driver call that has already begun. The separate native-USB byte writer rejects
an impossible accepted-count before arithmetic, including 32-bit offset wrap;
it does not replay a partially submitted frame or claim remote receipt.

The end line itself can reach the host before its drain returns an error; bytes
already delivered cannot be withdrawn. The production acceptance collector waits
250 ms after an end/error to include the following error instead of reporting
immediate success. It also independently checks dimensions, frame ID, every
index, declared/decoded/expected chunk sizes, per-chunk checksum, total size and
frame checksum. The serial callback uses a nonblocking 32-event queue; any
dropped event makes acceptance fail, even if all image bytes and an end marker
arrived. After requesting disconnect, acceptance waits at most 1 s for the
callback sender to exit, draining late errors; a sender still alive at that
deadline also fails closed. A requested disconnect alone is not evidence that
the callback has completed. Drop counts saturate rather than wrapping to zero.

Invalid or incomplete captures retain received local `.rgb565le` bytes and a
JSON diagnostic report before assertion; these raw bytes are unverified forensic
data. PNG output is created only after all checks pass. This does not
replace an application-level acknowledged transfer protocol or cryptographic
authentication.

Offline C fault injection runs the actual production sender under ASan/UBSan,
including a complete 200-chunk frame, partial positive writes, zero writes in
begin/body, encoder/drain failures, UART/USB routing and audio exclusion. Rust
collector tests save reports for corrupted, truncated, duplicate, out-of-order,
invalid-base64, missing-metadata, missing-end and explicit-error cases. One vector
deliberately retains all 200 indices while removing 240 bytes from chunk 87; it
must identify that chunk and fail. It is a synthetic reproduction of the observed
shape, **not evidence that this caused the earlier real 614160-byte capture**.
That earlier shortfall remains unattributed pending matching transport evidence.

`node pc/scripts/test-graphics-capture-offline.mjs /absolute/evidence/directory run`
imports the actual Rust integrity, pull and bounded-event modules into a small
offline Cargo harness. Tests include a complete frame followed by a final-drain
error lost to a full queue, delayed callback errors, missing callback shutdown,
counter saturation and a lossless close. It never compiles/runs the ignored USB
acceptance test or initializes a serial manager. Cargo dependencies must already
be cached; `PET_GRAPHICS_TARGET_DIR` may reuse an existing target cache.
