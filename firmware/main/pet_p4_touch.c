#include "pet_p4_touch.h"
#include "pet_p4_conversation.h"

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
#include "pet_p4_center_layout.h"
#include "pet_p4_touch_core.h"

#define PET_P4_TOUCH_SAMPLE_MS 15
#define PET_P4_TOUCH_SWIPE_THRESHOLD 48
#define PET_P4_TOUCH_LONG_PRESS_MS 700ULL
#define PET_P4_TOUCH_QUEUE_LENGTH 32
#define PET_P4_TOUCH_REACTION_MIN_MS 1200ULL

static const char *TAG = "pet-p4-touch";
static esp_lcd_touch_handle_t g_touch;
static esp_lcd_panel_io_handle_t g_touch_io;
static QueueHandle_t g_touch_queue;
static atomic_uint g_dropped_events;
static unsigned int g_event_sequence;
static unsigned int g_reaction_sequence;
static bool g_ready;
static const char *g_init_stage = "not_started";
static esp_err_t g_init_error, g_primary_error, g_backup_error;
static esp_err_t g_probe_15 = ESP_ERR_INVALID_STATE, g_probe_38 = ESP_ERR_INVALID_STATE, g_probe_10 = ESP_ERR_INVALID_STATE,
                 g_probe_18 = ESP_ERR_INVALID_STATE;
static int g_address;
static bool g_ft6336;
static bool g_cst826;
static uint8_t g_chip_id, g_vendor_id;
const char *pet_p4_touch_controller(void) {
  return g_cst826 ? "CST826" : g_ft6336 ? "FT6336U" : g_touch ? "GT911" : "unidentified";
}
static atomic_uint g_samples, g_contacts, g_poll_errors, g_consecutive_errors;
static atomic_int g_panel_x, g_panel_y, g_ui_x, g_ui_y;
static atomic_uint g_pointer_age_ms, g_stale_events;
typedef struct {
  bool has_pointer, has_gesture;
  pet_p4_pointer_event_t pointer;
  pet_p4_touch_event_t gesture;
} touch_sample_t;
static bool g_test_stroke;
static bool g_component_stroke;
static bool g_app_stroke;

void pet_p4_touch_health(pet_p4_touch_health_t *out) {
  if (!out) return;
  *out = (pet_p4_touch_health_t){
    .stage = g_init_stage, .error = g_init_error, .address = g_address,
    .primary_error = g_primary_error, .backup_error = g_backup_error,
    .probe_15 = g_probe_15, .probe_38 = g_probe_38, .probe_10 = g_probe_10, .probe_18 = g_probe_18,
    .chip_id = g_chip_id, .vendor_id = g_vendor_id,
    .samples = atomic_load(&g_samples), .contacts = atomic_load(&g_contacts),
    .poll_errors = atomic_load(&g_poll_errors),
    .consecutive_errors = atomic_load(&g_consecutive_errors),
    .panel_x = atomic_load(&g_panel_x), .panel_y = atomic_load(&g_panel_y),
    .ui_x = atomic_load(&g_ui_x), .ui_y = atomic_load(&g_ui_y),
    .pointer_age_ms = atomic_load(&g_pointer_age_ms), .stale_events = atomic_load(&g_stale_events),
  };
}

