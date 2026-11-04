# Decorative particles

`p4-particles-v1` uses the same allocation-free integer C core on firmware and in
`canvas-core.wasm`. Up to two emitters have separate seeds, rectangles and guards.
Each has at most 64 live slots; drawing is limited by its declared budget. The
20 ms step runs at most eight catch-up steps. Dot/spark rectangles are clipped to
the emitter rectangle. No node movement or collisions are implied.

`ParticleLayer` loads the core, initializes declaration slots and calls
`stepParticlePreview`. The latter consumes at most four queued actions per frame,
skips other pages, checks own integer snapshot properties and the six comparison
operators, then bursts, steps and draws in slot order. A closed guard prevents
new bursts/spawns; existing dots continue to age. Page 4 is global. A missing
snapshot fails the guard closed.

Firmware draws particles after cached package content. Final logical-frame reuse
must be disabled whenever a surface declares emitters, even if no dot is visible.
`p4_logical_retention_test.py` extracts both production `PET_PARTICLES_ACTIVE`
branches and executes the production eligibility expression with particles off/on.
The original default-off full-pixel comparison remains intact.

Require hello `widgetParticles: p4-particles-v1`. The shared surface owner must
pass current page, integer vars, clock and a bounded action queue into the layer;
this document does not assert that every shared owner already does so.
