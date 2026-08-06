/*
 * [Input] Waveshare panel/BSP operations, boot lifecycle, and RGB565 pixels.
 * [Output] Backlight-gated LCD initialization, first-frame reveal, and draws.
 * [Pos] ESP32-P4 LCD hardware boundary.
 * [Sync] If this file changes, update `esp-p4-runtime/.folder.md`.
 */

#include "pet_p4_lcd.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "pet-p4-lcd";
static const int LCD_BACKLIGHT_FORCE_LEVEL = 1;
static const TickType_t LCD_REFRESH_WAIT_TICKS = pdMS_TO_TICKS(250);

static esp_lcd_panel_handle_t g_panel_handle;
static esp_lcd_panel_io_handle_t g_lcd_io;
static SemaphoreHandle_t g_refresh_done_sem;
static esp_err_t g_backlight_status = ESP_ERR_INVALID_STATE;
static bool g_backlight_revealed;
static bool g_forced_backlight_logged;
static bool g_refresh_callback_logged;
static uint16_t *g_test_row_buffer;
static size_t g_test_row_pixels;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static esp_err_t force_lcd_backlight_gpio(int level) {
  gpio_config_t io_conf = {
    .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure LCD backlight GPIO");
  ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_BACKLIGHT, level), TAG, "force LCD backlight GPIO");
  if (level == LCD_BACKLIGHT_FORCE_LEVEL && !g_forced_backlight_logged) {
    ESP_LOGI(TAG, "LCD backlight revealed gpio=%d", LCD_BACKLIGHT_FORCE_LEVEL);
    g_forced_backlight_logged = true;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_prepare_boot(void) {
  g_backlight_revealed = false;
  g_backlight_status = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  return g_backlight_status;
}

static bool IRAM_ATTR lcd_refresh_done_callback(
  esp_lcd_panel_handle_t panel,
  esp_lcd_dpi_panel_event_data_t *edata,
  void *user_ctx
) {
  (void) panel;
  (void) edata;
  SemaphoreHandle_t sem = (SemaphoreHandle_t) user_ctx;
  BaseType_t task_awake = pdFALSE;
  if (sem) {
    xSemaphoreGiveFromISR(sem, &task_awake);
  }
  return task_awake == pdTRUE;
}

esp_err_t pet_p4_lcd_init(void) {
  if (g_panel_handle) return ESP_OK;

  ESP_LOGI(TAG, "Initialize Waveshare ESP32-P4 LCD BSP");
  ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &g_panel_handle, &g_lcd_io), TAG, "initialize display");
  if (!g_refresh_done_sem) {
    g_refresh_done_sem = xSemaphoreCreateBinary();
    if (!g_refresh_done_sem) return ESP_ERR_NO_MEM;
  }
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {
    .on_refresh_done = lcd_refresh_done_callback,
  };
  ESP_RETURN_ON_ERROR(
    esp_lcd_dpi_panel_register_event_callbacks(g_panel_handle, &callbacks, g_refresh_done_sem),
    TAG,
    "register LCD refresh callback"
  );
  if (!g_refresh_callback_logged) {
    ESP_LOGI(TAG, "LCD refresh_done callback registered");
    g_refresh_callback_logged = true;
  }
  ESP_LOGI(
    TAG,
    "LCD pixel format=%s bits=%d",
    BSP_LCD_COLOR_FORMAT == ESP_LCD_COLOR_FORMAT_RGB565 ? "RGB565" : "RGB888",
    BSP_LCD_BITS_PER_PIXEL
  );
  // Component/storage migration may run for several seconds after an OTA.
  // Keep the panel hidden until the renderer has committed the first complete
  // frame so the controller's blue reset surface is never shown to the user.
  g_backlight_status = bsp_display_backlight_off();
  esp_err_t force_err = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status != ESP_OK || force_err != ESP_OK) {
    ESP_LOGW(TAG, "display backlight hide failed: %s", esp_err_to_name(g_backlight_status));
  }
  if (g_backlight_status == ESP_OK && force_err != ESP_OK) {
    g_backlight_status = force_err;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_reveal(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (g_backlight_revealed) return g_backlight_status;
  g_backlight_status = bsp_display_backlight_on();
  esp_err_t force_err = force_lcd_backlight_gpio(LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status == ESP_OK && force_err != ESP_OK) {
    g_backlight_status = force_err;
  }
  if (g_backlight_status == ESP_OK) {
    g_backlight_revealed = true;
  } else {
    ESP_LOGW(TAG, "display first-frame reveal failed: %s", esp_err_to_name(g_backlight_status));
  }
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_backlight_status(void) {
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_keep_awake(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!g_backlight_revealed) return ESP_ERR_INVALID_STATE;
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_show_color_bar(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  ESP_LOGI(TAG, "Show LCD hardware color bar");
  esp_err_t err = esp_lcd_dpi_panel_set_pattern(g_panel_handle, MIPI_DSI_PATTERN_BAR_VERTICAL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "failed to show color bar: %s", esp_err_to_name(err));
  }
  return err;
}

esp_err_t pet_p4_lcd_show_software_test_pattern(uint32_t phase) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;

  const int row_height = 32;
  const size_t required_pixels = (size_t) BSP_LCD_H_RES * (size_t) row_height;
  if (required_pixels > g_test_row_pixels) {
    free(g_test_row_buffer);
    g_test_row_buffer = (uint16_t *) heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_test_row_buffer) {
      g_test_row_buffer = (uint16_t *) heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (!g_test_row_buffer) {
      g_test_row_pixels = 0;
      return ESP_ERR_NO_MEM;
    }
    g_test_row_pixels = required_pixels;
  }

  const uint16_t colors[] = {
    rgb565(255, 0, 0),
    rgb565(0, 255, 0),
    rgb565(0, 0, 255),
    rgb565(255, 255, 255),
    rgb565(0, 0, 0),
    rgb565(255, 255, 0),
    rgb565(0, 255, 255),
    rgb565(255, 0, 255),
  };
  const int color_count = (int) (sizeof(colors) / sizeof(colors[0]));
  const int moving_x = (int) ((phase * 19U) % (uint32_t) BSP_LCD_H_RES);

  for (int y = 0; y < BSP_LCD_V_RES; y += row_height) {
    int h = (y + row_height > BSP_LCD_V_RES) ? (BSP_LCD_V_RES - y) : row_height;
    for (int yy = 0; yy < h; yy += 1) {
      int screen_y = y + yy;
      for (int x = 0; x < BSP_LCD_H_RES; x += 1) {
        int bar = (x * color_count) / BSP_LCD_H_RES;
        uint16_t color = colors[bar < color_count ? bar : color_count - 1];
        if (x < 8 || x >= BSP_LCD_H_RES - 8 || screen_y < 8 || screen_y >= BSP_LCD_V_RES - 8) {
          color = rgb565(255, 255, 255);
        } else if (x >= moving_x && x < moving_x + 16) {
          color = rgb565(255, 120, 0);
        } else if (((x / 40) + (screen_y / 40) + (int) phase) % 9 == 0) {
          color = rgb565(16, 16, 16);
        }
        g_test_row_buffer[yy * BSP_LCD_H_RES + x] = color;
      }
    }
    esp_err_t err = pet_p4_lcd_draw_rgb565(0, y, BSP_LCD_H_RES, h, g_test_row_buffer);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_draw_rgb565(int x, int y, int width, int height, const uint16_t *pixels) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!pixels || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;

  if (g_refresh_done_sem) {
    while (xSemaphoreTake(g_refresh_done_sem, 0) == pdTRUE) {
    }
  }
  esp_err_t draw_err = esp_lcd_panel_draw_bitmap(g_panel_handle, x, y, x + width, y + height, pixels);
  if (draw_err != ESP_OK) return draw_err;
  if (g_refresh_done_sem && xSemaphoreTake(g_refresh_done_sem, LCD_REFRESH_WAIT_TICKS) != pdTRUE) {
    ESP_LOGW(TAG, "LCD refresh wait timed out");
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

int pet_p4_lcd_scan_i2c(char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) return 0;
  buffer[0] = '\0';

  esp_err_t init_err = bsp_i2c_init();
  if (init_err != ESP_OK) {
    snprintf(buffer, buffer_size, "init:%s", esp_err_to_name(init_err));
    return 0;
  }

  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (!bus) {
    snprintf(buffer, buffer_size, "no-bus");
    return 0;
  }

  int found = 0;
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr += 1) {
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
    int written = snprintf(
      buffer + used,
      buffer_size - used,
      "%s0x%02X",
      found == 0 ? "" : ",",
      addr
    );
    if (written < 0 || (size_t) written >= buffer_size - used) {
      buffer[buffer_size - 1] = '\0';
      break;
    }
    used += (size_t) written;
    found += 1;
  }

  ESP_LOGI(TAG, "I2C scan found %d device(s): %s", found, found > 0 ? buffer : "none");
  return found;
}
