/*
 * [Input] Debounced SW1-SW3/shared center-key/legacy encoder GPIO events,
 *         calibrated four-direction joystick ADC samples, and validated
 *         desktop bindings.
 * [Output] Persisted configurable input actions, context-local previous/next
 *          selection plus SW2 top-level page toggling, center-aware joystick
 *          decoding and ADC diagnostics with legacy encoder-event aliases, and component-local
 *          button routing that is active only while the component page is
 *          open, while the persisted global page-back gesture always wins and
 *          every other unmapped system-navigation gesture is suppressed, and
 *          package-authored navigation actions are ignored, plus correlated snapshots of the authoritative NVS-backed
 *          input configuration with versioned SW1-back/SW3-confirm defaults.
 * [Pos] ESP32-P4 physical-input runtime.
 * [Sync] If this file changes, update `esp-p4-runtime/.folder.md` and `protocol.md`.
 */

#include "pet_p4_input.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/adc_channel.h"

#include "pet_p4_audio.h"
#include "pet_p4_input_core.h"
#include "pet_p4_miniapp.h"

#define PET_P4_INPUT_SW1_GPIO GPIO_NUM_50
#define PET_P4_INPUT_SW2_GPIO GPIO_NUM_49
#define PET_P4_INPUT_SW3_GPIO GPIO_NUM_5
#define PET_P4_INPUT_ENCODER_PRESS_GPIO GPIO_NUM_4
#define PET_P4_INPUT_ENCODER_B_GPIO GPIO_NUM_3
#define PET_P4_INPUT_ENCODER_A_GPIO GPIO_NUM_2
#define PET_P4_INPUT_JOYSTICK_X_GPIO GPIO_NUM_21
#define PET_P4_INPUT_JOYSTICK_Y_GPIO GPIO_NUM_20
#define PET_P4_INPUT_JOYSTICK_X_CHANNEL ADC1_GPIO21_CHANNEL
#define PET_P4_INPUT_JOYSTICK_Y_CHANNEL ADC1_GPIO20_CHANNEL

#define PET_P4_INPUT_SAMPLE_MS 5
#define PET_P4_INPUT_DEBOUNCE_MS 25
#define PET_P4_INPUT_LONG_PRESS_MS 700
#define PET_P4_INPUT_JOYSTICK_CENTER_DEFAULT 2048
#define PET_P4_INPUT_JOYSTICK_CENTER_MIN 1200
#define PET_P4_INPUT_JOYSTICK_CENTER_MAX 2900
#define PET_P4_INPUT_JOYSTICK_CALIBRATION_SAMPLES 32
#define PET_P4_INPUT_JOYSTICK_ACTIVATION_DELTA 900
#define PET_P4_INPUT_JOYSTICK_RELEASE_DELTA 500
#define PET_P4_INPUT_JOYSTICK_REPEAT_DELAY_MS 350
#define PET_P4_INPUT_JOYSTICK_REPEAT_INTERVAL_MS 140
#define PET_P4_INPUT_QUEUE_LENGTH 32
#define PET_P4_INPUT_NVS_NAMESPACE "pet_input"
#define PET_P4_INPUT_NVS_KEY "config"

typedef struct {
  char event[PET_P4_INPUT_EVENT_MAX];
  char action[PET_P4_INPUT_ACTION_MAX];
  char value[PET_P4_INPUT_VALUE_MAX];
} pet_p4_input_binding_t;

typedef struct {
  uint32_t version;
  uint32_t binding_count;
  pet_p4_input_binding_t bindings[PET_P4_INPUT_MAX_BINDINGS];
} pet_p4_input_config_t;

typedef enum {
  PET_P4_INPUT_CONTROL_SW1,
  PET_P4_INPUT_CONTROL_SW2,
  PET_P4_INPUT_CONTROL_SW3,
  PET_P4_INPUT_CONTROL_ENCODER_PRESS,
  PET_P4_INPUT_CONTROL_ENCODER,
  PET_P4_INPUT_CONTROL_JOYSTICK,
} pet_p4_input_control_t;

typedef enum {
  PET_P4_INPUT_GESTURE_DOWN,
  PET_P4_INPUT_GESTURE_UP,
  PET_P4_INPUT_GESTURE_SHORT_PRESS,
  PET_P4_INPUT_GESTURE_LONG_PRESS,
  PET_P4_INPUT_GESTURE_HOLD_START,
  PET_P4_INPUT_GESTURE_HOLD_END,
  PET_P4_INPUT_GESTURE_ROTATE,
  PET_P4_INPUT_GESTURE_DIRECTION,
} pet_p4_input_gesture_t;

typedef struct {
  pet_p4_input_control_t control;
  pet_p4_input_gesture_t gesture;
  int delta;
  unsigned long long ts_ms;
} pet_p4_input_event_t;

typedef struct {
  gpio_num_t gpio;
  pet_p4_input_control_t control;
  pet_p4_button_decoder_t decoder;
} pet_p4_input_button_t;

static const char *TAG = "pet-p4-input";
static QueueHandle_t g_event_queue;
static pet_p4_input_config_t g_config;
static atomic_uint g_dropped_events;
static unsigned int g_event_sequence;
static adc_oneshot_unit_handle_t g_joystick_adc;
static atomic_bool g_joystick_ready;
static atomic_int g_joystick_center_x;
static atomic_int g_joystick_center_y;
static atomic_int g_joystick_current_x;
static atomic_int g_joystick_current_y;
static atomic_int g_joystick_minimum_x;
static atomic_int g_joystick_maximum_x;
static atomic_int g_joystick_minimum_y;
static atomic_int g_joystick_maximum_y;

static void update_atomic_minimum(atomic_int *value, int sample) {
  int current = atomic_load_explicit(value, memory_order_relaxed);
  while (sample < current
      && !atomic_compare_exchange_weak_explicit(
        value,
        &current,
        sample,
        memory_order_relaxed,
        memory_order_relaxed
      )) {}
}

static void update_atomic_maximum(atomic_int *value, int sample) {
  int current = atomic_load_explicit(value, memory_order_relaxed);
  while (sample > current
      && !atomic_compare_exchange_weak_explicit(
        value,
        &current,
        sample,
        memory_order_relaxed,
        memory_order_relaxed
      )) {}
}

