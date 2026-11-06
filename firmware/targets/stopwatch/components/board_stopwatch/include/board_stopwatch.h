#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum { STOPWATCH_WIDTH = 466, STOPWATCH_HEIGHT = 466 };
esp_err_t stopwatch_board_init(void);
esp_err_t stopwatch_board_present(const uint16_t *rgb565);
esp_err_t stopwatch_board_brightness(uint8_t value);
bool stopwatch_board_button(unsigned index);
bool stopwatch_board_touch(int *x, int *y);
bool stopwatch_board_touch_ready(void);
esp_err_t stopwatch_board_vibrate(bool enabled);
int stopwatch_board_battery_mv(void);
typedef struct {
  uint32_t display_frames;
  int display_error, motor_error;
  uint8_t ioe_address, touch_chip, touch_firmware, power_config;
  uint16_t ioe_mode, ioe_output, ioe_input, ioe_drive;
  bool touch_ready, motor_on;
} stopwatch_board_health_t;
esp_err_t stopwatch_board_health(stopwatch_board_health_t *health);