static void queue_sample(touch_sample_t *sample, pet_p4_touch_decoder_t *gesture,
                         pet_p4_pointer_decoder_t *pointer) {
  if (!sample->has_pointer && !sample->has_gesture) return;
  if (xQueueSend(g_touch_queue, sample, 0) == pdTRUE) return;
  atomic_fetch_add(&g_dropped_events, 1);
  /* Unknown missing motion must never become a cut or a legacy swipe. */
  xQueueReset(g_touch_queue);
  sample->has_pointer = true; sample->has_gesture = false;
  sample->pointer.kind = PET_P4_POINTER_CANCEL;
  xQueueSend(g_touch_queue, sample, 0);
  memset(pointer, 0, sizeof(*pointer));
  pet_p4_touch_decoder_init(gesture, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
}

static uint64_t monotonic_ms(void) {
  return (uint64_t) (esp_timer_get_time() / 1000ULL);
}

static void touch_task(void *arg) {
  (void) arg;
  pet_p4_touch_decoder_t decoder;
  pet_p4_pointer_decoder_t pointer = {0};
  int track_id = -1;
  TickType_t last_wake = xTaskGetTickCount();
  unsigned int consecutive_errors = 0;
  pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);

  while (true) {
    esp_err_t err = ESP_OK;
    bool pressed = false;
    bool contact_ambiguous = false;
    int ui_x = decoder.current_x;
    int ui_y = decoder.current_y;
    if (g_ft6336 || g_cst826) {
      uint8_t raw[13]; pet_p4_ft6336_frame_t frame;
      err = esp_lcd_panel_io_rx_param(g_touch_io, 0x02, raw, sizeof(raw));
      if (err == ESP_OK && !pet_p4_ft6336_decode(raw, sizeof(raw), &frame)) err = ESP_ERR_INVALID_RESPONSE;
      if (err == ESP_OK) {
        contact_ambiguous = frame.count > 1;
        pressed = frame.count == 1;
        if (pressed) {
          contact_ambiguous = pointer.down && track_id != frame.track_id;
          track_id = frame.track_id;
          if (g_cst826) pet_p4_touch_cst826_to_ui(frame.x, frame.y, &ui_x, &ui_y);
          else pet_p4_touch_panel_to_ui(frame.x, frame.y, &ui_x, &ui_y);
          atomic_store(&g_panel_x, frame.x); atomic_store(&g_panel_y, frame.y);
        }
      }
    } else if ((err = esp_lcd_touch_read_data(g_touch)) == ESP_OK) {
      esp_lcd_touch_point_data_t points[2] = {0};
      uint8_t point_count = 0;
      err = esp_lcd_touch_get_data(g_touch, points, &point_count, 2);
      contact_ambiguous = point_count > 1;
      if (err == ESP_OK && point_count == 1) {
        pressed = true;
        contact_ambiguous = pointer.down && track_id != points[0].track_id;
        track_id = points[0].track_id;
        pet_p4_touch_panel_to_ui((int) points[0].x, (int) points[0].y, &ui_x, &ui_y);
        atomic_store(&g_panel_x, points[0].x); atomic_store(&g_panel_y, points[0].y);
      }
    }
    if (err != ESP_OK) {
      consecutive_errors += 1;
      atomic_fetch_add(&g_poll_errors, 1);
      atomic_store(&g_consecutive_errors, consecutive_errors);
      touch_sample_t sample = {0};
      sample.has_pointer = pet_p4_pointer_update(&pointer, false, false, ui_x, ui_y,
                                                monotonic_ms(), &sample.pointer);
      queue_sample(&sample, &decoder, &pointer);
      pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
      if (consecutive_errors == 1 || consecutive_errors % 200 == 0) {
        ESP_LOGW(TAG, "%s poll failed count=%u error=%s",
                 pet_p4_touch_controller(), consecutive_errors, esp_err_to_name(err));
      }
      vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
      continue;
    }
    consecutive_errors = 0;
    atomic_store(&g_consecutive_errors, 0);
    atomic_fetch_add(&g_samples, 1);
    if (pressed) {
      atomic_fetch_add(&g_contacts, 1);
      atomic_store(&g_ui_x, ui_x); atomic_store(&g_ui_y, ui_y);
    }
    touch_sample_t sample = {0};
    uint64_t now = monotonic_ms();
    sample.has_pointer = pet_p4_pointer_update(&pointer, !contact_ambiguous, pressed, ui_x, ui_y, now, &sample.pointer);
    if (contact_ambiguous) {
      pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
      queue_sample(&sample, &decoder, &pointer);
      vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
      continue;
    }
    if (sample.has_pointer && sample.pointer.kind == PET_P4_POINTER_CANCEL)
      pet_p4_touch_decoder_init(&decoder, PET_P4_TOUCH_SWIPE_THRESHOLD, PET_P4_TOUCH_LONG_PRESS_MS);
    else sample.has_gesture = pet_p4_touch_decoder_update(&decoder, pressed, ui_x, ui_y, now, &sample.gesture);
    queue_sample(&sample, &decoder, &pointer);
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_TOUCH_SAMPLE_MS));
  }
}