static void record_joystick_sample(int x, int y) {
  atomic_store_explicit(&g_joystick_current_x, x, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_current_y, y, memory_order_relaxed);
  update_atomic_minimum(&g_joystick_minimum_x, x);
  update_atomic_maximum(&g_joystick_maximum_x, x);
  update_atomic_minimum(&g_joystick_minimum_y, y);
  update_atomic_maximum(&g_joystick_maximum_y, y);
}

static void copy_text(char *dest, size_t dest_size, const char *src) {
  if (!dest || dest_size == 0) return;
  snprintf(dest, dest_size, "%s", src ? src : "");
}

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static bool event_is_allowed(const char *event) {
  static const char *const allowed[] = {
    "button.sw1.short_press",
    "button.sw1.long_press",
    "button.sw1.hold",
    "button.sw2.short_press",
    "button.sw2.long_press",
    "button.sw2.hold",
    "button.sw3.short_press",
    "button.sw3.long_press",
    "button.sw3.hold",
    "button.encoder.short_press",
    "button.encoder.long_press",
    "button.encoder.hold",
    "knob.rotate_cw",
    "knob.rotate_ccw",
    "joystick.up",
    "joystick.down",
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i += 1) {
    if (strcmp(event, allowed[i]) == 0) return true;
  }
  return false;
}

static bool action_is_allowed(const char *action) {
  static const char *const allowed[] = {
    "disabled",
    "voice_ptt",
    "agent_enter",
    "agent_prompt",
    "session_next",
    "session_previous",
    "session_clear",
    "miniapp_screen_tap",
    "miniapp_screen_long_press",
    "page_toggle",
    "page_enter",
    "page_back",
    "page_main",
    "page_app",
    "component_center",
    "miniapp_action",
  };
  for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i += 1) {
    if (strcmp(action, allowed[i]) == 0) return true;
  }
  return false;
}

static bool binding_is_valid(const pet_p4_input_binding_t *binding) {
  if (!binding
      || strnlen(binding->event, sizeof(binding->event)) >= sizeof(binding->event)
      || strnlen(binding->action, sizeof(binding->action)) >= sizeof(binding->action)
      || strnlen(binding->value, sizeof(binding->value)) >= sizeof(binding->value)
      || !event_is_allowed(binding->event)
      || !action_is_allowed(binding->action)) {
    return false;
  }
  return true;
}

static void add_default_binding(
  pet_p4_input_config_t *config,
  const char *event,
  const char *action,
  const char *value
) {
  if (!config || config->binding_count >= PET_P4_INPUT_MAX_BINDINGS) return;
  pet_p4_input_binding_t *binding = &config->bindings[config->binding_count++];
  copy_text(binding->event, sizeof(binding->event), event);
  copy_text(binding->action, sizeof(binding->action), action);
  copy_text(binding->value, sizeof(binding->value), value);
}

static void load_default_config(pet_p4_input_config_t *config) {
  memset(config, 0, sizeof(*config));
  config->version = PET_P4_INPUT_CONFIG_VERSION;
  add_default_binding(config, "button.sw1.short_press", "page_back", "");
  add_default_binding(config, "button.sw1.long_press", "disabled", "");
  add_default_binding(config, "button.sw1.hold", "voice_ptt", "");
  add_default_binding(config, "button.sw2.short_press", "component_center", "");
  add_default_binding(config, "button.sw2.long_press", "disabled", "");
  add_default_binding(config, "button.sw2.hold", "disabled", "");
  add_default_binding(config, "button.sw3.short_press", "page_enter", "");
  add_default_binding(config, "button.sw3.long_press", "disabled", "");
  add_default_binding(config, "button.sw3.hold", "disabled", "");
  add_default_binding(config, "button.encoder.short_press", "page_enter", "");
  add_default_binding(config, "button.encoder.long_press", "disabled", "");
  add_default_binding(config, "button.encoder.hold", "disabled", "");
  add_default_binding(config, "knob.rotate_cw", "session_next", "");
  add_default_binding(config, "knob.rotate_ccw", "session_previous", "");
  add_default_binding(config, "joystick.up", "disabled", "");
  add_default_binding(config, "joystick.down", "disabled", "");
}

static bool config_bindings_are_valid(const pet_p4_input_config_t *config) {
  if (!config || config->binding_count > PET_P4_INPUT_MAX_BINDINGS) {
    return false;
  }
  for (uint32_t i = 0; i < config->binding_count; i += 1) {
    if (!binding_is_valid(&config->bindings[i])) return false;
  }
  return true;
}

static bool config_is_valid(const pet_p4_input_config_t *config) {
  return config
    && config->version == PET_P4_INPUT_CONFIG_VERSION
    && config_bindings_are_valid(config);
}

static void migrate_default_binding(
  pet_p4_input_config_t *config,
  const char *event,
  const char *old_action,
  const char *old_value,
  const char *new_action,
  const char *new_value
) {
  if (!config) return;
  for (uint32_t i = 0; i < config->binding_count; i += 1) {
    pet_p4_input_binding_t *binding = &config->bindings[i];
    if (strcmp(binding->event, event) != 0
        || strcmp(binding->action, old_action) != 0
        || strcmp(binding->value, old_value) != 0) {
      continue;
    }
    copy_text(binding->action, sizeof(binding->action), new_action);
    copy_text(binding->value, sizeof(binding->value), new_value);
    return;
  }
}

static bool migrate_v2_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 2 || !config_bindings_are_valid(config)) return false;

  // Version 2 shipped two successive default layouts. Only values matching
  // either old default are rewritten; unrelated user mappings stay intact.
  migrate_default_binding(config, "button.sw1.short_press", "page_toggle", "", "disabled", "");
  migrate_default_binding(config, "button.sw1.short_press", "agent_enter", "", "disabled", "");
  migrate_default_binding(config, "button.sw2.short_press", "agent_enter", "", "disabled", "");
  migrate_default_binding(
    config,
    "button.sw2.short_press",
    "agent_prompt",
    "继续当前任务并给出下一步。",
    "disabled",
    ""
  );
  migrate_default_binding(config, "button.sw3.short_press", "disabled", "", "component_center", "");
  migrate_default_binding(
    config,
    "button.sw3.long_press",
    "agent_prompt",
    "继续当前任务并给出下一步。",
    "disabled",
    ""
  );
  migrate_default_binding(config, "knob.rotate_cw", "disabled", "", "session_next", "");
  migrate_default_binding(config, "knob.rotate_ccw", "disabled", "", "session_previous", "");
  config->version = 3;
  return true;
}

