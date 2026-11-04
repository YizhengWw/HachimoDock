# Default-off staged program images and cooperative runtime

This is a pure-compute integration building block, not an enabled `.clawpkg`
capability. `PET_RESOURCE_PROGRAM` remains `{0,0}`. No package parser, installer
command, production lifecycle or peripheral dispatcher is connected to it.
Passing host tests is not permission to enable the capability on P4.

## P4P1 version 1

`pet_p4_program_image.h` specifies the complete 24-byte header and little-endian
instruction representation. A file is at most 2,072 bytes: at most 256 eight-byte
instructions, 4,096 total instructions per event and 64 signed 32-bit state cells.
The header includes a nonzero schema identifier, bounded fuel and cell count.
Every instruction, including unreachable code, is structurally checked. `INTENT`
is forbidden: an image cannot request network, audio, filesystem, peripherals,
arbitrary native callbacks, pointers or grants. Outputs are eight integer values.

CRC32 covers the header excluding the checksum field, plus the instruction body.
It detects accidental damage; it is not authentication or an authorization
boundary. Full package integrity, provenance and admission remain the installer's
responsibility. Unknown flags, version/reserved fields, trailing bytes, truncated
files and invalid instructions fail closed. The loader handles unaligned bytes
without casting native structs and copies immutable code into staging storage.

Decode only into an **inactive** `pet_program_image_t`. Decode failure clears
that staging image; it must never overwrite the currently active package or its
saved data. Null/overlap argument errors leave the supplied storage unchanged.
The runtime copies a successful image, so the staging buffer may be released
after successful activation. There are no heap allocations or file operations.

## Trusted integration sequence

1. Capture the expected installed generation/schema and acquire the installer's
   bounded mutation admission. Read/decode into inactive staging away from the
   UI state lock. Validate the entire package and preserve the previous image.
2. Use the durable transaction store to publish a prepared package. A write with
   an unknown outcome is not a reason to retry blindly or delete the old copy.
   This module does not provide filesystem atomicity or crash recovery.
3. Acquire an admitted `PROGRAM` lease with units at least the image's total fuel
   from the unified broker. Until the P4
   budget gate is met, admission must remain unavailable. Supply the runtime a
   callback checking that **exact** lease/generation and audio protection, never
   a package-supplied function or an unconditional success callback.
4. Under the trusted owner/state serialization boundary, activate the validated
   image with a strictly newer owner generation. A live instance or an unread
   report cannot be replaced. Invalid image, denied authority and incompatible
   saved state leave the previous runtime byte-for-byte unchanged.
5. Route typed events through the bounded queue and call `pump` at most once per
   scheduling turn. Yield between calls. One pump does either begin, up to 1–64
   instructions, or one commit. Do not synchronously drain the queue.
6. Consume each terminal report before further execution. Publication is local
   integer state only. Read a committed snapshot for the trusted save transaction;
   never persist the private in-progress workspace.
7. Revoke/cancel on deactivation, audio protection, deletion or owner change.
   Release the physical broker lease only after runtime calls have returned.
   Cancel cannot resume when audio becomes idle. Restart requires a fresh owner.

All APIs are externally serialized with owner revocation and committed-state
mutation. The reentry guard blocks callback recursion; it is not a mutex or a
substitute for cross-task serialization. The current-lease callback must be
bounded, non-blocking trusted code and cannot mutate state/input or perform IO.
Runtime storage must not move; API buffers must not alias its storage.

## Default-off host and portable save adapters

`pet_p4_program_host` now implements the trusted in-memory part of this sequence:
owned staging, PROGRAM-only owner creation, exact-ticket checks, monotonic native
owner generations, one pump per call, report backpressure and stop-before-release.
Failed starts leave the previous committed runtime readable. Staging another
image never changes the live runtime's code or admitted fuel. Its reentry guard
also prevents a backend callback from reinitializing a live host. Host storage
must be fresh `{0}` for the one-time initializer and remain at a fixed address.

`pet_p4_program_host_device` is only a backend factory. It binds the existing
single broker and four audio-state observers, never starts a program or audio,
and the actual zero-capacity production broker rejects every activation attempt.
Successful host execution tests use a separate synthetic admission backend;
they do not open production quotas or prove device scheduling.

