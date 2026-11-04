#include "pet_p4_audio_volume_deadline.h"
#include <stdatomic.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "deadline owner must be lock free");
static _Atomic(TaskHandle_t) owner;
static portMUX_TYPE scope_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t deadline_us;

bool pet_audio_volume_deadline_begin(uint32_t budget_us) {
  if (!budget_us || budget_us > 20000) return false;
  int64_t now = esp_timer_get_time();
  if (now < 0 || now > INT64_MAX - budget_us) return false;
  TaskHandle_t task = xTaskGetCurrentTaskHandle();
  if (!task) return false;
  portENTER_CRITICAL(&scope_mux);
  bool ok = atomic_load_explicit(&owner, memory_order_relaxed) == NULL;
  if (ok) {
    deadline_us = now + budget_us;
    atomic_store_explicit(&owner, task, memory_order_release);
  }
  portEXIT_CRITICAL(&scope_mux);
  return ok;
}
bool pet_audio_volume_deadline_active(void) {
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  return task && atomic_load_explicit(&owner, memory_order_acquire) == task;
}
void pet_audio_volume_deadline_end(void) {
  if (pet_audio_volume_deadline_active())
    atomic_store_explicit(&owner, NULL, memory_order_release);
}
int pet_audio_volume_i2c_timeout_ms(int ordinary_ms) {
  if (!pet_audio_volume_deadline_active()) return ordinary_ms;
  int64_t remaining = deadline_us - esp_timer_get_time();
  if (remaining < 1000) return 0;
  int ms = (int)(remaining / 1000);
  return ms < ordinary_ms ? ms : ordinary_ms;
}
