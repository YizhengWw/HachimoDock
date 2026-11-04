# Isolated bounded-program experiment

This is a research core and measurement fixture, **not an installable engine or
package capability**. It has no JSON/wire bytecode loader, package compiler,
general program scheduler, peripheral driver, external-effect dispatcher,
persistence or production UI. A fixed native diagnostic probe now uses the real
resource broker, without accepting program input. Existing installation contracts still forbid arbitrary executable
code. These files do not change that contract or the existing PC expression and
composition data formats.

## Design and tradeoffs

`main/pet_p4_program_core.c` is a genuine integer-register bytecode interpreter:
arithmetic, dynamic indexed loads/stores, comparisons, forward/backward branches,
one input event, model outputs, and staged capability intents. Backward branches
execute real loops; fuel, not a renamed state machine, bounds execution. The
compile-time collection fixture filters positive values, squares and collects
them, and sums them: `[-2,3,0,4,-1,5,1,-3]` produces `[9,16,25,1]`, sum `51`,
count `4`, using exactly 89 instructions including HALT.

The existing PC expression AST already supports JSON-oriented map/filter/sort
and bounded local transactions. It is a better fit for host workflow documents;
this experiment does not replace it. Fixed i32 cells trade away strings, general
JSON values, allocation, recursion and libraries to make a small deterministic
embedded execution envelope directly measurable. Compared with embedding a
language runtime or a general WebAssembly interpreter, this narrowly scoped
core has fewer features and a much smaller review surface, but invents a private
instruction format and has no ecosystem/compiler/debugger. These tradeoffs are
not a claim that a custom VM is generally superior. WASM here is a **compilation
target for the exact same C interpreter and test suite**, not its bytecode format.

## Explicit budgets and trust boundary

| Resource | Maximum |
| --- | ---: |
| Instructions | 256, 8 bytes each (2,048 bytes) |
| Fuel per invocation | 4,096 executed instructions |
| Cooperative execution slice | 1–64 executed instructions |
| Registers | 16 i32 |
| Addressable model cells | 64 i32, descriptor may reduce the limit |
| Events | Exactly one input event, 0–4 i32 words |
| Model outputs | 8 i32 |
| Trusted capability grants | 4, each with 16 operation bits |
| Staged capability intents | 8, descriptor may reduce the limit |
| External calls | Zero |
| Heap, recursion, ambient IO | Zero |

The trusted adapter owns the pointers, immutable code/event/grant descriptors,
owner generation, source state, revision and private non-aliasing workspace.
Bytecode cannot manufacture or modify them. `pet_program_init` explicitly zeroes
every state byte. The structural validator visits every declared instruction,
including unreachable ones, with a fixed maximum of 256. Invalid opcode, register,
unused field or jump target rejects the program before execution. Each executed
instruction, including back edges and HALT, checks the live owner and consumes
one fuel unit. There is no unmetered collection opcode or host call. Signed i32
overflow, divide/modulo by zero, `INT_MIN / -1` and `INT_MIN % -1` fail without
undefined C behavior; dynamic memory indices and actual event length are checked.

Fuel bounds interpreter work, not arbitrary callback time. An adapter must
provide a bounded generation/lease check; it may revoke its own authority on
observed cancellation, but cannot change model state or execute external effects.
Run may use a stable
state snapshot; publication must use the live state. The caller must serialize
owner revocation and live state mutation across `commit`/`ack_outbox` so that the
current/generation/revision check cannot go stale before publication.

## Transaction and external-effect semantics

`begin` copies the descriptor, event and source into the private workspace,
validates all code, and returns RUNNING without executing instructions. `step`
accepts only 1–64 fuel and retains pc, registers, total fuel, candidate and staged
outbox on RUNNING. Code storage must stay immutable/alive, but later slices do
not read a live source pointer or caller-owned event/descriptor. Every instruction
still checks fresh authority. A canceled/trapped workspace cannot resume; terminal
or duplicate step is inert. The compatibility `run` drains begin/step synchronously
without scheduler yields and is not the device's cooperative scheduling API.

Execution mutates only the caller's private workspace. READY is not publication.
`commit` checks the trusted owner generation, live revision and current grants
for each staged intent (at most 8 × 4 grant comparisons), then publishes
the model and outbox in one caller-serialized in-memory operation and increments
revision. Errors, cancellation, exhausted fuel, stale revision and old-instance
completion do not publish. A workspace can commit at most once. There is no
post-publication cancellation check that could misreport an already-completed
commit as unexecuted. This is **not crash-durable storage**: no persistence adapter
exists in this experiment.

An intent is only an inert outbox record, never evidence of external execution.
A pending outbox blocks another invocation. The trusted explicit acknowledgement
must match owner and revision, clears the outbox and advances revision. A failed
or unknown provider result must not be mapped to automatic acknowledgement or
replay. A future effect adapter would need broker admission, durable operation
identity, provider-specific outcome semantics and recovery; none is implemented
here, and there are no network/audio/peripheral calls. A future program scheduler
must itself acquire a typed program/CPU lease; the device's currently disabled
program resource does not become enabled merely by compiling this core.

