# Opt-in bounded TX audit

This is RAM-only evidence for locating corrupted or truncated ordinary JSON
responses. It is not a framing change, transport retry, delivery receipt or a
claim that the remaining physical-link problem has been solved.

## Protocol entry and capture scope

The protocol worker accepts exactly these requests, with no other root keys or
payload fields:

```json
{"topic":"debug/transport_audit_start","payload":{}}
{"topic":"debug/transport_audit_read","payload":{}}
```

Audit is disabled at boot. Start discards the previous RAM capture while keeping
capture disabled, sends `debug/transport_audit_started`, then arms a new session.
Read closes capture and copies its fixed state under the existing transport
mutex; JSON encoding and sending happen after unlocking. The start ACK, audit
results and audit errors are excluded. The handler is entered before the general
runtime-state lock; it does not hold that lock while formatting/sending.
The 6 KiB reply buffer and snapshot scratch live in external RAM, not the
audio-sensitive internal SRAM. Only the small fixed ring stays near its lock.

Start/read observe the existing audio flags and reject a busy audio session with
a short `audio_busy` error, disarming but preserving previously captured records.
Any actual audio/media/no-drain TX also disarms pending/active observation using
the existing TX classification, with no per-frame SDK polling. It does not clear
history or automatically resume. After audio stops, read retrieves that history;
only a new explicit start creates another capture.

One session observes at most 64 **physical-route records** and retains the most
recent 16. The response includes session, seen, overwritten and unused-budget
counts. Reading again does not resume capture or clear the retained evidence.
Only an explicit new start can arm another session. A session counter reaching
its integer limit fails closed. No capture is persisted across reboot.

Only ordinary drained JSON from `transport_send_line_impl` is observed. Audio,
media, the explicit no-drain callback, console logs and the separate checked
screenshot path do not consume budget and are not hashed. Existing driver write
calls, positive-partial-write continuation and drain calls are unchanged. There
is no additional drain, sleep, full-message retransmission or automatic retry.
Normal traffic with audit disabled adds only constant checks; it does not hash,
encode a diagnostic or retain body bytes.

## Record interpretation

| Field | Meaning |
| --- | --- |
| `seq` | Physical-route observation sequence within this session |
| `route` | 1 UART, 2 USB serial/JTAG, 3 native USB |
| `bodyBytes`, `fnv`, `hashAvailable` | Original JSON body length and optional FNV-1a fingerprint, excluding the appended newline |
| `driverObserved` | Whether this path exposes write/drain results |
| `bodyAccepted`, `newlineAccepted` | Separate byte counts accepted by the driver for the body and line terminator |
| `bodyLast`, `newlineLast` | Last raw write return for each part; a zero/negative return identifies its failed boundary |
| `bodyDrain`, `newlineDrain` | Results of the two pre-existing completion waits, not newly added waits |
| `invalidReturn` | A driver claimed more bytes than requested; the invalid claim is not counted as accepted |

Native USB currently has a void callback, so its record explicitly uses
`driverObserved:false` and the sentinel `-2147483648` for unobserved return/drain
results. Its zero accepted counts are **unknown**, not proof that nothing was
sent. A broadcast may create one record per physical path; one path's successful
enqueue cannot imply another path succeeded.

The fingerprint is calculated once per captured logical line, immediately before
enqueue while the transport mutex is held. The caller must keep the line
immutable for the entire send, as already required by the send API. Hash work is
capped at 32,768 bytes; larger lines explicitly report `hashAvailable:false`.
There is no additional allocation or payload retention. Hashes are correlation
fingerprints, not encryption or a claim of complete anonymization.

## Host comparison and evidential limits

Capture the original received bytes before UTF-8 decoding, replacement of invalid
characters, whitespace trimming or JSON parsing. Remove only the actual transport
newline before comparing `bodyBytes` and FNV-1a. Do not hash reserialized JSON.

- A shorter accepted body or missing accepted newline establishes a failure
  visible at the driver API boundary. A successful drain cannot repair it.
- A matching device/host length and fingerprint on malformed JSON suggests the
  malformed data was present before transport, rather than being a later loss.
- Full driver acceptance with a host mismatch narrows the investigation to
  after fingerprinting, which still includes caller-buffer races, queued-driver
  behavior, physical transport and PC receive handling. It does not identify one
  of those layers by itself.
- Driver acceptance is not proof of physical transmission; drain success is not
  a PC acknowledgment. This diagnostic never treats either as task delivery.

When 16 retained records are insufficient, the host can request one audit read
immediately after detecting a bad raw line. No periodic audit logging or extra
traffic is generated automatically.

## Local comparison tool

Use this only during a short, quiet diagnostic session, with audio capture,
playback and real-time conversation stopped. No special device credentials are
needed, and the tool itself never opens a device or sends a command.

1. Send the empty-payload start request through the existing diagnostic link and
   wait for `debug/transport_audit_started`.
2. Save the relevant received bytes directly to a private binary capture file.
   Keep malformed JSON and incomplete final lines exactly as received; do not
   copy them through a terminal text renderer, decode-and-reencode them, or
   discard frames that fail JSON parsing. Restrict the interval to this capture
   session and physical link.
3. Send the empty-payload read request. Save its complete
   `debug/transport_audit_result` JSON envelope as a separate metadata file. A
   busy-audio error is not a usable audit result: it retains the previous records,
   so stop audio before reading them. Do not start again before collecting those
   records, because a new start intentionally clears the prior capture.
4. Run the local comparison tool from the repository root:

```sh
python3 firmware/tools/compare_transport_audit.py \
  --audit /path/to/private-capture/audit.json \
  --wire /path/to/private-capture/wire.bin \
  --route 1
```

Select `--route 1` for UART/USB-UART, `2` for the USB serial/JTAG driver, or `3`
for the separately routed native USB path. Do not combine captures from multiple
ports. The tool emits metadata correlations and always leaves
`deliveryConfirmed:false`; it does not resend anything. Multiple identical
records, repeated raw lines or fingerprints are ambiguous, not delivery proof.

The raw capture can contain private protocol data even though the audit report
does not contain message bodies. Keep `wire.bin` and source captures outside
public commits/releases, and share only the metadata report when sufficient.
The tool validates bounded local inputs and emits a generic error rather than
echoing a private path or malformed JSON on a rejected input.

## Regression coverage

`p4_transport_audit_test.py` compiles the production module and extracts the actual
TX and handler code with ASan/UBSan. Tests cover disabled/default and audio/media
no-hash/no-drain paths, unchanged existing waits, positive/zero/negative/invalid
write results, separate newline failure, drain error, three-route broadcast,
bounded-ring wrap, fixed budget, session exhaustion, worst-case JSON capacity,
strict empty payloads, caller routing and unlocked reply callbacks. The capture
test reuses the original asynchronous bounded-queue fixture and its complete
frame/log/audio interleaving assertions rather than replacing its writer.
Silent concurrent capture separately exercises ordinary frames, screenshots and
logs; audio/media arrival is verified to cancel capture without losing evidence.

The existing media transport, audio frontend and screenshot integrity regressions
remain separate and retain their previous behavioral assertions. No test opens a
USB port, records audio, plays sound, flashes firmware or transmits user data.
