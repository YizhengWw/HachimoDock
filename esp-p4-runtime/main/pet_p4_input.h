/*
 * [Input] Physical-input runtime lifecycle and desktop input-config commands.
 * [Output] Version-7 P4 input configuration API with 16 persisted bindings
 *          plus a lock-free joystick ADC diagnostic snapshot.
 * [Pos] Public interface for the ESP32-P4 input runtime.
 * [Sync] If this file changes, update `esp-p4-runtime/.folder.md` and `protocol.md`.
 */

#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_INPUT_CONFIG_VERSION 7
#define PET_P4_INPUT_MAX_BINDINGS 16
#define PET_P4_INPUT_EVENT_MAX 40
#define PET_P4_INPUT_ACTION_MAX 24
#define PET_P4_INPUT_VALUE_MAX 160

typedef struct {
  bool ready;
  int center_x;
  int center_y;
  int current_x;
  int current_y;
  int minimum_x;
  int maximum_x;
  int minimum_y;
  int maximum_y;
} pet_p4_input_joystick_snapshot_t;

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
void pet_p4_input_get_joystick_snapshot(pet_p4_input_joystick_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
