# Default-off scheduler / SPIFFS observation experiment

## Current r50 development wiring

The history below describes earlier isolated stages. r50 now compiles the
observer and the separately named `pet_p4_sched_trace_rpc.c/.h`. They remain
disabled, with no active hooks/ring/command in normal or UI builds. The separate
`esp32_p4_scheduler_probe` target opts into all five wrappers, registers existing
main/save/protocol task handles and exposes bounded numeric window/read requests.
The final enabled ELF must show actual wrapped callers, in addition to the SDK
archive check. This is a passive window, not the save-triggered lifecycle probe.

The original `pet_p4_sched_probe.c/.h` and its 37 lifecycle regressions are
retained unchanged; that separate one-shot save adapter is still unregistered.
Neither endpoint is a component/program capability or a release authorization.
Hardware clock, ISR coverage, observer perturbation, overflow/truncation and
physical timing remain distinct acceptance questions. Same-quantum program tags
use numeric kind/sample/slice only; do not serialize task addresses or names.

## Earlier stages

This is a private diagnostic candidate, not an installed application capability,
not a scheduler policy change, and not an explanation of the r18 long frame.
`pet_p4_sched_trace.c/.h` is **not** added to CMake, main, protocol or versioning.
It is absent from the r19 build. `PET_P4_SCHED_TRACE_ENABLE` defaults to zero;
that translation unit then has no hooks, external dependencies or ring storage.

## Audited interface and coverage

The existing IDF legacy dual-core RISC-V kernel has these ABI signatures:

| Symbol | ABI | Referencing object audited |
| --- | --- | --- |
| `spiffs_api_lock`, `spiffs_api_unlock` | `void (struct spiffs_t *)` | `spiffs_hydrogen.c.o` |
| `xTaskPriorityInherit`, `xTaskPriorityDisinherit` | `BaseType_t (TaskHandle_t)` | `queue.c.o` |
| `vTaskSwitchContext` | `void (void)` | `portasm.S.o` |

Enable requires the macro **and** all five `--wrap` linker options. `__real_*`
must resolve back to the original SDK functions. Every wrapper calls its real
function exactly once, outside the observer lock, with unchanged parameters and
return value. No changes to task priority, affinity, audio, semaphore semantics,
timeouts, or filesystem operations are made.

The sources were checked for IDF 5.5.1 and 5.5.4. The existing v3 SDK archives
contain undefined `R_RISCV_CALL_PLT` references for the external callers above.
An offline `ld -r --wrap=...` test redirects those calls to `__wrap_*`.
The **same test confirms a coverage hole**: `tasks.c.o` has an internal
`vTaskSwitchContext` call that does not redirect. GNU wrap is not a complete
kernel hook. A final enabled image must be re-audited, not inferred from this
test or a prior ELF. The reviewed r18 v1 final ELF contains one direct call to
`vTaskSwitchContext`, from the port assembly. This does not guarantee a later
build's reachability, including LTO or changed configuration.

`CONFIG_FREERTOS_SMP`, non-P4, non-dual-core, non-32-bit and SDK versions other
than 5.5.1/5.5.4 fail compilation when enabled. No private TCB struct/offset is
copied. The exported `pxCurrentTCBs` array is read only while local interrupts
are masked; pointers are compared with three explicitly registered long-lived
task handles and never exported. All other tasks have the anonymous class 0.

## Observation boundaries

* Window: explicit trusted begin, at most 600,000 us, no extension or timer.
  The next hook, mark or stop closes an expired window. It is not a timeout on
  the underlying I/O. Stopping a trace never cancels or retries an operation.
* A 1024 × 16-byte DRAM circular ring retains the most recent numeric events.
  Overwrite count and overflow flag mean the trace is incomplete. A permanent
  partial-wrap-coverage flag prevents interpreting a clean ring as all-system
  coverage. Negative/backwards clocks close the window with an invalid flag.
* Old physical operations cannot append their paired completion to a new
  generation. Opening inside an already-held lock/wait does not reconstruct
  its start. Generation exhaustion rejects new windows; it never aliases 0.
* `SWITCH_SELECT` is a scheduler selection observed after the real switch
  function returns, **before** the interrupt epilogue restores the task. It is
  not an exact task CPU-start timestamp and includes no ISR-duration accounting.
  Unchanged selections are omitted; changed OTHER-to-OTHER selections remain.
* Inherit/disinherit values are the original result bit-pattern, **not** the
  effective priority or proof that a new boost occurred. Timeout disinheritance,
  explicit priority setters and other changes are not observed. The module
  deliberately avoids priority getters that acquire the kernel lock.
* SPIFFS release lies between `RELEASE_BEGIN` and `RELEASE_RETURN`; preemption
  after release may delay the latter. No file path, file contents, task name,
  pointer or volume identity is recorded. Multiple mounts are not distinguished.
* Timestamps are sampled before the observer lock. Across cores or nested
  interrupts, append order need not be timestamp order. Preserve both when
  analyzing a suspected causal chain; incomplete intervals are not CPU time.