`pet_p4_program_store` defines the exact 320-byte little-endian P4S1 state format
and its 652-byte canonical JSON/hex envelope, within the existing 1024-byte
private app-store limit. Decode requires the trusted image's schema and layout.
Every rejection preserves the previous output, without fallback to empty state.
It restores only committed cells/outputs/revision, never events or receipts.
CRC is accidental-damage detection, not authentication or package permission.

## Explicit local scheduling and event composition

`pet_p4_program_timers` has eight copied timer specifications, an explicitly
supplied monotonic clock and a one-offer-per-turn enqueue boundary. Repeating
deadlines retain phase and coalesce missed intervals; they never catch up in a
burst. Full/busy local queues leave a due timer unconsumed. Clock/sequence faults
latch stop. Its approved enqueue adapter must still use the admitted host and
its shared event sequencer. Parent cancellation must stop BOTH timers and host;
it cannot free a resource lease before actual runtime work returns. Timers are
not persisted or automatically reconstructed from an old queue after reboot.

`pet_p4_event_bus` adds four native owner registrations with four volatile
integer messages each. Trusted host policy supplies publish/subscribe topic
masks after its own grant decisions; a component cannot supply a native token,
callback, broker pointer or authority. Registrations belong to one live broker.
Publication checks owner identity and atomically fans out to current matching
subscribers, or makes no enqueue if any live matching queue is full. Publisher
sequence IDs cannot repeat within that registration. New subscribers get no
replay, and a new receiver registration never inherits an old queue/token.

One delivery checks the exact current receiver and calls one approved LOCAL
enqueue, using the receiver's shared sequence. Busy/full is proven unaccepted,
so it retains the message. Any other non-OK enqueue latches stopped; it does not
guess acceptance, drop evidence or automatically resubmit. Receiver cancellation
also cancels its runtime/owner separately. Closing the sender does not undo a
publication already accepted into still-current recipients. This is not durable
cross-reboot exactly-once delivery or permission for external side effects.

Tests include native sanitizers, same-C fixed-memory WASM, P4 static objects,
30,000 mixed queue operations with non-vacuous accepted/delivered counts, and
the actual admitted host's queue-full/audio-revoke/new-generation path with
synthetic admission. Production quotas stay zero. A package registry, real
device lifecycle/scheduling integration and P4 timing/audio gates are STILL
required; no component-specific bus route or raw hardware capability is added.

Neither adapter is an installer or durable filesystem transaction. Before
activation can be opened, the parent must still bind package publication and
collision-free save keys, preserve BUSY/UNAVAILABLE load outcomes, wire real
lifecycle/commit locks and pass P4 timing/resource gates. In particular, sampled
audio flags alone are not atomic exclusion between cancellation and publication.
These adapters must not be advertised by hello, package schemas or generation
tools until that integration is actually accepted.

## State, cancellation and outcome semantics

The optional activation snapshot contains schema, cell-layout size, revision,
64 cells and eight outputs. It is a native in-memory representation, **not** an
on-disk ABI. The trusted save adapter must encode its own versioned format.
Nonmatching schema/layout or nonzero unused cells fail activation. A committed
`UINT32_MAX` revision is a valid terminal snapshot and can be saved/restored.
Subsequent events report `OVERFLOW` with no instruction execution or state change;
the revision never wraps or resets, and an accepted event is not replayed.
Schema equality is a compatibility declaration, not a proof that
arbitrary program changes are safe. Explicit migrations belong to the store.
Passing `NULL` means the host explicitly requested a new blank model; it must
never be used as automatic fallback when an old save cannot be read or migrated.

Four events can wait behind one inflight event. A host-generated nonzero sequence
is accepted only if greater than all previously accepted IDs in this owner.
Queue-full/rejected events do not consume the ID. An accepted event is not
replayed after a trap or an unknown external receipt. Terminal reports exert
backpressure and preserve result, used fuel, generation, sequence and revision.
This queue is volatile: no reboot-persistent exactly-once promise is made. The
host must not restore/replay events unless its durable transaction journal can
establish the appropriate outcome.

Cancellation drops queued/private work, retains committed state, and counts the
discarded events. Inflight cancellation creates a cancelled report; an already
committed report is never relabelled by subsequent cancellation. Snapshot/report
reads remain available after cancellation. Stale-generation calls cannot mutate
the replacement instance. No current-lease callback runs after a successful
commit in that pump; a later revocation cannot retroactively change its result.

