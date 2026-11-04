/* Read-only diagnostic telemetry, NEVER free-space/write authority or a save.
 * Caller serializes the fixed DRAM snapshot. No IO, heap, locks or callbacks. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_DIAGNOSTIC_STORAGE_CACHED
#define PET_P4_DIAGNOSTIC_STORAGE_CACHED 0
#endif
typedef struct {
  uint64_t sampled_ms;
  uint32_t total,used;
  int32_t last_error;
  bool available,refresh_failed;
} pet_storage_snapshot_t;
typedef struct {
  pet_storage_snapshot_t value;
  uint64_t age_ms;
  bool stale,clock_invalid;
} pet_storage_snapshot_view_t;
static inline bool pet_storage_snapshot_publish(pet_storage_snapshot_t *state,
    uint64_t total,uint64_t used,uint64_t sampled_ms,int32_t error){
  if(!state)return false;
  if(error||!total||total>UINT32_MAX||used>total
      ||(state->available&&sampled_ms<state->sampled_ms)){
    state->refresh_failed=true;state->last_error=error?error:-1;return false;
  }
  *state=(pet_storage_snapshot_t){.sampled_ms=sampled_ms,.total=(uint32_t)total,.used=(uint32_t)used,.available=true};
  return true;
}
static inline pet_storage_snapshot_view_t pet_storage_snapshot_observe(pet_storage_snapshot_t state,uint64_t now_ms){
  pet_storage_snapshot_view_t view={.value=state,.stale=true};
  if(!state.available)return view;
  if(now_ms<state.sampled_ms){view.clock_invalid=true;return view;}
  view.age_ms=now_ms-state.sampled_ms;
  view.stale=state.refresh_failed||view.age_ms>3000;
  return view;
}
