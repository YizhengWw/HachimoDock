#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_INPUT_CONFIG_VERSION 4
#define PET_P4_INPUT_MAX_BINDINGS 16
#define PET_P4_INPUT_EVENT_MAX 40
#define PET_P4_INPUT_ACTION_MAX 24
#define PET_P4_INPUT_VALUE_MAX 160

esp_err_t pet_p4_input_init(void);
esp_err_t pet_p4_input_reset_config(void);
void pet_p4_input_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_input_handle_config(
  const cJSON *payload,
  bool legacy_button_config,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_input_send_config_state(
  const cJSON *request,
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
);
unsigned int pet_p4_input_dropped_events(void);

#ifdef __cplusplus
}
#endif