static bool migrate_v3_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 3 || !config_bindings_are_valid(config)) return false;

  // Rewrite only bindings that still match version 3 defaults. User mappings
  // on all other gestures remain untouched.
  migrate_default_binding(
    config,
    "button.sw2.short_press",
    "disabled",
    "",
    "component_center",
    ""
  );
  migrate_default_binding(
    config,
    "button.sw3.short_press",
    "component_center",
    "",
    "page_back",
    ""
  );
  migrate_default_binding(
    config,
    "button.encoder.long_press",
    "page_back",
    "",
    "disabled",
    ""
  );
  config->version = 4;
  return true;
}

static bool migrate_v4_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 4 || !config_bindings_are_valid(config)) return false;

  // Version 5 adds the two directions that did not exist on the encoder.
  // Old left/right/press bindings keep their event names and user actions.
  add_default_binding(config, "joystick.up", "disabled", "");
  add_default_binding(config, "joystick.down", "disabled", "");
  config->version = 5;
  return true;
}

static bool migrate_v5_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 5 || !config_bindings_are_valid(config)) return false;

  // Version 6 promotes the unchanged SW1 short-press default to Confirm.
  // Rewrite only the former default so explicit user mappings remain intact.
  migrate_default_binding(
    config,
    "button.sw1.short_press",
    "disabled",
    "",
    "page_enter",
    ""
  );
  config->version = 6;
  return true;
}

static bool migrate_v6_config(pet_p4_input_config_t *config) {
  if (!config || config->version != 6 || !config_bindings_are_valid(config)) return false;

  // Version 7 swaps only the shipped SW1/SW3 navigation defaults. Explicit
  // user mappings on either switch and every unrelated gesture stay intact.
  migrate_default_binding(
    config,
    "button.sw1.short_press",
    "page_enter",
    "",
    "page_back",
    ""
  );
  migrate_default_binding(
    config,
    "button.sw3.short_press",
    "page_back",
    "",
    "page_enter",
    ""
  );
  config->version = PET_P4_INPUT_CONFIG_VERSION;
  return true;
}

static bool migrate_input_config(pet_p4_input_config_t *config) {
  if (!config) return false;
  if (config->version == 2 && !migrate_v2_config(config)) return false;
  if (config->version == 3 && !migrate_v3_config(config)) return false;
  if (config->version == 4 && !migrate_v4_config(config)) return false;
  if (config->version == 5 && !migrate_v5_config(config)) return false;
  return config->version == 6 && migrate_v6_config(config);
}

static esp_err_t persist_config(const pet_p4_input_config_t *config) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(PET_P4_INPUT_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, PET_P4_INPUT_NVS_KEY, config, sizeof(*config));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