## Lock and timing discipline

The trace lock protects only bounded metadata and one 16-byte record (or one
small status copy). No real SDK call, output, allocation, priority lookup, FS
operation or task lookup occurs while holding it. Caller read/status outputs
must be internal RAM. Ring initialization resets counters without zeroing or
copying the 16 KiB buffer under the lock. All observer functions are IRAM;
state is DRAM. The ISR clock is IDF's IRAM `esp_timer_get_time`, which calls the
systimer counter getter/conversion rather than a timer task or a kernel lock.

The enabled observer still adds instruction, stack and short spinlock costs.
Host sanitizers do not prove audio safety or a wall-time bound on hardware.
No hardware run has been performed for this module. An enabled, separately
authorized diagnostic build must measure its cost and preserve the unmodified
load/priority/affinity; disabling animations would not validate the original
237 ms callback-to-return condition.

## Reproducible offline checks

Normal host-only tests:

```sh
python -m pytest -q firmware/tests/p4_sched_trace_test.py
```

The optional read-only archive/link test requires explicit paths:

```sh
PET_SCHED_TRACE_SDK_BUILD=/absolute/existing/build \
PET_SCHED_TRACE_RISCV_TOOLS=/absolute/existing/toolchain/bin \
python -m pytest -q firmware/tests/p4_sched_trace_test.py
```

It extracts existing objects into pytest temporary directories and runs `ld -r`;
it does not build/download an SDK, invoke PIO, touch an existing build product,
or run code on hardware. It includes the same-TU negative control.

Initial results on 2026-10-29: 4 tests passed including the opt-in archive test;
ASan/UBSan host execution covers SDK return preservation, exact-once original
calls, observer reentry, cross-core resume, old-generation completions, expiry,
negative/backwards clocks, overflow retention and two concurrent writers.
The candidate also compiled as one standalone object with the current v1
IDF 5.5.1/GCC 14.2 +20250730 flags, entirely in a temporary directory:
2166 bytes text, 16464 bytes DRAM section data, `observe` own frame 96 bytes,
wrappers at most 48 bytes. These are object/own-frame measurements, **not** total
linked growth, ISR worst-case stack, or observed latency. Undefined-symbol
inspection found no libatomic, logging, allocator or priority-getter helper.
The 5.5.4 enabled object and a fully wrapped final image remain to be validated
in their separately authorized build; the archive ABI check is not a substitute.

## Proposed transport/build wiring, deliberately not implemented

Use a private PIO environment with a distinct diagnostic version, explicit
enable macro and wrap flags. Do not enable the production/default environment.
Register the existing main/save/protocol handles from their trusted task setup
boundaries, outside all observer locks; do not discover tasks by user names.

An adapter needs a one-shot state machine:

`idle -> explicitly armed -> next eligible save captures <=600ms -> frozen`

* Only a root-requested arm may authorize the next save. Save must not arm the
  trace itself, repeat capture, or overwrite frozen evidence. Validate transport
  owner, idle audio, no incompatible diagnostic and an explicit arm deadline.
* Capture PPA begin/callback/return with the existing transaction cookie as the
  numeric tag. No callback may dump data. Save begin/end markers are numeric.
* Cancellation, transport owner loss, audio becoming active or timeout stops
  the exact generation; no automatic resume/re-arm. The underlying save/PPA
  operation is neither canceled nor replayed by this observer.
* Prefer sharing the trusted NATIVE_PROBE exclusion slot with the existing
  program diagnostic. Its current documented 4096 units are instruction-fuel,
  **not** this ring's bytes or a CPU-time allowance. A new adapter must explicitly
  define its reservation/consumer scope rather than claim that obtaining that
  lease proves a 2 ms budget. PROGRAM/PERIPHERAL remain unopened.
* Keep ownership through physical trace stop, then expose frozen numeric data
  only to the same trusted diagnostic owner. Reading is bounded and read-only;
  explicit discard/expiry releases the retained snapshot and lease. A stale
  owner must not read, restart or replace another owner's capture.
* Do not call the resource broker or audio APIs inside kernel/ISR wrappers or
  while holding the trace lock. A safe cancellation/owner propagation adapter,
  lease finish boundary and bounded frozen-result lifetime are still required.

The default-off native lifecycle adapter below now implements the arm/owner/
broker model in isolation. Actual task hooks, transport ownership, bounded poll
service and build/protocol wiring are still absent. This is not an operational
on-device diagnostic, real-time proof, or resolved causal claim.

## Isolated one-shot adapter

`pet_p4_sched_probe.c/.h` is another independent, default-off translation unit.
Enabling it without explicitly enabling the trace fails compilation. Neither
file is registered in the ordinary firmware build. No main, save, renderer,
audio, protocol, resource limits, PIO environment or version has been changed
to use the adapter.

Trusted C registers the three existing long-lived task handles once. It opens
a monotonically generated controller owner and obtains the current IDLE ticket
from status. `arm(expected_ticket, window_us)` requires that exact owner and
revision, consumes the revision, reserves the existing entire NATIVE_PROBE
exclusion slot, and produces one capture ticket. The 4096-unit reservation is
solely a full-slot exclusion convention shared with the program diagnostic;
it measures neither executed instruction fuel nor ring bytes or CPU time.
Existing resource quota implementations are unchanged.

