# Default-closed external outcome boundary

`pc/src-tauri/src/component_external_outcome.rs` is native orchestration logic,
not a new composition schema or usable cloud connector. No production transport,
credential reader, background worker, IPC command or provider is registered.
A private native Unix file-journal adapter now exists, but has no application
caller or registration. Existing public GET workflows and local transactions are
unchanged; they do not silently adopt this experimental storage format.

## One submission attempt, then explicit reconciliation

An operation key binds component ID, installed definition digest, stable event
ID, step, audited connector/operation names and an immutable request digest.
These are host metadata, not permission supplied by a component. A native
connector must separately bind its actual body/destination/credential scope;
the metadata/hash alone is NOT a grant or proof of request identity.
The durable slot and process-local logical occupancy use component+event+step.
The mutable definition/connector/payload fingerprint is deliberately excluded
from slot identity: changing a request must encounter the existing record and
conflict, not silently create a second empty journal for the same event.

The only dispatch-ready transition is `Prepared/AwaitingAdmission -> Claimed`, committed durably before
dispatch. Only a trusted journal's proven `Committed` response permits native
admission/provider invocation. Known noncommit, ambiguous persistence and CAS
conflict invoke no provider. There is no implicit journal initialization or
reset on a missing, damaged, changed or unavailable record.

Both entry points use the same native, 32-slot process-local occupancy registry
before journal access, retaining an exact serial lease through outcome
publication. Busy/full, serial exhaustion and poisoned state never evict a live
slot or reset the registry. A stale permit cannot release a newer slot, including
when the caller changes its request fingerprint. Only short table access is
mutex-protected; no table/lifecycle mutex crosses journal or provider calls.

Native admission must authorize the exact key and reserve physical resources.
A cancel cannot release a physical operation that is still executing. This is an owned reservation, not
a lifecycle mutex held across network IO. A real adapter must implement/review
these requirements; a trait and memory fake do not establish production safety.
Process-local logical occupancy is not a new network/storage quota or a grant;
cross-process CAS is supplied by the private file backend below, not by this
process-local registry. Cross-process physical admission is still required.
Submission and read-only reconciliation have distinct native access kinds;
granting receipt lookup does not grant a create/submit operation. This is not
permission to borrow the existing public-GET grant or credentials.

Physical busy/unavailable admission is separate from revoked authority. Because
the approved submit method has provably not been called, a durable
`AwaitingAdmission` checkpoint permits a later **explicit** admission attempt.
The module does not schedule/retry it. If saving that checkpoint is uncertain,
it returns reconciliation-required; an old `Claimed` record is not guessed safe.
Admission failure during reconciliation NEVER rearms an unknown submitted job.

`Claimed` and `Unknown` never become `Prepared` on timeout, reload, cancellation
or a new host owner. Reinvocation does not call submit again. Completed/rejected
records return their recorded outcome. If the provider returned a proven result
but final journal replacement fails or is uncertain, the returned decision keeps
that observed result with `durable=false`; it does not falsely claim the action
never executed. The prior durable claim still blocks another submission.

Recovery is a separately admitted **read-only** reconciliation call. An ordinary
not-found/404, timeout or model assertion cannot prove nonacceptance; it remains
unknown. Only an audited provider's proven nonacceptance is terminal rejected,
not automatic permission to resubmit. A fresh user action needs its own event
and authorization; the framework never generates that permission itself.

## Data and evidence limits

Records encode at most 2 KiB of bounded metadata and an opaque native provider
reference, not credentials, headers, request bodies or raw exception strings.
Record/key do not implement Debug. Decision's Debug deliberately omits provider
references. These remain private host records, never package/share assets.
JSON decoding is strict and validates phase/reference relationships. A stable
component ID follows the existing 44-byte lowercase identifier grammar and is
not a filesystem path; future file adapters must still own and validate paths.