static void load_persisted_config(void) {
  nvs_handle_t handle;
  pet_p4_input_config_t stored;
  size_t size = sizeof(stored);
  load_default_config(&g_config);
  if (nvs_open(PET_P4_INPUT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
  esp_err_t err = nvs_get_blob(handle, PET_P4_INPUT_NVS_KEY, &stored, &size);
  nvs_close(handle);
  if (err == ESP_OK && size == sizeof(stored) && config_is_valid(&stored)) {
    g_config = stored;
    ESP_LOGI(TAG, "restored %u input bindings from NVS", (unsigned int) g_config.binding_count);
  } else if (err == ESP_OK && size == sizeof(stored)) {
    const uint32_t stored_version = stored.version;
    if (!migrate_input_config(&stored)) {
      ESP_LOGW(TAG, "ignored input config version %u", (unsigned int) stored_version);
      return;
    }
    esp_err_t persist_err = persist_config(&stored);
    g_config = stored;
    if (persist_err == ESP_OK) {
      ESP_LOGI(
        TAG,
        "migrated %u input bindings from config v%u",
        (unsigned int) g_config.binding_count,
        (unsigned int) stored_version
      );
    } else {
      ESP_LOGW(TAG, "using migrated input config without persistence: %s", esp_err_to_name(persist_err));
    }
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "ignored invalid input config: %s", esp_err_to_name(err));
  }
}

static pet_p4_input_binding_t *find_binding(pet_p4_input_config_t *config, const char *event) {
  if (!config || !event) return NULL;
  for (uint32_t i = 0; i < config->binding_count; i += 1) {
    if (strcmp(config->bindings[i].event, event) == 0) return &config->bindings[i];
  }
  return NULL;
}

static const pet_p4_input_binding_t *active_binding(const char *event) {
  return find_binding(&g_config, event);
}

static bool apply_binding_json(pet_p4_input_config_t *config, const cJSON *item, char *error, size_t error_size) {
  const char *event = json_string(item, "event");
  const char *action = json_string(item, "action");
  const char *value = json_string(item, "value");
  if (!event_is_allowed(event)) {
    snprintf(error, error_size, "unsupported input event: %s", event);
    return false;
  }
  if (!action_is_allowed(action)) {
    snprintf(error, error_size, "unsupported input action: %s", action);
    return false;
  }
  if (strlen(value) >= PET_P4_INPUT_VALUE_MAX) {
    snprintf(error, error_size, "input action value exceeds %d bytes", PET_P4_INPUT_VALUE_MAX - 1);
    return false;
  }
  pet_p4_input_binding_t *binding = find_binding(config, event);
  if (!binding) {
    if (config->binding_count >= PET_P4_INPUT_MAX_BINDINGS) {
      snprintf(error, error_size, "too many input bindings");
      return false;
    }
    binding = &config->bindings[config->binding_count++];
    memset(binding, 0, sizeof(*binding));
    copy_text(binding->event, sizeof(binding->event), event);
  }
  copy_text(binding->action, sizeof(binding->action), action);
  copy_text(binding->value, sizeof(binding->value), value);
  return true;
}

static void send_config_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  bool legacy,
  const char *request_id,
  bool ok,
  unsigned int binding_count,
  const char *error
) {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", legacy ? "button-config-ack" : "input/config-ack");
  cJSON_AddStringToObject(payload, "requestId", request_id ? request_id : "");
  cJSON_AddBoolToObject(payload, "ok", ok);
  cJSON_AddNumberToObject(payload, "version", PET_P4_INPUT_CONFIG_VERSION);
  cJSON_AddNumberToObject(payload, "bindingCount", binding_count);
  cJSON_AddBoolToObject(payload, "persisted", ok);
  if (error && error[0]) {
    cJSON_AddStringToObject(payload, "error", error);
    cJSON_AddStringToObject(payload, "message", error);
  } else {
    cJSON_AddStringToObject(payload, "message", "input config written");
  }
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

bool pet_p4_input_handle_config(
  const cJSON *payload,
  bool legacy_button_config,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *request_id = json_string(payload, "requestId");
  const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(payload, "bindings");
  const cJSON *reset = cJSON_GetObjectItemCaseSensitive(payload, "reset");
  pet_p4_input_config_t *next = heap_caps_malloc(
    sizeof(*next),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  char error[128] = {0};

  if (!request_id[0]) request_id = json_string(payload, "request_id");
  if (!next) next = malloc(sizeof(*next));
  if (!next) {
    snprintf(error, sizeof(error), "input config allocation failed");
    send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
    return false;
  }
  load_default_config(next);
  if (!cJSON_IsTrue(reset)) {
    if (!cJSON_IsArray(bindings)) {
      snprintf(error, sizeof(error), "bindings array is required");
      free(next);
      send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
      return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, bindings) {
      if (!cJSON_IsObject(item) || !apply_binding_json(next, item, error, sizeof(error))) {
        if (!error[0]) snprintf(error, sizeof(error), "invalid input binding");
        free(next);
        send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
        return false;
      }
    }
  }

  esp_err_t err = persist_config(next);
  if (err != ESP_OK) {
    snprintf(error, sizeof(error), "persist input config failed: %s", esp_err_to_name(err));
    free(next);
    send_config_ack(send_line, ctx, legacy_button_config, request_id, false, 0, error);
    return false;
  }
  g_config = *next;
  free(next);
  send_config_ack(
    send_line,
    ctx,
    legacy_button_config,
    request_id,
    true,
    (unsigned int) g_config.binding_count,
    NULL
  );
  ESP_LOGI(TAG, "applied %u input bindings", (unsigned int) g_config.binding_count);
  return true;
}

bool pet_p4_input_send_config_state(
  const cJSON *request,
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *request_id = json_string(request, "requestId");
  const char *voice_button = "";
  bool voice_enabled = false;
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  cJSON *config = cJSON_CreateObject();
  cJSON *bindings = cJSON_CreateArray();
  char *line;
  bool sent;

  if (!root || !payload || !config || !bindings) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    cJSON_Delete(config);
    cJSON_Delete(bindings);
    return false;
  }

  for (uint32_t i = 0; i < g_config.binding_count; i += 1) {
    const pet_p4_input_binding_t *binding = &g_config.bindings[i];
    cJSON *item = cJSON_CreateObject();
    if (!item) {
      cJSON_Delete(root);
      cJSON_Delete(payload);
      cJSON_Delete(config);
      cJSON_Delete(bindings);
      return false;
    }
    cJSON_AddStringToObject(item, "event", binding->event);
    cJSON_AddStringToObject(item, "action", binding->action);
    cJSON_AddStringToObject(item, "value", binding->value);
    cJSON_AddItemToArray(bindings, item);
    if (!voice_enabled && strcmp(binding->action, "voice_ptt") == 0) {
      voice_enabled = true;
      voice_button = binding->event;
    }
  }

  cJSON_AddNumberToObject(config, "version", PET_P4_INPUT_CONFIG_VERSION);
  cJSON_AddBoolToObject(config, "voiceEnabled", voice_enabled);
  cJSON_AddStringToObject(config, "voiceButton", voice_button);
  cJSON_AddItemToObject(config, "bindings", bindings);
  cJSON_AddStringToObject(payload, "requestId", request_id);
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddStringToObject(payload, "boardDeviceId", board_device_id ? board_device_id : "");
  cJSON_AddStringToObject(payload, "runtime", "esp-p4");
  cJSON_AddNumberToObject(payload, "bindingCount", g_config.binding_count);
  cJSON_AddItemToObject(payload, "config", config);
  cJSON_AddStringToObject(root, "topic", "input/config-state");
  cJSON_AddItemToObject(root, "payload", payload);

  line = cJSON_PrintUnformatted(root);
  sent = line && send_line;
  if (sent) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
  return sent;
}

static const char *control_name(pet_p4_input_control_t control) {
  switch (control) {
    case PET_P4_INPUT_CONTROL_SW1: return "key.1";
    case PET_P4_INPUT_CONTROL_SW2: return "key.2";
    case PET_P4_INPUT_CONTROL_SW3: return "key.3";
    case PET_P4_INPUT_CONTROL_ENCODER_PRESS: return "encoder.press";
    case PET_P4_INPUT_CONTROL_ENCODER: return "encoder";
    case PET_P4_INPUT_CONTROL_JOYSTICK: return "joystick";
    default: return "unknown";
  }
}

static const char *button_event_prefix(pet_p4_input_control_t control) {
  switch (control) {
    case PET_P4_INPUT_CONTROL_SW1: return "button.sw1";
    case PET_P4_INPUT_CONTROL_SW2: return "button.sw2";
    case PET_P4_INPUT_CONTROL_SW3: return "button.sw3";
    case PET_P4_INPUT_CONTROL_ENCODER_PRESS: return "button.encoder";
    default: return "";
  }
}

static void queue_event(
  pet_p4_input_control_t control,
  pet_p4_input_gesture_t gesture,
  int delta
) {
  pet_p4_input_event_t event = {
    .control = control,
    .gesture = gesture,
    .delta = delta,
    .ts_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL),
  };
  if (!g_event_queue || xQueueSend(g_event_queue, &event, 0) != pdTRUE) {
    atomic_fetch_add_explicit(&g_dropped_events, 1, memory_order_relaxed);
  }
}

static void queue_button_flags(pet_p4_input_control_t control, uint8_t flags) {
  if (flags & PET_P4_BUTTON_EVENT_SHORT_PRESS) queue_event(control, PET_P4_INPUT_GESTURE_SHORT_PRESS, 0);
  if (flags & PET_P4_BUTTON_EVENT_LONG_PRESS) {
    queue_event(control, PET_P4_INPUT_GESTURE_LONG_PRESS, 0);
    queue_event(control, PET_P4_INPUT_GESTURE_HOLD_START, 0);
  }
  if (flags & PET_P4_BUTTON_EVENT_LONG_RELEASE) {
    queue_event(control, PET_P4_INPUT_GESTURE_HOLD_END, 0);
  }
}

static bool read_joystick_axes(int *x, int *y) {
  if (!g_joystick_adc || !x || !y) return false;
  return adc_oneshot_read(g_joystick_adc, PET_P4_INPUT_JOYSTICK_X_CHANNEL, x) == ESP_OK
    && adc_oneshot_read(g_joystick_adc, PET_P4_INPUT_JOYSTICK_Y_CHANNEL, y) == ESP_OK;
}

