#include "pet_p4_touch.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pet_p4_assets.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_touch_core.h"

#define PET_P4_TOUCH_SAMPLE_MS 15
#define PET_P4_TOUCH_SWIPE_THRESHOLD 48
#define PET_P4_TOUCH_LONG_PRESS_MS 700ULL
#define PET_P4_TOUCH_QUEUE_LENGTH 16
#define PET_P4_TOUCH_REACTION_MIN_MS 1200ULL

static const char *TAG = "pet-p4-touch";
static esp_lcd_touch_handle_t g_touch;
static esp_lcd_panel_io_handle_t g_touch_io;
static QueueHandle_t g_touch_queue;
static atomic_uint g_dropped_events;
static unsigned int g_event_sequence;
static unsigned int g_reaction_sequence;
static bool g_ready;

static uint64_t monotonic_ms(void) {
  return (uint64_t) (esp_timer_get_time() / 1000ULL);
}

static void touch_task(void *arg) {
  (void) arg;
  pet_p4_touch_decoder_t decoder;
  TickType_t last_wake = xTaskGetTickCount();
  unsigned int consecutive_errors = 0;
  pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);

  while (true) {
    esp_err_t err = esp_lcd_touch_read_data(g_touch);
    bool pressed = false;
    int ui_x = decoder.current_x;
    int ui_y = decoder.current_y;
    if (err == ESP_OK) {
      esp_lcd_touch_point_data_t point = {0};
      uint8_t point_count = 0;
      err = esp_lcd_touch_get_data(g_touch, &point, &point_count, 1);
      if (err == ESP_OK && point_count > 0) {
        pressed = true;
        pet_p4_touch_panel_to_ui((int) point.x, (int) point.y, &ui_x, &ui_y);
      }
    }
    if (err != ESP_OK) {
      consecutive_errors += 1;
      if (consecutive_errors == 1 || consecutive_errors % 200 == 0) {
        ESP_LOGW(TAG, "GT911 poll failed count=%u error=%s",
                 consecutive_errors, esp_err_to_name(err));
      }
      vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
      continue;
    }
    consecutive_errors = 0;

    pet_p4_touch_event_t event;
    if (pet_p4_touch_decoder_update(&decoder, pressed, ui_x, ui_y, monotonic_ms(), &event)) {
      if (!g_touch_queue || xQueueSend(g_touch_queue, &event, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&g_dropped_events, 1, memory_order_relaxed);
      }
    }
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
  }
}

esp_err_t pet_p4_touch_init(void) {
  if (g_ready) return ESP_OK;
  esp_err_t err = bsp_i2c_init();
  if (err != ESP_OK) return err;
  i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
  if (!i2c_bus) return ESP_ERR_INVALID_STATE;

  esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  if (i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 50) == ESP_OK) {
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
  } else if (i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 50) == ESP_OK) {
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
  } else {
    return ESP_ERR_NOT_FOUND;
  }
  io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
  err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &g_touch_io);
  if (err != ESP_OK) return err;

  const esp_lcd_touch_config_t touch_config = {
    .x_max = BSP_LCD_H_RES,
    .y_max = BSP_LCD_V_RES,
    .rst_gpio_num = GPIO_NUM_NC,
    .int_gpio_num = GPIO_NUM_NC,
    .levels = {
      .reset = 0,
      .interrupt = 0,
    },
    .flags = {
      .swap_xy = 0,
      .mirror_x = 0,
      .mirror_y = 0,
    },
  };
  err = esp_lcd_touch_new_i2c_gt911(g_touch_io, &touch_config, &g_touch);
  if (err != ESP_OK) {
    esp_lcd_panel_io_del(g_touch_io);
    g_touch_io = NULL;
    return err;
  }

  g_touch_queue = xQueueCreate(PET_P4_TOUCH_QUEUE_LENGTH, sizeof(pet_p4_touch_event_t));
  if (!g_touch_queue) return ESP_ERR_NO_MEM;
  if (xTaskCreate(touch_task, "pet_p4_touch", 4096, NULL, 8, NULL) != pdPASS) {
    vQueueDelete(g_touch_queue);
    g_touch_queue = NULL;
    return ESP_ERR_NO_MEM;
  }
  g_ready = true;
  ESP_LOGI(TAG, "GT911 ready addr=0x%02x reset=disabled sample=%dms",
           io_config.dev_addr, PET_P4_TOUCH_SAMPLE_MS);
  return ESP_OK;
}

bool pet_p4_touch_ready(void) {
  return g_ready;
}

static const char *touch_event_name(pet_p4_touch_gesture_t gesture) {
  if (gesture == PET_P4_TOUCH_TAP) return "screen.region.tap";
  if (gesture == PET_P4_TOUCH_LONG_PRESS) return "screen.region.long_press";
  if (gesture == PET_P4_TOUCH_SWIPE_LEFT) return "touch.swipe_left";
  if (gesture == PET_P4_TOUCH_SWIPE_RIGHT) return "touch.swipe_right";
  if (gesture == PET_P4_TOUCH_SWIPE_UP) return "touch.swipe_up";
  if (gesture == PET_P4_TOUCH_SWIPE_DOWN) return "touch.swipe_down";
  return "touch.unknown";
}

static bool gesture_is_swipe(pet_p4_touch_gesture_t gesture) {
  return gesture == PET_P4_TOUCH_SWIPE_LEFT
    || gesture == PET_P4_TOUCH_SWIPE_RIGHT
    || gesture == PET_P4_TOUCH_SWIPE_UP
    || gesture == PET_P4_TOUCH_SWIPE_DOWN;
}

