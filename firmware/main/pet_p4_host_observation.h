/* Private native gesture observation. Never a wire-provided grant or handle.
 * Capture all fields under the host mux and compare again at request admission.
 * Matching labels/context alone cannot revive an earlier view incarnation. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct {
  uint64_t variable_epoch;
  uint32_t incarnation,revision;
  char component_id[48],context[65];
  bool valid;
} pet_host_observation_t;
static inline bool pet_host_observation_equal(const pet_host_observation_t *a,const pet_host_observation_t *b){
  return a&&b&&a->valid&&b->valid&&a->variable_epoch==b->variable_epoch
    &&a->incarnation==b->incarnation&&a->revision==b->revision
    &&!strcmp(a->component_id,b->component_id)&&!strcmp(a->context,b->context);
}