static void calibrate_joystick_center(int *center_x, int *center_y) {
  long long sum_x = 0;
  long long sum_y = 0;
  int samples = 0;
  for (int i = 0; i < PET_P4_INPUT_JOYSTICK_CALIBRATION_SAMPLES; i += 1) {
    int x;
    int y;
    if (read_joystick_axes(&x, &y)) {
      sum_x += x;
      sum_y += y;
      samples += 1;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  *center_x = samples > 0 ? (int) (sum_x / samples) : PET_P4_INPUT_JOYSTICK_CENTER_DEFAULT;
  *center_y = samples > 0 ? (int) (sum_y / samples) : PET_P4_INPUT_JOYSTICK_CENTER_DEFAULT;
  if (*center_x < PET_P4_INPUT_JOYSTICK_CENTER_MIN
      || *center_x > PET_P4_INPUT_JOYSTICK_CENTER_MAX) {
    *center_x = PET_P4_INPUT_JOYSTICK_CENTER_DEFAULT;
  }
  if (*center_y < PET_P4_INPUT_JOYSTICK_CENTER_MIN
      || *center_y > PET_P4_INPUT_JOYSTICK_CENTER_MAX) {
    *center_y = PET_P4_INPUT_JOYSTICK_CENTER_DEFAULT;
  }
  ESP_LOGI(TAG, "joystick center calibrated x=%d y=%d samples=%d", *center_x, *center_y, samples);
}

static void input_task(void *arg) {
  (void) arg;
  pet_p4_input_button_t buttons[] = {
    {.gpio = PET_P4_INPUT_SW1_GPIO, .control = PET_P4_INPUT_CONTROL_SW1},
    {.gpio = PET_P4_INPUT_SW2_GPIO, .control = PET_P4_INPUT_CONTROL_SW2},
    {.gpio = PET_P4_INPUT_SW3_GPIO, .control = PET_P4_INPUT_CONTROL_SW3},
    {.gpio = PET_P4_INPUT_ENCODER_PRESS_GPIO, .control = PET_P4_INPUT_CONTROL_ENCODER_PRESS},
  };
  pet_p4_rotary_decoder_t rotary;
  pet_p4_joystick_decoder_t joystick;
  int joystick_center_x;
  int joystick_center_y;
  TickType_t last_wake;

  for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i += 1) {
    pet_p4_button_decoder_init(
      &buttons[i].decoder,
      gpio_get_level(buttons[i].gpio) == 0,
      PET_P4_INPUT_DEBOUNCE_MS,
      PET_P4_INPUT_LONG_PRESS_MS
    );
  }
  pet_p4_rotary_decoder_init(
    &rotary,
    gpio_get_level(PET_P4_INPUT_ENCODER_A_GPIO),
    gpio_get_level(PET_P4_INPUT_ENCODER_B_GPIO),
    4
  );
  calibrate_joystick_center(&joystick_center_x, &joystick_center_y);
  atomic_store_explicit(&g_joystick_center_x, joystick_center_x, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_center_y, joystick_center_y, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_current_x, joystick_center_x, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_current_y, joystick_center_y, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_minimum_x, joystick_center_x, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_maximum_x, joystick_center_x, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_minimum_y, joystick_center_y, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_maximum_y, joystick_center_y, memory_order_relaxed);
  atomic_store_explicit(&g_joystick_ready, true, memory_order_release);
  pet_p4_joystick_decoder_init(
    &joystick,
    joystick_center_x,
    joystick_center_y,
    PET_P4_INPUT_JOYSTICK_ACTIVATION_DELTA,
    PET_P4_INPUT_JOYSTICK_RELEASE_DELTA,
    PET_P4_INPUT_JOYSTICK_REPEAT_DELAY_MS,
    PET_P4_INPUT_JOYSTICK_REPEAT_INTERVAL_MS
  );
  last_wake = xTaskGetTickCount();

  while (true) {
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i += 1) {
      uint8_t flags = pet_p4_button_decoder_update(
        &buttons[i].decoder,
        gpio_get_level(buttons[i].gpio) == 0,
        PET_P4_INPUT_SAMPLE_MS
      );
      queue_button_flags(buttons[i].control, flags);
    }

    pet_p4_rotary_direction_t direction = pet_p4_rotary_decoder_update(
      &rotary,
      gpio_get_level(PET_P4_INPUT_ENCODER_A_GPIO),
      gpio_get_level(PET_P4_INPUT_ENCODER_B_GPIO)
    );
    if (direction != PET_P4_ROTARY_NONE) {
      queue_event(PET_P4_INPUT_CONTROL_ENCODER, PET_P4_INPUT_GESTURE_ROTATE, (int) direction);
    }
    int joystick_x;
    int joystick_y;
    if (read_joystick_axes(&joystick_x, &joystick_y)) {
      record_joystick_sample(joystick_x, joystick_y);
      pet_p4_joystick_direction_t joystick_direction = pet_p4_joystick_decoder_update(
        &joystick,
        joystick_x,
        joystick_y,
        PET_P4_INPUT_SAMPLE_MS
      );
      if (joystick_direction != PET_P4_JOYSTICK_CENTER) {
        queue_event(
          PET_P4_INPUT_CONTROL_JOYSTICK,
          PET_P4_INPUT_GESTURE_DIRECTION,
          (int) joystick_direction
        );
      }
    }
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PET_P4_INPUT_SAMPLE_MS));
  }
}

