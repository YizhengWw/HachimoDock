# Bodies: implemented shared device/preview core; HIL pending

The physics implementation is now `pet_p4_game.c` + `pet_p4_bodies_core.h`.
`scene-core.js` and its skill mirror configure the same C code through the
fixed-memory, import-free `classic-core.wasm`. There is no independent JS solver.
Device integration includes miniapp parsing, impulse operations, original scene
rule dispatch, snapshots, state/visibility/confirmation pause and body rendering.
`widgetBodies` remains withheld until unified build/HIL validates the device path.
Offline tests are not hardware evidence. Installation still requires the exact
hello string `widgetBodies: p4-bodies-v1`; do not bypass that negotiation.

## Declaration

Use a v4 runtime's ordinary `scene` with an open 4..16 by 4..16 grid (no rows or
solid_tones), tick_ms 20..200, optional integer gravity -64..64 (default zero),
and a `bodies` array of 1..8 entries. Existing entity/rule limits remain 12/20.
Each entry has `entity`, optionally `mass` 1..64 (default 1), `restitution` 0..100
(default 100), and boolean `static`/`rolling` flags (default false). References
must be unique declared entities. Bodies must be collidable and use clamp bounds.
Initial entity velocities remain integer -4..4 cells/second. Static bodies and
non-body decorative entities must start with zero velocity. Only declared bodies
collide physically; declare a stationary obstacle as a static body.

Rolling supports rect, circle, ball, coin, capsule, triangle and diamond, including
surface atlas sprites bound to those entities. Other semantic shapes can be
non-rolling bodies. Render snapshots carry Q8 position/velocity and rolling angle;
physics collision geometry is an axis-aligned rectangle regardless of artwork.

`{op: "impulse", entity: "ball", dx: 8, dy: -4}` applies a nonzero integer vector
in -64..64, divided by mass. Speed saturates at ±64 cells/second. Run, stop,
restart, show/hide, tone, score, var and sfx retain existing scene semantics.
Legacy move/place/velocity/accelerate/bounce and motion charge/launch/cancel are
rejected in bodies scenes. Existing motion and bodies cannot be mixed. Explicit
nulls, booleans masquerading as integers and unknown fields are invalid.

## Determinism and bounds

Each logical tick executes existing tick rules, then 16 fixed microsteps. Integer
millisecond partitions and division remainders preserve slow Q8 movement. Gravity
updates velocity before position. Walls clamp/reflect; ordered AABB pairs separate
on the shallowest axis (x wins ties), then exchange a mass-weighted normal impulse
using the smaller restitution. Two pair passes per microstep bound work. At the
maximum tick/speed, relative movement is below two one-cell widths per microstep.

Collision pairs and wall edges are deduplicated for one dispatch per logical tick,
after integration, using the existing rule engine. Restart restores initial body
state and score without resetting the existing runtime-variable semantics. Pause
rebases the scene clock; catch-up is at most eight ticks. Hidden bodies do not
integrate. Rolling is visual and does not change collision bounds. The solver has
no friction, angular momentum, polygon contacts or unbounded relaxation loop.

Device rendering uses a clipped inverse sampler for rolling shape/atlas pixels,
including alpha; other scene nodes retain their existing path. The package app
cache already excludes live games. AppSurface's actual scene preview now pauses bodies while confirmation is open,
and prevents late WASM initialization after disposal; the browser entrypoint
lifecycle test verifies pause/resume and cleanup.

## Validation

`contracts/bodies-v1.json` is shared by Python, Rust and preview schema tests.
Native tests extract the production miniapp entity/op/rule parsers and run the
actual device solver under ASan/UBSan. Native/WASM traces match through collisions,
gravity/static bodies, mass/restitution changes, large clock jumps and restart.
Analytical tests cover elastic/inelastic unequal-mass collisions and a high-speed
one-cell obstacle. Actual renderer tests cover 90-degree sampling, alpha and clip.
`smoke_bodies.mjs` uses that same core; its result explicitly has deviceVerified=false.
