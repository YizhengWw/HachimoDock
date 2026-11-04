# Graphics v2 integration contract

This document describes bounded quotas, not proof of device availability. Install
only after hello reports `widgetGraphicsV2: p4-graphics-v2`. The package must name
`graphics: p4-graphics-v2` on a `p4-bounded-runtime-v4` runtime. Merely enabling a
firmware flag must not enlarge v1 package budgets.

| Budget | v1 | graphics v2 |
| --- | ---: | ---: |
| Surface images, including JPEG background | 4 | 6 |
| Total palette pixels, excluding JPEG background | 98304 | 196608 |
| Compiled palette image bytes | 131072 | 262144 |
| Fonts / characters per font | 2 / 49 | 4 / 98 |
| Font atlas maximum width | 240 | 512 |
| Scene sprite sheets / decoded pixels | 4 / 4096 | 8 / 16384 |
| JPEG background frames | 1 | 4 |

Each palette image remains at most 512×240 and 98304 pixels. JPEG is allowed only
at images[0], at 640×480. Image dimensions are declarations; compiled payload byte
limits and decoded headers still need independent package checks. Font and node
reference checks must also remain in place.

`app_graphics::validate_image_budget` validates declaration counts, individual
palette dimensions and summed palette pixels. It does not replace structural,
identifier, reference, font or compiled-byte validation. The public legacy `validate_surface` wrapper keeps v1 ceilings. App-surface
normalization now calls the internal `validate_surface_with_budget` with the
explicit package profile, retaining all legacy structural checks.

Canvas v2 requires the graphics v2 build parent; explicitly disabling
`PET_CANVAS_ENABLED` beneath `PET_P4_CANVAS_V2` is an error. Particle and bodies
switches also require graphics v2. Keyframes additionally require native prefetch.
Every switch defaults to off. No build or HIL result is asserted here.

Scene-view bindings are also profile scoped: 4 in v1, 8 in graphics-v2, including
compiled C array capacity. Eight entity bindings can reuse one atlas; the surface
image limit remains 6. The shared valid8/invalid9/valid4 vectors check this path,
and the actual preview component is tested rendering all eight entity images.
Native scene source sheets retain the separate 8 / 16384 decoded-pixel quota.
