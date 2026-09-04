/*
 * [Input] diagnostics/recovery topics, runtime state, and persistent boot data.
 * [Output] bounded health/session-retention snapshots including stable queue
 *          IDs and joystick ADC travel, plus transfer-safe delayed reboot actions.
 * [Pos] ESP32-P4 diagnostics and recovery node in firmware/main.
 * [Sync] If this file changes, update firmware/protocol.md and .folder.md.
 */

#include "pet_p4_diagnostics.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "pet_p4_audio.h"
#include "pet_p4_build_info.h"
#include "pet_p4_input.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_ota.h"
#include "pet_p4_touch.h"

#define PET_P4_DIAGNOSTICS_NVS_NAMESPACE "pet_diag"
#define PET_P4_DIAGNOSTICS_NVS_KEY "boot"
#define PET_P4_DIAGNOSTICS_VERSION 1
#define PET_P4_DIAGNOSTICS_REBOOT_DELAY_MS 350ULL

typedef struct {
  uint32_t version;
  uint32_t boot_count;
  uint32_t fault_reset_count;
  uint32_t last_reset_reason;
} pet_p4_boot_record_t;

static pet_p4_boot_record_t g_boot_record;
static unsigned long long g_reboot_at_ms;
static atomic_bool g_reboot_pending;
static SemaphoreHandle_t g_diagnostics_mutex;

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
  }
}

static bool reset_reason_is_fault(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC
    || reason == ESP_RST_INT_WDT
    || reason == ESP_RST_TASK_WDT
    || reason == ESP_RST_WDT
    || reason == ESP_RST_BROWNOUT
    || reason == ESP_RST_PWR_GLITCH
    || reason == ESP_RST_CPU_LOCKUP;
}

static const char *ota_state_name(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default: return "undefined";
  }
}

