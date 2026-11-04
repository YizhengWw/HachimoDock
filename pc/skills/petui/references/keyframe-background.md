# Keyframe JPEG backgrounds

Require package `graphics: p4-graphics-v2` and hello
`widgetKeyframeBackground: p4-keyframe-bg-v1`. The first image must be a 640×480
JPEG declaration with 2–4 frames, 1–2 fps, and an id of at most 12 bytes. A static
background remains frames=1/fps=1 and does not require keyframe capability.

Frame 1 uses the declared PNG source and id. Frame k>=2 uses `assets/bg.k.png`
and pack id `bg.k`. Each encoded P4J1 body is at most 100000 bytes and the full
set at most 400000 bytes. The source PNG is not itself a device JPEG payload.

Preview index is floor(clock_ms*fps/1000) modulo frames. Missing preview siblings
fall back to the declared image. Actual decoding must stay on the existing native
asset prefetch worker; no frame-path filesystem/decode work is authorized.
The negotiation regression test deliberately selects an animated fixture, rather
than the first fixture, which is a static background.