Twenty-three production-source orchestration tests use only synthetic memory journals,
native permit fakes and injected provider outcomes. They cover claim/finalize
failure ambiguity, cancellation, crash/panic, unknown outcomes, changed request
identity, bounded codecs, reference-redacted diagnostics, reconciliation without
submission, 16-thread single-owner contention and 1,024 distinct-key concurrent
admissions without false Busy, exact-serial release, full/poisoned
registries and exclusive occupancy. Fifteen additional actual-source tests exercise
the file backend in isolated temporary directories, including eight real child
processes racing one slot and a process exiting without unwinding inside a
synthetic provider. No real service, credential or user journal is contacted.
The broader offline runner includes both modules and hashes the journal source.

## Private append-only file adapter

`component_external_outcome/file_journal.rs` supports native Linux/macOS only.
Windows has no implementation or fallback. It accepts a parent-owned existing,
durably provisioned absolute directory (owner-only mode 0700), uses an opened
directory descriptor and `openat` with final-symlink refusal, and checks private
regular files (0600, same uid, one link, same inode at the anchored path).
Trusted path ancestors and authorization belong to the parent, not a component.
These checks do not claim protection from another malicious process with the
same uid or root, or make filesystem paths a permission system.

An explicit fresh-event `create_new` is exclusive; `open_existing` never creates,
resets, changes permissions or deletes anything. The filename hashes only the
stable component/event/step slot. Changed request metadata encounters the same
record and conflicts in orchestration instead of creating another empty slot.
Each compare/append holds a nonblocking OS file lock, compares the complete
expected record, validates allowed transitions and appends a strict canonical
record with sequence and hash chain. SHA-256 here detects damage/chain mismatch,
not authentication. Logs hold at most 128 records/273408 bytes per slot; full
logs retain their state and refuse further writes, without eviction/compaction.
The directory additionally admits at most 128 slots. A process-local try-lock
protects callers sharing one open directory description; a nonblocking directory
flock protects separate opens/processes. Fresh creation holds both through its
initial durable record. A bounded descriptor-relative scan counts interrupted
empty files too and refuses unknown entries, read/close errors or a full quota;
nothing is recursively read, reset, compacted or evicted. Existing records remain
loadable/reconcilable when new creation is full or an unknown entry is present.
This is private namespace allocation admission, not a lifecycle mutex or a grant.

Before a successful claim, file and directory barriers must succeed; macOS also
uses F_FULLFSYNC for file write-cache flushing and again after the directory
barrier so later metadata writes are covered. Loads establish barriers too, so
an observed record after a lost acknowledgement is not simply called durable.
Any failure after the first append write returns Unknown. Torn tails, bad chains,
unknown fields, truncation, permission/path changes and missing records fail
closed, preserving evidence; there is no truncate-to-Prepared repair. A complete
old Claimed record or a damaged completion never permits another submit.
Proven provider results stay observable with durable=false when finalization
fails. Local OS barriers depend on the filesystem/storage honoring them; these
host tests are not a sudden-power-loss or hardware durability acceptance.

The OS lock covers only bounded journal work, not provider calls or lifecycle
mutexes. A durable claim supplies submit exclusion across processes, but a real
admission adapter must still coordinate actual provider occupancy and separately
authorize read-only reconciliation. Synthetic tests cannot grant those rights.
Injected failures cover before/partial/full append, file barrier, lost completion
acknowledgement, process exit, changed fingerprints, full logs, stale CAS and
invalid transitions. No provider body, URL, credential or raw error is persisted
or emitted. Opaque references remain private records, not exported diagnostics.

Still required before opening a connector: concrete audited grant/credential
handling; destination/redirect/SSRF policy; Windows durability and cross-process
physical admission; authorized terminal-record archival without destroying unknown outcomes; physical
resource and cancellation integration; UI consent/confirmation; real provider
submit/poll/reconcile and privacy acceptance. Do not adapt existing temporary
file sync+rename as a proven durable journal without closing its parent-directory
and crash-outcome gaps. This module is not a paid-API or exactly-once claim.
