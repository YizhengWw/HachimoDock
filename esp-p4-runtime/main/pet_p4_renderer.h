#pragma once

#include "esp_err.h"
#include "pet_p4_protocol.h"
#include "pet_p4_view.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pet_p4_renderer_init(void);
esp_err_t pet_p4_renderer_render(
  const pet_p4_runtime_state_t *state,
  const pet_p4_view_model_t *view,
  unsigned long long now_ms
);
void pet_p4_renderer_reset_playback(void);
const uint16_t *pet_p4_renderer_logical_framebuffer(int *width, int *height);

#ifdef __cplusplus
}
#endif