/* CST826 uses CHIPTYPE at 0xAA (0x11), not FT6336U's 0xA3 (0x64).
 * Register facts: Adafruit CST8XX and Espressif FT6336U documentation.
 * Both use count at 0x02 and six-byte point records; only one contact is
 * dispatched. Identification is read-only, with no GPIO/config writes. */
static esp_err_t identify_register_touch(void) {
  uint8_t cst_id = 0, ft_id = 0;
  esp_err_t err = esp_lcd_panel_io_rx_param(g_touch_io, 0xaa, &cst_id, 1);
  if (err == ESP_OK && cst_id == 0x11) {
    g_cst826 = true;
    g_chip_id = cst_id;
  } else {
    err = esp_lcd_panel_io_rx_param(g_touch_io, 0xa3, &ft_id, 1);
    if (err != ESP_OK) return err;
    g_chip_id = ft_id;
    if (ft_id != 0x64) {
      ESP_LOGW(TAG, "unknown touch addr=0x%02x CST ID=0x%02x FT ID=0x%02x", g_address, cst_id, ft_id);
      return ESP_ERR_NOT_SUPPORTED;
    }
    g_ft6336 = true;
  }
  /* Module/vendor ID is diagnostic only; it is not an identity gate. */
  (void)esp_lcd_panel_io_rx_param(g_touch_io, 0xa8, &g_vendor_id, 1);
  uint8_t raw[13]; pet_p4_ft6336_frame_t frame;
  err = esp_lcd_panel_io_rx_param(g_touch_io, 0x02, raw, sizeof(raw));
  if (err == ESP_OK && !pet_p4_ft6336_decode(raw, sizeof(raw), &frame)) err = ESP_ERR_INVALID_RESPONSE;
  if (err != ESP_OK) g_cst826 = g_ft6336 = false;
  return err;
}

