/* The real component parent owns this fixed-address session and serializes it
 * with component/model changes. One pump per UI turn, never a synchronous drain.
 * No IO, credential, effects or ambient task; the real backend must admit work.
 * Ingress and one-turn pumping use the common group/scheduler. The private group
 * may be reincarnated only for a strictly newer admitted native generation after
 * physical retirement and exact deregistration; scheduler serials never reset.
 * Default callers keep the private dispatcher. Bound shared-parent callers
 * step that parent once, then collect their own reports; session_step refuses
 * to dispatch a bound parent, which could otherwise select a different child.
 * Explicit retirement retains the last outcome; later authority recovery cannot
 * resume that owner. Public firmware keeps execution unavailable by default. */
#pragma once
#include "pet_p4_component_program.h"
#include "pet_p4_program_scheduler.h"
#if PET_COMPONENT_PROGRAM_ENABLED
typedef struct {
  pet_program_host_t host;
  pet_program_timers_t timers;
  pet_program_group_t group;
  pet_program_scheduler_t scheduler;
  pet_program_scheduler_t *dispatch; /* Optional immutable trusted parent. */
  pet_program_schedule_t schedule;
  pet_program_report_t last_report;
  uint64_t generation,revision,sequence;
  pet_program_runtime_result_t phase;
  bool live,report_valid,clocked,retiring,starting,stop_requested,scheduled;
} pet_component_program_session_t;
bool pet_component_program_session_init(pet_component_program_session_t *session,const pet_program_host_backend_t *backend);
/* Fresh native session only, before any admission. Parent storage is fixed and
 * must outlive all exact registrations and physical retirement. The parent
 * alone steps/rearms this scheduler; no package pointer, task or grant is added. */
bool pet_component_program_session_bind_scheduler(pet_component_program_session_t *session,pet_program_scheduler_t *parent);
/* For an externally stepped parent only: consume this session's own terminal
 * report/projected output, never pump any child. Repeat polling cannot replay
 * a commit. The same parent serialization protects step, collect and stop. */
bool pet_component_program_session_collect(pet_component_program_session_t *session,uint64_t revision,
  const int32_t base[8],int32_t outputs[8]);
pet_program_runtime_result_t pet_component_program_session_start(pet_component_program_session_t *session,
  const pet_component_program_config_t *config,const pet_program_snapshot_t *seed,uint64_t revision,uint64_t now_ms);
pet_program_runtime_result_t pet_component_program_session_enqueue(pet_component_program_session_t *session,
  uint64_t revision,const pet_program_event_t *event);
/* Read only under the same parent mutex; final enqueue still checks authority. */
bool pet_component_program_session_can_enqueue(const pet_component_program_session_t *session,uint64_t revision);
/* On COMPLETE, explicitly consumes the VM terminal report into last_report;
 * committed outputs are staged from the latest base and copied atomically.
 * A false return never changes outputs. Inspect phase/last_report for failure.
 * No diagnostic string contains a program, variable value or private record. */
bool pet_component_program_session_step(pet_component_program_session_t *session,uint64_t revision,
  uint64_t now_ms,const int32_t base[8],int32_t outputs[8]);
pet_program_runtime_result_t pet_component_program_session_stop(pet_component_program_session_t *session);
bool pet_component_program_session_snapshot(pet_component_program_session_t *session,pet_program_snapshot_t *out);
/* Historical snapshot observation; same parent serialization/alias rules.
 * Never skips retirement or consumes the final report after a committed event. */
pet_program_runtime_result_t pet_component_program_session_snapshot_if_changed(
  pet_component_program_session_t *session,const pet_program_snapshot_cursor_t *known,
  pet_program_snapshot_t *out,pet_program_snapshot_cursor_t *cursor);
#if PET_COMPONENT_PROGRAM_RAM_RESUME
/* Explicit activation preparation only: a physically retired exact revision's
 * latest committed model, validated against its unchanged program/projection.
 * Never starts/rearms a task, timer, owner or pending event; failure leaves out
 * unchanged. The serialized caller must still admit a NEW runtime lifetime. */
bool pet_component_program_session_resume_snapshot(pet_component_program_session_t *session,
  const pet_component_program_config_t *config,uint64_t revision,pet_program_snapshot_t *out);
#endif
#endif
