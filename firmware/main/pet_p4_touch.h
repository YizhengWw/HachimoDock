#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_touch_init(void);
bool pet_p4_touch_ready(void);
void pet_p4_touch_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
unsigned int pet_p4_touch_dropped_events(void);

#ifdef __cplusplus
}
#endif
