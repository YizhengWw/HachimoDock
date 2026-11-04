#pragma once
#include "pet_p4_miniapp.h"
#include "pet_p4_protocol.h"
#include "pet_p4_host_observation.h"
#include "pet_p4_host_render.h"
#include "pet_p4_variable_owners.h"
#include "pet_p4_service_bridge.h"
/* One active, ephemeral host dashboard. No private data is written to Flash.
 * Empty id in tick revokes the previous host view (including non-host navigation).
 * Reply ownership is reset on reentry/replacement; hiding never retransmits an action.
 * A trusted device HOST_VIEW lease gates this lifetime; no package-supplied token. */
void pet_app_host_transport(pet_p4_send_line_fn send,void *ctx,const char *board);
void pet_app_host_tick(const char *id,uint32_t revision,uint8_t page,bool visible,uint64_t now);
bool pet_app_host_action(const char *id,uint32_t revision,uint8_t page,const char *action,uint64_t now);
bool pet_app_host_confirmed_action(const char *id,uint32_t revision,uint8_t page,const char *action,const char *context,uint64_t now);
/* Atomically captures the native view identity. False clears output. A guarded
 * action is consumed, never falls through, if the observation became stale.
 * It is NOT a confirmation: ordinary guarded clicks keep confirmed absent. */
bool pet_app_host_observe(const char *id,pet_host_observation_t *out);
bool pet_app_host_observation_current(const char *id,const pet_host_observation_t *observed);
#if PET_HOST_COLLECTION_ENABLED
/* Source/visible profile, freshness and native gesture identity are captured
 * under one host lock. Caller still uses guarded action at the enqueue point. */
bool pet_app_host_collection_observe(const char *id,const pet_host_collection_config_t *config,unsigned slot,uint64_t now,pet_host_observation_t *out);
#endif
bool pet_app_host_observed_action(const char *id,uint32_t revision,uint8_t page,const char *action,const pet_host_observation_t *observed,uint64_t now);
int pet_app_host_mode(const char *id,uint64_t now);
bool pet_app_host_enabled(const char *id,const char *action,uint64_t now);
void pet_app_host_context(const char *id,char context[65]);
bool pet_app_host_apply(const cJSON *payload,uint64_t now);
void pet_app_host_view(pet_p4_miniapp_view_t *out,uint64_t now);
bool pet_app_host_variable(const char *id,const char *name,bool integer,char out[64],int32_t *number,uint64_t now);
/* Typed-variable changes, including frame-between away/back, invalidate gestures.
 * Status sequence/unchanged variables do not change this monotonic in-RAM epoch. */
uint64_t pet_app_host_variable_epoch(const char *id);
/* All-or-nothing integer capture from one reply/lifetime under one host lock.
 * Native owned names/output must be distinct. Failure does not change output.
 * Used by consumers whose geometry/logic cannot mix two variable snapshots. */
bool pet_app_host_integer_snapshot(const char *id,const char names[][32],unsigned count,int32_t out[8],uint64_t now);
#if PET_VARIABLE_OWNERS_ENABLED
/* Mixed consumers require an actually applied data reply, not pending UI. */
bool pet_app_host_owned_integer_snapshot(const char *id,const char names[][32],unsigned count,int32_t out[8],uint64_t now);
bool pet_app_host_owned_observation_current(const char *id,const pet_host_observation_t *observed,uint64_t now);
/* Pointer identity and typed words are checked/copied under the SAME mux. */
bool pet_app_host_integer_snapshot_observed(const char *id,const char names[][32],unsigned count,
    int32_t out[8],const pet_host_observation_t *observed,uint64_t now);
#endif
/* Caller owns storage. Captures variables/UI and decorates the matching view
 * under one host lock. Never use this read-only data to authorize an action. */
void pet_app_host_render_capture(const char *id,pet_host_render_t *snapshot,pet_p4_miniapp_view_t *view,uint64_t now);
#if PET_VARIABLE_OWNERS_ENABLED
/* One pointer-bound publication for geometry AND exact native observation. */
void pet_app_host_render_capture_observed(const char *id,pet_host_render_t *snapshot,
    pet_host_observation_t *observed,uint64_t now);
#endif

/* D read-only outputs, drained by the normal host-process lane. The borrowed
 * payload is never retained and a failed send is not replayed. */
bool pet_app_host_emit(const char *topic,const cJSON *payload);

#if PET_SERVICE_BRIDGE_ENABLED
void pet_app_host_tick_bound(const char*id,uint32_t revision,uint8_t page,bool visible,uint64_t now,const char*binding);
bool pet_app_host_observed_value_action(const char*id,uint32_t revision,uint8_t page,const char*action,const char*parameter,int32_t value,const pet_host_observation_t*observed,uint64_t now);
bool pet_app_host_instance_matches(const char*id,const char*token);
bool pet_app_host_image_ready(const cJSON*p,uint64_t now);
bool pet_app_host_image_apply(const cJSON*p,uint64_t now);
bool pet_app_host_image_borrow(const char*id,const char*alias,uint64_t now,pet_bridge_image_lease_t*out);
void pet_app_host_image_release(pet_bridge_image_lease_t*lease);
#endif
