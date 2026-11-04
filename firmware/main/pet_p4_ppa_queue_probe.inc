/* Included once after g_ppa_trace/mux. Link wrap is PRIVATE DEFAULT-OFF.
 * SDK 5.5.1 private opaque tags/signature are checked in native tests. No
 * descriptor dereference, transaction ownership, new task, allocation or IO. */
#if PET_P4_PPA_QUEUE_PROBE
#ifndef PET_PPA_QUEUE_HOST_TEST
#include "esp_idf_version.h"
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1)
#error "Review original PPA queue boundary on a different SDK/target"
#endif
#endif
typedef struct ppa_engine_t ppa_engine_t;
typedef struct ppa_trans_s ppa_trans_t;
static DRAM_ATTR TaskHandle_t g_ppa_queue_owner;
static DRAM_ATTR ppa_client_handle_t g_ppa_queue_client;
static DRAM_ATTR uint32_t g_ppa_queue_cookie;
static void ppa_queue_scope_begin(ppa_client_handle_t client,uint32_t cookie){
  int saved_errno=errno;
  TaskHandle_t owner=xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&g_ppa_trace_mux);
  /* Never adopt a nested scope. Existing physical work is still forwarded. */
  if(!g_ppa_queue_owner&&cookie){g_ppa_queue_owner=owner;g_ppa_queue_client=client;g_ppa_queue_cookie=cookie;}
  portEXIT_CRITICAL(&g_ppa_trace_mux);
  errno=saved_errno;
}
static void ppa_queue_scope_end(uint32_t cookie){
  int saved_errno=errno;
  TaskHandle_t owner=xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&g_ppa_trace_mux);
  if(g_ppa_queue_owner==owner&&g_ppa_queue_cookie==cookie){g_ppa_queue_owner=NULL;g_ppa_queue_client=NULL;g_ppa_queue_cookie=0;}
  portEXIT_CRITICAL(&g_ppa_trace_mux);
  errno=saved_errno;
}
esp_err_t __real_ppa_do_operation(ppa_client_handle_t client,ppa_engine_t *engine,ppa_trans_t *transaction,ppa_trans_mode_t mode);
esp_err_t __wrap_ppa_do_operation(ppa_client_handle_t client,ppa_engine_t *engine,ppa_trans_t *transaction,ppa_trans_mode_t mode){
  int saved_errno=errno;
  if(!xPortInIsrContext()){
    TaskHandle_t owner=xTaskGetCurrentTaskHandle();uint32_t cookie=0;
    portENTER_CRITICAL(&g_ppa_trace_mux);
    if(owner==g_ppa_queue_owner&&client==g_ppa_queue_client)cookie=g_ppa_queue_cookie;
    portEXIT_CRITICAL(&g_ppa_trace_mux);
    if(cookie){
      uint64_t now=(uint64_t)esp_timer_get_time();
      portENTER_CRITICAL(&g_ppa_trace_mux);
      if(owner==g_ppa_queue_owner&&cookie==g_ppa_queue_cookie&&client==g_ppa_queue_client)pet_ppa_trace_queue(&g_ppa_trace,cookie,now);
      portEXIT_CRITICAL(&g_ppa_trace_mux);
    }
  }
  errno=saved_errno;
  return __real_ppa_do_operation(client,engine,transaction,mode);
}
#endif
