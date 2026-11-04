# B workstream integration handoff (updated)

## Completed implementation

- `game.c/h`: appended impulse opcode, body configuration/Q8 state/snapshots,
  reset/init, validated bounds, physics tick dispatch, capped catch-up and pointer
  refusal for physics scenes. `pet_p4_bodies_core.h` holds the allocation-free
  device/WASM solver; no audio, task priority, clock gate or persistent stack edits.
- `miniapp.c`: only B body resolver include, impulse parsing, bodies/gravity scene
  keys/tick range, v4 check, resolver between entity and rule parsing, and bodies
  in the existing confirmation pause predicate. C's `_ext.inc` is untouched.
- `renderer.c`: B rolling-body sampler/helper and the body-aware position/rotation
  branch inside draw_app_scene. Existing generic image rendering/caching untouched.
- `protocol.c`: removed only the widgetBodies advertisement; it stays withheld
  until main-agent unified build/HIL verifies the complete device path.
- `component_extension.rs`: public validate_surface keeps V1; the internal
  validate_surface_with_budget accepts explicit image count/pixel ceilings, and
  app-surface normalization uses app_graphics::limits(runtime). Original image
  identifier/path/reference checks and the per-image 98304 ceiling remain.
  Original declaration validation counts JPEG toward the image-count limit before
  its separate pixels are removed. No D shell/form functions were edited.
- `app_graphics.rs`: bodies schema, shared vectors, profile and 8-binding tests.
  `clawpkg.rs`: only B bodies/gravity scene keys, tick range and impulse admission.
- Python `app_bodies.py`, B scene/impulse hooks in validate_generated_widget.py,
  bodies mechanical-claim detection, same-core smoke route and smoke_bodies.mjs.
- Frontend scene schema/admission now accepts valid bodies and graphics-v2
  sprite count/pixel ceilings. Device and preview use one game core; rebuilt
  classic-core.wasm and skill mirrors with the installed NDK compiler, no download.

## Eight sprites and scene bindings

`pet_surface_scene_view_t` now has capacity 8 on graphics-v2 builds and 4 otherwise.
Its parser still selects 4 for v1 packages even on an enabled build. Python/Rust
and frontend binding validators use the same profile-dependent limit. Renderer
and preview iterate all bindings; actual preview SSR tests render eight images.

Binding count differs from source-image count: eight entity bindings can share
one atlas. Surface image count remains 6 for graphics-v2; the new shared vector
uses one 128x16 atlas with eight bindings. The independent native scene sheet
quota remains 8 source sheets / 16384 decoded pixels (v1: 4 / 4096). No image quota
was silently raised to make the binding test pass. Frontend scene sheet admission
has also been updated from its stale fixed 4/4096 to the profile's 8/16384.

Shared graphics vectors now include valid8, invalid9 and valid4 binding cases.
They run under enabled/profiled, enabled/unprofiled and default-off C builds;
existing Rust profile tests consume the same vectors. Unknown/duplicate entity,
image/atlas and geometry checks remain intact.

## AppSurface entrypoint completed

The user subsequently delegated the bodies-only SceneAppPreview path to B.
AppSurface.jsx now pauses bodies during confirmation and checks cancellation
before creating a WASM core after a late fetch. Only these two lines in the
SceneAppPreview function changed; D form/notify/refresh paths were not edited.
There is no remaining AppSurface TODO for the main agent.

`pc/scripts/test-bodies-entrypoint-offline.mjs` runs the actual exported
ComponentAppPreview -> SceneAppPreview -> AppSurface with real C/WASM in Chromium.
`/tmp/b-entrypoint-pause-v3.log`: pass. It covers confirmation pause/cancel/accept,
no catch-up on resume, state pause/resume, active=false timer cleanup, unmount,
late fetch disposal without WASM instantiation, remount and runtime replacement.
Manual synthetic fixture: pc/tests/manual/bodies-entrypoint.html/.jsx. The browser
check is a separate opt-in script, not a new browser requirement for npm's Node
unit test suite. Set PET_TEST_BROWSER to an available executable when running it.

D owns C shell+canvas refusal. B did not edit that region: surface.c changes are
only its scene-view binding count; app_surface.py changes only that binding count;
Rust changes are B image budget plumbing and scene binding quota. Do not duplicate
D's shell admission patch.

## Validation ownership

No Cargo, PIO, serial or HIL was run after the user's disk recovery instruction.
The main agent owns unified Rust tests, device builds and HIL. Rust syntax was
parsed with rustfmt only; this is not type-check/test evidence. In particular run
app_graphics tests, the new bodies schema tests and full clawpkg regression.