esp_err_t pet_p4_input_init(void) {
  gpio_config_t config = {
    .pin_bit_mask = (1ULL << PET_P4_INPUT_SW1_GPIO)
      | (1ULL << PET_P4_INPUT_SW2_GPIO)
      | (1ULL << PET_P4_INPUT_SW3_GPIO)
      | (1ULL << PET_P4_INPUT_ENCODER_PRESS_GPIO)
      | (1ULL << PET_P4_INPUT_ENCODER_A_GPIO)
      | (1ULL << PET_P4_INPUT_ENCODER_B_GPIO),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  adc_oneshot_unit_init_cfg_t adc_unit_config = {
    .unit_id = ADC_UNIT_1,
    .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  adc_oneshot_chan_cfg_t adc_channel_config = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  esp_err_t err = adc_oneshot_new_unit(&adc_unit_config, &g_joystick_adc);
  if (err != ESP_OK) return err;
  err = adc_oneshot_config_channel(
    g_joystick_adc,
    PET_P4_INPUT_JOYSTICK_X_CHANNEL,
    &adc_channel_config
  );
  if (err == ESP_OK) {
    err = adc_oneshot_config_channel(
      g_joystick_adc,
      PET_P4_INPUT_JOYSTICK_Y_CHANNEL,
      &adc_channel_config
    );
  }
  if (err == ESP_OK) err = gpio_config(&config);
  if (err != ESP_OK) {
    adc_oneshot_del_unit(g_joystick_adc);
    g_joystick_adc = NULL;
    return err;
  }

  load_persisted_config();
  g_event_queue = xQueueCreate(PET_P4_INPUT_QUEUE_LENGTH, sizeof(pet_p4_input_event_t));
  if (!g_event_queue) {
    adc_oneshot_del_unit(g_joystick_adc);
    g_joystick_adc = NULL;
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(input_task, "pet_p4_input", 4096, NULL, 9, NULL) != pdPASS) {
    vQueueDelete(g_event_queue);
    g_event_queue = NULL;
    adc_oneshot_del_unit(g_joystick_adc);
    g_joystick_adc = NULL;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(
    TAG,
    "inputs ready sw1=%d sw2=%d sw3=%d center_key=%d enc_b=%d enc_a=%d joy_x=%d joy_y=%d",
    PET_P4_INPUT_SW1_GPIO,
    PET_P4_INPUT_SW2_GPIO,
    PET_P4_INPUT_SW3_GPIO,
    PET_P4_INPUT_ENCODER_PRESS_GPIO,
    PET_P4_INPUT_ENCODER_B_GPIO,
    PET_P4_INPUT_ENCODER_A_GPIO,
    PET_P4_INPUT_JOYSTICK_X_GPIO,
    PET_P4_INPUT_JOYSTICK_Y_GPIO
  );
  return ESP_OK;
}

esp_err_t pet_p4_input_reset_config(void) {
  pet_p4_input_config_t defaults;
  load_default_config(&defaults);
  esp_err_t err = persist_config(&defaults);
  if (err == ESP_OK) g_config = defaults;
  return err;
}

static void send_input_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture,
  const pet_p4_input_binding_t *binding,
  const char *action_override,
  bool handled_locally
) {
  cJSON *root = cJSON_CreateObject();
  cJSON *payload = cJSON_CreateObject();
  const char *resolved_action = action_override && action_override[0]
    ? action_override
    : (binding ? binding->action : "disabled");
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", "input/event");
  cJSON_AddNumberToObject(payload, "version", PET_P4_INPUT_CONFIG_VERSION);
  cJSON_AddNumberToObject(payload, "seq", ++g_event_sequence);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddStringToObject(payload, "control", control_name(event->control));
  cJSON_AddStringToObject(payload, "gesture", gesture ? gesture : "");
  cJSON_AddNumberToObject(payload, "delta", event->delta);
  cJSON_AddStringToObject(payload, "event", event_name ? event_name : "");
  cJSON_AddStringToObject(payload, "context", state ? state->screen_page : "main");
  cJSON_AddStringToObject(payload, "action", resolved_action);
  if (state
      && (strcmp(resolved_action, "session_next") == 0
          || strcmp(resolved_action, "session_previous") == 0)
      && state->current_session_index > 0
      && state->current_session_index <= state->session_queue_count) {
    const pet_p4_session_queue_item_t *selected =
      &state->session_queue[state->current_session_index - 1];
    cJSON_AddStringToObject(payload, "sessionId", selected->id);
    cJSON_AddStringToObject(payload, "sessionTitle", selected->title);
    cJSON_AddNumberToObject(payload, "sessionIndex", state->current_session_index);
    cJSON_AddNumberToObject(payload, "sessionCount", state->session_queue_count);
  }
  if (binding && binding->value[0]) cJSON_AddStringToObject(payload, "value", binding->value);
  cJSON_AddBoolToObject(payload, "handledLocally", handled_locally);
  cJSON_AddNumberToObject(payload, "tsMs", (double) event->ts_ms);
  cJSON_AddNumberToObject(
    payload,
    "dropped",
    atomic_load_explicit(&g_dropped_events, memory_order_relaxed)
  );
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static bool apply_local_action(
  pet_p4_runtime_state_t *state,
  const pet_p4_input_binding_t *binding,
  unsigned long long ts_ms,
  char *action_override,
  size_t action_override_size
) {
  if (!binding || strcmp(binding->action, "disabled") == 0) return true;
  if (!state) return false;
  if (strcmp(binding->action, "session_clear") == 0) {
    memset(state->session_queue, 0, sizeof(state->session_queue));
    state->session_queue_count = 0;
    state->current_session_index = 0;
    state->current_session_count = 0;
    state->current_session_title[0] = '\0';
    state->current_session_notice[0] = '\0';
    state->session_notice_until_ms = 0;
    state->session_voice_active = false;
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "session_next") == 0
      || strcmp(binding->action, "session_previous") == 0) {
    const bool main_open = strcmp(state->screen_page, "main") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!main_open && !center_open) return false;
    const int direction = strcmp(binding->action, "session_previous") == 0 ? -1 : 1;
    if (center_open) {
      if (!pet_p4_miniapp_catalog_move(direction)) return false;
      copy_text(action_override, action_override_size, "component_select");
      state->last_update_ms = ts_ms;
      return true;
    }
    if (state->session_queue_count == 0) return false;
    unsigned int selected = state->current_session_index > 0
      ? state->current_session_index - 1
      : 0;
    if (selected >= state->session_queue_count) selected = 0;
    if (direction < 0) {
      selected = selected == 0 ? state->session_queue_count - 1 : selected - 1;
    } else {
      selected = (selected + 1) % state->session_queue_count;
    }
    state->current_session_index = selected + 1;
    state->current_session_count = state->session_queue_count;
    copy_text(
      state->current_session_id,
      sizeof(state->current_session_id),
      state->session_queue[selected].id
    );
    copy_text(
      state->current_session_title,
      sizeof(state->current_session_title),
      state->session_queue[selected].title
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_toggle") == 0) {
    const bool app_open = strcmp(state->screen_page, "app") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!app_open && !center_open) pet_p4_miniapp_catalog_focus_active();
    copy_text(
      state->screen_page,
      sizeof(state->screen_page),
      app_open ? "components" : (center_open ? "main" : "components")
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_enter") == 0) {
    if (strcmp(state->screen_page, "app") == 0) return false;
    if (strcmp(state->screen_page, "components") == 0) {
      if (!pet_p4_miniapp_catalog_activate_selected()) return false;
      copy_text(state->screen_page, sizeof(state->screen_page), "app");
    } else return false;
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_back") == 0) {
    const bool app_open = strcmp(state->screen_page, "app") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!app_open && !center_open) return false;
    if (app_open) pet_p4_miniapp_catalog_focus_active();
    copy_text(
      state->screen_page,
      sizeof(state->screen_page),
      app_open ? "components" : "main"
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_main") == 0) {
    copy_text(state->screen_page, sizeof(state->screen_page), "main");
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "component_center") == 0) {
    const bool main_open = strcmp(state->screen_page, "main") == 0;
    const bool center_open = strcmp(state->screen_page, "components") == 0;
    if (!main_open && !center_open) return false;
    if (main_open) pet_p4_miniapp_catalog_focus_active();
    copy_text(
      state->screen_page,
      sizeof(state->screen_page),
      center_open ? "main" : "components"
    );
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "page_app") == 0) {
    if (!pet_p4_miniapp_active()) return false;
    copy_text(state->screen_page, sizeof(state->screen_page), "app");
    state->last_update_ms = ts_ms;
    return true;
  }
  if (strcmp(binding->action, "miniapp_action") == 0) {
    if (strcmp(state->screen_page, "app") != 0 || !binding->value[0]) return false;
    if (!pet_p4_miniapp_dispatch_action(binding->value, ts_ms)) return false;
    copy_text(action_override, action_override_size, binding->value);
    state->last_update_ms = ts_ms;
    return true;
  }
  const char *miniapp_event = NULL;
  if (strcmp(binding->action, "miniapp_screen_tap") == 0) {
    miniapp_event = "screen.region.tap";
  } else if (strcmp(binding->action, "miniapp_screen_long_press") == 0) {
    miniapp_event = "screen.region.long_press";
  }
  if (miniapp_event) {
    if (strcmp(state->screen_page, "app") != 0) return false;
    if (!pet_p4_miniapp_dispatch_input(
          miniapp_event,
          ts_ms,
          action_override,
          action_override_size
        )) return false;
    state->last_update_ms = ts_ms;
    return true;
  }
  return false;
}

