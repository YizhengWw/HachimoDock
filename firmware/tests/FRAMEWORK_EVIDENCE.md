# Source, firmware and capture evidence

r113 emits `p4-render-stack-v3` (schema 3), adding the three native async
pack/I420-fallback/packed-fallback render paths to every original v2 path.
PC evidence readers retain strict read-only v1/v2 compatibility. The same
4096-byte reviewed-path budget remains; the native worker needs separate
actual ELF/static-frame and physical high-water checks. No full call-graph or
acoustic claim follows from these static paths.

r43 emitted `p4-render-stack-v2` (schema 2), retaining the same 4096-byte reviewed
path budget and adding the actual host collection resolver/match chain. Its
surface and collection compiler reports join the existing four modules. The PC
reader still accepts historical v1 records under their original exact shape;
it does not invent the new path in old reports. Neither version covers all call
edges, vendor/ISR stack or physical high-water/timing acceptance.

These development tools do not make the eight architecture gaps complete and
do not authorize a release. They distinguish evidence coherence from execution,
timing, component-package identity, acoustic acceptance and authentication.

## Read-only boot identity

Trusted startup initializes a 16-hex nonce once before worker tasks, using the
existing platform random source. Reinitialization cannot replace it. It is not
persisted, a device serial, a credential or a cryptographic authentication claim.
HELLO advertises `debugEvidenceIdentity=p4-evidence-identity-v1` and debugBootId.

`debug/identity` accepts exactly one bounded ASCII requestId and returns the
same requestId, engine, bootId, boardDeviceId, buildId, gitSha, buildDirty,
appElfSha256, protocolSchema and monotonic uptimeUs. The ELF digest is the
actual application descriptor's 32 bytes, not a guess from a version string.
Unknown request fields, duplicates, embedded NUL, invalid lengths or unavailable
identity fail. This does not install/activate components, change ownership,
access files, grant peripherals or start audio. Normal host-activity accounting
still applies as for existing messages. Screenshot begin additionally carries
bootId; older screenshot collectors may ignore this additive metadata.

The PC uses one request, at most 500 ms, 64 polls and 64 events per identity
observation. No write task is retried. Post-capture queries continue feeding
late screenshot errors to the collector; they cannot consume an error and turn
it into a success. Binding requires distinct request IDs, the same board, boot,
firmware/ELF/schema, increasing uptime and matching screenshot bootId. Reports
omit the raw board ID. These checks are not device authentication.

`PET_GRAPHICS_EXPECTED_ELF_SHA256` supplies an explicit hash from the trusted
local build artifact. Without it, sourceBound remains false. Opted-in hardware
capture can set `PET_GRAPHICS_REQUIRE_IDENTITY=1` to fail rather than accept an
old device or unbound source. Neither variable causes the offline runner to
connect. Actual transport acceptance remains an explicitly ignored/manual test.

## Explicit local build receipts

```sh
node pc/scripts/build-framework-evidence.mjs \
  --output /absolute/private/evidence \
  --pio-python /absolute/existing/platformio/python
```

This explicitly compiles the allowlisted normal v1 and separate UI candidate
targets in sequence, using an already installed SDK/toolchain. It has no flash,
installer, release, upload, audio or device command. Normal compiler-generated
binary/factory files remain local development artifacts. SDK caches must exist;
do not use this as a dependency installer. Local logs can contain machine paths
and must not be published as public release assets.

