# Bounded native keyed collection (not enabled as a package capability)

`pet_p4_collection` is a pure native model, not a renderer, installer, record
store, background source or authorization API. It has no component-ID branch,
IO, callbacks, heap, clock or hardware dependency. It is compiled for the P4
candidate but no protocol, main loop or package parser calls it. Existing PC
composition collection declarations and their saved files are unchanged.

## Model and storage ownership

The trusted caller supplies immutable native rows with a unique 1–63-byte UTF-8
key and 0–512-byte label/detail. Controls, invalid UTF-8, duplicate keys and
missing fields are refused. There are at most 128 records and 1–8 visible slots.
The caller owns row/string lifetimes and serializes dataset changes with each
projection. No pointer or source record is retained or returned by the core.

The full records stay with the parent. A view copies only the stable key and
bounded display labels. Each display field is cut at a valid scalar boundary to
at most 63 bytes, with `truncated=true`; this never edits the source record or
uses a truncated label as an identity. Selected records can be resolved by the
parent's stable key; this layer does not return an unbounded full JSON record.

State contains only a saved offset and selected key. Status may clamp its
rendered page, but does not silently rewrite either saved field. Sorting or
filtering can temporarily hide a selected ID without selecting another record
or erasing the saved key. Explicit navigation/selection changes only the view
model, never edits, removes or submits records. Nonexistent rows are errors,
not a fallback to the last record. All failures preserve state and output.

Workspace and view buffers are fixed caller-owned objects (less than 1.9 KiB
each), intended for private off-stack storage, not large task-stack temporaries.
Input rows, strings, state, output and workspace must have the documented
nonoverlap/lifetime relationship. Source aliases to mutated buffers are rejected.
Full data validation is bounded but includes duplicate-key comparisons; it has
NOT passed a P4 wall-clock deadline. Do not put it into the render critical path
or remove capability/resource gates based on a host benchmark.

## Same-source authoring preview and compatibility evidence

The optional `collection-core.js` authoring utility runs the same C model in a
fixed 256 KiB zero-import product WASM, never component-supplied code. Creating a
replacement dataset first builds/validates a fresh instance with the previous
saved view state. Failure leaves the old instance intact. No file or record
mutation, credentials, network, audio or device execution is involved.
Read-only authoring status returns the already committed native view; it does
not rerun full immutable-source validation per paint. Explicit actions still
use the same complete C projection, and replacement uses a new validated
instance. Caller edits to returned JS objects cannot mutate native state.

`collection-model-v1.json` runs in the native C test, product WASM/JS and existing
Rust PC service. It checks status-only clamping, stable selection after reorder,
filtered/empty data, missing-row/duplicate-key rollback and navigation. Rust
remains an independent implementation; matching vectors do not mean all source
code is already shared. Native and same-C WASM sanitizer/property tests cover
30,000 mixed dataset/selection operations, UTF-8 extremes, aliases and malformed
data; product tests add replacement isolation and raw ABI narrowing checks.

The offline browser fixture projects this model through the actual existing
`AppSurface` with bounded scalar bindings. Real pointer tests cover stable
selection, empty-slot disabling and dataset-context changes canceling old taps.
This is an authoring fixture, not a new component route or device control.

## Remaining admission and hardware gates

## Default-closed owned native lifecycle bridge (r41)

`pet_p4_collection_host` prepares an inactive fixed bank by validating and copying
all projected source strings. Native source pointers do not survive preparation.
Fresh/unbound banks may be explicitly prepared; an in-use bank is never rewritten.
Caller-private scratch and banks stay off task stacks. Preparation is worker work,
not a renderer or UI-lock operation; the actual parent must provide PSRAM and
physical resource admission. This code does not allocate or grant either.

The current UI receiver copies its expected source/declaration identity at init;
the first candidate cannot choose the receiver's identity. First publication
requires an explicit seed, including an explicit zero state for a new model.
Replacement retains navigation/selection when seed is absent, requires the same
identity and a strictly newer dataset revision, and preserves old state/binding
on failure. Incompatible saved states are refused, not cleared or clamped.
The parent still namespaces saves by verified component/declaration identity;
this library neither reads files nor proves an arbitrary seed's provenance.

The shared core's private prevalidated path is used only with those immutable
owned records; old public/authoring projections keep full validation. A paint
reads only the already committed copied view. Native tests poison all source
row/text storage during 10,000 cached reads to detect accidental source scans.
Display truncation remains explicit and never changes the owned full strings.

Each publication/action changes a native observation epoch. Actions require
exact host/generation/epoch/revision, so reorder/filter/page and away-back changes
cannot turn old clicks into selections of other records. No-row errors preserve
state/epoch; a successful action consumes its observation even if display values
are unchanged. No automatic retry, record edit or external submission occurs.

Capture under the shared parent UI/model/lifecycle lock; rasterize the copied
view outside it; reacquire it to recheck context/UI identity/physical lease before
frame publication. The exact-context observer does not scan data or authorize a
different UI. Callback reentry is rejected; the inside flag is not a mutex.
Authority loss, binding loss and explicit cancellation latch stop, detach only
their exact bank and retain committed historical state. Old hosts cannot detach
a bank reused by a newer host. Parent still releases actual leases after work.

The fixed product WASM now has an explicit memory + cv_* export list. It does
not expose the trusted prevalidated native path, host lifecycle or memory helper
symbols. It remains 256 KiB, zero-import and non-growable; the JS/skill mirrors
and existing public-core semantics are unchanged.

Native sanitizer/property tests cover owned source isolation, transactional
replacement, receiver identity before first bind, stale/foreign observations,
source/text bounds, aliases, callback reentry, binding loss, cancellation/reuse,
counter exhaustion, cached reads and 10,000 turns against the original complete
core. P4 object checks constrain own frames and layout caps only. No real UI
authority backend, native installed control, package declaration, persistence,
mixed form/service route or physical timing/input/font gate is wired by this
bridge. Do not advertise or enable it from these offline results.

## Remaining installed-control work

A real installed collection still needs a versioned package contract, exact
parser/host negotiation, dataset/UI resource ownership, current-generation
gesture observations, lifetime-safe immutable frame capture and bounded save
publication. A view snapshot itself is NOT an input grant: parent code must
reject old dataset/row identities under its own UI/state/lifecycle lock.

Device-local editing/submission, dataset persistence/migrations, mixed forms
and host sources, virtualized native layout and its P4 performance/input/font
acceptance remain separate work. This core does not raise 32-node/8-variable
limits, open PROGRAM/PERIPHERAL quotas, remove existing incompatible-profile
checks, certify long frames or replace voice/AEC acceptance.