esp_err_t pet_p4_touch_init(void) {
  if (g_ready) return ESP_OK;
  g_address = 0;
  g_cst826 = g_ft6336 = false;
  g_init_stage = "i2c_init";
  esp_err_t err = bsp_i2c_init();
  if (err != ESP_OK) { g_init_error = err; return err; }
  i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
  if (!i2c_bus) { g_init_error = ESP_ERR_INVALID_STATE; return g_init_error; }

  esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
  g_init_stage = "probe";
  g_primary_error = g_backup_error = ESP_ERR_NOT_FOUND;
  /* Allow controller power-up, bounded to under one second. No shared bus
   * reset and no speculative GPIO drive: the installed panel must be verified. */
  for (unsigned attempt = 0; attempt < 4 && !g_address; attempt++) {
    g_primary_error = i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 50);
    if (g_primary_error == ESP_OK) g_address = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    else {
      g_backup_error = i2c_master_probe(i2c_bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 50);
      if (g_backup_error == ESP_OK) g_address = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    }
    if (!g_address && attempt < 3) vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!g_address) {
    /* Address ACKs only, not controller identification or register writes.
     * Distinguish missing GT911 from an entirely unreachable shared bus. */
    g_probe_38 = i2c_master_probe(i2c_bus, 0x38, 50);
    g_probe_15 = i2c_master_probe(i2c_bus, 0x15, 50);
    g_probe_10 = i2c_master_probe(i2c_bus, 0x10, 50);
    g_probe_18 = i2c_master_probe(i2c_bus, 0x18, 50);
    g_init_error = (g_primary_error == ESP_ERR_TIMEOUT || g_backup_error == ESP_ERR_TIMEOUT)
      ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FOUND;
    ESP_LOGW(TAG, "GT911 probe failed SDA=%d SCL=%d 0x5d=%s 0x14=%s; reset not driven",
      BSP_I2C_SDA, BSP_I2C_SCL, esp_err_to_name(g_primary_error), esp_err_to_name(g_backup_error));
    if (g_probe_38 != ESP_OK && g_probe_15 != ESP_OK) return g_init_error;
    io_config = (esp_lcd_panel_io_i2c_config_t){
      .dev_addr = 0x38, .scl_speed_hz = 100000, .control_phase_bytes = 1,
      .lcd_cmd_bits = 8, .flags.disable_control_phase = 1,
    };
    const int addresses[] = {0x15, 0x38};
    for (unsigned i = 0; i < 2; ++i) {
      if ((addresses[i] == 0x15 ? g_probe_15 : g_probe_38) != ESP_OK) continue;
      g_address = io_config.dev_addr = addresses[i];
      g_init_stage = "identify_register_touch";
      err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &g_touch_io);
      if (err == ESP_OK) err = identify_register_touch();
      if (err == ESP_OK) break;
      if (g_touch_io) esp_lcd_panel_io_del(g_touch_io);
      g_touch_io = NULL;
    }
    if (!g_touch_io) { g_init_error = err; return err; }
  }
  io_config.dev_addr = g_address;
  g_init_stage = "panel_io";
  io_config.scl_speed_hz = (g_ft6336 || g_cst826) ? 100000 : CONFIG_BSP_I2C_CLK_SPEED_HZ;
  if (!g_touch_io) err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &g_touch_io);
  if (err != ESP_OK) { g_init_error = err; return err; }

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
  g_init_stage = "controller";
  if (!g_ft6336 && !g_cst826) err = esp_lcd_touch_new_i2c_gt911(g_touch_io, &touch_config, &g_touch);
  if (err != ESP_OK) {
    esp_lcd_panel_io_del(g_touch_io);
    g_touch_io = NULL;
    g_init_error = err; return err;
  }

  g_init_stage = "queue";
  g_touch_queue = xQueueCreate(PET_P4_TOUCH_QUEUE_LENGTH, sizeof(touch_sample_t));
  if (!g_touch_queue) goto no_memory;
  g_init_stage = "task";
  if (xTaskCreate(touch_task, "pet_p4_touch", 4096, NULL, 8, NULL) != pdPASS) {
    vQueueDelete(g_touch_queue);
    g_touch_queue = NULL;
    goto no_memory;
  }
  g_ready = true;
  g_init_stage = "ready"; g_init_error = ESP_OK;
  ESP_LOGI(TAG, "%s ready addr=0x%02x chip=0x%02x reset=disabled sample=%dms",
           pet_p4_touch_controller(), io_config.dev_addr, g_chip_id, PET_P4_TOUCH_SAMPLE_MS);
  return ESP_OK;
no_memory:
  if (g_touch) esp_lcd_touch_del(g_touch);
  g_touch = NULL; g_ft6336 = false;
  esp_lcd_panel_io_del(g_touch_io); g_touch_io = NULL;
  g_init_error = ESP_ERR_NO_MEM;
  return g_init_error;
}

