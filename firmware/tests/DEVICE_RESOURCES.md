# Device resources, first enforced slice

`pet_p4_resources.c` is a fixed-storage C authority core. It has no heap,
FreeRTOS dependency, blocking wait, I/O, callback or component-ID branch. Its
caller must serialize operations on the **same** broker. The device adapter in
`pet_p4_resources_device.c` owns the single boot-lifetime broker and its short
`portMUX` critical sections. It must never call back into host, transport,
renderer, heap or audio code while that section is held.

## Enforced budgets

| Resource | Concurrent leases | Units | Current caller |
| --- | ---: | ---: | --- |
| Host view | 1 | 1 | `pet_p4_app_host` |
| Screenshot snapshot | 1 | at most 614400 raw bytes | Screenshot debug admission |
| Debug bulk | 1 | 1 transfer | Same screenshot transaction |
| Compressed asset buffers | 2 | total 4 MiB, at most 2 MiB/file | Renderer-owned cache; worker-owned while loading |
| Asset preparation | 1 | 1 worker job | Low-priority chunked reader/validator |
| Native diagnostic probe | 1 | 4096 maximum instruction-fuel units | Fixed `debug/program_probe` only |
| Controlled program | 0 | 0 | Reserved, unavailable |
| Peripheral service | 0 | 0 | Reserved, unavailable |
| Background storage work | 1 | 1 physical job | Trusted storage scope integration |

There are eight owner slots, eight lease slots and at most two simultaneous
leases per owner. A debug request acquires snapshot and bulk together under one
adapter critical section or retains neither. The raw snapshot byte limit does
not include the existing, separately bounded base64 scratch buffer. This is
admission accounting, not an allocator or a replacement for PSRAM exhaustion
handling. The screenshot caller must still check allocation failure.

Owner generations and lease serials are 64-bit, monotonically issued by the
broker, bound to that broker's address, and fail closed rather than wrap.
Allowed-resource masks and all tokens originate only in trusted C. Neither
package JSON nor protocol requests supply an owner, lease or expanded quota.
The broker is zero-initialized once per boot; it must not be reset or copied
while any token can survive. These are in-process authority handles, not
cryptographic capabilities for arbitrary native plugins.

Eight separate fixed realtime-reservation slots are part of this **same broker**.
They do not consume ordinary owner/lease slots, so a full background owner table
cannot prevent audio from reserving priority. A trusted native actor obtains a
broker-address/64-bit-serial/slot ticket before publishing its requested/active
state. Reservation serial exhaustion and capacity exhaustion fail closed. This
is not a newly advertised package capability or an arbitrary realtime thread API.

## Cancellation is not physical release

`pet_resources_owner_revoke` stops future acquisition and makes existing leases
non-current. It does **not** release their units or make their owner slot reusable.
Only `pet_resources_finish` consumes the exact owner/generation/serial/type/units
ticket. A revoked completion releases its own physical occupancy but returns
`PET_RESOURCE_FINISH_REVOKED`; a stale or foreign completion releases nothing.
There is no timeout that guesses a worker has stopped and reuses its buffer.

For debug work, use a zeroed `pet_resource_debug_session_t`:

1. `pet_resources_device_debug_begin(raw_bytes, observed_audio_busy, &session)`
   before allocating raw/base64 storage or taking the renderer lock. A competing
   request returns busy without waiting for another debug job.
2. Check `pet_resources_device_debug_current` at bounded copy/send boundaries.
   False means stop further work/delivery. Observing audio busy here revokes this
   owner permanently, so audio becoming idle cannot revive the canceled job.
3. Explicit transport/job cancellation can call `pet_resources_device_debug_revoke`.
   Do not free or reuse storage while its physical copy/write remains in flight.
4. On every exit, after freeing the job buffers and after physical calls return,
   call `pet_resources_device_debug_end`. It frees exact matching occupancy and
   zeros the session. Its boolean return distinguishes current from canceled or
   stale completion; it does not prove that the PC received the bytes.

Checks cannot undo bytes already handed to a transport driver. They also cannot
bound a blocking driver call or release its memory before it returns. Existing
fail-closed screenshot metadata and PC integrity validation remain necessary.
Do not hold the broker critical section across copy, encoding, write, drain,
allocation/free or a renderer/transport semaphore. This broker does not change
the existing audio/media no-drain transport path.

Asset preparation uses the same broker, not a separate quota counter. Cancellation
and storage suspension revoke an in-flight owner without freeing its buffer or
releasing its worker slot. The worker returns the preparation lease only after
physical read/scan exits and publishes completion under its short lock; the render
owner later frees the buffer and releases that exact buffer lease. No new request
reuses a loading slot. Cache keys include captured slot and storage generation;
ready data and decoder state are invalidated across committed source replacement.
Each 16 KiB chunk checks current owner/audio flags and yields one scheduler tick.
Those checks cannot interrupt an already-entered storage driver call.

## Host lifetime and observed audio protection

The actual app-host view owns a `HOST_VIEW` lease. Reentry, content/transport
replacement and sequence wrap end the old lease before creating a new one.
Hide/non-host navigation ends that lease immediately because no physical
background operation uses the logical view slot. Heartbeats retain the owner.
Actions and accepted snapshots check the current view lease; stale cleanup
cannot end its replacement. Existing wire instance/sequence checks still apply.
The lock order is `app_host mux -> resource mux`; the broker never acquires the
host mux. No private data, persistent service record or audio owner is cleared.

`pet_resources_device_audio_observed` remains a sampled-policy input for legacy
debug checks. It is stored separately from actual realtime reservations; passing
`false` cannot clear a live reservation's protection. Reported `protected_mask`
combines both sources and `realtime_reservations` reports the actual ticket count.

