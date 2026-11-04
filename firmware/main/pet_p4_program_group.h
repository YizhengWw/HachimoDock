/* Default-off native lifecycle group. No task, installer, wire API or grant.
 * The trusted parent first verifies/publishes a program, creates its deliberate
 * schedule and optional bus registration, then transfers their invocation paths
 * to ONE fixed-address group. All objects stay private and externally serialized
 * under the SAME lifecycle/model/bus lock. Never mix direct host/timer/delivery
 * calls with these wrappers after attachment or move/reinitialize live storage.
 * Physical broker locking remains the backend's separate short critical section.
 * No IO, heap, automatic restart, queue replay, or package-specific behavior.
 */
#pragma once
#include "pet_p4_program_host.h"
#include "pet_p4_program_timers.h"
#include "pet_p4_event_bus.h"

typedef struct pet_program_group {
  const void *instance;
  pet_program_host_t *host;
  pet_program_timers_t *timers;
  pet_event_bus_t *bus;
  pet_event_registration_t subscription;
  pet_resource_owner_t owner;
  uint64_t generation,sequence;
  uint32_t marker;
  bool inside,stopped,retired,stop_requested;
  const struct pet_program_scheduler *scheduler_owner;
} pet_program_group_t;

/* Fresh {0} output only. Optional children must belong to the EXACT admitted
 * host owner/generation, be distinct objects, and have no call in progress.
 * Attachment itself is not authority: every live operation rechecks the host.
 * Registry/topic policy and verified package/UI identity stay with the parent.
 * No child is initialized, replaced or reset here. Failure preserves output.
 * A live host accepts only ONE group. Failed restarts preserve its binding;
 * only successful activation of a new native owner clears it for attachment.
 */
bool pet_program_group_attach(pet_program_group_t *out,pet_program_host_t *host,
  pet_program_timers_t *timers,pet_event_bus_t *bus,pet_event_registration_t subscription);
/* Bounded live-authority observation only: no event, pump or report consumption.
 * An attached scheduler's latched stop also denies all future live wrappers.
 * Exact generation and the common lifecycle/model lock remain required. */
pet_program_runtime_result_t pet_program_group_check_current(pet_program_group_t *group,uint64_t generation);
/* Shared monotonic sequencer across all THREE local enqueue paths. Only a
 * proven OK enqueue consumes a sequence; full/busy retains the child event.
 * Any unexpected duplicate/unknown enqueue result stops the whole group; never
 * infer acceptance or retry an accepted event. sequence out changes only on OK.
 * Arguments/output may not overlap any grouped object or one another.
 */
pet_program_runtime_result_t pet_program_group_enqueue(pet_program_group_t *group,
  uint64_t generation,const pet_program_event_t *event,uint64_t *sequence);
pet_program_timers_result_t pet_program_group_tick(pet_program_group_t *group,
  uint64_t generation,uint64_t now_ms,pet_program_timer_diagnostic_t *diagnostic);
pet_event_bus_result_t pet_program_group_deliver(pet_program_group_t *group,
  uint64_t generation,pet_event_diagnostic_t *diagnostic);
/* Exactly one host pump. Parent must yield and provide fair scheduling; this
 * library is not a scheduler or evidence of P4 latency/voice/AEC acceptance. */
pet_program_runtime_result_t pet_program_group_pump(pet_program_group_t *group,uint64_t generation);
/* Stops timers and closes ONLY this receiver's exact registration before
 * cancelling host work. Physical release is still performed by the real host,
 * after runtime work returns. New-generation children/host are never cancelled
 * by an old group. Repeated cancel is safe and cannot resume anything.
 * retired=true means these local retirement calls completed, not durable saving.
 * On observed authority loss the host may already have stopped/released itself
 * before child retirement; the common parent lock prevents any intervening
 * child delivery. This does not add an atomic audio-transition lock.
 */
pet_program_runtime_result_t pet_program_group_cancel(pet_program_group_t *group,uint64_t generation);
/* Historical reads remain available after stop, without creating authority. */
pet_program_runtime_result_t pet_program_group_snapshot(pet_program_group_t *group,
  uint64_t generation,pet_program_snapshot_t *out);
pet_program_runtime_result_t pet_program_group_take_report(pet_program_group_t *group,
  uint64_t generation,pet_program_report_t *out);
/* Parent still holds its UI publication lock and exact UI lease/identity. */
pet_program_runtime_result_t pet_program_group_project(pet_program_group_t *group,
  uint64_t generation,const int32_t base[PET_PROGRAM_UI_VARIABLES],pet_program_ui_snapshot_t *out);
