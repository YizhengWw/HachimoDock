#pragma once

/* ESP32-P4 A/B update, boot validation, and transfer/restart activity guards. */

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifndef PET_P4_OTA_METADATA_CACHED
#define PET_P4_OTA_METADATA_CACHED 0
#endif
#if PET_P4_OTA_METADATA_CACHED
#include "esp_ota_ops.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_ota_init(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_ota_handle_topic(
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
);
void pet_p4_ota_runtime_ready(unsigned long long now_ms);
void pet_p4_ota_runtime_failed(unsigned long long now_ms);
void pet_p4_ota_process(unsigned long long now_ms);
void pet_p4_ota_send_status(pet_p4_send_line_fn send_line, void *ctx);
bool pet_p4_ota_active(void);
bool pet_p4_ota_transfer_active(void);
#if PET_P4_OTA_METADATA_CACHED
/* Read-only observed native boot/validation result; no flash/MMU access or
 * long OTA mutex wait. An error is unavailable, never invented validity. */
esp_err_t pet_p4_ota_observed_image_state(esp_ota_img_states_t *out);
#endif

#ifdef __cplusplus
}
#endif