The receipt snapshots declared framework sources before/after compilation. A
changed/unavailable source or nonzero/signal/timeout cannot reuse an old image.
It compares the exact RISC-V ELF hash with the application image descriptor,
recomputes internal/external static memory from bounded ELF32 section tables,
and checks actual image length, OTA headroom and existing RAM guardrails. It
does not infer free heap, runtime latency or a clean public release checkout.
RAM accounting is explicitly `p4-allocated-ram-v2`: every nonempty allocated
section in the P4 shared IRAM/DRAM, TCM and LP-SRAM address ranges contributes,
including `.dram1.bss`, noinit and alignment/reserved storage. It is not the old
three-name sum, which omitted the second DRAM bank. Bounds/crossings/overlaps and
section names are checked; Flash/MMU dummy sections do not count as RAM storage.
The Python image guard independently reads section sizes AND addresses from the
actual size tool, and the build tool independently parses ELF flags/addresses.
Both retain the unchanged 384 KiB internal-static ceiling and 128 KiB minimum
OTA headroom. Reports include the exact accounted section contributions. Old
unversioned memory records are rejected by coherence verification, not silently
promoted to the corrected accounting. This remains static allocation evidence,
not free heap, DMA headroom, task stack, driver contention or timing acceptance.
The snapshot explicitly covers declared framework trees/configs, not every SDK,
external asset and private file on the computer; its scope is not hidden.
The current v2 hash encoding includes every regular file in those trees except
declared build/cache directories and `.DS_Store`, not just selected source
extensions. Local BSP/components, their CMake/YAML recipes, firmware build scripts,
factory config, binary source assets and the test vendors are included explicitly.
Paths and contents are length-framed and sorted canonically; repeated declarations
are deduplicated. Symlinks, special files, source changes while reading, files
over 32 MiB, total input over 128 MiB or over 8192 files fail closed rather than
silently being omitted. This is still not SDK/cache attestation or a sandbox
against a hostile filesystem/host, and it does not hash arbitrary user files.

Run build receipts BEFORE the offline suite, not concurrently. ESP-IDF temporarily
expands the three local dependency paths in `dependencies.lock`; the existing
PlatformIO post-action restores `$PET_P4_PROJECT_DIR` only after a successful
build. A concurrent suite can correctly observe the expanded lock and block
source consistency. Do not normalize it inside the hash function or reinterpret
that blocked run as passed: let the build finish, verify its own before/after
receipt, then start a new offline run against the stable portable inputs.

The image hook additionally checks ten reviewed static UI paths across renderer,
miniapp, host capture and mixed-form compiler `.su` reports. The 4096-byte path
budget is unchanged; no task stack is enlarged. Required external entries and
all reports must exist; dynamic frames or an oversized sum fail before image
generation. Optimized-away helpers are counted in their emitted callers, and
source tests guard helper renames. A bounded `.stack-budget.json` is tied to the
exact ELF hash and independently projected into the build receipt. It explicitly
sets allCallEdges=false: these sums exclude unreviewed/libc/driver/interrupt edges
and do not prove worst-case stack, runtime high-water or timing/AEC safety.

## Metadata-only coherence report

```sh
node pc/scripts/verify-framework-evidence.mjs \
  --offline /absolute/private/offline/report.json \
  --builds /absolute/private/firmware/report.json \
  --output /absolute/private/summary.json
```

Optional explicit `--capture ...json --raw ...rgb565le --profile normal|ui`
adds an already captured frame. No connection or capture occurs here. Missing
device evidence, skipped tests or missing stages stay pending. Incorrect source,
stale/failed builds, mismatched boot/ELF or false-success counts fail closed.
The checker independently recomputes all 200 chunk checksums, full pixel checksum,
byte sizes and raw SHA-256; an old report's passed=true is not sufficient.

The summary copies hashes, bounded version/budget/boot metadata and fixed gate
codes only—not images, raw board IDs, paths or arbitrary source/device text.
It creates a new file without overwriting an existing report. Pixel files and
input logs remain private; this is not a privacy approval to publish them.

evidenceChainReady only means the supplied artifacts correlate. frameworkComplete
and releaseReady remain false: correlated timing/operation and component-package
evidence, remaining capability acceptance, and voice/AEC gates are still separate.
An unsigned local receipt is not proof against a malicious device or modified
host. No task execution, component/data cleanup or automatic retry is inferred.