bool pet_p4_touch_ready(void) {
  return g_ready && atomic_load(&g_consecutive_errors) < 3;
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
  if (pet_p4_conversation_active(state)) {
    if (event->gesture == PET_P4_TOUCH_SWIPE_LEFT || event->gesture == PET_P4_TOUCH_SWIPE_UP)
      pet_p4_conversation_move(state, -1, event->ts_ms);
    else if (event->gesture == PET_P4_TOUCH_SWIPE_RIGHT || event->gesture == PET_P4_TOUCH_SWIPE_DOWN)
      pet_p4_conversation_move(state, 1, event->ts_ms);
    return;
  }
  bool handled_locally = false;
  char action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  if(state&&!strcmp(state->screen_page,"components")) {
    const size_t count=pet_p4_miniapp_catalog_count();
    const size_t selected=pet_p4_miniapp_catalog_selected();
    if(event->gesture==PET_P4_TOUCH_TAP) {
      int hit=pet_center_hit(event->x,event->y,selected,count);
      if(hit>=0&&pet_p4_miniapp_catalog_select((size_t)hit)) {
        if(pet_p4_miniapp_catalog_activate_selected()) {
          pet_p4_miniapp_sync_stats(&state->stats);
          snprintf(state->screen_page,sizeof(state->screen_page),"app");
          snprintf(action,sizeof(action),"component_open");handled_locally=true;
        }
      }
    }else if(event->gesture==PET_P4_TOUCH_SWIPE_LEFT||event->gesture==PET_P4_TOUCH_SWIPE_RIGHT) {
      size_t next=pet_center_page(selected,count,event->gesture==PET_P4_TOUCH_SWIPE_LEFT?1:-1);
      handled_locally=count>0&&pet_p4_miniapp_catalog_select(next);
      snprintf(action,sizeof(action),"component_select");
    }
    if(handled_locally)state->last_update_ms+=1;
    send_touch_event(state,send_line,ctx,event,action,handled_locally);
    return;
  }
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
      if(event->gesture==PET_P4_TOUCH_TAP&&pet_p4_miniapp_instrument_config(NULL,NULL)){
        snprintf(action,sizeof(action),"instrument.strike");
        handled_locally=pet_p4_miniapp_dispatch_action(action,event->ts_ms);
      }else handled_locally = pet_p4_miniapp_dispatch_input(
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
  touch_sample_t sample;
  if (!g_touch_queue) return;
  while (xQueueReceive(g_touch_queue, &sample, 0) == pdTRUE) {
    bool test = state && !pet_p4_conversation_active(state) && !strcmp(state->screen_page, "touch_test");
    bool component = state && !pet_p4_conversation_active(state) && !strcmp(state->screen_page,"app")
      && pet_p4_miniapp_slicing_active();
    bool application = state && !pet_p4_conversation_active(state) && !strcmp(state->screen_page,"app")
      && pet_p4_miniapp_pointer_active();
    bool captured = g_test_stroke;
    if (sample.has_pointer) {
      if (sample.pointer.kind == PET_P4_POINTER_DOWN) g_test_stroke = test;
      if (sample.pointer.kind == PET_P4_POINTER_DOWN) g_component_stroke = component;
      if (sample.pointer.kind == PET_P4_POINTER_DOWN) g_app_stroke = application;
      captured = g_test_stroke;
      if(g_component_stroke) {
        if(!component || monotonic_ms()-sample.pointer.ts_ms>150)sample.pointer.kind=PET_P4_POINTER_CANCEL;
        pet_p4_miniapp_pointer(&sample.pointer);
        captured=true;
      }
      if(g_app_stroke) {
        pet_p4_pointer_event_t event=sample.pointer;
        if(!application || monotonic_ms()-event.ts_ms>150)event.kind=PET_P4_POINTER_CANCEL;
        pet_p4_miniapp_pointer(&event);
        captured=true;
      }
      if (g_test_stroke && test) {
        /* Discard stale queued input; never join it to a fresh stroke. */
        uint64_t age = monotonic_ms() - sample.pointer.ts_ms;
        atomic_store(&g_pointer_age_ms, (unsigned)age);
        if (age > 150) {
          sample.pointer.kind = PET_P4_POINTER_CANCEL;
          atomic_fetch_add(&g_stale_events, 1);
        }
        pet_p4_slice_tick(&state->touch_test, sample.pointer.ts_ms);
        pet_p4_slice_pointer(&state->touch_test, &sample.pointer);
      }
      if (sample.pointer.kind == PET_P4_POINTER_UP || sample.pointer.kind == PET_P4_POINTER_CANCEL)
        g_test_stroke = g_component_stroke = g_app_stroke = false;
    }
    /* An application owns the entire stroke, including swipes and cancellation.
       Never reinterpret an in-app drag as global navigation. */
    if (sample.has_gesture && !captured && !test && !component
        && !application)
      process_touch_event(state, send_line, ctx, &sample.gesture);
  }
}

unsigned int pet_p4_touch_dropped_events(void) {
  return atomic_load_explicit(&g_dropped_events, memory_order_relaxed);
}