## Reproduce without hardware

From the repository root, using an existing Python environment with pytest:

```sh
python -m pytest -q firmware/tests/p4_program_test.py
python firmware/tests/p4_program_test.py --measure
```

The script creates temporary compiler artifacts only. It does not install tools,
invoke PlatformIO, build firmware, open USB or change a user library. Optional
`CC` and `P4_CC` select already-installed compilers. WASM requires existing clang
with wasm32, wasm-ld (or Rust's bundled rust-lld), and Node. Missing optional tools
are explicit pytest skips / `unavailable` report fields, never passing evidence.

The four tests cover native ASan+UBSan execution; identical freestanding C→WASM
execution with zero imports; direct P4 object size/stack/ABI measurement; and
absence of ambient IO calls in the core. Each execution suite includes targeted
traps, all budget boundaries, zeroed initialization, whole-program validation,
cancel before HALT and before commit, stale revision, owner reentry, one-shot
commit, post-commit cancellation, and unresolved-outbox blocking. Deterministic
fuzzing runs 50,000 bounded random structural/control-flow cases plus 50,000
arithmetic cases checked against an independent i64 oracle. It is not a proof of
exhaustive coverage. The optimized 10,000-iteration benchmark checks a stable
checksum of 1,400,000.

Observed 2026-10-29 on this workspace:

| Measurement | Result |
| --- | --- |
| Core-focused pytest | 4 passed, 0 skipped |
| Native and WASM result parity | selftests + fuzz + benchmark checksum pass |
| P4 compiler | Espressif GCC 14.2.0, `-Os -march=rv32imafc_zicsr_zifencei_xesppie -mabi=ilp32f -ffreestanding -fno-builtin` |
| P4 core object text / data / BSS | 2,264 / 0 / 0 bytes |
| P4 unresolved symbols | none |
| P4 ABI: state / workspace / event | 432 / 544 / 20 bytes (host workspace: 552) |
| P4 ABI: descriptor / authority / grant | 12 / 24 / 8 bytes |
| P4 compiler static frames: begin / step / compatibility run / current | 32 / 48 / 16 / 16 bytes |
| P4 compiler static frames: commit / ack | 16 / 16 bytes |
| Sliced P4 runtime (r15, parent-controlled) | functional checks passed; maximum quantum 9,498 µs, **failed 2 ms gate** |
| Phase-diagnostic candidate runtime | **pending controlled device rerun** |

The sliced core call chain is step→current→authority check: 64 bytes **plus
the trusted callback and caller**. This is compiler stack analysis, not a task
high-water measurement, and excludes interrupts. State/workspace are caller-owned
(976 bytes together); zero BSS is not zero RAM. The maximum code, state, workspace,
event, descriptors and four grants total 3,112 bytes on this ABI if all kept in
RAM, excluding adapter/queue/lock overhead; immutable code can reside in flash.
WASM's 128 KiB fixed memory and 16 KiB stack are test-harness reservations, not the
core's embedded RAM requirement. Native batch CPU timings are reported only for
reproducibility, never as P4 WCET evidence.

## Fixed native P4 measurement entry

`pet_p4_program_probe.c/.h` exposes only `pet_p4_program_probe(send, context)` to
the trusted protocol worker. Main accepts the exact `debug/program_probe` topic
with an empty payload and no additional fields. It checks install/OTA/reboot
busy under the state lock, then releases that lock before the probe. Responses
use the normal `{topic:"debug/program_probe_result",payload:{...}}` envelope and
the original request's route, never a broadcast or an automatic retry.

The same collection vector now lives in `main/pet_p4_program_vectors.h`; the test
fixture is a compatibility include, and production never includes a tests path.
The four fixed cases are filter/map/reduce with commit (89 fuel), a backedge
that exhausts 4096 fuel without committing, and whole-program structural
validation of 256 instructions (no execution, usedFuel=0), and the same backedge
with the native 750us cooperative deadline checkpoint. Each runs eight times.
The fourth reports deadlineYields/zeroProgressYields; eight consecutive zero-work
slices or 512 slices in one diagnostic sample stop explicitly. This is not a
hard preemptive deadline; wall time still includes interrupts/cache/scheduling.
The ordinary core API remains fuel-only and same-C preview arithmetic is
unchanged. Only a trusted native factory may bind a monotonic clock; bytecode
cannot provide it. A checkpoint yields private work without consuming another
instruction or restarting its event. Invalid/backwards clocks cancel, and
accepted reports/committed state survive cancellation without task replay.
Execution uses begin/step with at most 64 fuel per slice, and yields one scheduler
tick after every successful slice. The 4096-fuel backedge takes 64 slices, not a
reduced total budget. First-slice timing includes begin validation; final-slice
timing includes the case's commit. Cancellation/old-generation checks use the
same sliced path. No supplied bytes,
case selector, user state, grants or owner handles are accepted.

The entry has a short concurrent-entry guard and five-second start interval.
Its owner acquires the unified broker's NATIVE_PROBE lease (one slot, 4096 fuel
units); PROGRAM and PERIPHERAL remain unavailable. Each instruction calls the
real lease check and freshly reads capture/playback/conversation/music flags.
These existing audio queries are atomic loads, not driver calls; the broker
check takes one short resource mux. Observed audio revokes the probe. Physical
occupancy remains until the core returns, and idle audio cannot resume the job.
Source, workspace, pre-state comparison and reply buffers are static probe-only
storage, with no user record or real outbox dispatcher. The protocol worker
exclusively owns this private state/authority through commit.

