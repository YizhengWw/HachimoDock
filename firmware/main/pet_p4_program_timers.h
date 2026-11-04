/* Cooperative, fixed-storage timer events for the bounded program layer.
 * This is NOT an RTOS task, package permission, persistent alarm or wall clock.
 * The trusted owner supplies monotonic milliseconds and serializes every call
 * with lifecycle changes. A tick offers AT MOST ONE event to an approved native
 * enqueue adapter; it never pumps a program or loops until a queue is drained.
 * Use pet_program_host_enqueue in a device adapter, never bypass its admission.
 * No ambient clock, allocation, IO, ISR, audio or automatic recovery/replay.
 */
#pragma once
#include "pet_p4_program_runtime.h"

#define PET_PROGRAM_TIMERS 8U
#define PET_PROGRAM_TIMER_MIN_INTERVAL_MS 100U
typedef struct {
  uint16_t id;
  uint32_t delay_ms,interval_ms; /* interval=0 is one-shot */
  pet_program_event_t event;
} pet_program_timer_spec_t;
typedef enum {
  PET_TIMERS_OK, PET_TIMERS_INVALID, PET_TIMERS_BUSY, PET_TIMERS_STALE,
  PET_TIMERS_IDLE, PET_TIMERS_ENQUEUED, PET_TIMERS_BACKPRESSURE,
  PET_TIMERS_STOPPED, PET_TIMERS_CLOCK_ERROR, PET_TIMERS_SEQUENCE_ERROR
} pet_program_timers_result_t;
typedef struct {
  const void *instance;
  pet_program_timer_spec_t specs[PET_PROGRAM_TIMERS];
  uint64_t deadlines[PET_PROGRAM_TIMERS];
  uint64_t generation,last_now,last_sequence;
  uint32_t marker,missed,accepted;
  uint8_t count,cursor,active_mask;
  bool inside,cancelled;
} pet_program_timers_t;
typedef struct {
  uint16_t timer_id;
  uint32_t coalesced;
  pet_program_runtime_result_t enqueue_result;
  bool offered,accepted,deadline_exhausted;
} pet_program_timer_diagnostic_t;
typedef pet_program_runtime_result_t (*pet_program_timer_enqueue_t)(void *context,
  uint64_t generation,uint64_t sequence,const pet_program_event_t *event);

/* Fresh {0} private storage only, copied specifications, unique nonzero IDs,
 * 1..8 timers. Fresh schedule means a deliberate NEW schedule, not recovered
 * persisted work. Failed validation does not mutate output. No re-init/reset of
 * a started or cancelled object. Same generation as its admitted program owner.
 * Deadlines must be representable. Unused event words are explicitly cleared.
 */
pet_program_timers_result_t pet_program_timers_start(pet_program_timers_t *timers,
  uint64_t generation,uint64_t now_ms,const pet_program_timer_spec_t *specs,unsigned count);
/* Approved enqueue callback only, at most once. It cannot mutate timers,
 * diagnostic or callback inputs; synchronous reentry is rejected. No lock is
 * provided here. Distinct output/context storage is the adapter's obligation.
 * Accepted events advance phase-aligned to the FIRST FUTURE deadline; missed
 * intervals coalesce into one event, never a catch-up burst. Rejected busy/full
 * queues retain the due event. Rejected duplicate/invalid is a latched protocol
 * failure, not proof that this event was accepted. No automatic retry of an
 * event already accepted, regardless of its later program result.
 * Sequence comes from the owner's shared event sequencer and must increase
 * relative to this scheduler's accepted events. No wrap/reset on overflow.
 * A regressing clock latches cancellation before invoking any callback.
 */
pet_program_timers_result_t pet_program_timers_tick(pet_program_timers_t *timers,
  uint64_t generation,uint64_t now_ms,uint64_t sequence,
  pet_program_timer_enqueue_t enqueue,void *context,pet_program_timer_diagnostic_t *diagnostic);
/* Stops only scheduling. Parent must ALSO cancel the admitted program/owner
 * to discard queued/private work; never release a physical lease from here. */
pet_program_timers_result_t pet_program_timers_cancel(pet_program_timers_t *timers,uint64_t generation);