static bool component_system_action(const char *action) {
  return action
    && (
      strcmp(action, "page_toggle") == 0
      || strcmp(action, "session_next") == 0
      || strcmp(action, "session_previous") == 0
      || strcmp(action, "page_enter") == 0
      || strcmp(action, "page_back") == 0
      || strcmp(action, "page_main") == 0
      || strcmp(action, "page_app") == 0
      || strcmp(action, "component_center") == 0
    );
}

static const pet_p4_input_binding_t *active_global_exit_binding(
  const pet_p4_runtime_state_t *state,
  const char *event_name
) {
  if (!state || strcmp(state->screen_page, "app") != 0) return NULL;
  const pet_p4_input_binding_t *binding = active_binding(event_name);
  return binding && strcmp(binding->action, "page_back") == 0 ? binding : NULL;
}

static bool dispatch_component_binding_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture
) {
  pet_p4_input_binding_t binding = {0};
  char component_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  char action_override[PET_P4_MINIAPP_ACTION_MAX] = {0};
  bool handled_locally;
  if (!state || strcmp(state->screen_page, "app") != 0) return false;
  if (!pet_p4_miniapp_resolve_input(
        event_name,
        component_action,
        sizeof(component_action)
      )) return false;

  // Component packages own gameplay actions only. Older packages may still
  // contain page_main/page_back records; ignore those records so the current
  // persisted global exit mapping remains the single navigation authority.
  if (component_system_action(component_action)) return false;

  copy_text(binding.event, sizeof(binding.event), event_name);
  copy_text(binding.action, sizeof(binding.action), "miniapp_action");
  copy_text(binding.value, sizeof(binding.value), component_action);
  handled_locally = apply_local_action(
    state,
    &binding,
    event->ts_ms,
    action_override,
    sizeof(action_override)
  );
  send_input_event(
    state,
    send_line,
    ctx,
    event,
    event_name,
    gesture,
    &binding,
    action_override,
    handled_locally
  );
  return true;
}

static void send_ignored_component_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture
) {
  pet_p4_input_binding_t ignored_binding = {0};
  copy_text(ignored_binding.event, sizeof(ignored_binding.event), event_name);
  copy_text(ignored_binding.action, sizeof(ignored_binding.action), "disabled");
  send_input_event(
    state,
    send_line,
    ctx,
    event,
    event_name,
    gesture,
    &ignored_binding,
    "",
    true
  );
}

static void dispatch_binding_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event,
  const char *event_name,
  const char *gesture
) {
  const pet_p4_input_binding_t *binding = active_binding(event_name);
  const pet_p4_input_binding_t *global_exit = active_global_exit_binding(state, event_name);
  if (global_exit) {
    char miniapp_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
    const bool handled_locally = apply_local_action(
      state,
      global_exit,
      event->ts_ms,
      miniapp_action,
      sizeof(miniapp_action)
    );
    send_input_event(
      state,
      send_line,
      ctx,
      event,
      event_name,
      gesture,
      global_exit,
      miniapp_action,
      handled_locally
    );
    return;
  }
  if (dispatch_component_binding_event(
        state,
        send_line,
        ctx,
        event,
        event_name,
        gesture
      )) return;
  // While a component is open, its package owns optional gameplay buttons.
  // An unmapped system-navigation gesture must not fall through to the global
  // action (for example SW2 -> component_center) and look like a second exit.
  // The configured page_back gesture was already handled above and remains
  // the only system-navigation escape from a running component.
  if (state
      && strcmp(state->screen_page, "app") == 0
      && binding
      && component_system_action(binding->action)) {
    send_ignored_component_event(
      state,
      send_line,
      ctx,
      event,
      event_name,
      gesture
    );
    return;
  }
  char miniapp_action[PET_P4_MINIAPP_ACTION_MAX] = {0};
  bool handled_locally = apply_local_action(
    state,
    binding,
    event->ts_ms,
    miniapp_action,
    sizeof(miniapp_action)
  );
  if (binding && strcmp(binding->action, "voice_ptt") == 0) {
    if (strcmp(gesture, "hold_end") == 0) {
      if (state) state->session_voice_active = false;
      handled_locally = pet_p4_audio_capture_stop() == ESP_OK;
    } else if (!handled_locally && strcmp(gesture, "hold_start") == 0) {
      if (state) state->session_voice_active = true;
      handled_locally = pet_p4_audio_capture_start(state && state->session_queue_count == 0) == ESP_OK;
    }
  }
  send_input_event(
    state,
    send_line,
    ctx,
    event,
    event_name,
    gesture,
    binding,
    miniapp_action,
    handled_locally
  );
}