Results retain per-case min/max/sum of cumulative active microseconds for each
complete program, and separately report wallMinUs/wallMaxUs/wallSumUs including
all yields. Per-case quantumMaxUs/quanta describe the slices. Top-level
quantumFuel=64, quantumMaxUs and quanta also include the boundary-check programs.
They report exact result and total fuel, actual sizeof state/workspace, total wall time including yields but
excluding reply transport, and cancellation-before-commit / stale-generation
rejection checks. `taskStackMinBytes` converts the FreeRTOS watermark using
`sizeof(StackType_t)` (one byte in this ESP-IDF RISC-V port). It is the task's
lifetime minimum remaining stack, not a new-task baseline or incremental cost.
Errors are fixed codes and never echo private data. Partial/error samples cannot
be presented as a completed eight-sample performance result.
`slowestQuantum` captures the actual single slowest completed timing interval
(first on a tie), including kind/sample/slice and beginUs, stepUs, commitUs,
freshCurrentUs, stackScanUs, wallUs. Sample/slice numbering starts at one; null
means no valid timed interval. The five phase durations sum to this wallUs and
the top-level quantumMaxUs; unrelated phase maxima are never combined. Validation
uses beginUs for structural validation and freshCurrentUs for its two explicit
checks. Per-instruction authority checks remain inside stepUs; commit's own
authority check remains inside commitUs. Boundary-check kinds are
cancel_before_commit and stale_owner. The original per-slice watermark scan is
unchanged. These are wall-clock buckets, including measurement overhead and any
interrupt/preemption time, not task-exclusive CPU attribution. No priorities,
animation, system tracing or admission/performance gates are changed. Every
phase rejects negative/backwards clocks and stops without undoing a commit.
The sample count and timing aggregates include only fully verified measurements;
result/fuel retain the last actual core outcome independently. Audio observed
after a private commit (including during its final yield) aborts the overall
probe with `ok:false/error:audio_busy`, but retains `committed/89` even if the
verified sample count is still zero. No later sample runs or retries, and only
the probe's own owner is revoked. `not_run/0` is reserved for an unexecuted case.

Host verification, without PIO or USB:

```sh
python -m pytest -q firmware/tests/p4_program_probe_test.py firmware/tests/p4_program_test.py
```

Observed: 44 passed. Tests compile the exact production probe plus actual broker,
device adapter and core under ASan/UBSan with platform-only stubs. They exercise
all four audio flags before/during execution and between slices, cancellation between READY
and commit, all four flags immediately after commit and during its final yield
with another HOST_VIEW owner held unchanged, physical lease retention at core return, competing capacity,
thread/callback reentry, explicit rate-limited recovery, clock failure and byte/
word watermark conversion. A separate harness compiles the actual main entry
function with cJSON to verify strict complete-input parsing, empty payload,
caller route and unlocked read-only install/OTA/reboot gates.
Phase tests use deliberately different slow step/slow stack slices, assert the
reported tuple is from one actual slice with exact phase/wall equality, inject
negative and backwards clocks at all five phase boundaries, and parse a bounded
JSON response with timing values above UINT32_MAX without truncation.

Direct P4 object compilation with the existing SDK headers and `-O2` succeeded;
this is not a firmware build or runtime measurement. The fixed probe plus its
vectors report 8,659 bytes object text, 8 bytes data and 3,906 bytes BSS;
compiler-reported entry frame is 352 bytes, sliced-execution frame 128 bytes and current-callback frame 112 bytes,
in addition to core/broker/libc call chains. Final link/map and real task watermark
remain the device measurement's authority. Per-instruction atomic and broker
checks are intentionally inside the timed run; only the controlled P4 measurement
can establish their worst observed wall cost. No performance deadline or full
installable program capability is claimed before those measurements and a
separately reviewed loader, effect adapter and capability gate.

The prior unsliced r14 device observation (parent-controlled acceptance) reported
89-fuel filter/map/reduce at 181–1,451 us and 4096-fuel backedge at 7,239–30,873 us;
the latter failed the 10ms uninterrupted-call gate. That is not passing evidence
for this sliced version. The new acceptance target is observed per-quantum wall
time below 2ms, including actual authority checks, while publishing unchanged
total fuel and full invocation wall time. Tick yields can increase full elapsed
time; they do not make that duration satisfy the old 10ms figure. A 64-instruction
slice is a software work bound, not hard WCET or preemption of a delayed callback.
The core's original tests remain, plus all slice sizes 1–64, cross-slice overflow/
cancellation/reentry/revision conflict, duplicate finish and 50,000 random
sliced-versus-run parity comparisons in native sanitizer and WASM execution.