The state machine is:

`IDLE -> ARMED -> CAPTURING -> FROZEN -> DRAINING -> IDLE`

* An arm expires after 35 seconds without an eligible save. The next eligible
  save calls `save_begin` before its existing I/O and consumes the arm exactly
  once. Only that already-planned save is observed. The adapter never schedules,
  cancels, repeats, retries or approves the underlying save.
* `save_begin` returns a physical completion token containing controller owner,
  capture revision and exact trace generation. A nonzero token **must** be
  paired with `save_end` after original I/O returns, including when begin raced
  with cancellation. The token is an accounting duty, not a work permission.
* Every window is at most 600 ms. Completion, expiry, cancellation or observed
  audio stops capture and freezes numeric evidence. Further saves cannot restart
  or overwrite it. The frozen controller may page-read a maximum of 16 records
  per call, repeat the same page, or explicitly discard. No page read consumes
  records. Terminal capture reason and successful physical save completion are
  separate; audio observed after completion does not relabel it as unexecuted.
* FROZEN retains the exact lease and evidence until explicit discard or its
  30-second TTL. Discard/expiry/owner loss becomes DRAINING. A physical save or
  marker still in flight retains occupancy; only its exact completion permits
  finish. The ring is not cleared under a large critical section.
* Revocation and physical completion are short independent latches even if a
  control operation has the nonblocking claim. Revocation immediately removes
  read authority and disables published PPA marks. Claimed operations reconcile
  revocation before publishing a result. Actual trace stop runs outside the
  state lock; when a claim is busy it may be deferred to that operation/poll.
  This is not a hard ISR-time cancellation guarantee.
* `save_end` latches once even during claim contention, so the worker need not
  retry either notification or real I/O. Duplicate tokens cannot change the
  first completion result; an old owner/revision/trace generation cannot finish
  a later run. Asynchronous completion timestamps are compared to capture start,
  not to a newer unrelated control sample.
* Finish failure is permanently fail-closed (`FAILED`). It does not clear the
  token, retry finish, permit another arm, or pretend resource release occurred.
  There is no runtime reset/recovery API for an unknown broker outcome.

Fresh capture/playback/conversation/music/stream flags are read independently
without short-circuiting. Arm checks before and after reservation; save entry
checks before and after trace begin; page read checks again before returning
its copy. Observed busy audio rejects new admission or freezes active capture.
Reading pages while audio is busy is denied, without modifying audio itself.
This samples current state, not audio history; an audio transition between
polls is not necessarily observed. A future trusted service must call poll at
an explicitly bounded cadence and reconcile real transport teardown. There is
no hidden timer or worker; arm/frozen TTL cleanup is evaluated by task-context
calls, so no autonomous cleanup latency is currently claimed.

Every ordinary control API has one fail-fast claim. All trace, broker, audio
and clock calls occur outside its state lock. PPA ISR marks use a lock-free
single-attempt gate, never the resource broker/audio getters/output. A contended
mark is dropped with an explicit incomplete flag. Draining also waits for this
gate before allowing a new generation, preventing a delayed old marker from
writing into another capture.

Owner/tickets are trusted native tokens, never values decoded from wire or
component resources. A future transport adapter must keep the controller tied
to its real endpoint generation and recheck that transport epoch before sending
any copied page. The returned C objects contain only enums, counters, timestamps
and bounded numeric records, but must be serialized **field-by-field**, never
as raw struct memory/padding. Copies are authorized at the API's linearization
point; the module does not itself implement or protect a later transport send.

Pure host test command:

```sh
python -m pytest -q firmware/tests/p4_sched_probe_test.py
```

Initial adapter validation: 37 tests passed with ASan/UBSan, linking the actual
resource broker/device adapter and actual trace. Host stubs assert that no SDK,
audio or clock call occurs while any observer/control/broker lock is held.
Tests cover all five audio flags at both admission boundaries, stale tickets,
pagination/no overwrite, arm and frozen TTL, negative/backwards clocks, trace
begin failure plus finish failure, revocation during blocked admission, physical
completion during a claimed control operation, late/wrong save generations,
in-flight marker drain, and the earlier-completion/later-control timestamp race.
The host fixture includes the unchanged production adapter translation unit to
seed its private owner/revision counters at `UINT64_MAX` boundaries. There is no
product seeding API. Tests confirm the last nonzero values do not wrap/reuse 0,
old tickets remain rejected, and revision exhaustion during a physical save
retains the lease until completion, then stays fail-closed without a second
finish. The combined observer/PPA/render/resource focused run passed 51 tests;
one opt-in live-SDK archive inspection was deliberately skipped in this stage.
These tests do not establish real IRQ behavior, acoustic safety, final wrapped
link coverage, hardware timing or successful transport integration. No hardware
or live SDK build was used for this adapter stage.