## Current-owner integer UI projection

The default-off host can start an image with copied receiver metadata: up to
eight integer UI slots, their native domains and one output-to-slot mapping per
writer. Duplicate writers, empty mappings, unused nonzero domains, schema
mismatches and incompatible restored outputs are refused. The derived VM output
contract rejects an entire event before publishing any cell, output or revision.
There is no clamp, automatic reset or caller-swappable mapping on a live owner.
Legacy starts remain unprojectable and only clear the old recipe on success.

`pet_program_host_project_current` stages a full model from the latest caller
base, preserving unbound slots. It reads only committed outputs, including while
private work is inflight, and does not consume the terminal report. Exact native
lease/generation and audio protection are checked before and after staging.
Observed revocation latches cancellation; stopped historical snapshots remain
readable for saving, but cannot be projected into another UI. Errors preserve the
output buffer. Parent code must hold the same package/model/lifecycle mutex
through both this call and actual UI publication, and separately validate UI
identity and its resource lease. This is NOT a cross-component UI grant or an
installer route. Integer ranges do not validate arbitrary enum/text fields.

Native sanitizer tests cover immutable mapping, committed-only reads, receiver
edits, stale generations, revocation at final publication, audio cancellation,
failed migration, report preservation, aliases/reentry and all 64 port/slot pairs
at int32 extremes. They do not establish P4 scheduling or wall-clock bounds.

The trusted prepare/publication bridge now accepts this same receiver recipe
with the current verified package ID, identity and operation. A successful
publication copies the recipe and consumes the preparation exactly once.
Stale identities, overlapping metadata, denied admission and incompatible saved
outputs preserve preparation, prior host staging/runtime and durable state.
Publication performs no save read or other filesystem operation: the borrowed
storage ticket stays with the worker through preparation and publication.
The parent must checkpoint the prior committed state before preparing its
replacement and hold the package/model/lifecycle lock at publication. The
bridge does not guess an old host's component identity or auto-save/reset it.

The actual native store/activation/host integration fixture restores a durable
snapshot, refuses an incompatible receiver domain, publishes an immutable
projection, commits another event, checkpoints it and refuses UI projection
after cancellation. P4 object checks constrain each activation function's own
static frame and imported dependencies; they are not a complete stack graph or
device scheduling evidence. The installed package route remains closed.

## Shared native lifecycle and event sequence

`pet_p4_program_group` attaches a private already-admitted host, optional existing
timer schedule and exact native bus registration. It never constructs package
grants, substitutes its own unconditional broker or starts a program/task.
The parent transfers all invocation paths to this one group and holds the same
lifecycle/model/bus mutex for each call. Direct legacy host/timer/delivery calls
must not bypass it afterward. Objects stay at fixed private addresses; a stopped
group is not reset or reused. A new owner gets a fresh group.
The native host records one group attachment; a second fresh group cannot bind
the same instance. Failed restart preserves that attachment and old data. Only
successful activation of a new native owner clears it, so an old group cannot
cancel the new group's instance even when the host storage address is reused.

Input, timer offers and one-message bus delivery share a monotonic sequence.
Only OK local enqueue advances it; full/busy preserves pending child work. A
duplicate, unexpected result, sequence overflow or backwards timer clock never
guesses acceptance or retries an accepted event. The group performs at most one
host pump per call and never drains queues. Actual scheduling turns, fairness,
wall-clock bounds and native task contention remain the device parent's work.

Every live wrapper checks the exact host authority without executing a program.
Manual stop closes the owned timer/subscription and cancels the host. An observed
host self-stop/revocation also retires its children before returning; the parent
mutex prevents intervening offers. Physical lease release remains in the host
after private runtime work returns, not in timer/bus cleanup. Old groups cannot
close a new registration, cancel a reused timer with a new generation or release
a replacement program. A busy/invalid physical retirement retains occupancy and
does not falsely declare retirement complete. This is not an atomic audio-start
lock or proof that every possible native consumer has joined that exclusion.