static void send_topic(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *topic,
  cJSON *payload
) {
  cJSON *root = cJSON_CreateObject();
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  cJSON_AddStringToObject(root, "topic", topic);
  cJSON_AddItemToObject(root, "payload", payload);
  char *line = cJSON_PrintUnformatted(root);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

static void add_request_id(cJSON *response, const cJSON *request) {
  const cJSON *request_id = request
    ? cJSON_GetObjectItemCaseSensitive(request, "requestId")
    : NULL;
  if (!response || !request_id) return;
  cJSON *copy = cJSON_Duplicate(request_id, true);
  if (copy) cJSON_AddItemToObject(response, "requestId", copy);
}

static void send_status(
  const cJSON *request,
  const pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  size_t storage_total = 0;
  size_t storage_used = 0;
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_app_desc_t *app = esp_app_get_description();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t image_state_err = running
    ? esp_ota_get_state_partition(running, &image_state)
    : ESP_ERR_NOT_FOUND;
  cJSON *payload = cJSON_CreateObject();
  cJSON *memory = cJSON_CreateObject();
  cJSON *storage = cJSON_CreateObject();
  cJSON *runtime = cJSON_CreateObject();
  if (!payload || !memory || !storage || !runtime) {
    cJSON_Delete(payload);
    cJSON_Delete(memory);
    cJSON_Delete(storage);
    cJSON_Delete(runtime);
    return;
  }

  (void) esp_spiffs_info("storage", &storage_total, &storage_used);
  add_request_id(payload, request);
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddNumberToObject(payload, "bootCount", g_boot_record.boot_count);
  cJSON_AddNumberToObject(payload, "faultResetCount", g_boot_record.fault_reset_count);
  cJSON_AddNumberToObject(payload, "lastResetReasonCode", g_boot_record.last_reset_reason);
  cJSON_AddStringToObject(
    payload,
    "lastResetReason",
    reset_reason_name((esp_reset_reason_t) g_boot_record.last_reset_reason)
  );
  cJSON_AddBoolToObject(
    payload,
    "lastResetWasFault",
    reset_reason_is_fault((esp_reset_reason_t) g_boot_record.last_reset_reason)
  );

  cJSON_AddNumberToObject(memory, "freeHeapBytes", esp_get_free_heap_size());
  cJSON_AddNumberToObject(memory, "minimumFreeHeapBytes", esp_get_minimum_free_heap_size());
  cJSON_AddNumberToObject(memory, "freePsramBytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cJSON_AddNumberToObject(
    memory,
    "minimumFreePsramBytes",
    heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)
  );
  cJSON_AddItemToObject(payload, "memory", memory);

  cJSON_AddNumberToObject(storage, "totalBytes", (double) storage_total);
  cJSON_AddNumberToObject(storage, "usedBytes", (double) storage_used);
  cJSON_AddNumberToObject(storage, "activeAppearanceSlot", pet_p4_asset_active_slot());
  cJSON_AddItemToObject(payload, "storage", storage);

  cJSON_AddNumberToObject(
    runtime,
    "uptimeMs",
    (double) ((unsigned long long) esp_timer_get_time() / 1000ULL)
  );
  cJSON_AddNumberToObject(runtime, "taskCount", uxTaskGetNumberOfTasks());
  cJSON_AddNumberToObject(runtime, "inputDroppedEvents", pet_p4_input_dropped_events());
  pet_p4_input_joystick_snapshot_t joystick_snapshot = {0};
  pet_p4_input_get_joystick_snapshot(&joystick_snapshot);
  cJSON *joystick = cJSON_CreateObject();
  if (joystick) {
    cJSON_AddBoolToObject(joystick, "ready", joystick_snapshot.ready);
    cJSON_AddNumberToObject(joystick, "centerX", joystick_snapshot.center_x);
    cJSON_AddNumberToObject(joystick, "centerY", joystick_snapshot.center_y);
    cJSON_AddNumberToObject(joystick, "currentX", joystick_snapshot.current_x);
    cJSON_AddNumberToObject(joystick, "currentY", joystick_snapshot.current_y);
    cJSON_AddNumberToObject(joystick, "minimumX", joystick_snapshot.minimum_x);
    cJSON_AddNumberToObject(joystick, "maximumX", joystick_snapshot.maximum_x);
    cJSON_AddNumberToObject(joystick, "minimumY", joystick_snapshot.minimum_y);
    cJSON_AddNumberToObject(joystick, "maximumY", joystick_snapshot.maximum_y);
    cJSON_AddItemToObject(runtime, "joystick", joystick);
  }
  cJSON_AddBoolToObject(runtime, "touchReady", pet_p4_touch_ready());
  cJSON_AddNumberToObject(runtime, "touchDroppedEvents", pet_p4_touch_dropped_events());
  cJSON_AddStringToObject(runtime, "screenPage", state ? state->screen_page : "");
  cJSON_AddStringToObject(runtime, "agentState", state ? state->current_state : "");
  unsigned int retained_session_count = 0;
  unsigned long long now_ms = (unsigned long long) esp_timer_get_time() / 1000ULL;
  if (state) {
    for (unsigned int i = 0; i < state->session_queue_count; i += 1) {
      if (state->session_queue[i].terminal_until_ms > now_ms) retained_session_count += 1;
    }
  }
  cJSON_AddNumberToObject(
    runtime,
    "sessionQueueCount",
    state ? state->session_queue_count : 0
  );
  cJSON_AddNumberToObject(runtime, "retainedSessionCount", retained_session_count);
  cJSON_AddStringToObject(runtime, "currentSessionId", state ? state->current_session_id : "");
  cJSON *session_queue_ids = cJSON_CreateArray();
  if (session_queue_ids) {
    if (state) {
      for (unsigned int i = 0; i < state->session_queue_count; i += 1) {
        cJSON *session_id = cJSON_CreateString(state->session_queue[i].id);
        if (session_id) cJSON_AddItemToArray(session_queue_ids, session_id);
      }
    }
    cJSON_AddItemToObject(runtime, "sessionQueueIds", session_queue_ids);
  }
  cJSON_AddBoolToObject(runtime, "audioReady", pet_p4_audio_ready());
  cJSON_AddBoolToObject(runtime, "audioPlaybackReady", pet_p4_audio_playback_ready());
  cJSON_AddBoolToObject(runtime, "audioPlaybackActive", pet_p4_audio_playback_active());
  cJSON_AddBoolToObject(runtime, "miniappActive", pet_p4_miniapp_active());
  cJSON_AddStringToObject(runtime, "firmware", app ? app->version : "");
  cJSON_AddStringToObject(runtime, "buildId", PET_P4_BUILD_ID);
  cJSON_AddStringToObject(runtime, "gitSha", PET_P4_BUILD_GIT_SHA);
  cJSON_AddBoolToObject(runtime, "buildDirty", PET_P4_BUILD_DIRTY != 0);
  cJSON_AddNumberToObject(runtime, "protocolSchema", PET_P4_PROTOCOL_SCHEMA);
  cJSON_AddStringToObject(runtime, "projectName", app ? app->project_name : "");
  cJSON_AddStringToObject(runtime, "idfVersion", esp_get_idf_version());
  cJSON_AddStringToObject(runtime, "runningPartition", running ? running->label : "");
  cJSON_AddStringToObject(
    runtime,
    "imageState",
    image_state_err == ESP_OK ? ota_state_name(image_state) : "unavailable"
  );
  cJSON_AddBoolToObject(runtime, "rebootPending", g_reboot_at_ms != 0);
  cJSON_AddItemToObject(payload, "runtime", runtime);

  send_topic(send_line, ctx, "diagnostics/status", payload);
}

