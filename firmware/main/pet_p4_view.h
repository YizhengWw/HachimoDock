#pragma once

#include <stdbool.h>

#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PET_P4_VIEW_STATUS_IDLE = 0,
  PET_P4_VIEW_STATUS_WORKING,
  PET_P4_VIEW_STATUS_WAITING,
  PET_P4_VIEW_STATUS_DONE,
  PET_P4_VIEW_STATUS_ERROR,
} pet_p4_view_status_t;

typedef struct {
  const char *page;
  const char *agent;
  const char *title;
  const char *body;
  const char *stats_json;
  pet_p4_view_status_t status;
  bool show_bubble;
  bool compact_bubble;
} pet_p4_view_model_t;

void pet_p4_build_view_model(const pet_p4_runtime_state_t *state, pet_p4_view_model_t *out);

#ifdef __cplusplus
}
#endif
