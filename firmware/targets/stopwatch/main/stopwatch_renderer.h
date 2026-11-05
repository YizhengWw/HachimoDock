#pragma once
#include "esp_err.h"
#include "pet_device_sessions.h"
esp_err_t stopwatch_renderer_init(void);
esp_err_t stopwatch_renderer_draw(const pet_device_sessions_t *model, uint64_t now_ms, int battery_mv);
esp_err_t stopwatch_renderer_diagnostic(unsigned pattern, int touch_x, int touch_y);
