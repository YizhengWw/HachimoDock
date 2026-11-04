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
/* Native boot / existing save worker only, outside every model/store lock.
 * Private cached comparison is default-off. Never called by a query. */
void pet_p4_diagnostics_storage_refresh(void);
bool pet_p4_diagnostics_handle_topic(
  const char *topic,
  const cJSON *payload,
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
/* Optional single immutable query reply; never captures an action response or
 * stores a callback/context. The old API retains synchronous compatibility. */
bool pet_p4_diagnostics_handle_topic_with_reply(
  const char *topic,
  const cJSON *payload,
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  pet_p4_query_reply_t *reply
);
void pet_p4_diagnostics_process(
  unsigned long long now_ms,
  const pet_p4_runtime_state_t *state
);
bool pet_p4_diagnostics_reboot_pending(void);

#ifdef __cplusplus
}
#endif