Evidence logs are listed in the final workstream report. The older disk-full
runs are invalid; the former standalone-JS preview notes have been superseded.
Hardware budget acceptance and widgetBodies advertisement must wait for actual
main-agent evidence; no gates were loosened.

## Final offline evidence for this pass

- `/tmp/b-final-c-python.log`: 37/37 pass. Includes production entity/op/rule
  extraction, body solver native/WASM parity, real renderer rotation/alpha/clip,
  default-off admission, full Python validation, package smoke, retention pixels,
  canvas/particle tests, 8-binding profile/build matrix and old game regressions.
- `/tmp/b-final-node.log`: 41/41 pass. Includes analytical mass/restitution,
  maximum-speed obstacle, lifecycle, frontend bodies admission, actual eight-image
  preview, profile-scoped bindings and 8-sheet/16384-pixel frontend boundaries.
- Three changed JSX components parse with esbuild; Rust files parse with rustfmt
  --emit stdout. Neither is a substitute for main-agent unified build/tests.

The regenerated classic WASM is 28 KiB with fixed 262144-byte memory and no
ambient imports; C/WASM traces compare integer Q8 snapshots exactly. No hardware
or frame-time/heap-budget acceptance is claimed by these offline results.

## Private combined HIL candidate (data only)

The private candidate workspace (kept outside the public source; not distributed)
contains package/, explicit offline candidate capabilities, validator/smoke results,
plan.json, deterministic expected-snapshots.json and replay.mjs. Complete Python
validator and same-core smoke pass. Ten snapshots cover eight bindings/bodies,
real collision score, canvas v2 and 16 particles. Replay ran twice byte-identically.
The candidate caps are synthetic, not an actual device report. Nothing was
packaged, published or installed. widgetBodies remains withheld for unified
main-agent hardware admission. No B code blockers remain for unified build.

## 候选协商更新

主代理集成阶段在 `#if PET_P4_BODIES` 下恢复 `widgetBodies`，该 flag 默认关闭，仅私有能力候选打开。solver 无异步初始化，无额外 ready 条件。上文“withheld”是此前阶段状态；当前允许候选安装以执行真实 HIL，不能据此推断实机通过或 releaseReady。

## Urgent shared Track C entrypoint integration (B, frozen)

C supplied address-bound `pet_cext_publish_ticket_t` prepare/commit/abort APIs in
its owned header/fragment. B changed only shared caller/snapshot/render branches:

- miniapp.c: memory activation checks prepare/commit and aborts failed commits;
  prepared activation reserves while storage is unlocked, aborts on cancel/error,
  commits before marking its in-memory publication durable. Install and active
  delete replacement prepare before irreversible catalog writes, abort on write
  failure, and check commit before runtime/catalog publication. ext_swap uses one
  captured decision. No new resource load is attempted after catalog durability.
- A defensive durable commit failure is quarantined: old runtime/catalog RAM and
  old files remain, no successful publication/cleanup occurs, recovery is blocked.
  PUBLISHED/CLEANUP phase advancement here is terminal error reconciliation, not
  successful runtime publication; it permits the physical storage scope to close
  without a durable-unfinished assertion. Activation is blocked until recovery.
- miniapp.h + miniapp.c: render snapshot media_generation copied from cext under
  the same runtime mutex as view/surface/images, zero when extension is disabled.
- renderer.c: media-generation tag from that snapshot, hidden/page-invisible/no
  clip retirement every frame, main H264 source gate until retirement completes,
  continued retirement on transfer-status and init/rotation-error frame paths.
- tests/component_media_entrypoint_test.py: actual production memory activation,
  prepared/cancelable activation, full install/delete functions, full renderer
  entry, production snapshot and real C clip-retire fragment. ASan/UBSan tests
  exercise failed prepare, failed catalog, defensive failed commit, exact ticket
  cleanup, same-lock old snapshots and worker/reader/EMPTY barriers.
- tests/petui_storage_mutation_test.py: old harness gains the actual unchanged
  PET_FILE_CLEANUP_MAX_TARGETS=24 constant for the main agent's two-plan tail.
  B did not edit that tail, increase its 24-target gate or alter forget ordering.

`/tmp/b-cmedia-entrypoints-final.log`: **9 passed in 4.50s**, including original
storage mutation and cold activation regression cases. No Cargo/PIO/HIL/full
pytest was run by B. C fragment/store and D form/notify/refresh remained owned by
those workstreams. No remaining B code blocker; shared-entry changes are frozen.
