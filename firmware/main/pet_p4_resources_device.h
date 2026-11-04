/* The only device broker adapter. Calls hold one short portMUX section; no
 * callback, allocation, filesystem, renderer/transport lock or driver call. */
#pragma once
#include "pet_p4_resources.h"
#include "pet_p4_program_core.h"

typedef struct {
  pet_resource_owner_t owner;
  pet_resource_lease_t snapshot, bulk;
} pet_resource_debug_session_t;
typedef struct {
  pet_resource_owner_t owner;
  pet_resource_lease_t view;
} pet_resource_view_session_t;

/* Sessions must be zero-initialized. Debug begin admits the entire pair or none.
 * Take it before allocating/copying the snapshot. No owner comes from the wire. */
pet_resource_result_t pet_resources_device_debug_begin(uint32_t snapshot_bytes,
  bool audio_busy, pet_resource_debug_session_t *out);
/* An observed busy audio service revokes this debug owner, never frees its
 * buffers. Recheck at bounded copy/send boundaries; false must abort the job. */
bool pet_resources_device_debug_current(const pet_resource_debug_session_t *session, bool audio_busy);
/* Explicit cancellation, e.g. transport teardown. Occupancy is unchanged. */
bool pet_resources_device_debug_revoke(const pet_resource_debug_session_t *session);
/* After free(snapshot) and all physical writes return, on every exit path.
 * True means the exact completion still had authority, not peer receipt. */
bool pet_resources_device_debug_end(pet_resource_debug_session_t *session);

pet_resource_result_t pet_resources_device_view_begin(pet_resource_view_session_t *out);
bool pet_resources_device_view_current(const pet_resource_view_session_t *session);
void pet_resources_device_view_end(pet_resource_view_session_t *session);

/* Observation only: denies NEW debug admission; does not schedule, stop, own or
 * reconfigure audio and does not release in-flight resource occupancy. */
void pet_resources_device_audio_observed(bool busy);
void pet_resources_device_usage(pet_resource_usage_t *out);

/* Trusted realtime actor reservation, separate from sampled audio observations.
 * Begin BEFORE publishing requested/active or touching a codec. A stop request
 * alone must not finish a ticket still held by a physical worker. No waits. */
pet_resource_result_t pet_resources_device_realtime_begin(pet_resource_realtime_t *out);
bool pet_resources_device_realtime_current(pet_resource_realtime_t ticket);
bool pet_resources_device_realtime_video_ready(pet_resource_realtime_t ticket,bool ready);
void pet_resources_device_realtime_end(pet_resource_realtime_t *ticket);

/* Trusted native consumers only. Each call holds the same short broker mux;
   callers never hold it across allocation, driver calls, waits or callbacks. */
pet_resource_result_t pet_resources_device_owner_open(uint32_t mask,pet_resource_owner_t *out);
pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t owner,pet_resource_kind_t kind,uint32_t units,pet_resource_lease_t *out);
bool pet_resources_device_current(pet_resource_lease_t lease);
void pet_resources_device_revoke(pet_resource_owner_t owner);
pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t lease);
/* Native PROGRAM-only fixed-state publication. The SAME broker mux linearizes
 * this one <=512-byte v1 copy with realtime_begin/revoke, never VM execution.
 * V2 requires pet_program_state_bind_publication on caller-owned storage: a
 * full copy goes only to the inactive slot OUTSIDE the mux; a second exact
 * lease/generation/revision check flips the small selector. Use
 * pet_program_state_read under the same model lock for every payload read.
 * The facade's inline arrays are not the v2 authoritative committed state.
 * Keep selector/slots fixed and serialized until physical retirement.
 * No caller callbacks, IO, waiting or driver operations while holding the mux.
 * Native pointers only; package/wire input cannot provide target addresses. */
bool pet_resources_device_program_publish(pet_resource_lease_t lease,
  pet_program_state_t *target,const pet_program_state_t *candidate);
/* Fixed compiled-vector diagnostic only: same publication implementation but
 * exact NATIVE_PROBE admission, never an alternative package execution grant. */
bool pet_resources_device_probe_program_publish(pet_resource_lease_t lease,
  pet_program_state_t *target,const pet_program_state_t *candidate);
typedef struct {uint32_t critical_cycles;bool cycle_measured;} pet_program_publication_observation_t;
/* Diagnostic only: local CPU cycles INSIDE the broker critical section.
 * Host stubs explicitly report unmeasured; no callback/clock SDK call in lock.
 * Whole-call/scheduler wall time remains a separate observation. */
bool pet_resources_device_probe_program_publish_observed(pet_resource_lease_t lease,
  pet_program_state_t *target,const pet_program_state_t *candidate,pet_program_publication_observation_t *observation);