static void process_button_event(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const pet_p4_input_event_t *event
) {
  char event_name[PET_P4_INPUT_EVENT_MAX];
  const char *prefix = button_event_prefix(event->control);
  if (!prefix[0]) return;

  if (event->gesture == PET_P4_INPUT_GESTURE_HOLD_START
      || event->gesture == PET_P4_INPUT_GESTURE_HOLD_END) {
    char long_event_name[PET_P4_INPUT_EVENT_MAX];
    snprintf(long_event_name, sizeof(long_event_name), "%s.long_press", prefix);
    if (state
        && strcmp(state->screen_page, "app") == 0
        && pet_p4_miniapp_has_input(long_event_name)) {
      return;
    }
    snprintf(event_name, sizeof(event_name), "%s.hold", prefix);
    const pet_p4_input_binding_t *binding = active_binding(event_name);
    if (binding && strcmp(binding->action, "disabled") != 0) {
      dispatch_binding_event(
        state,
        send_line,
        ctx,
        event,
        event_name,
        event->gesture == PET_P4_INPUT_GESTURE_HOLD_START ? "hold_start" : "hold_end"
      );
    }
    return;
  }
  if (event->gesture == PET_P4_INPUT_GESTURE_SHORT_PRESS) {
    snprintf(event_name, sizeof(event_name), "%s.short_press", prefix);
    dispatch_binding_event(state, send_line, ctx, event, event_name, "short_press");
  } else if (event->gesture == PET_P4_INPUT_GESTURE_LONG_PRESS) {
    snprintf(event_name, sizeof(event_name), "%s.long_press", prefix);
    dispatch_binding_event(state, send_line, ctx, event, event_name, "long_press");
  }
}

void pet_p4_input_process(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  pet_p4_input_event_t event;
  if (!g_event_queue) return;
  while (xQueueReceive(g_event_queue, &event, 0) == pdTRUE) {
    if (event.control == PET_P4_INPUT_CONTROL_JOYSTICK
        && event.gesture == PET_P4_INPUT_GESTURE_DIRECTION) {
      const char *event_name = "";
      const char *gesture = "";
      switch ((pet_p4_joystick_direction_t) event.delta) {
        case PET_P4_JOYSTICK_UP:
          event_name = "joystick.up";
          gesture = "up";
          event.delta = 0;
          break;
        case PET_P4_JOYSTICK_DOWN:
          event_name = "joystick.down";
          gesture = "down";
          event.delta = 0;
          break;
        case PET_P4_JOYSTICK_LEFT:
          event_name = "knob.rotate_ccw";
          gesture = "left";
          event.delta = -1;
          break;
        case PET_P4_JOYSTICK_RIGHT:
          event_name = "knob.rotate_cw";
          gesture = "right";
          event.delta = 1;
          break;
        default:
          break;
      }
      if (!event_name[0]) continue;
      if (state
          && strcmp(state->screen_page, "app") == 0
          && !pet_p4_miniapp_has_input(event_name)
          && !active_global_exit_binding(state, event_name)) {
        send_ignored_component_event(
          state,
          send_line,
          ctx,
          &event,
          event_name,
          gesture
        );
        continue;
      }
      dispatch_binding_event(state, send_line, ctx, &event, event_name, gesture);
    } else if (event.control == PET_P4_INPUT_CONTROL_ENCODER
        && event.gesture == PET_P4_INPUT_GESTURE_ROTATE) {
      const char *event_name = event.delta > 0 ? "knob.rotate_cw" : "knob.rotate_ccw";
      if (state && strcmp(state->screen_page, "components") == 0) {
        static const pet_p4_input_binding_t select_binding = {
          .event = "knob.rotate_cw",
          .action = "component_select",
          .value = "",
        };
        (void) pet_p4_miniapp_catalog_move(event.delta);
        state->last_update_ms = event.ts_ms;
        send_input_event(
          state,
          send_line,
          ctx,
          &event,
          event_name,
          event.delta > 0 ? "rotate_cw" : "rotate_ccw",
          &select_binding,
          "component_select",
          true
        );
        continue;
      }
      if (state
          && strcmp(state->screen_page, "app") == 0
          && !pet_p4_miniapp_has_input(event_name)
          && !active_global_exit_binding(state, event_name)) {
        send_ignored_component_event(
          state,
          send_line,
          ctx,
          &event,
          event_name,
          event.delta > 0 ? "rotate_cw" : "rotate_ccw"
        );
        continue;
      }
      dispatch_binding_event(
        state,
        send_line,
        ctx,
        &event,
        event_name,
        event.delta > 0 ? "rotate_cw" : "rotate_ccw"
      );
    } else {
      process_button_event(state, send_line, ctx, &event);
    }
  }
}

unsigned int pet_p4_input_dropped_events(void) {
  return atomic_load_explicit(&g_dropped_events, memory_order_relaxed);
}

void pet_p4_input_get_joystick_snapshot(pet_p4_input_joystick_snapshot_t *snapshot) {
  if (!snapshot) return;
  snapshot->ready = atomic_load_explicit(&g_joystick_ready, memory_order_acquire);
  snapshot->center_x = atomic_load_explicit(&g_joystick_center_x, memory_order_relaxed);
  snapshot->center_y = atomic_load_explicit(&g_joystick_center_y, memory_order_relaxed);
  snapshot->current_x = atomic_load_explicit(&g_joystick_current_x, memory_order_relaxed);
  snapshot->current_y = atomic_load_explicit(&g_joystick_current_y, memory_order_relaxed);
  snapshot->minimum_x = atomic_load_explicit(&g_joystick_minimum_x, memory_order_relaxed);
  snapshot->maximum_x = atomic_load_explicit(&g_joystick_maximum_x, memory_order_relaxed);
  snapshot->minimum_y = atomic_load_explicit(&g_joystick_minimum_y, memory_order_relaxed);
  snapshot->maximum_y = atomic_load_explicit(&g_joystick_maximum_y, memory_order_relaxed);
}