static void send_action(
  const cJSON *request,
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *action,
  bool ok,
  const char *message
) {
  cJSON *payload = cJSON_CreateObject();
  if (!payload) return;
  add_request_id(payload, request);
  cJSON_AddStringToObject(payload, "action", action);
  cJSON_AddBoolToObject(payload, "ok", ok);
  cJSON_AddStringToObject(payload, "message", message ? message : "");
  cJSON_AddBoolToObject(payload, "preservedAppearanceAssets", true);
  send_topic(send_line, ctx, "diagnostics/action", payload);
}

esp_err_t pet_p4_diagnostics_init(void) {
  nvs_handle_t handle;
  size_t size = sizeof(g_boot_record);
  if (!g_diagnostics_mutex) g_diagnostics_mutex = xSemaphoreCreateMutex();
  if (!g_diagnostics_mutex) return ESP_ERR_NO_MEM;
  memset(&g_boot_record, 0, sizeof(g_boot_record));
  g_boot_record.version = PET_P4_DIAGNOSTICS_VERSION;

  esp_err_t err = nvs_open(PET_P4_DIAGNOSTICS_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  pet_p4_boot_record_t stored;
  memset(&stored, 0, sizeof(stored));
  err = nvs_get_blob(handle, PET_P4_DIAGNOSTICS_NVS_KEY, &stored, &size);
  if (err == ESP_OK && size == sizeof(stored) && stored.version == PET_P4_DIAGNOSTICS_VERSION) {
    g_boot_record = stored;
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    memset(&g_boot_record, 0, sizeof(g_boot_record));
    g_boot_record.version = PET_P4_DIAGNOSTICS_VERSION;
    (void) nvs_erase_key(handle, PET_P4_DIAGNOSTICS_NVS_KEY);
  }

  esp_reset_reason_t reason = esp_reset_reason();
  g_boot_record.boot_count += 1;
  if (reset_reason_is_fault(reason)) g_boot_record.fault_reset_count += 1;
  g_boot_record.last_reset_reason = (uint32_t) reason;
  err = nvs_set_blob(handle, PET_P4_DIAGNOSTICS_NVS_KEY, &g_boot_record, sizeof(g_boot_record));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

bool pet_p4_diagnostics_handle_topic(
  const char *topic,
  const cJSON *payload,
  const pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (!topic || !payload) return false;
  bool handled = true;
  if (!g_diagnostics_mutex
      || xSemaphoreTake(g_diagnostics_mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (strcmp(topic, "diagnostics/query") == 0) {
    send_status(payload, state, send_line, ctx);
  } else if (strcmp(topic, "system/reset-inputs") == 0) {
    esp_err_t err = pet_p4_input_reset_config();
    send_action(
      payload,
      send_line,
      ctx,
      "reset-inputs",
      err == ESP_OK,
      err == ESP_OK ? "input bindings restored to safe defaults" : esp_err_to_name(err)
    );
  } else if (strcmp(topic, "system/reboot") == 0) {
    if (pet_p4_ota_active() || (state && state->asset_transfer_active)) {
      send_action(
        payload,
        send_line,
        ctx,
        "reboot",
        false,
        "device transfer is active; reboot was not scheduled"
      );
    } else {
      atomic_store_explicit(&g_reboot_pending, true, memory_order_release);
      send_action(payload, send_line, ctx, "reboot", true, "device reboot scheduled");
      g_reboot_at_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL)
        + PET_P4_DIAGNOSTICS_REBOOT_DELAY_MS;
    }
  } else {
    handled = false;
  }
  xSemaphoreGive(g_diagnostics_mutex);
  return handled;
}

void pet_p4_diagnostics_process(
  unsigned long long now_ms,
  const pet_p4_runtime_state_t *state
) {
  bool restart = false;
  if (!g_diagnostics_mutex
      || xSemaphoreTake(g_diagnostics_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  if (g_reboot_at_ms && now_ms >= g_reboot_at_ms) {
    g_reboot_at_ms = 0;
    restart = true;
  }
  xSemaphoreGive(g_diagnostics_mutex);
  if (!restart) return;
  if (pet_p4_ota_active() || (state && state->asset_transfer_active)) {
    if (xSemaphoreTake(g_diagnostics_mutex, portMAX_DELAY) == pdTRUE) {
      g_reboot_at_ms = now_ms + PET_P4_DIAGNOSTICS_REBOOT_DELAY_MS;
      xSemaphoreGive(g_diagnostics_mutex);
    }
    return;
  }
  esp_restart();
}

bool pet_p4_diagnostics_reboot_pending(void) {
  return atomic_load_explicit(&g_reboot_pending, memory_order_acquire);
}
