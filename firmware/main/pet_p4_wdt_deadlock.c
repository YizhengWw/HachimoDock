/* [Input] ESP task watchdog init/track/feed for bounded-loop tasks.
 * [Output] watchdog panic when a tracked task stops feeding (wedged).
 * [Pos] ESP32-P4 main firmware component; see pet_p4_wdt_deadlock.h.
 * [Sync] If this file changes, update `firmware/.folder.md`. */
#include "pet_p4_wdt_deadlock.h"

#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "wdt-deadlock";
static bool s_initialized = false;

bool pet_p4_wdt_deadlock_init(int feed_timeout_sec) {
  if (feed_timeout_sec < 1) return false;
  esp_task_wdt_config_t cfg = {
    .timeout_ms = (uint32_t) feed_timeout_sec * 1000U,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_err_t err = esp_task_wdt_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "task WDT init failed: %s", esp_err_to_name(err));
    return false;
  }
  s_initialized = true;
  ESP_LOGI(TAG, "task WDT deadlock detector active, feed deadline %ds", feed_timeout_sec);
  return true;
}

bool pet_p4_wdt_deadlock_track(void) {
  if (!s_initialized) return false;
  esp_err_t err = esp_task_wdt_add(NULL);
  if (err == ESP_ERR_INVALID_STATE) return true;
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "task WDT add failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "tracking task %s", pcTaskGetName(NULL));
  return true;
}

void pet_p4_wdt_deadlock_feed(void) {
  if (s_initialized) (void) esp_task_wdt_reset();
}
