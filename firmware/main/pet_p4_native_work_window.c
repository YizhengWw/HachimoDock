#include "pet_p4_native_work_window.h"
#if PET_P4_NATIVE_WORK_WINDOW
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stddef.h>
_Static_assert(ATOMIC_POINTER_LOCK_FREE==2,"native owner observation must not call a lock library");
static DRAM_ATTR StaticSemaphore_t g_native_work_storage;
static DRAM_ATTR SemaphoreHandle_t g_native_work_window;
static DRAM_ATTR _Atomic(TaskHandle_t) g_native_work_owner;
bool pet_native_work_window_init(void){
  if(g_native_work_window)return false; /* Boot-only, never reset an occupied native gate. */
  g_native_work_window=xSemaphoreCreateBinaryStatic(&g_native_work_storage);
  return g_native_work_window&&xSemaphoreGive(g_native_work_window)==pdTRUE;
}
static bool begin(TickType_t wait){
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  if(!g_native_work_window||!task||atomic_load_explicit(&g_native_work_owner,memory_order_acquire)==task)return false;
  if(xSemaphoreTake(g_native_work_window,wait)!=pdTRUE)return false;
  atomic_store_explicit(&g_native_work_owner,task,memory_order_release);return true;
}
bool pet_native_work_window_try_begin(void){return begin(0);}
bool pet_native_work_window_control_begin(void){return begin(portMAX_DELAY);}
bool pet_native_work_window_end(void){
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  if(!g_native_work_window||!task||atomic_load_explicit(&g_native_work_owner,memory_order_acquire)!=task)return false;
  atomic_store_explicit(&g_native_work_owner,NULL,memory_order_release);
  return xSemaphoreGive(g_native_work_window)==pdTRUE;
}
#endif
