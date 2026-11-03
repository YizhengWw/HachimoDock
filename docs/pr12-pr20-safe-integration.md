# Joint PR12 / PR20 integration

Baseline: `b72fce800594947d57310c3dd3b260b8fffa28a7`.
Retained merge ancestry:

- PR12: `56d507fe8ba1becbb12363bb0e94ba64a75ab1ce`.
- PR20: `7a790df5b2185d7fd8f501e82a68e4be5ffbbf0d`.

The clean integration excludes the separate private R168/0.1.178 development
overlay. It preserves both branches' native IPC registration/permissions,
authenticated DSH ownership and bounded bubble authority snapshots. Session
cards keep their session owner while text changes advance the bubble version;
omitted owners retire independently of DSH approval/voice leases. Approval text
uses local font rendering and cannot borrow a normal bubble's host pixel pin.
The original 21,207-glyph fallback, 199-glyph native32 font and font notices
remain as supplied by the reviewed PR12 branch. No later private font/storage
implementation is imported here.

Automatic merging required three code corrections:

1. Pass the new optional pixel pin parameter as `NULL` for local approval text.
   Otherwise the combined renderer does not compile.
2. Emit the stopped-session cue only after a card passes revision/retention
   admission. Remove the second unconditional event and retain revision-based
   deduplication, preventing duplicate or discarded-history sounds.
3. Let `approval/cancel` reach the common parsed-message cleanup. Its early
   return leaked the JSON allocation even for stale request IDs.

Focused host tests compile the actual approval draw functions against the actual
body-helper prototype, the actual session snapshot handler against real cJSON,
and the actual cancellation branch with allocation accounting. They cover local
text isolation, current/stale/expired/replayed stop transitions and stale/current
cancellation cleanup. Existing cue fixtures now include `session_terminated`
and recognize the production conditional allow/deny call.

Joint verification on 2026-11-03: 148 selected Node tests and 22 focused host
tests passed in a synthetic HOME with external network, real credentials and
serial/IOKit denied by the OS sandbox. Rust parsing and whitespace checks passed.
The joint offline release Cargo lib check was **not started** because the
shared build/HIL lock was held by the priority mainline lane; no dependency
installation, bundling, downloads or full regression was performed.

Pending: combined native type check, actual dual-target firmware build/OTA
headroom/static RAM and stack accounting, device/audio acceptance, actual
Windows ACL execution and installed DSH API/runtime validation. Earlier separate
branch checks do not establish these joint results. No HIL or release-readiness
claim follows from the focused host checks.
