#pragma once

/* Diagnostics query/action API and the shared delayed-reboot guard. */

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_diagnostics_init(void);
bool pet_p4_diagnostics_handle_topic(
  const char *topic,
  const cJSON *payload,
  const pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
void pet_p4_diagnostics_process(
  unsigned long long now_ms,
  const pet_p4_runtime_state_t *state
);
bool pet_p4_diagnostics_reboot_pending(void);

#ifdef __cplusplus
}
#endif
