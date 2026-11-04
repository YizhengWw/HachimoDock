# P4 trusted read-only service boundary

## Scope and admission

`pet_p4_trusted_services.c` provides a fixed service directory, exact per-service
grants, a bounded read budget, cancellation and typed diagnostics. The concrete
`pet_p4_trusted_services_device.c` adapter has two providers only:

| ID | Service | Native source | Effect |
| --- | --- | --- | --- |
| 1 | `clock.monotonic`, version 1 | `esp_timer_get_time()` | Read monotonic microseconds |
| 2 | `input.joystick`, version 1 | `pet_p4_input_get_joystick_snapshot()` | Read the input runtime's existing ADC diagnostic snapshot |

The joystick provider does not initialize the ADC, take a new hardware sample,
change calibration/bindings or produce an input event. A runtime that has not
prepared its joystick snapshot returns `NOT_READY`.
The input task publishes diagnostic rows through `pet_p4_observation`, not a
sequence of independent atomic-field reads. A reader retains a physical slot
pin until its bounded copy completes; writers cannot reuse that slot early.
Three fixed slots (64-byte maximum payload) and weak-CAS acquisition attempts
do not block a writer behind a preempted reader. If no coherent row is available,
the public diagnostic getter returns zeroed `ready=false`. This does not take a
new ADC sample, change calibration or affect the decoder's authoritative inputs.
Native ASan/UBSan and ThreadSanitizer stress covers two writers/four readers;
the actual input writer/getter is also paused before publication to verify that
partially updated private fields never become a public row. P4 atomics/stack
compilation is checked, not substituted for measured device timing or audio.

Compiled descriptors are **not advertised package capabilities**. The current
production resource broker keeps `PERIPHERAL` at zero capacity until the parent
runtime's integration, timing and lifecycle gates have passed. The adapter then
returns `UNAVAILABLE` without calling the clock or joystick provider. This change
does not add a wire command or a package manifest feature.

No GPIO, I²C, SPI, camera, DMA, codec or interrupt operation is exposed. An
unrecognized service ID returns `UNKNOWN_SERVICE`; there is no generic native
function lookup. Packages cannot supply a GPIO number, bus address, register,
interrupt handler, native pointer or provider callback. Future hardware access
needs a reviewed board-specific native provider and exact capability grant; a
raw peripheral name is not permission to claim its pins or drivers.

## Connection to the shared lifecycle

1. The trusted runtime admits an event using its **existing** broker owner and
   generation. If approved, that owner's resource mask includes `PERIPHERAL` in
   addition to the runtime resource (for example `PROGRAM`).
2. The runtime maps its already-validated service grants to the clock/joystick
   mask and initializes a private authority for that invocation. The authority
   stores the same owner; it never creates an independent owner or privilege.
   Its read limit is 1–16 and cannot be reset by the program, a BUSY result or a
   retry of the same event. Authority storage must first be zero-initialized;
   initialization rejects an already initialized, cancelled or in-flight object.
   A new event needs a fresh private authority and normal runtime admission.
3. Each read acquires one physical `PERIPHERAL` unit from the shared device
   broker, checks the owner before and after the provider, and finishes that
   exact lease only after the native call returns. Providers run outside the
   broker critical section. Revocation never frees executing work early.
4. A stale owner, observed audio activity, bad clock or time-budget overrun
   cancels this authority. Audio cancellation revokes the same runtime owner and
   does not automatically resume when audio becomes idle. None of the four audio
   status reads starts, stops or reconfigures audio/AEC.
5. The caller serializes the private authority and consumes the returned snapshot
   only under its normal owner/state generation checks. For a program, its
   retained `PROGRAM` lease can be checked before local commit/delivery. A
   successful service read does not create a new view lifecycle or a durable
   workflow.

Native callback reentry on the same authority returns `BUSY` without invoking a
provider or consuming another read. The in-flight flag is not a cross-thread
lock: external lifecycle serialization is still mandatory. Each invocation holds
an immutable copy of its backend and original owner, so a callback cannot redirect
completion to a replacement callback or owner. Local cancellation and unexpected
authority mutation are checked across callback boundaries. A failed/stale finish
revokes that original owner, not just the local authority object; another authority
cannot revive it. A malformed returned lease is never finished as another owner's
work; its unknown physical occupancy is retained for native diagnosis/cleanup.

The four public argument objects (authority, backend, snapshot and diagnostic)
must not overlap. Complete or partial overlap is rejected before any write,
including diagnostic initialization. Diagnostics are staged locally and published
only on return. Native backend context storage is opaque: callbacks remain trusted
code and must not mutate caller buffers or authority state through context aliases.
This boundary is not a sandbox for arbitrary native C callbacks.

The time guard is 2,000 microseconds per read, measured across the native read.
This is an overrun detector after return, **not hard preemption of arbitrary C**.
Only these short, nonblocking native providers qualify. A future blocking driver
must use its own bounded asynchronous physical-job lifecycle; it cannot be
inserted behind this synchronous snapshot interface.

## Outcomes and diagnostics

Diagnostics contain only result enums, resource admission/finish outcomes,
elapsed microseconds and whether a provider started, returned or published.
They do not contain ADC values, input events, program source, user text or keys.

Explicit results cover invalid calls, unknown service, denied grant, exhausted
call budget, unavailable/busy broker, cancelled owner, audio protection, timeout,
clock failure, unready input and provider failure. On every non-success result,
the output snapshot remains byte-for-byte unchanged. A read may have completed
without publishing if its owner was revoked; that is represented as cancellation,
not success. These providers have no external write/submission outcome, and there
is no automatic resubmission for any status.

## Regression evidence and remaining gates

Run `p4_trusted_services_test.py` and `p4_resources_test.py` with pytest. Both native
harnesses compile the production implementation with warnings-as-errors,
AddressSanitizer and UndefinedBehaviorSanitizer. The boundary harness injects
provider/admission faults to verify exact grants, unknown IDs, budget exhaustion,
audio before/during work, cancellation, unchanged failed outputs, physical-finish
order, callback reentry, all public argument overlap pairs, failed-finish owner
revocation and deadline/clock boundaries. The device harness compiles the concrete
P4 adapter and real resource broker with host SDK stubs. With production capacity
zero it proves providers are not called. A separate temporary, host-test-only
copy enables one peripheral slot to exercise the same adapter's field mapping
and monotonic values; shipping quotas remain zero. Both variants verify that
reinitializing a fresh authority cannot revive a cancelled broker owner.

Passing host tests does not constitute installed-package admission, device
performance evidence, peripheral hardware verification or voice/AEC acoustic
validation. Those remain separate parent-runtime gates. No audio capture,
playback, device write or firmware installation is performed by these tests.
