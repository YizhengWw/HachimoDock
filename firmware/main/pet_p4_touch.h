#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_touch_init(void);
bool pet_p4_touch_ready(void);
const char *pet_p4_touch_controller(void);
typedef struct {
  const char *stage;
  int error, address, primary_error, backup_error;
  int probe_15, probe_38, probe_10, probe_18;
  int chip_id, vendor_id;
  unsigned int samples, contacts, poll_errors, consecutive_errors;
  int panel_x, panel_y, ui_x, ui_y;
  unsigned int pointer_age_ms, stale_events;
} pet_p4_touch_health_t;
void pet_p4_touch_health(pet_p4_touch_health_t *out);
void pet_p4_touch_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
unsigned int pet_p4_touch_dropped_events(void);

#ifdef __cplusplus
}
#endif
