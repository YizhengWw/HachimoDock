/* Default-closed native cooperative dispatcher, not an RTOS task or capability.
 * One call visits ONE of at most four admitted native lifecycle groups. It may
 * offer ONE timer OR bus event, then perform ONE host pump; never drains queues
 * or consumes reports. The parent must yield, choose the cadence/quantum after
 * P4 measurement, and hold the SAME lifecycle/model/bus lock for ALL groups.
 * No heap, IO, clock reads, automatic restart, grants or package-specific code.
 * All objects stay at fixed private addresses for the scheduler's lifetime.
 */
#pragma once
#include "pet_p4_program_group.h"
#define PET_PROGRAM_SCHEDULER_SLOTS 4U
typedef struct {
  const struct pet_program_scheduler *scheduler;
  uint64_t serial;
  uint8_t slot;
} pet_program_schedule_t;
typedef struct {
  pet_program_group_t *group;
  uint64_t generation,serial;
  bool bus_next,retiring;
} pet_program_schedule_slot_t;
typedef struct pet_program_scheduler {
  const void *instance;
  pet_program_schedule_slot_t slots[PET_PROGRAM_SCHEDULER_SLOTS];
  uint64_t next_serial,last_now;
  uint32_t marker;
  uint8_t cursor;
  bool inside,stopped,clock_seen;
} pet_program_scheduler_t;
typedef enum {
  PET_SCHED_OK,PET_SCHED_INVALID,PET_SCHED_BUSY,PET_SCHED_FULL,PET_SCHED_STALE,
  PET_SCHED_EXHAUSTED,PET_SCHED_IDLE,PET_SCHED_TURN,PET_SCHED_STOPPING,
  PET_SCHED_STOPPED,PET_SCHED_CLOCK_ERROR
} pet_program_schedule_result_t;
typedef enum {PET_SCHED_NO_INGRESS,PET_SCHED_TIMER,PET_SCHED_BUS} pet_program_ingress_t;
typedef struct {
  pet_program_schedule_t schedule;
  uint64_t generation;
  pet_program_ingress_t ingress;
  pet_program_timers_result_t timer_result;
  pet_event_bus_result_t bus_result;
  pet_program_runtime_result_t pump_result;
  bool selected,pump_attempted,removed,stopped;
} pet_program_schedule_diagnostic_t;
/* Fresh {0} private storage only, never reset/reinitialize a live/stopped object.
 * There is no permission or background task created by initialization. */
bool pet_program_scheduler_init(pet_program_scheduler_t *scheduler);
/* Native caller output must not overwrite scheduler or known registered
 * groups/hosts/timers/buses. This only observes identity/ranges, never work. */
bool pet_program_scheduler_disjoint(const pet_program_scheduler_t *scheduler,const void *buffer,size_t bytes);
/* Explicit new lifecycle after ALL exact registrations physically retired.
 * Preserve serial exhaustion/uniqueness; old tickets can never match new ones.
 * No restart, re-admission, timer catch-up or work is performed here. */
pet_program_schedule_result_t pet_program_scheduler_rearm(pet_program_scheduler_t *scheduler);
/* An already-admitted group may attach ONCE to one scheduler for its lifetime.
 * Inputs/output cannot overlap scheduler or any grouped object. Out starts zero.
 * Capacity/argument failure preserves old registrations and caller output.
 * Live observation may stop a group if its real authority has been revoked.
 * Direct tick/delivery/pump must not bypass this scheduling path afterward;
 * input enqueue and explicit committed snapshot/report reads keep group APIs.
 */
pet_program_schedule_result_t pet_program_scheduler_add(pet_program_scheduler_t *scheduler,
  pet_program_group_t *group,uint64_t generation,pet_program_schedule_t *out);
/* Latch stop for this EXACT registration before attempting any cleanup, not a
 * later owner reusing the host address. BUSY retains its occupied slot and stop
 * intent: later turns only retry retirement, never ingress or execution.
 * OK/STALE plus proven completed retirement removes only that slot.
 * It does not reset the group, discard reports, save data or free native memory.
 */
pet_program_schedule_result_t pet_program_scheduler_remove(pet_program_scheduler_t *scheduler,
  pet_program_schedule_t schedule);
/* now_ms is explicit monotonic time, not wall-clock/persisted work. Cursor moves
 * on every selected turn, including BUSY, pending-report and revoked entries.
 * Both timer/bus present: alternate which gets this turn's single offer, even
 * under backpressure. Each group retains its own monotonic event sequence.
 * No report is silently drained; caller must consume it through the group API.
 * Backwards time latches scheduler stop and performs at most four cancellations,
 * never a program pump. If a child is BUSY its slot remains for explicit later
 * stop/step calls. Latched stop denies external live group wrappers too.
 * Errors preserve diagnostics except a valid idle/turn/stop/clock observation.
 */
pet_program_schedule_result_t pet_program_scheduler_step(pet_program_scheduler_t *scheduler,
  uint64_t now_ms,pet_program_schedule_diagnostic_t *out);
/* Latched manual stop, no auto-resume when clock/audio recovers. At most four
 * native cancellations per call, no pumping/IO. Stale tokens never touch new
 * owners. This is not atomic hardware/audio exclusion or P4 deadline evidence.
 */
pet_program_schedule_result_t pet_program_scheduler_stop(pet_program_scheduler_t *scheduler);
