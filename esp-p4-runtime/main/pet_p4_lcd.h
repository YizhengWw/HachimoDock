#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t pet_p4_lcd_init(void);
esp_err_t pet_p4_lcd_backlight_status(void);
esp_err_t pet_p4_lcd_keep_awake(void);
esp_err_t pet_p4_lcd_show_color_bar(void);
esp_err_t pet_p4_lcd_show_software_test_pattern(uint32_t phase);
esp_err_t pet_p4_lcd_draw_rgb565(int x, int y, int width, int height, const uint16_t *pixels);
int pet_p4_lcd_scan_i2c(char *buffer, size_t buffer_size);