Stopped groups retain committed snapshot/report reads without enabling UI
publication or automatic recovery. Saving those snapshots is still an explicit
parent worker/checkpoint operation, not done under this group's mutex. Tests use
the actual host/runtime/core/timers/bus with synthetic admission: complete
projection, shared sequences, full queues, single-pump slices, callback reentry,
mid-enqueue revocation, manual stop, report preservation, aliases, ownership
mismatch, old/new generation isolation and a broken parent bypassing sequencing.
P4 object checks constrain own static frames/dependencies only. None of these
tests opens the production quota, registers an installed program or measures P4
voice/AEC contention. Package routing and physical acceptance are still required.

## Verification and remaining hardware gates

### Native bounded cooperative dispatch (unreleased r38)

`pet_p4_program_scheduler` registers at most four already-admitted native groups
under the parent's one shared lifecycle/model/bus mutex. One caller turn selects
only one occupied slot, advances round-robin even when that group is busy or
waiting for its report to be consumed, offers at most one timer OR bus event,
then performs at most one pump. Both child sources present: alternate the offer
on each visit, even under queue pressure. Each group retains its own event
sequence; reports are never consumed to manufacture progress. Caller input and
explicit snapshot/report reads continue through group wrappers.

One group can attach only once to one fixed-address scheduler. Slot serials do
not wrap; delayed removal cannot cancel a newer registration or a new host owner
reusing an address. Removal/stop retains a BUSY retirement's slot and physical
capacity until the actual group can retire. Historical state/reports survive.
Per-registration removal latches stop intent before any potentially busy cleanup.
Later turns can only retire that slot, not pump or offer events; external live
group wrappers also refuse. A stale return alone is not proof of completed
retirement: an uncertain child still occupies its slot.
Manual stop and backwards explicit monotonic time latch stop for the entire
scheduler; subsequent live group wrappers also deny work. Stop performs at most
four native cancellations, never pumps a program, performs IO or auto-resumes.
Parent calls must yield and remain serialized; callbacks are trusted bounded
observers, not permission to run scheduler work recursively.

Tests exercise actual native modules with synthetic admission: four-way fair
progress, one quantum, pending-report isolation, timer/bus backpressure,
capacity, registration exhaustion, exact slot reuse, replaced native owners,
busy retirement, latched clock/manual/audio stop, aliases, callback reentry
during registration and pumping, and 10,000 nonvacuous mixed turns. P4 object
checks cover own frames and imported dependencies only. This is NOT a task,
installed program route, durable alarm, real-time fairness or deadline proof.
Production quotas stay zero; cadence, maximum quantum, combined load and voice/
AEC remain physical gates. Existing device timing failures are not waived.

Run `python -m pytest -q firmware/tests/p4_program_runtime_test.py
firmware/tests/p4_program_test.py firmware/tests/p4_program_probe_test.py`.

Observed for this change: **58 passed, 0 skipped** (14 new loader/runtime tests
and 44 existing core/probe tests). New coverage includes native ASan/UBSan,
10,000 malformed/mutated images, all quantum sizes 1–64, same-source WASM with
zero host imports, independent zlib CRC golden bytes, unaligned/max-size input,
cancel-before-commit, instruction-level revocation, callback reentry, queue
wrap/full/backpressure, stale generation, duplicate events, trap/no-replay,
save-schema rejection, and failed-activation rollback.

Direct P4 `-O2` object compilation (not firmware build or device measurement):
image storage 2,060 B; runtime 3,248 B including its owned image, state, workspace
and queue; optional snapshot 300 B; report 32 B. New object text is 780 B for the
loader plus 2,660 B for the runtime; data/BSS are zero. Maximum single function
frame is 80 B, excluding callees and the trusted adapter. Keep staging/runtime
off task stack; runtime plus staging plus snapshot plus report total 5,640 B,
before installer wire buffers, state locks and callback storage.

These are size bounds, not performance measurements. Prior P4 evidence for the
64-fuel probe had a 9,498 us slice against a 2,000 us gate. Before enabling any
program package, measure decode/activation/begin/step/commit separately with
the real broker/audio checks, max-size images, exhausted fuel, cancellation,
concurrent rendering and worst observed cold storage transitions. Also measure
complete invocation elapsed time, task watermark and total queue admission.
The host can choose a smaller quantum, but that alone does not prove a deadline
or guarantee audio/AEC isolation. No USB, flashing, audio or package publishing
was performed by these tests.
