# Host-view lifecycle, typed host resources and bounded admission

This increment connects ownership checks to the existing application host. It
does not add a package capability, permit new profile combinations, change audio
arbitration, or claim every DMA/heap/network/audio consumer has been migrated.
The PC now has a typed broker for the specific network/storage consumers below.
The later device-resource slice also gates the actual firmware host-view lifetime
with a typed lease; see [DEVICE_RESOURCES.md](DEVICE_RESOURCES.md). That targeted
snapshot/debug/view accounting is not a hardware-wide broker claim.

## Enforced runtime boundaries

`component_app` creates opaque, process-owned generation tokens. Component JSON
and desktop callers cannot supply these tokens. Device wire sequences must remain
in `1..UINT32_MAX`; comparison is scoped to board/component/instance, and ordinary
status polls do not rotate a generation. A stale or duplicate sequence is dropped,
an exit can address only its own current instance, and the last 32 replaced wire
identities are retained to reject delayed re-entry. The transport remains an
ordered, trusted local device stream, not an arbitrary replay-proof remote API.

The actual device event worker uses `OwnedQueue`: at most 24 pending requests and
one running physical lease. Pending statuses coalesce to the latest snapshot;
actions have priority with a status opportunity after eight consecutive actions.
At capacity an action may replace a pending status, never another accepted action.
Full action queues return the existing explicit busy receipt and do not retry.
Only bounded validated protocol fields enter this queue. Desktop requests acquire
one of eight permits **before** `spawn_blocking`; permits live through completion
or unwinding, so canceled frontend futures cannot free still-running work early.

Exit, instance replacement and observed disconnect revoke pending view work.
Already-running work continues to occupy its physical slot until it actually
finishes. Completion/release and cancellation require the exact owner/lease;
an old completion cannot free or cancel a new owner's lease. Owner validity is
checked at dequeue, after package resolution, before service dispatch, and before
reply delivery. No cancellation rolls back an already-entered synchronous local
transaction or promises to undo an already-submitted external side effect.

The firmware clears ephemeral values/actions/confirmation context when a host
view is hidden or its transport is replaced. Switching to a non-host page also
sends the prior instance's hidden event. Re-entry, content replacement and sequence
wrap issue a new wire token, even if the random source repeats in the same boot.
Replies to hidden or former tokens are rejected; the first matching action reply
acknowledges its reserved sequence, but later duplicate action/status replies
cannot extend freshness or replace values. Delayed actions cannot mount a different
component. These operations do not write Flash or private user storage.

Accepted typed-variable changes also advance an in-RAM epoch immediately under
the host lock. Gesture callers can detect changes away and back between rendered
frames. Unchanged status values and JSON object key reordering do not advance it;
clearing nonempty variables does. This does not reset physical-key repeat state.

## Distinct lifetimes

A view owner is not a durable workflow owner. Hiding a view does **not** disable
the component service, erase persistent records, clear composition source caches,
or cancel an already-accepted durable workflow. Explicit service disable keeps
its existing service-specific cancellation path; workflow run tokens and journals
own late-result rejection and completed checkpoints. Audio, microphone, speech,
AEC, media and resource-install transactions retain their existing arbiters.

## Integrated PC resource consumers

`component_runtime_lifecycle/resource_broker.rs` owns one process-local broker
with opaque ResourceOwner identity and non-cloneable physical ResourceLease
guards. There are two independent typed budgets:

| Resource | Capacity | Actual consumers | Admission |
| --- | ---: | --- | --- |
| PublicNetwork | 2 physical GETs total | Composition periodic/manual public JSON GET plus durable v3 workflow GET | At most 250 ms wait, at most 16 waiters, on background workers only |
| StorageCommit | 16 concurrent atomic replacements | Shared `component_app::atomic`, including composition/checkpoints and existing local record services such as journey | Immediate admission or busy; never wait or automatically retry |

The public network limit is shared across polling and workflow worker pools, so
adding durable workers cannot double actual GET concurrency. It does not yet
govern unrelated weather/news/media/API network paths; HTTPS validation, DNS
pinning and service-specific request limits remain in their existing adapters.
These permits do not authorize new endpoints, authenticated writes or paid calls.

The real device dispatch enters its queue owner's synchronous thread-local
scope. Durable workflow jobs explicitly enter an independent run owner: view
hide/reentry does not cancel accepted background work, while explicit run
cancel, service disable and retirement revoke that exact owner. Cache polling
has its own owner. Same-key replacement uses opaque identity, not only component/
flow/event strings, so an old completion cannot retire a newer run. The scope
guard cannot migrate across threads and restores its prior owner when nested.
Legacy synchronous atomic callers without a scope receive a one-operation
owner; this is not a persistent user identity or implicit authorization.

Cancellation denies new admission and invalidates results, but occupied slots
remain until the exact lease drops after physical work returns (including
unwinding). Network owners are checked before and after GET; cancellation does
not abort a blocking socket by pretending it has finished. Atomic storage checks
before preparing a candidate and immediately before rename, with no cancellation
check after publication that could label an already-completed commit unexecuted.
No broker mutex spans IO. Storage admission never waits while existing store
transaction locks may be held, avoiding nested-atomic wait deadlocks. Existing
per-store locks still own consistency; file formats and v1/v2 migration remain
unchanged. The broker does not strengthen power-loss durability or infer whether
an ambiguous external operation happened.

Resource tests cover cancellation/reentry, held capacity, waiter cancellation,
nested scope restoration, actual local-store writes, shared polling/workflow
network capacity and a late completion with a reused logical run key. They use
temporary directories/injected GETs, not user stores or real providers. CPU,
audio/recording, device transport and all other services retain their existing
schedulers/arbiters; this is an integrated first resource slice, not universal
arbitration or audio-priority replacement.

## Local regression coverage

- `component_runtime_lifecycle::tests`: exact-owner stale cancellation/completion,
  physical lease occupancy, queue capacity, status coalescing, bounded fairness,
  desktop admission and permit release on unwind.
- `component_app::tests`: actual device epoch + production queue integration,
  scoped wire sequence, stale hide/duplicate/replaced instance and reconnect.
- `petui_resource_lifecycle_test.py`: production C transport under ASan/UBSan,
  boot-time request, hide/non-host navigation, same-revision re-entry, late reply,
  delayed exit, action ownership, transport replacement and sequence rollover.
- Existing `petui_app_host_test.py`: action throttling, bounded snapshots,
  variable typing, confirmations and stale sequence compatibility.

Run only these focused tests; no ignored hardware or microphone suites are
required. Passing host tests is not physical input, USB reconnect, device frame
time, audio coexistence or acoustic acceptance. Those gates remain separate.
