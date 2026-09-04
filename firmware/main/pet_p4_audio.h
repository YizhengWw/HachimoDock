#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pet_p4_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_AUDIO_SAMPLE_RATE 16000
#define PET_P4_AUDIO_CHANNELS 1
#define PET_P4_AUDIO_BITS_PER_SAMPLE 16
#define PET_P4_AUDIO_FRAME_MS 20

esp_err_t pet_p4_audio_init(
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_audio_ready(void);
bool pet_p4_audio_enabled(void);
bool pet_p4_audio_active(void);
const char *pet_p4_audio_codec_name(void);
bool pet_p4_audio_playback_ready(void);
bool pet_p4_audio_playback_active(void);
esp_err_t pet_p4_audio_set_enabled(bool enabled);
esp_err_t pet_p4_audio_capture_start(bool session_queue_empty);
esp_err_t pet_p4_audio_capture_stop(void);
void pet_p4_audio_process(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
void pet_p4_audio_send_status(void);

#ifdef __cplusplus
}
#endif