Actual capture, stream, conversation, music, file-cue and short sound-effect
paths now reserve priority through `pet_resources_device_realtime_begin`.
That operation and background `try_acquire` share the same broker mutex. A
reservation denies new `STORAGE_WORK`, asset-prepare, screenshot/debug-bulk and
native-probe admission, and revokes existing owners holding those work leases.
Revocation retains all physical leases. Host views and ready asset buffers are
not revoked merely because audio starts. No AEC parameter, audio frame size,
codec format, task priority or task core assignment is changed by the broker.

Capture pending requests and executing workers carry different exact tickets.
The worker takes its request ticket under a short capture-state lock; stop may
cancel an unconsumed request but cannot release the worker's ticket. A new start
can hold its own ticket while the old worker finishes, and stale completion
cannot release it. The same exact request serial gates the actual capture loop
before reads, after codec/AFE return and before chunk submission. A stop/start
cannot restore a boolean and merge a new request into the old session; timeout
or encode failure from that old worker cannot cancel the replacement. The
per-session queue policy is captured when the worker takes its request.
Stream/music/conversation controls reuse the existing
recursive stream mutex. Their tickets cover codec setup/restore and remain held
until an in-flight codec write returns; a paused music session retains priority
because resume is still accepted. File-cue tickets cover file access through
close and codec completion. Short effects cover their physical render/write step.

This closes the **new-admission** check-then-act race for integrated storage
scopes. It does not preempt a filesystem call already executing when audio is
requested: that call must return before cancellation can be acted on at its next
boundary. A durable storage commit must still publish/reconcile its recorded
result even after cancellation; it cannot be rolled back or reported as an
unsubmitted operation. A worker must retain its exact storage lease until its
physical cleanup and lock reconciliation finish. No realtime claim can be made
for arbitrary uninstrumented filesystem or driver actors.

This slice does not claim a hardware-wide broker for every DMA channel, decoder,
heap allocation, network transaction or peripheral. Existing audio, install,
render and transport arbiters continue to own their real operations. Program
and peripheral resource names have zero quotas; no package or protocol
capability advertises those services as available.

## Fixed native program probe

`pet_p4_program_probe.c` uses the same device broker's trusted generic
owner-open/acquire/current/revoke/finish API, not a separate quota counter.
`NATIVE_PROBE` has one slot and 4096 units; this authorizes only three built-in
diagnostic vectors and does not enable the reserved PROGRAM/PERIPHERAL kinds.
An independent short entry mux protects private static state and a 5-second
start-to-start rate limit, never spans execution, IO or a scheduler yield.

Each executed instruction checks the real lease and freshly reads four existing
atomic audio flags. Observed audio busy revokes the owner, but the lease remains
occupied until the physical core call returns. Cancellation is permanent for
that invocation; audio becoming idle never resumes or retries it. Eight samples
per fixed case now execute at most 64 fuel per slice, yielding one tick after
each slice. The same physical lease remains occupied across yields until the
whole invocation ends; cancellation between slices cannot commit or restart it.
Total invocation fuel remains 4096. Instruction bounds do not provide a hard
wall-clock guarantee; the diagnostic reports both quantum maxima and full wall
time, and the new per-quantum 2ms target still requires actual P4 measurement.
Source model, workspace and inert outbox are
probe-only, with no user-state reset or external-effect dispatch. The final
bounded JSON is sent on the caller's route only after physical cleanup.

The probe's host tests link the production broker/adapter/core, inject audio and
clock observations, inspect occupancy at actual core return boundaries, and
cover capacity contention, reentry and cleanup. They are not device performance
or live-audio coexistence evidence. See [PROGRAM_CORE_EXPERIMENT.md](PROGRAM_CORE_EXPERIMENT.md).

## Focused verification and integration ownership

- `p4_resources_test.c`: quotas, typed/foreign/stale handles, owner exhaustion,
  cancellation retaining physical occupancy, admission protection and generation/
  serial exhaustion. Production core, ASan/UBSan, no platform stub needed.
- `p4_resources_device_test.c`: production adapter with a host critical-section
  shim; atomic pair admission, eight concurrent contenders, audio cancellation,
  explicit revoke/finish, stale completion, view/debug isolation and clean exit.
- `petui_resource_lifecycle_test.py` and `petui_app_host_test.py`: production host
  linked to the actual broker/adapter, including hide/reentry/transport replacement
  and stale cleanup, while retaining existing sequence/variable regressions.

Register both new C sources in the firmware build. The screenshot caller in
`pet_p4_main.c` owns the real acquire/check/free/end integration and its fault
injection tests; it must use this same adapter rather than a second broker.
Passing these local tests is not USB, device performance, live audio or acoustic
acceptance. No physical device, microphone or speaker is used by these tests.

`p4_audio_realtime_test.py` adds 200 concurrent real-adapter admission races,
reserved-slot exhaustion/ABA/foreign-token checks, sampled-observer isolation,
retained in-flight storage occupancy, and extracted production audio control
tests. Those exercise capture stop/start handoff, overlapping capture and
conversation/stream requests, duplicate conversation control, paused music,
allocation/codec failures, and a blocked codec write racing a real production
flush call. The host driver is intentionally held until the test releases it;
storage remains protected while flush waits. These are silent software tests,
not microphone/speaker or acoustic-quality validation.

The extracted production capture loop also runs through stop/start injected
inside successful and failed codec reads, an old timeout check, and an old
failed chunk enqueue. Each replacement must begin a distinct session, retain
its own queue policy, send its own frame and end without losing its pending
request. Already-entered driver/network operations remain non-preemptible;
the next exact-owner boundary rejects obsolete work without replaying it.