static const char *choose_touch_family(
  const pet_p4_runtime_state_t *state,
  int x,
  char *family,
  size_t family_size
) {
  const pet_p4_asset_catalog_t *catalog = state ? &state->asset_catalog : NULL;
  const char *directional = x < 214 ? "touch.left" : (x > 426 ? "touch.right" : "");
  if (directional[0] && catalog && pet_p4_asset_catalog_find_exact(catalog, directional) >= 0) {
    snprintf(family, family_size, "%s", directional);
    return family;
  }
  const char *first = (g_reaction_sequence++ % 2U) == 0 ? "touch.lick" : "touch.what";
  const char *second = strcmp(first, "touch.lick") == 0 ? "touch.what" : "touch.lick";
  if (catalog && pet_p4_asset_catalog_find_exact(catalog, first) >= 0) {
    snprintf(family, family_size, "%s", first);
  } else if (catalog && pet_p4_asset_catalog_find_exact(catalog, second) >= 0) {
    snprintf(family, family_size, "%s", second);
  } else {
    snprintf(family, family_size, "%s", first);
  }
  return family;
}

static uint64_t touch_reaction_duration_ms(
  const pet_p4_runtime_state_t *state,
  const char *family
) {
  const pet_p4_asset_catalog_t *catalog = state ? &state->asset_catalog : NULL;
  int index = catalog ? pet_p4_asset_catalog_find_exact(catalog, family) : -1;
  if (index < 0 && catalog) index = pet_p4_asset_catalog_nth_prefix(catalog, "touch.", 0);
  uint64_t duration = PET_P4_TOUCH_REACTION_MIN_MS;
  if (catalog && index >= 0 && index < catalog->count) {
    const pet_p4_asset_entry_t *entry = &catalog->entries[index];
    if (entry->frame_duration_ms > 0) {
      duration = (uint64_t) entry->frames * entry->frame_duration_ms;
    } else if (entry->fps > 0) {
      duration = ((uint64_t) entry->frames * 1000ULL) / (uint64_t) entry->fps;
    }
  }
  if (duration < PET_P4_TOUCH_REACTION_MIN_MS) duration = PET_P4_TOUCH_REACTION_MIN_MS;
  return duration;
}

static void send_touch_event(
  const pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_touch_event_t *event,
  const char *action,
  bool handled_locally
) {
  if (!send_line || !event) return;
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", "input/event");
  cJSON_AddNumberToObject(payload, "version", 1);
  cJSON_AddNumberToObject(payload, "seq", ++g_event_sequence);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddStringToObject(payload, "control", "screen.touch");
  cJSON_AddStringToObject(payload, "gesture", pet_p4_touch_gesture_name(event->gesture));
  cJSON_AddStringToObject(payload, "event", touch_event_name(event->gesture));
  cJSON_AddStringToObject(payload, "context", state ? state->screen_page : "main");
  cJSON_AddStringToObject(payload, "action", action ? action : "disabled");
  cJSON_AddNumberToObject(payload, "x", event->x);
  cJSON_AddNumberToObject(payload, "y", event->y);
  cJSON_AddNumberToObject(payload, "dx", event->dx);
  cJSON_AddNumberToObject(payload, "dy", event->dy);
  cJSON_AddNumberToObject(payload, "durationMs", (double) event->duration_ms);
  cJSON_AddBoolToObject(payload, "handledLocally", handled_locally);
  cJSON_AddNumberToObject(payload, "tsMs", (double) event->ts_ms);
  cJSON_AddNumberToObject(
    payload,
    "dropped",
    atomic_load_explicit(&g_dropped_events, memory_order_relaxed)
  );
  cJSON_AddItemToObject(root, "payload", payload);
  char *line = cJSON_PrintUnformatted(root);
  if (line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static void process_touch_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_touch_event_t *event
) {
  bool handled_locally = false;
  char action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  if (gesture_is_swipe(event->gesture)) {
    const bool main_open = state && strcmp(state->screen_page, "main") == 0;
    const char *next_page = main_open ? "components"
      : state && strcmp(state->screen_page, "app") == 0 ? "components"
      : "main";
    if (state) {
      if (main_open) pet_p4_miniapp_catalog_focus_active();
      snprintf(state->screen_page, sizeof(state->screen_page), "%s", next_page);
      state->last_update_ms += 1;
      handled_locally = true;
      snprintf(action, sizeof(action), "page_%s", next_page);
    }
  } else {
    const char *event_name = touch_event_name(event->gesture);
    if (state && strcmp(state->screen_page, "app") == 0) {
      handled_locally = pet_p4_miniapp_dispatch_input(
        event_name,
        event->ts_ms,
        action,
        sizeof(action)
      );
      if (handled_locally) state->last_update_ms += 1;
    }
    if (!handled_locally && state && strcmp(state->screen_page, "main") == 0) {
      char family[PET_P4_STATE_MAX];
      choose_touch_family(state, event->x, family, sizeof(family));
      handled_locally = pet_p4_state_request_touch(
        state,
        family,
        event->x,
        event->y,
        touch_reaction_duration_ms(state, family),
        event->ts_ms
      );
      if (handled_locally) snprintf(action, sizeof(action), "pet_touch");
    }
  }
  send_touch_event(state, send_line, ctx, event, action, handled_locally);
}

void pet_p4_touch_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  pet_p4_touch_event_t event;
  if (!g_touch_queue) return;
  while (xQueueReceive(g_touch_queue, &event, 0) == pdTRUE) {
    process_touch_event(state, send_line, ctx, &event);
  }
}

unsigned int pet_p4_touch_dropped_events(void) {
  return atomic_load_explicit(&g_dropped_events, memory_order_relaxed);
}
