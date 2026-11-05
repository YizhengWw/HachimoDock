#include "pet_p4_audio_patch.h"
/*
 * [Input] UART/native USB protocol traffic, display/input state, and assets.
 * [Output] initialized P4 runtime with serialized state access, origin-aware
 *          protocol replies, non-destructive storage mounting, and independent
 *          render/RX health samples, plus post-OTA synchronization of the
 *          built-in component bundle carried inside the application image,
 *          idle appearance-transfer recovery, a lightweight live transfer
 *          screen, and a backlight-gated first complete frame.
 * [Pos] ESP32-P4 firmware entry point and task coordinator.
 * [Sync] If this file changes, update firmware/.folder.md.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_p4_task_policy.h"
#include "pet_p4_reset_phase.h"
#include "pet_p4_native_panic.h"
#include "pet_p4_reset_cost.h"
#include "pet_p4_boot_trace.h"
#include "pet_p4_boot_dma.h"
#include "pet_p4_app_store.h"

#include "pet_p4_storage_policy.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"
#include "pet_p4_audio_volume.h"

#include "pet_p4_lcd.h"
#include "pet_p4_audio.h"
#include "pet_p4_system_cues.h"
#include "pet_p4_audio_frontend.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_input.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_media.h"
#include "pet_p4_ota.h"
#include "pet_p4_protocol.h"
#include "pet_p4_bubble_pixels.h"
#include "pet_p4_transport_config.h"
#include "pet_p4_control_integrity.h"
#include "pet_p4_control_session.h"
#include "pet_p4_native_work_window.h"
#include "pet_p4_transport_audit.h"
#include "pet_p4_uart_pacing.h"
#include "pet_p4_uart_control_staging.h"
#include "pet_p4_sched_trace_rpc.h"
#include "pet_p4_storage_io_trace_rpc.h"
#include "pet_p4_renderer.h"
#include "pet_p4_worker_locked.h"
#include "pet_p4_font_bitmap.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_fs_activity.h"
#include "pet_p4_program_probe.h"
#include "pet_p4_touch.h"
#include "pet_p4_conversation.h"
#include "pet_p4_usb_native.h"
#include "pet_p4_view.h"

#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
void pet_app_store_housekeeping(void){pet_p4_diagnostics_storage_refresh();}
#endif

static const char *TAG = "pet-p4";

#define PET_P4_UART_NUM UART_NUM_0
#define PET_P4_UART_RX_BUFFER_BYTES (128 * 1024)
#define PET_P4_UART_READ_CHUNK_BYTES 2048
#define PET_P4_TRANSPORT_RX_TASK_STACK_BYTES 8192
#define PET_P4_PROTOCOL_TASK_STACK_BYTES 12288
#define PET_P4_PROTOCOL_QUEUE_DEPTH 24
#define PET_P4_MAIN_RENDER_INTERVAL_MS 50ULL
#define PET_P4_MAIN_LOOP_DELAY_MS 1
#define PET_P4_MAIN_RX_HEALTH_TIMEOUT_MS 1000ULL

#ifndef PET_P4_STATE_EXTERNAL
#define PET_P4_STATE_EXTERNAL 0
#endif
#if PET_P4_STATE_EXTERNAL
/* Stable task-only model: manifest, captions and session declarations. Never
 * submitted to DMA, retained by an ISR or accessed by the panic recorder.
 * Preserve the exact ABI, address lifetime and existing state mutex. Startup
 * external BSS is ready before app_main, just like the existing render model.
 * Keep scarce internal contiguous memory for drivers/codec tasks. This private
 * placement changes neither the audio allocations nor the resource quotas. */
static EXT_RAM_BSS_ATTR pet_p4_runtime_state_t g_state;
#else
static pet_p4_runtime_state_t g_state;
#endif
#define PET_P4_PROTOCOL_LINE_BUFFER_BYTES 32768
// RX tasks parse/copy these buffers in task context; they are not DMA buffers.
static char *g_line_buffer;
static size_t g_line_used;
static char *g_uart_line_buffer;
static size_t g_uart_line_used;
static SemaphoreHandle_t g_render_mutex;
static SemaphoreHandle_t g_transport_mutex;
static bool g_transport_ready;
// Shared under g_transport_mutex, not on the audio/render task stacks.
static char g_transport_log_buffer[1024];
static SemaphoreHandle_t g_state_mutex;
static QueueHandle_t g_protocol_queue;
static pet_p4_runtime_state_t *g_render_state;
static portMUX_TYPE g_rx_health_mutex = portMUX_INITIALIZER_UNLOCKED;
static TickType_t g_usb_rx_last_tick;
static TickType_t g_uart_rx_last_tick;
static bool g_usb_rx_seen;
static bool g_uart_rx_seen;
static bool g_usb_protocol_seen;

typedef enum {
  PET_P4_TRANSPORT_BROADCAST = 0,
  PET_P4_TRANSPORT_UART,
  PET_P4_TRANSPORT_USB_SERIAL_JTAG,
} pet_p4_transport_route_t;

typedef struct {
  char *line;
  pet_p4_send_line_fn send_line;
  void *reply_ctx;
  /* Trusted internal work only. Wire requests cannot supply this token. */
  uint64_t activation_token;
  uint64_t cursor_token;
#if PET_P4_HELLO_ASYNC
  bool unsolicited_hello;
#endif
} pet_p4_protocol_request_t;

static const pet_p4_transport_route_t g_uart_route = PET_P4_TRANSPORT_UART;
static const pet_p4_transport_route_t g_usb_serial_jtag_route = PET_P4_TRANSPORT_USB_SERIAL_JTAG;
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
static bool usb_protocol_seen(void);
#endif

#if PET_P4_HELLO_ASYNC
/* Initialized before the worker is created, immutable for this boot. The
 * bounded native queue carries only a work tag, never a model pointer/clone.
 * Pending/in-flight announcements coalesce. No heap, wait or TX in the mux. */
static char g_hello_board_id[PET_P4_DEVICE_ID_MAX];
static bool g_hello_pending;
static void transport_send_line(const char *line,void *ctx);
static bool enqueue_unsolicited_hello(void) {
  if(!g_protocol_queue)return false;
  portENTER_CRITICAL(&g_rx_health_mutex);
  bool pending=g_hello_pending;
  if(!pending)g_hello_pending=true;
  portEXIT_CRITICAL(&g_rx_health_mutex);
  if(pending)return true;
  pet_p4_protocol_request_t request={.unsolicited_hello=true};
  if(xQueueSend(g_protocol_queue,&request,0)==pdTRUE)return true;
  portENTER_CRITICAL(&g_rx_health_mutex);g_hello_pending=false;portEXIT_CRITICAL(&g_rx_health_mutex);
  return false;
}
static void run_unsolicited_hello(void) {
  /* Only the existing protocol worker calls this. The original full payload
   * and route remain; its queue/drain waits cannot retain the state mutex. */
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
  /* Each physical route gets a payload consistent with THAT route's framing.
   * Native USB has no guarded writer and must not claim serial integrity. */
  pet_p4_send_hello_for_board(g_hello_board_id,transport_send_line,(void*)&g_uart_route);
  if(usb_protocol_seen())pet_p4_send_hello_for_board(g_hello_board_id,transport_send_line,(void*)&g_usb_serial_jtag_route);
#else
  pet_p4_send_hello_for_board(g_hello_board_id,transport_send_line,NULL);
#endif
  portENTER_CRITICAL(&g_rx_health_mutex);g_hello_pending=false;portEXIT_CRITICAL(&g_rx_health_mutex);
}
#endif

/* Only an explicitly entered protocol command may lend this lock to storage.
 * Boot/fallback calls never get permission merely by configuring callbacks. */
static TaskHandle_t g_protocol_task;
#if PET_P4_GAP_PEAK_HOLDER
/* Protocol worker locked spans; the topic hash is published while the worker
 * holds the state mutex so the main loop can read it before its own wait. */
static pet_worker_locked_peak_t g_worker_locked;
static int64_t g_worker_locked_started_us;
static volatile uint32_t g_worker_locked_topic_hash;
const pet_worker_locked_peak_t *pet_p4_worker_locked_peak(void){return &g_worker_locked;}
static void worker_locked_begin(void){
  if(xTaskGetCurrentTaskHandle()!=g_protocol_task)return;
  g_worker_locked_started_us=esp_timer_get_time();
}
static void worker_locked_end(void){
  if(xTaskGetCurrentTaskHandle()!=g_protocol_task)return;
  int64_t now=esp_timer_get_time();
  pet_worker_locked_record(&g_worker_locked,(uint32_t)(now-g_worker_locked_started_us),g_worker_locked_topic_hash,(uint64_t)now);
}
#endif
static bool g_protocol_storage_background;
static bool g_protocol_storage_command_entered;
static _Atomic bool g_storage_query_window;
static uintptr_t storage_command_owner(void *context) {
  (void)context;
  return (uintptr_t)xTaskGetCurrentTaskHandle();
}
static void storage_command_unlock(void *context) {
  (void)context;
  configASSERT(xSemaphoreGetMutexHolder(g_state_mutex) == xTaskGetCurrentTaskHandle());
  /* Invalidate asynchronous readers before the first filesystem mutation,
   * not one main-loop iteration after the state lock has been released. */
  pet_p4_renderer_set_asset_preparation_suspended(true);
#if PET_P4_GAP_PEAK_HOLDER
  worker_locked_end();
#endif
  atomic_store_explicit(&g_storage_query_window,true,memory_order_release);
  BaseType_t released = xSemaphoreGive(g_state_mutex);
  configASSERT(released == pdTRUE);
  (void)released;
  pet_storage_io_protocol_window(true);
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_STORAGE_UNLOCKED);
#endif
  /* No state/runtime/renderer mutex is held here. The worker remains the
   * physical storage owner, but slow scans/writes must not outrank rendering.
   * Never save an inherited effective priority and promote it as a new base. */
  if (xTaskGetCurrentTaskHandle() == g_protocol_task) {
    configASSERT(!g_protocol_storage_background);
    g_protocol_storage_background = true;
    vTaskPrioritySet(g_protocol_task, PET_P4_STORAGE_IO_TASK_PRIORITY);
  }
}
static void storage_command_lock(void *context) {
  (void)context;
  atomic_store_explicit(&g_storage_query_window,false,memory_order_release);
  pet_storage_io_protocol_window(false);
  /* Restore the known protocol base BEFORE waiting for the state mutex; the
   * normal lock-held publish/ACK path and audio/control priorities are intact. */
  if (xTaskGetCurrentTaskHandle() == g_protocol_task && g_protocol_storage_background) {
    g_protocol_storage_background = false;
    vTaskPrioritySet(g_protocol_task, PET_P4_PROTOCOL_TASK_PRIORITY);
  }
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_STATE_WAIT);
#endif
  BaseType_t acquired = xSemaphoreTake(g_state_mutex, portMAX_DELAY);
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_STATE_HELD);
#endif
  configASSERT(acquired == pdTRUE);
  (void)acquired;
#if PET_P4_GAP_PEAK_HOLDER
  worker_locked_begin();
#endif
}

static bool asset_query_io_begin(void *context) {
  if (xTaskGetCurrentTaskHandle()!=g_protocol_task
      || !g_protocol_storage_command_entered
      || xSemaphoreGetMutexHolder(g_state_mutex)!=g_protocol_task) return false;
  storage_command_unlock(context);
  return true;
}
static void asset_query_io_end(void *context) { storage_command_lock(context); }
static bool raw_asset_rx_io_begin(void *context) {
  TaskHandle_t task=xTaskGetCurrentTaskHandle();
  if(!context||task!=(TaskHandle_t)context||task==g_protocol_task
      ||xSemaphoreGetMutexHolder(g_state_mutex)!=task)return false;
  /* RX is not the protocol worker. Do not borrow its global trace/query/IO
   * window flags or priority bookkeeping; a concurrent busy reply owns those.
   * The accepted transfer and phase operation keep asset preparation suspended. */
  pet_p4_renderer_set_asset_preparation_suspended(true);
  return xSemaphoreGive(g_state_mutex)==pdTRUE;
}
static void raw_asset_rx_io_end(void *context){
  configASSERT(xTaskGetCurrentTaskHandle()==(TaskHandle_t)context);
  BaseType_t acquired=xSemaphoreTake(g_state_mutex,portMAX_DELAY);
  configASSERT(acquired==pdTRUE);(void)acquired;
}
static bool raw_asset_rx_current(void *context){return xTaskGetCurrentTaskHandle()==(TaskHandle_t)context;}
static void asset_idle_worker(void){
  xSemaphoreTake(g_state_mutex,portMAX_DELAY);
#if PET_P4_GAP_PEAK_HOLDER
  g_worker_locked_topic_hash=pet_topic_hash("asset/idle-cleanup",sizeof("asset/idle-cleanup")-1);
  worker_locked_begin();
#endif
  bool entered=pet_p4_miniapp_storage_lane_enter();g_protocol_storage_command_entered=entered;
  if(entered){
    pet_p4_asset_transfer_cleanup_worker(&g_state);
    bool left=pet_p4_miniapp_storage_lane_leave();configASSERT(left);(void)left;
  }
  g_protocol_storage_command_entered=false;
#if PET_P4_GAP_PEAK_HOLDER
  worker_locked_end();g_worker_locked_topic_hash=0;
#endif
  xSemaphoreGive(g_state_mutex);
}
static bool asset_query_worker_pending(void *context) {
  (void)context;
  return xTaskGetCurrentTaskHandle()!=g_protocol_task||!g_protocol_queue
    ||uxQueueMessagesWaiting(g_protocol_queue)!=0;
}


static void mark_usb_protocol_seen(void);
static bool usb_protocol_seen(void);

#define PET_P4_SCREENSHOT_W 320
#define PET_P4_SCREENSHOT_H 240
#define PET_P4_SCREENSHOT_RAW_CHUNK 3072

static esp_err_t build_board_device_id(char *out, size_t out_size) {
  uint8_t mac[6] = {0};
  if (!out || out_size < 16) return ESP_ERR_INVALID_ARG;
  esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
  if (err != ESP_OK) {
    snprintf(out, out_size, "p4-unknown");
    return err;
  }
  snprintf(
    out,
    out_size,
    "p4-%02x%02x%02x%02x%02x%02x",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return ESP_OK;
}

static void mark_rx_healthy(bool usb) {
  TickType_t now = xTaskGetTickCount();
  portENTER_CRITICAL(&g_rx_health_mutex);
  if (usb) {
    g_usb_rx_last_tick = now;
    g_usb_rx_seen = true;
  } else {
    g_uart_rx_last_tick = now;
    g_uart_rx_seen = true;
  }
  portEXIT_CRITICAL(&g_rx_health_mutex);
}

static bool rx_tasks_healthy(void) {
  TickType_t usb_last;
  TickType_t uart_last;
  bool usb_seen;
  bool uart_seen;
  TickType_t now = xTaskGetTickCount();
  TickType_t timeout = pdMS_TO_TICKS(PET_P4_MAIN_RX_HEALTH_TIMEOUT_MS);
  portENTER_CRITICAL(&g_rx_health_mutex);
  usb_last = g_usb_rx_last_tick;
  uart_last = g_uart_rx_last_tick;
  usb_seen = g_usb_rx_seen;
  uart_seen = g_uart_rx_seen;
  portEXIT_CRITICAL(&g_rx_health_mutex);
  return usb_seen && uart_seen
    && now - usb_last <= timeout
    && now - uart_last <= timeout;
}

/* All audit state is serialized by the existing transport mutex. Disabled at
 * boot; no frame hash, extra driver call or wait on the ordinary default path. */
static pet_transport_audit_t g_transport_audit;
/* Only an explicit audit read arms this separate fixed result observation. */
static pet_transport_audit_t g_transport_reply_audit;
static bool g_transport_reply_observed;
#if PET_P4_SERIAL_CONTROL_INTEGRITY && PET_P4_UART_PACING_ACTIVE
/* Existing transport mutex serializes prefix debt with its first body chunk.
 * Do not turn a 21-byte queued prefix + 256-byte body into a 277-byte burst. */
static size_t g_uart_control_prefix_queued;
_Static_assert(PET_CONTROL_PREFIX_BYTES<PET_P4_UART_TX_BURST_BYTES,"prefix must fit the control pacing window");
#endif

static void usb_queue_all(const char *data, size_t len, pet_transport_write_observation_t *observation) {
  size_t written = 0;
  if (observation) *observation = (pet_transport_write_observation_t){
    .last_return = PET_TRANSPORT_AUDIT_NOT_OBSERVED, .drain = PET_TRANSPORT_AUDIT_NOT_OBSERVED};
  while (written < len) {
    /* SDK BYTEBUF sends are all-or-nothing, unlike UART's internally
     * segmented writer. Never offer an item larger than our TX ring. */
    size_t count=len-written;if(count>4096U)count=4096U;
    int n = usb_serial_jtag_write_bytes(data + written, count, pdMS_TO_TICKS(20));
    if (observation) observation->last_return = n;
    if (n <= 0) break;
    if ((size_t)n > count) {
      if (observation) observation->invalid_return = true;
      break;
    }
    written += (size_t) n;
  }
  if (observation) observation->accepted = written;
}

static void usb_write_all(const char *data, size_t len, pet_transport_write_observation_t *observation) {
  usb_queue_all(data, len, observation);
  int result = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
  if (observation) observation->drain = result;
}

static void uart_queue_all(const char *data, size_t len, pet_transport_write_observation_t *observation) {
  size_t written = 0;
  if (observation) *observation = (pet_transport_write_observation_t){
    .last_return = PET_TRANSPORT_AUDIT_NOT_OBSERVED, .drain = PET_TRANSPORT_AUDIT_NOT_OBSERVED};
  while (written < len) {
    int n = uart_write_bytes(PET_P4_UART_NUM, data + written, len - written);
    if (observation) observation->last_return = n;
    if (n <= 0) break;
    if ((size_t)n > len - written) {
      if (observation) observation->invalid_return = true;
      break;
    }
    written += (size_t) n;
  }
  if (observation) observation->accepted = written;
}

/* Audio only: isolate queued UART copies from the JSON/PSRAM source without
 * introducing control pacing, drains, delays or frame retries. The existing
 * transport mutex owns this buffer through each synchronous driver copy. */
#define PET_UART_AUDIO_STAGE_BYTES 256U
#define PET_UART_AUDIO_STAGE_ALIGNMENT 64U
static DRAM_ATTR unsigned char g_uart_audio_stage[PET_UART_AUDIO_STAGE_BYTES]
  __attribute__((aligned(PET_UART_AUDIO_STAGE_ALIGNMENT)));
_Static_assert(__alignof__(g_uart_audio_stage)>=PET_UART_AUDIO_STAGE_ALIGNMENT,
  "audio UART staging must be 64-byte aligned");
static void uart_audio_queue_all(const char *data,size_t len) {
  size_t written=0;
  while(written<len){
    size_t count=len-written;
    if(count>PET_UART_AUDIO_STAGE_BYTES)count=PET_UART_AUDIO_STAGE_BYTES;
    volatile unsigned char *dst=g_uart_audio_stage;
    const volatile unsigned char *src=(const volatile unsigned char *)(data+written);
    for(size_t i=0;i<count;i++)dst[i]=src[i];
    /* Re-copy ONLY the unaccepted suffix, including after an odd partial return. */
    int n=uart_write_bytes(PET_P4_UART_NUM,g_uart_audio_stage,count);
    if(n<=0||(size_t)n>count)break;
    written+=(size_t)n;
  }
}

#if PET_P4_UART_PACING_ACTIVE
/* CONTROL only; the existing transport mutex owns this one internal buffer.
 * No asynchronous caller may retain it after uart_write_bytes returns. */
static DRAM_ATTR unsigned char g_uart_control_stage[PET_UART_CONTROL_STAGE_BYTES]
  __attribute__((aligned(PET_UART_CONTROL_STAGE_ALIGNMENT)));
_Static_assert(PET_P4_UART_TX_BURST_BYTES <= sizeof(g_uart_control_stage),
  "control burst exceeds staging bound");
_Static_assert(__alignof__(g_uart_control_stage) >= PET_UART_CONTROL_STAGE_ALIGNMENT,
  "control staging must be 64-byte aligned");

static void uart_control_queue_all(const char *data, size_t len,
    pet_transport_write_observation_t *observation) {
  size_t written = 0;
  if (observation) *observation = (pet_transport_write_observation_t){
    .last_return = PET_TRANSPORT_AUDIT_NOT_OBSERVED, .drain = PET_TRANSPORT_AUDIT_NOT_OBSERVED};
  while (written < len) {
    /* Re-copy ONLY the unaccepted original suffix, including after odd partial
     * returns. Never advance the driver's aligned staging source pointer. */
    const char *staged = pet_uart_control_stage_copy(
      g_uart_control_stage, data + written, len - written);
    if (!staged) {
      if (observation) observation->invalid_return = true;
      break;
    }
    int n = uart_write_bytes(PET_P4_UART_NUM, staged, len - written);
    if (observation) observation->last_return = n;
    if (n <= 0) break;
    if ((size_t)n > len - written) {
      if (observation) observation->invalid_return = true;
      break;
    }
    written += (size_t)n;
  }
  if (observation) observation->accepted = written;
}
#endif

static void uart_write_all(const char *data, size_t len, pet_transport_write_observation_t *observation) {
#if PET_P4_UART_PACING_ACTIVE
  /* Bounded drained control path; audio/media retain uart_queue_all. */
  pet_transport_write_observation_t aggregate={.last_return=PET_TRANSPORT_AUDIT_NOT_OBSERVED,
    .drain=PET_TRANSPORT_AUDIT_NOT_OBSERVED};
  while(aggregate.accepted<len){
    size_t allowance=PET_P4_UART_TX_BURST_BYTES;
#if PET_P4_SERIAL_CONTROL_INTEGRITY
    allowance-=g_uart_control_prefix_queued;
#endif
    size_t count=len-aggregate.accepted;if(count>allowance)count=allowance;
    pet_transport_write_observation_t part;
    uart_control_queue_all(data+aggregate.accepted,count,&part);
    aggregate.accepted+=part.accepted;aggregate.last_return=part.last_return;
    aggregate.invalid_return|=part.invalid_return;
    aggregate.drain=uart_wait_tx_done(PET_P4_UART_NUM,pdMS_TO_TICKS(100));
#if PET_P4_SERIAL_CONTROL_INTEGRITY
    g_uart_control_prefix_queued=0;
#endif
    if(part.accepted!=count||part.invalid_return||aggregate.drain!=ESP_OK)break;
    /* Also separate the final body/newline and subsequent frame boundaries. */
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if(observation)*observation=aggregate;
#else
  uart_queue_all(data, len, observation);
  int result = uart_wait_tx_done(PET_P4_UART_NUM, pdMS_TO_TICKS(100));
  if (observation) observation->drain = result;
#endif
}

#if PET_P4_SERIAL_CONTROL_INTEGRITY
/* Same mutex/queue as the original body. Do NOT drain this short prefix: a
 * separate USB short-packet drain could add 100ms before the JSON body. */
static bool transport_control_prefix(const char prefix[PET_CONTROL_PREFIX_BYTES+1],bool usb){
  pet_transport_write_observation_t written;
  if(usb)usb_queue_all(prefix,PET_CONTROL_PREFIX_BYTES,&written);
  else {
#if PET_P4_UART_PACING_ACTIVE
    uart_control_queue_all(prefix,PET_CONTROL_PREFIX_BYTES,&written);
#else
    uart_queue_all(prefix,PET_CONTROL_PREFIX_BYTES,&written);
#endif
  }
  if(written.accepted==PET_CONTROL_PREFIX_BYTES&&!written.invalid_return){
#if PET_P4_UART_PACING_ACTIVE
    if(!usb)g_uart_control_prefix_queued=PET_CONTROL_PREFIX_BYTES;
#endif
    return true;
  }
  /* Partial header is never followed by an unguarded body. Best-effort line
   * termination only; unknown delivery never authorizes business replay. */
  if(usb)usb_queue_all("\n",1,NULL);
  else {
#if PET_P4_UART_PACING_ACTIVE
    uart_control_queue_all("\n",1,NULL);
#else
    uart_queue_all("\n",1,NULL);
#endif
  }
  return false;
}
#endif

static int transport_log_vprintf(const char *format, va_list args) {
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(log_phase,PET_PHASE_LOG_WAIT);
#endif
  if (g_transport_mutex) xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_LOG_HELD);
#endif
  int written;
  if (!g_transport_ready) {
    // Before driver initialization there are no queued protocol frames.
    written = vprintf(format, args);
    fflush(stdout);
  } else {
    // A task mutex alone cannot serialize the console's direct FIFO writes
    // against bytes still in an interrupt-driven TX queue. Use that SAME queue
    // for logs, in every audio state (capture-active excludes radio playback).
    // No drain waits: a short USB packet can otherwise add 100 ms to an ACK.
    written = vsnprintf(g_transport_log_buffer, sizeof(g_transport_log_buffer), format, args);
    if (written > 0) {
      size_t len = (size_t) written;
      if (len >= sizeof(g_transport_log_buffer)) len = sizeof(g_transport_log_buffer) - 1;
      // Bound long diagnostics and always terminate the record. Even a clipped
      // log must not absorb the next protocol frame into its physical line.
      if (g_transport_log_buffer[len - 1] != '\n') {
        if (len == sizeof(g_transport_log_buffer) - 1) --len;
        g_transport_log_buffer[len++] = '\n';
      }
      uart_queue_all(g_transport_log_buffer, len, NULL);
      if (usb_protocol_seen()) usb_queue_all(g_transport_log_buffer, len, NULL);
    }
  }
  if (g_transport_mutex) xSemaphoreGive(g_transport_mutex);
  return written;
}

static void transport_audit_observe(pet_transport_audit_t *audit,const char *line, pet_transport_audit_route_t route, size_t bytes,
    uint32_t hash, bool hash_available, const pet_transport_write_observation_t *body,
    const pet_transport_write_observation_t *newline) {
  if(!pet_transport_audit_active(audit))return;
  pet_transport_audit_record_t record = {
    .route = route, .body_bytes = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes,
    .fingerprint = hash, .fingerprint_available = hash_available,
    .driver_observed = body && newline,
    .body_last_return = PET_TRANSPORT_AUDIT_NOT_OBSERVED,
    .newline_last_return = PET_TRANSPORT_AUDIT_NOT_OBSERVED,
    .body_drain = PET_TRANSPORT_AUDIT_NOT_OBSERVED,
    .newline_drain = PET_TRANSPORT_AUDIT_NOT_OBSERVED
  };
  /* Opt-in source-buffer correlation after driver return, not delivery proof. */
  record.fingerprint_after_available=hash_available
    &&pet_transport_audit_fingerprint(line,bytes,&record.fingerprint_after);
  if (body && newline) {
    record.body_accepted = (uint32_t)body->accepted;
    record.newline_accepted = (uint32_t)newline->accepted;
    record.body_last_return = body->last_return; record.newline_last_return = newline->last_return;
    record.body_drain = body->drain; record.newline_drain = newline->drain;
    record.invalid_return = body->invalid_return || newline->invalid_return;
  }
  pet_transport_audit_record(audit, &record);
}

static void transport_send_line_impl(const char *line, void *ctx, bool drain) {
  pet_p4_transport_route_t route = ctx
    ? *((const pet_p4_transport_route_t *) ctx)
    : PET_P4_TRANSPORT_BROADCAST;
  if (!line) return;
#if PET_P4_NATIVE_WORK_WINDOW
  bool work_window=false;
  if(drain){work_window=pet_native_work_window_control_begin();if(!work_window)return;}
#endif
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(tx_phase,PET_PHASE_TX_WAIT);
#endif
  if (g_transport_mutex) xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_TX_HELD);
#endif
#if PET_P4_SERIAL_CONTROL_INTEGRITY
  char control_prefix[PET_CONTROL_PREFIX_BYTES+1];size_t control_bytes;
  bool guarded=true;
  bool needs_prefix=drain;
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
  /* Snapshot ONCE for this entire physical line. A legacy RX busy reply can
   * overlap the protocol worker's next bind; never mix a newly observed mode
   * with an old/uninitialized prefix. Audio does not even sample this policy. */
  bool uart_framed=drain&&(route==PET_P4_TRANSPORT_UART||route==PET_P4_TRANSPORT_BROADCAST)
    &&pet_control_session_framed(&g_uart_route);
  bool jtag_framed=drain&&(route==PET_P4_TRANSPORT_USB_SERIAL_JTAG||route==PET_P4_TRANSPORT_BROADCAST)
    &&pet_control_session_framed(&g_usb_serial_jtag_route);
  needs_prefix=uart_framed||jtag_framed;
#endif
  if(needs_prefix)guarded=pet_control_prefix(control_prefix,line,&control_bytes);
#endif
  /* Existing audio/media classification is enough to stop observation without
   * adding four SDK flag calls to every TX frame. Retain the evidence. */
  if (!drain && (g_transport_audit.armed || g_transport_audit.prepared))
    pet_transport_audit_disarm(&g_transport_audit);
  if(!drain&&(g_transport_reply_audit.armed||g_transport_reply_audit.prepared))
    pet_transport_audit_disarm(&g_transport_reply_audit);
  static const char audit_prefix[] = "{\"topic\":\"debug/transport_audit";
  static const char result_prefix[] = "{\"topic\":\"debug/transport_audit_result\",";
  bool observing_reply = drain && pet_transport_audit_active(&g_transport_reply_audit)
    && strncmp(line,result_prefix,sizeof(result_prefix)-1)==0;
  pet_transport_audit_t *audit=observing_reply?&g_transport_reply_audit:&g_transport_audit;
  bool observing = observing_reply || (drain && pet_transport_audit_active(audit)
    && strncmp(line, audit_prefix, sizeof(audit_prefix) - 1) != 0);
  size_t audited_bytes = 0;
  uint32_t fingerprint = 0;
  bool hash_available = false;
  if (observing) {
    /* Caller retains an immutable line until send returns. Hash immediately
     * before enqueue, once per line rather than once per broadcast route. */
    audited_bytes = strlen(line);
    hash_available = pet_transport_audit_fingerprint(line, audited_bytes, &fingerprint);
  }
  if (route == PET_P4_TRANSPORT_BROADCAST || route == PET_P4_TRANSPORT_UART) {
    if (drain) {
#if PET_P4_SERIAL_CONTROL_INTEGRITY
      if(
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
          !uart_framed||
#endif
          (guarded&&transport_control_prefix(control_prefix,false))){
#endif
      pet_transport_write_observation_t body, newline;
      uart_write_all(line, observing ? audited_bytes : strlen(line), observing ? &body : NULL);
      uart_write_all("\n", 1, observing ? &newline : NULL);
      if (observing) transport_audit_observe(audit,line, PET_TRANSPORT_AUDIT_UART, audited_bytes,
        fingerprint, hash_available, &body, &newline);
#if PET_P4_SERIAL_CONTROL_INTEGRITY
      }
#endif
    } else {
      static const char audio_prefix[]="{\"topic\":\"audio/";
      if(strncmp(line,audio_prefix,sizeof(audio_prefix)-1)==0){
        uart_audio_queue_all(line,strlen(line));
        uart_audio_queue_all("\n",1);
      }else{
        uart_queue_all(line,strlen(line),NULL);
        uart_queue_all("\n",1,NULL);
      }
    }
  }
  if (route == PET_P4_TRANSPORT_USB_SERIAL_JTAG
      || (route == PET_P4_TRANSPORT_BROADCAST && usb_protocol_seen())) {
    if (drain) {
#if PET_P4_SERIAL_CONTROL_INTEGRITY
      if(
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
          !jtag_framed||
#endif
          (guarded&&transport_control_prefix(control_prefix,true))){
#endif
      pet_transport_write_observation_t body, newline;
      usb_write_all(line, observing ? audited_bytes : strlen(line), observing ? &body : NULL);
      usb_write_all("\n", 1, observing ? &newline : NULL);
      if (observing) transport_audit_observe(audit,line, PET_TRANSPORT_AUDIT_USB_SERIAL_JTAG, audited_bytes,
        fingerprint, hash_available, &body, &newline);
#if PET_P4_SERIAL_CONTROL_INTEGRITY
      }
#endif
    } else {
      usb_queue_all(line, strlen(line), NULL);
      usb_queue_all("\n", 1, NULL);
    }
  }
  if (route == PET_P4_TRANSPORT_BROADCAST) {
    pet_p4_native_usb_send_json_line(line, NULL);
    if (observing) transport_audit_observe(audit,line, PET_TRANSPORT_AUDIT_NATIVE_USB, audited_bytes,
      fingerprint, hash_available, NULL, NULL);
  }
  if(observing_reply){
    g_transport_reply_observed=g_transport_reply_audit.seen!=0;
    pet_transport_audit_disarm(&g_transport_reply_audit);
  }
  if (g_transport_mutex) xSemaphoreGive(g_transport_mutex);
#if PET_P4_NATIVE_WORK_WINDOW
  if(work_window){bool released=pet_native_work_window_end();configASSERT(released);(void)released;}
#endif
}

static void transport_send_line(const char *line, void *ctx) {
  const char audio_prefix[] = "{\"topic\":\"audio/";
  const char media_prefix[] = "{\"topic\":\"media/";
  // A media chunk contains only 80 ms of audio. Waiting for USB TX completion
  // before queuing the newline delays its ACK by ~100 ms on native USB and
  // guarantees underruns. Queue the complete frame under the same mutex, as
  // for realtime audio; firmware/widget transfers keep their drained path.
  bool audio = line && (strncmp(line, audio_prefix, sizeof(audio_prefix) - 1) == 0
    || strncmp(line, media_prefix, sizeof(media_prefix) - 1) == 0);
  transport_send_line_impl(line, ctx, !audio);
}

static void transport_send_audio_line(const char *line, void *ctx) {
  // Driver queues copy each frame; draining twice per 20 ms frame stalls capture.
  // The transport mutex still prevents JSON/console interleaving.
  transport_send_line_impl(line, ctx, false);
}

/* Screenshot-only checked writes. Normal audio/media queueing above is unchanged. */
#include "pet_p4_screenshot_write.h"
#include "pet_p4_screenshot_close.h"
static pet_screenshot_tx_observation_t g_screenshot_tx;
#if PET_P4_SCREENSHOT_CLOSE_ACK
/* Protocol-worker owned fixed correlation only. No request text/image retained. */
static struct {
  uint32_t bytes,hash,received_bytes,received_hash,expected_bytes,expected_hash;
} g_screenshot_rx;
#endif
#define PET_P4_SCREENSHOT_PULL_IDLE_MS 3000
#define PET_P4_SCREENSHOT_PULL_LIFETIME_MS 30000
/* Only protocol_worker_task owns this state. No timer callback frees a buffer
   while a physical driver write still uses it. */
static struct {
  pet_resource_debug_session_t resources;
  uint8_t *raw;
  char *b64;
  char id[32], request_id[33];
  int width, height;
  unsigned non_black, checksum, reads;
  TickType_t created, touched;
  pet_p4_transport_route_t route;
  bool pull;
} g_screenshot;
static uint32_t g_screenshot_sequence;
typedef enum {
  SCREENSHOT_RELEASE_UNKNOWN,SCREENSHOT_RELEASE_IDLE,SCREENSHOT_RELEASE_LIFETIME,
  SCREENSHOT_RELEASE_REVOKED,SCREENSHOT_RELEASE_TRANSPORT,SCREENSHOT_RELEASE_CLOSED
} screenshot_release_reason_t;
/* Last exact snapshot's bounded terminal category only. No image, requested
 * JSON, path, key or external identity survives release. Protocol-worker owned. */
static struct {char id[32];pet_p4_transport_route_t route;screenshot_release_reason_t reason;pet_screenshot_tx_observation_t tx;} g_screenshot_terminal;
static bool screenshot_audio_busy(void){
  return pet_p4_audio_active()||pet_p4_audio_playback_active()||pet_p4_audio_conversation_active()||pet_p4_audio_music_active();
}
static void screenshot_release_as(screenshot_release_reason_t reason){
  if(g_screenshot.raw&&g_screenshot.id[0]){
    memcpy(g_screenshot_terminal.id,g_screenshot.id,sizeof(g_screenshot_terminal.id));
    g_screenshot_terminal.route=g_screenshot.route;g_screenshot_terminal.reason=reason;
    g_screenshot_terminal.tx=g_screenshot_tx;
  }
  free(g_screenshot.raw);
  free(g_screenshot.b64);
  pet_resources_device_debug_end(&g_screenshot.resources);
  memset(&g_screenshot,0,sizeof(g_screenshot));
}
static void screenshot_release(void){screenshot_release_as(SCREENSHOT_RELEASE_UNKNOWN);}
static void screenshot_poll(void){
  if(!g_screenshot.raw)return;
  TickType_t now=xTaskGetTickCount();
  if(!pet_resources_device_debug_current(&g_screenshot.resources,screenshot_audio_busy()))
    screenshot_release_as(SCREENSHOT_RELEASE_REVOKED);
  else if(g_screenshot.pull&&now-g_screenshot.created>=pdMS_TO_TICKS(PET_P4_SCREENSHOT_PULL_LIFETIME_MS))
    screenshot_release_as(SCREENSHOT_RELEASE_LIFETIME);
  else if(g_screenshot.pull&&now-g_screenshot.touched>=pdMS_TO_TICKS(PET_P4_SCREENSHOT_PULL_IDLE_MS))
    screenshot_release_as(SCREENSHOT_RELEASE_IDLE);
}
static uint32_t screenshot_chunk_checksum(const uint8_t *raw,size_t len){
  uint32_t checksum=2166136261u;
  for(size_t i=0;i<len;i++){checksum^=raw[i];checksum*=16777619u;}
  return checksum;
}
static bool screenshot_write_all(const char *data,size_t len,bool usb,TickType_t started){
  size_t written=0;
  g_screenshot_tx.part++;
  g_screenshot_tx.requested+=(uint32_t)len;
  while(written<len){
    TickType_t elapsed=xTaskGetTickCount()-started;
    if(screenshot_audio_busy())return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_AUDIO,elapsed);
    if(elapsed>=pdMS_TO_TICKS(100))return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_DEADLINE,elapsed);
    if(g_screenshot.raw&&!pet_resources_device_debug_current(&g_screenshot.resources,false))
      return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_REVOKED,elapsed);
    size_t count=len-written;
#if PET_P4_UART_PACING_ACTIVE
    /* Prior reference paced control replies but NOT screenshot bodies. A
     * clean query window followed by a broken frame did not test pacing on
     * that failed path. Private comparison only; no audio/media queue change.
     * The original shared 100ms send and snapshot lifetime stay authoritative. */
    if(!usb&&count>PET_P4_UART_TX_BURST_BYTES)count=PET_P4_UART_TX_BURST_BYTES;
#endif
    int n=usb?usb_serial_jtag_write_bytes(data+written,count,pdMS_TO_TICKS(20))
      :uart_write_bytes(PET_P4_UART_NUM,data+written,count);
    g_screenshot_tx.calls++;g_screenshot_tx.last_return=n;
    if(n<=0)return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_DRIVER_ZERO,xTaskGetTickCount()-started);
    if((size_t)n>count)return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_DRIVER_COUNT,xTaskGetTickCount()-started);
    g_screenshot_tx.accepted+=(uint32_t)n;
    written+=(size_t)n;
#if PET_P4_UART_PACING_ACTIVE
    if(!usb){
      g_screenshot_tx.drain_return=uart_wait_tx_done(PET_P4_UART_NUM,pdMS_TO_TICKS(100));
      if(g_screenshot_tx.drain_return!=ESP_OK)return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_DRAIN,xTaskGetTickCount()-started);
      vTaskDelay(pdMS_TO_TICKS(1));
    }
#endif
  }
  return true;
}
static bool screenshot_send_parts(const char *header,const char *body,size_t body_len,const char *suffix,void *reply_ctx){
  g_screenshot_tx=(pet_screenshot_tx_observation_t){0};
  pet_p4_transport_route_t route=reply_ctx?*((const pet_p4_transport_route_t*)reply_ctx):PET_P4_TRANSPORT_BROADCAST;
  if(screenshot_audio_busy())return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_AUDIO,0);
#if PET_P4_NATIVE_WORK_WINDOW
  if(!pet_native_work_window_control_begin())return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_MUTEX,0);
#endif
  if(!g_transport_mutex||xSemaphoreTake(g_transport_mutex,pdMS_TO_TICKS(20))!=pdTRUE)
  {
#if PET_P4_NATIVE_WORK_WINDOW
    bool released=pet_native_work_window_end();configASSERT(released);(void)released;
#endif
    return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_MUTEX,0);
  }
  bool ok=true,sent=false;TickType_t started=xTaskGetTickCount();
  if(route==PET_P4_TRANSPORT_BROADCAST||route==PET_P4_TRANSPORT_UART){
    sent=true;
    ok=screenshot_write_all(header,strlen(header),false,started)
      &&screenshot_write_all(body,body_len,false,started)
      &&screenshot_write_all(suffix,strlen(suffix),false,started);
    if(ok&&screenshot_audio_busy())ok=pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_AUDIO,xTaskGetTickCount()-started);
    if(ok&&g_screenshot.raw&&!pet_resources_device_debug_current(&g_screenshot.resources,false))ok=pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_REVOKED,xTaskGetTickCount()-started);
    if(ok){g_screenshot_tx.drain_return=uart_wait_tx_done(PET_P4_UART_NUM,pdMS_TO_TICKS(100));
      if(g_screenshot_tx.drain_return!=ESP_OK)ok=pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_DRAIN,xTaskGetTickCount()-started);}
  }
  if(ok&&(route==PET_P4_TRANSPORT_USB_SERIAL_JTAG||(route==PET_P4_TRANSPORT_BROADCAST&&usb_protocol_seen()))){
    sent=true;started=xTaskGetTickCount();
    ok=screenshot_write_all(header,strlen(header),true,started)
      &&screenshot_write_all(body,body_len,true,started)
      &&screenshot_write_all(suffix,strlen(suffix),true,started);
    if(ok&&screenshot_audio_busy())ok=pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_AUDIO,xTaskGetTickCount()-started);
    if(ok&&g_screenshot.raw&&!pet_resources_device_debug_current(&g_screenshot.resources,false))ok=pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_REVOKED,xTaskGetTickCount()-started);
    if(ok){g_screenshot_tx.drain_return=usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
      if(g_screenshot_tx.drain_return!=ESP_OK)ok=pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_DRAIN,xTaskGetTickCount()-started);}
  }
  if(!sent)(void)pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_ROUTE,0);
  xSemaphoreGive(g_transport_mutex);
#if PET_P4_NATIVE_WORK_WINDOW
  bool released=pet_native_work_window_end();configASSERT(released);(void)released;
#endif
  return sent&&ok;
}
static bool screenshot_send_line(const char *line,void *ctx){return screenshot_send_parts(line,"",0,"\n",ctx);}
static void screenshot_error(const char *id,const char *reason,int index,size_t sent_bytes,void *ctx){
#if PET_P4_SCREENSHOT_CLOSE_ACK
  char error[768];pet_screenshot_tx_observation_t failed=g_screenshot_tx;
  snprintf(error,sizeof(error),"\n{\"topic\":\"debug/screenshot_error\",\"payload\":{\"id\":\"%s\",\"error\":\"%s\",\"failedIndex\":%d,\"sentBytes\":%u,\"writerReason\":%u,\"writerPart\":%u,\"writerRequested\":%u,\"writerAccepted\":%u,\"writerCalls\":%u,\"writerReturn\":%ld,\"writerDrain\":%ld,\"writerElapsedTicks\":%u,\"requestBytes\":%u,\"requestHash\":%u,\"receivedIdBytes\":%u,\"receivedIdHash\":%u,\"expectedIdBytes\":%u,\"expectedIdHash\":%u}}",id?id:"",reason,index,(unsigned)sent_bytes,
    (unsigned)failed.reason,(unsigned)failed.part,(unsigned)failed.requested,(unsigned)failed.accepted,(unsigned)failed.calls,(long)failed.last_return,(long)failed.drain_return,(unsigned)failed.elapsed_ticks,
    (unsigned)g_screenshot_rx.bytes,(unsigned)g_screenshot_rx.hash,(unsigned)g_screenshot_rx.received_bytes,(unsigned)g_screenshot_rx.received_hash,(unsigned)g_screenshot_rx.expected_bytes,(unsigned)g_screenshot_rx.expected_hash);
#else
  char error[512];pet_screenshot_tx_observation_t failed=g_screenshot_tx;
  snprintf(error,sizeof(error),"\n{\"topic\":\"debug/screenshot_error\",\"payload\":{\"id\":\"%s\",\"error\":\"%s\",\"failedIndex\":%d,\"sentBytes\":%u,\"writerReason\":%u,\"writerPart\":%u,\"writerRequested\":%u,\"writerAccepted\":%u,\"writerCalls\":%u,\"writerReturn\":%ld,\"writerDrain\":%ld,\"writerElapsedTicks\":%u}}",id?id:"",reason,index,(unsigned)sent_bytes,
    (unsigned)failed.reason,(unsigned)failed.part,(unsigned)failed.requested,(unsigned)failed.accepted,(unsigned)failed.calls,(long)failed.last_return,(long)failed.drain_return,(unsigned)failed.elapsed_ticks);
#endif
  /* Leading newline resynchronizes a partially written JSON frame. This is
     best effort; a disconnected writer may only be diagnosed by PC timeout. */
  (void)screenshot_send_line(error,ctx);
  g_screenshot_tx=failed; /* Error delivery must not erase the original failure. */
}
static bool send_screenshot_chunk(
  const char *id,
  int index,
  const uint8_t *raw,
  size_t raw_len,
  char *b64,
  size_t b64_capacity,
  void *reply_ctx
) {
  size_t b64_len = 0;
  char header[256];
  if (!b64_capacity||mbedtls_base64_encode((unsigned char *) b64, b64_capacity - 1, &b64_len, raw, raw_len) != 0||b64_len>=b64_capacity) {
    g_screenshot_tx=(pet_screenshot_tx_observation_t){0};
    return pet_screenshot_tx_fail(&g_screenshot_tx,PET_SCREENSHOT_TX_ENCODE,0);
  }
  b64[b64_len] = '\0';
  snprintf(
    header,
    sizeof(header),
    "{\"topic\":\"debug/screenshot_chunk\",\"payload\":{\"id\":\"%s\",\"index\":%d,\"rawSize\":%u,\"checksum\":\"%08x\",\"data\":\"",
    id,
    index,
    (unsigned)raw_len,
    (unsigned)screenshot_chunk_checksum(raw,raw_len)
  );
  return screenshot_send_parts(header,b64,b64_len,"\"}}\n",reply_ctx);
}

static const char *screenshot_unavailable_reason(const cJSON *id,pet_p4_transport_route_t route){
  g_screenshot_tx=(pet_screenshot_tx_observation_t){0};
  if(g_screenshot.raw)return g_screenshot.route!=route?"snapshot route mismatched":"snapshot id mismatched";
  if(cJSON_IsString(id)&&g_screenshot_terminal.route==route&&!strcmp(id->valuestring,g_screenshot_terminal.id)){
    switch(g_screenshot_terminal.reason){
    case SCREENSHOT_RELEASE_IDLE:return "snapshot idle expired";
    case SCREENSHOT_RELEASE_LIFETIME:return "snapshot lifetime expired";
    case SCREENSHOT_RELEASE_REVOKED:return "snapshot lease revoked";
    case SCREENSHOT_RELEASE_TRANSPORT:g_screenshot_tx=g_screenshot_terminal.tx;return "snapshot transport failed";
    case SCREENSHOT_RELEASE_CLOSED:return "snapshot already closed";
    default:break;
    }
  }
  return "snapshot expired or mismatched";
}
#if PET_P4_SCREENSHOT_CLOSE_ACK && PET_P4_SERIAL_CONTROL_INTEGRITY
static bool screenshot_control_framed(void *reply_ctx){
#if PET_P4_SERIAL_CONTROL_INTEGRITY
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
  return reply_ctx&&pet_control_session_framed(reply_ctx);
#else
  (void)reply_ctx;return true;
#endif
#else
  (void)reply_ctx;return false;
#endif
}
#endif
static bool maybe_handle_debug_line(const char *line, void *reply_ctx) {
  if (!line || !strstr(line, "\"debug/screenshot")) return false;
  cJSON *request=cJSON_ParseWithOpts(line,NULL,true);
  const cJSON *topic=cJSON_GetObjectItemCaseSensitive(request,"topic");
  if(!cJSON_IsString(topic)){cJSON_Delete(request);return false;}
  bool start=!strcmp(topic->valuestring,"debug/screenshot");
  bool read=!strcmp(topic->valuestring,"debug/screenshot_read");
  bool close=!strcmp(topic->valuestring,"debug/screenshot_close");
  if(!start&&!read&&!close){cJSON_Delete(request);return false;}
  const cJSON *payload=cJSON_GetObjectItemCaseSensitive(request,"payload");
  pet_p4_transport_route_t route=reply_ctx?*((const pet_p4_transport_route_t*)reply_ctx):PET_P4_TRANSPORT_BROADCAST;
#if PET_P4_SCREENSHOT_CLOSE_ACK
  memset(&g_screenshot_rx,0,sizeof(g_screenshot_rx));
  /* Bound hashing independently of the larger JSON admission limit. Zero bytes
   * explicitly means unavailable, never a fingerprint of an omitted body. */
  size_t request_bytes=strnlen(line,4097);
  if(request_bytes&&request_bytes<=4096){g_screenshot_rx.bytes=(uint32_t)request_bytes;g_screenshot_rx.hash=screenshot_chunk_checksum((const uint8_t*)line,request_bytes);}
  const cJSON *received_id=cJSON_GetObjectItemCaseSensitive(payload,"id");
  if(cJSON_IsString(received_id)){
    size_t bytes=strnlen(received_id->valuestring,32);g_screenshot_rx.received_bytes=(uint32_t)bytes;
    if(bytes&&bytes<=31)g_screenshot_rx.received_hash=screenshot_chunk_checksum((const uint8_t*)received_id->valuestring,bytes);
  }
  if(g_screenshot.raw&&g_screenshot.route==route){
    g_screenshot_rx.expected_bytes=(uint32_t)strlen(g_screenshot.id);
    g_screenshot_rx.expected_hash=screenshot_chunk_checksum((const uint8_t*)g_screenshot.id,g_screenshot_rx.expected_bytes);
  }
#endif
  screenshot_poll();
  if(read||close){
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(payload,"id");
    const cJSON *index=cJSON_GetObjectItemCaseSensitive(payload,"index");
    bool matches=g_screenshot.raw&&g_screenshot.pull&&g_screenshot.route==route
      &&cJSON_IsString(id)&&!strcmp(id->valuestring,g_screenshot.id);
    if(!matches){const char *reason=screenshot_unavailable_reason(id,route);cJSON_Delete(request);screenshot_error("",reason,-1,0,reply_ctx);return true;}
    if(close){
#if PET_P4_SCREENSHOT_CLOSE_ACK
      const cJSON *ack=cJSON_GetObjectItemCaseSensitive(payload,"ack");
      const cJSON *rid=cJSON_GetObjectItemCaseSensitive(payload,"requestId");
      bool acknowledged=cJSON_IsTrue(ack);
      if((ack&&!cJSON_IsBool(ack))||(acknowledged&&(!cJSON_IsString(rid)||strcmp(rid->valuestring,g_screenshot.request_id)))){
        cJSON_Delete(request);screenshot_error("","invalid snapshot close",-1,0,reply_ctx);return true;
      }
      char closed[224],closed_id[32];
      if(acknowledged){
        memcpy(closed_id,g_screenshot.id,sizeof(closed_id));
        snprintf(closed,sizeof(closed),"{\"topic\":\"debug/screenshot_closed\",\"payload\":{\"id\":\"%s\",\"requestId\":\"%s\",\"closed\":true}}",g_screenshot.id,g_screenshot.request_id);
      }
#endif
      cJSON_Delete(request);screenshot_release_as(SCREENSHOT_RELEASE_CLOSED);
#if PET_P4_SCREENSHOT_CLOSE_ACK
      /* Physical retirement precedes acknowledgement. If delivery fails, the
       * caller retains failure; it may not infer another snapshot was closed. */
      if(acknowledged){
#if PET_P4_SERIAL_CONTROL_INTEGRITY
        /* A control receipt, not raw image data: retain this connection's
         * existing required prefix instead of widening reader exemptions. */
        char prefix[PET_CONTROL_PREFIX_BYTES+1];size_t bytes=0;
        bool sent=screenshot_control_framed(reply_ctx)
          ?pet_control_prefix(prefix,closed,&bytes)&&screenshot_send_parts(prefix,closed,bytes,"\n",reply_ctx)
          :screenshot_send_line(closed,reply_ctx);
#else
        bool sent=screenshot_send_line(closed,reply_ctx);
#endif
        if(!sent)screenshot_error(closed_id,"snapshot close acknowledgement failed",-1,0,reply_ctx);
      }
#endif
      return true;
    }
    int chunks=(g_screenshot.width*g_screenshot.height*2+PET_P4_SCREENSHOT_RAW_CHUNK-1)/PET_P4_SCREENSHOT_RAW_CHUNK;
    if(!cJSON_IsNumber(index)||index->valuedouble!=index->valueint||index->valueint<0||index->valueint>chunks||g_screenshot.reads>=800){
      cJSON_Delete(request);screenshot_error(g_screenshot.id,"invalid snapshot read",-1,0,reply_ctx);return true;
    }
    int at=index->valueint;cJSON_Delete(request);
    g_screenshot.reads++;g_screenshot.touched=xTaskGetTickCount();
    size_t total=(size_t)g_screenshot.width*g_screenshot.height*2;
    bool sent;
    if(at==chunks){
      char end[256];
      snprintf(end,sizeof(end),"{\"topic\":\"debug/screenshot_end\",\"payload\":{\"id\":\"%s\",\"chunks\":%d,\"totalBytes\":%u,\"nonBlack\":%u,\"checksum\":\"%08x\"}}",g_screenshot.id,chunks,(unsigned)total,g_screenshot.non_black,g_screenshot.checksum);
      sent=screenshot_send_line(end,reply_ctx);
    }else{
      size_t offset=(size_t)at*PET_P4_SCREENSHOT_RAW_CHUNK,n=total-offset;
      if(n>PET_P4_SCREENSHOT_RAW_CHUNK)n=PET_P4_SCREENSHOT_RAW_CHUNK;
      sent=send_screenshot_chunk(g_screenshot.id,at,g_screenshot.raw+offset,n,g_screenshot.b64,((PET_P4_SCREENSHOT_RAW_CHUNK+2)/3)*4+8,reply_ctx);
    }
    if(!sent){
      if(pet_screenshot_tx_unwritten_busy(&g_screenshot_tx)){
        /* No byte entered a driver. Keep this immutable snapshot for a bounded
         * reread of this SAME index; never re-capture or extend its lifetime. */
        screenshot_error(g_screenshot.id,"snapshot transport busy",at,0,reply_ctx);
      }else{
        screenshot_error(g_screenshot.id,"transport or encode failed",at,0,reply_ctx);
        screenshot_release_as(SCREENSHOT_RELEASE_TRANSPORT);
      }
    }
    return true;
  }
  bool full=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload,"fullResolution"));
  const cJSON *mode=cJSON_GetObjectItemCaseSensitive(payload,"mode");
  bool pull=cJSON_IsString(mode)&&!strcmp(mode->valuestring,"pull-v1");
  const cJSON *rid=cJSON_GetObjectItemCaseSensitive(payload,"requestId");
  char request_id[33]={0};
  if(pull){
    bool valid=cJSON_IsString(rid)&&rid->valuestring[0]&&strlen(rid->valuestring)<=32;
    if(valid)for(const char *s=rid->valuestring;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'))valid=false;
    if(!valid||route==PET_P4_TRANSPORT_BROADCAST){cJSON_Delete(request);screenshot_error("","invalid snapshot request",-1,0,reply_ctx);return true;}
    memcpy(request_id,rid->valuestring,strlen(rid->valuestring)+1);
  }else if(mode){cJSON_Delete(request);screenshot_error("","unsupported snapshot mode",-1,0,reply_ctx);return true;}
  cJSON_Delete(request);
  if(g_screenshot.raw){
    /* A lost begin response can be repeated without recapturing another frame.
       A different request never steals someone else's immutable snapshot. */
    if(pull&&g_screenshot.pull&&g_screenshot.route==route&&!strcmp(request_id,g_screenshot.request_id)
       &&g_screenshot.width==(full?640:PET_P4_SCREENSHOT_W))goto send_begin;
    screenshot_error("","snapshot busy",-1,0,reply_ctx);return true;
  }
  g_screenshot_tx=(pet_screenshot_tx_observation_t){0};
  if(screenshot_audio_busy()){screenshot_error("","audio busy",-1,0,reply_ctx);return true;}
  const int capture_w=full?640:PET_P4_SCREENSHOT_W,capture_h=full?480:PET_P4_SCREENSHOT_H;
  if(pet_resources_device_debug_begin((uint32_t)capture_w*capture_h*2,screenshot_audio_busy(),&g_screenshot.resources)!=PET_RESOURCE_OK){screenshot_error("","snapshot resource busy",-1,0,reply_ctx);return true;}
  uint8_t *raw = (uint8_t *) heap_caps_malloc(capture_w*capture_h*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  char *b64 = (char *) malloc(((PET_P4_SCREENSHOT_RAW_CHUNK + 2) / 3) * 4 + 8);
  g_screenshot.raw=raw;g_screenshot.b64=b64;
  if (!raw || !b64) {
    screenshot_release();
    screenshot_error("","no memory",-1,0,reply_ctx);
    return true;
  }
  if (!g_render_mutex || xSemaphoreTake(g_render_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    screenshot_release();
    screenshot_error("","renderer busy",-1,0,reply_ctx);
    return true;
  }

  int src_w = 0;
  int src_h = 0;
  const uint16_t *src = pet_p4_renderer_logical_framebuffer(&src_w, &src_h);
  if (!src || src_w <= 0 || src_h <= 0) {
    xSemaphoreGive(g_render_mutex);
    screenshot_release();
    screenshot_error("","no framebuffer",-1,0,reply_ctx);
    return true;
  }

  unsigned int non_black = 0;
  unsigned int checksum = 2166136261u;
  for (int y = 0; y < capture_h; y += 1) {
    if(y%16==0&&!pet_resources_device_debug_current(&g_screenshot.resources,screenshot_audio_busy())){
      xSemaphoreGive(g_render_mutex);screenshot_release();return true;
    }
    int sy = y * src_h / capture_h;
    for (int x = 0; x < capture_w; x += 1) {
      int sx = x * src_w / capture_w;
      uint16_t px = src[sy * src_w + sx];
      if (px != 0) non_black += 1;
      checksum ^= (unsigned int) px;
      checksum *= 16777619u;
      size_t off = (size_t) (y * capture_w + x) * 2;
      raw[off] = (uint8_t) (px & 0xff);
      raw[off + 1] = (uint8_t) (px >> 8);
    }
  }
  xSemaphoreGive(g_render_mutex);
  if(!pet_resources_device_debug_current(&g_screenshot.resources,screenshot_audio_busy())){screenshot_release();return true;}
  g_screenshot.width=capture_w;g_screenshot.height=capture_h;
  g_screenshot.non_black=non_black;g_screenshot.checksum=checksum;
  g_screenshot.route=route;g_screenshot.pull=pull;
  g_screenshot.created=g_screenshot.touched=xTaskGetTickCount();
  memcpy(g_screenshot.request_id,request_id,sizeof(request_id));
  snprintf(g_screenshot.id,sizeof(g_screenshot.id),"%llu-%u",(unsigned long long)(esp_timer_get_time()/1000ULL),++g_screenshot_sequence);
send_begin: ;
  const char *id=g_screenshot.id;
  char begin[384];
  snprintf(
    begin,
    sizeof(begin),
    "{\"topic\":\"debug/screenshot_begin\",\"payload\":{\"id\":\"%s\",\"width\":%d,\"height\":%d,\"format\":\"rgb565le\",\"integrity\":\"chunk-fnv1a32-v1\",\"totalBytes\":%u,\"chunkBytes\":%u,\"mode\":\"%s\",\"requestId\":\"%s\",\"bootId\":\"%s\"}}",
    id,
    g_screenshot.width,
    g_screenshot.height,
    (unsigned)(g_screenshot.width*g_screenshot.height*2),
    (unsigned)PET_P4_SCREENSHOT_RAW_CHUNK,
    g_screenshot.pull?"pull-v1":"push",g_screenshot.request_id,pet_p4_boot_identity()
  );
  bool sent=screenshot_send_line(begin,reply_ctx);
  if(g_screenshot.pull){
    /* A lost/busy begin may be observed again only through its exact nonce,
     * route and dimensions. Other requests cannot steal/re-capture this image. */
    if(!sent&&!pet_screenshot_tx_unwritten_busy(&g_screenshot_tx))screenshot_release_as(SCREENSHOT_RELEASE_TRANSPORT);
    return true;
  }

  size_t total = capture_w*capture_h*2;
  int index = 0;
  size_t sent_bytes=0;
  for (size_t offset = 0; sent&&offset < total; offset += PET_P4_SCREENSHOT_RAW_CHUNK) {
    size_t n = total - offset;
    if (n > PET_P4_SCREENSHOT_RAW_CHUNK) n = PET_P4_SCREENSHOT_RAW_CHUNK;
    sent=send_screenshot_chunk(
      id,
      index,
      raw + offset,
      n,
      b64,
      ((PET_P4_SCREENSHOT_RAW_CHUNK + 2) / 3) * 4 + 8,
      reply_ctx
    );
    if(!sent)break;
    index++;sent_bytes+=n;
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  char end[192];
  snprintf(
    end,
    sizeof(end),
    "{\"topic\":\"debug/screenshot_end\",\"payload\":{\"id\":\"%s\",\"chunks\":%d,\"totalBytes\":%u,\"nonBlack\":%u,\"checksum\":\"%08x\"}}",
    id,
    index,
    (unsigned)sent_bytes,
    non_black,
    checksum
  );
  if(!sent||!screenshot_send_line(end,reply_ctx))screenshot_error(id,"transport or encode failed",index,sent_bytes,reply_ctx);
  screenshot_release();
  return true;
}

/* Only protocol_worker_task owns these scratch buffers. Copy under the TX
 * mutex; encode/send after unlocking, so the audit cannot recurse into it. */
static EXT_RAM_BSS_ATTR pet_transport_audit_snapshot_t g_transport_audit_snapshot;
static EXT_RAM_BSS_ATTR char g_transport_audit_reply[PET_TRANSPORT_AUDIT_REPLY_BYTES];
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("Os")))
#endif
static bool maybe_handle_transport_audit(const char *line, pet_p4_send_line_fn send, void *ctx) {
  if (!line || !send || !strstr(line, "debug/transport_audit")) return false;
  cJSON *request = cJSON_ParseWithOpts(line, NULL, true);
  const cJSON *topic = cJSON_GetObjectItemCaseSensitive(request, "topic");
  if (!cJSON_IsString(topic)) { cJSON_Delete(request); return false; }
  bool start = !strcmp(topic->valuestring, "debug/transport_audit_start");
  bool read = !strcmp(topic->valuestring, "debug/transport_audit_read");
  bool reply_read = !strcmp(topic->valuestring,"debug/transport_audit_reply_read");
  if (!start && !read && !reply_read) { cJSON_Delete(request); return false; }
  const cJSON *payload = cJSON_GetObjectItemCaseSensitive(request, "payload");
  bool valid = cJSON_IsObject(request) && cJSON_GetArraySize(request) == 2
    && cJSON_IsObject(payload) && cJSON_GetArraySize(payload) == 0;
  cJSON_Delete(request);
  if (!valid || !g_transport_mutex) {
    send("{\"topic\":\"debug/transport_audit_result\",\"payload\":{\"ok\":false,\"error\":\"invalid_request_or_unavailable\"}}", ctx);
    return true;
  }
  if (screenshot_audio_busy()) {
    xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
    pet_transport_audit_disarm(&g_transport_audit);
    xSemaphoreGive(g_transport_mutex);
    send("{\"topic\":\"debug/transport_audit_result\",\"payload\":{\"ok\":false,\"error\":\"audio_busy\"}}", ctx);
    return true;
  }
  if(reply_read){
    xSemaphoreTake(g_transport_mutex,portMAX_DELAY);
    bool available=g_transport_reply_observed;
    if(available)pet_transport_audit_close(&g_transport_reply_audit,&g_transport_audit_snapshot);
    xSemaphoreGive(g_transport_mutex);
    if(!available)send("{\"topic\":\"debug/transport_audit_reply_result\",\"payload\":{\"ok\":false,\"error\":\"reply_unavailable\"}}",ctx);
    else if(pet_transport_audit_encode_reply(&g_transport_audit_snapshot,g_transport_audit_reply,sizeof(g_transport_audit_reply)))
      send(g_transport_audit_reply,ctx);
    else send("{\"topic\":\"debug/transport_audit_reply_result\",\"payload\":{\"ok\":false,\"error\":\"encode_failed\"}}",ctx);
  }else if (start) {
    xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
    uint32_t session = pet_transport_audit_prepare(&g_transport_audit);
    xSemaphoreGive(g_transport_mutex);
    if (!session) {
      send("{\"topic\":\"debug/transport_audit_result\",\"payload\":{\"ok\":false,\"error\":\"session_exhausted\"}}", ctx);
      return true;
    }
    snprintf(g_transport_audit_reply, sizeof(g_transport_audit_reply),
      "{\"topic\":\"debug/transport_audit_started\",\"payload\":{\"ok\":true,\"session\":%u,\"budget\":%u,\"capacity\":%u}}",
      (unsigned)session, PET_TRANSPORT_AUDIT_BUDGET, PET_TRANSPORT_AUDIT_RECORDS);
    send(g_transport_audit_reply, ctx); /* Still disarmed; never captures its ACK. */
    xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
    (void)pet_transport_audit_arm(&g_transport_audit, session);
    xSemaphoreGive(g_transport_mutex);
  } else {
    xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
    pet_transport_audit_close(&g_transport_audit, &g_transport_audit_snapshot);
    g_transport_reply_observed=false;
    uint32_t reply_session=pet_transport_audit_prepare(&g_transport_reply_audit);
    if(reply_session)(void)pet_transport_audit_arm(&g_transport_reply_audit,reply_session);
    xSemaphoreGive(g_transport_mutex);
    if (pet_transport_audit_encode(&g_transport_audit_snapshot, g_transport_audit_reply,
        sizeof(g_transport_audit_reply))) send(g_transport_audit_reply, ctx);
    else send("{\"topic\":\"debug/transport_audit_result\",\"payload\":{\"ok\":false,\"error\":\"encode_failed\"}}", ctx);
    /* Other transport callbacks may not provide observations. Never leave an
     * unconsumed capture armed for some future, unrelated response. */
    xSemaphoreTake(g_transport_mutex,portMAX_DELAY);
    pet_transport_audit_disarm(&g_transport_reply_audit);
    xSemaphoreGive(g_transport_mutex);
  }
  return true;
}

static bool enqueue_protocol_line(
  const char *line,
  pet_p4_send_line_fn send_line,
  void *reply_ctx,
  void *enqueue_ctx
) {
  (void) enqueue_ctx;
  if (!line || !line[0] || !send_line || !g_protocol_queue) return false;
  /* Storage still owns the mutation lane. Keep only the explicit RAM query
   * whitelist responsive on the existing RX task; no extra task or replay. */
  if (atomic_load_explicit(&g_storage_query_window,memory_order_acquire)
      && xSemaphoreTake(g_state_mutex,pdMS_TO_TICKS(20))==pdTRUE) {
    pet_p4_query_reply_t reply={0};
    bool handled=atomic_load_explicit(&g_storage_query_window,memory_order_acquire)
      &&pet_p4_handle_storage_query(&g_state,line,&reply);
    xSemaphoreGive(g_state_mutex);
    if (handled) {
      if (reply.line) { send_line(reply.line,reply_ctx); cJSON_free(reply.line); }
      return true;
    }
    cJSON_free(reply.line);
  }
  size_t line_len = strlen(line);
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(enqueue_phase,PET_PHASE_ENQUEUE_ALLOC);
#endif
  char *line_copy = (char *) heap_caps_malloc(
    line_len + 1,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!line_copy) line_copy = (char *) malloc(line_len + 1);
  if (!line_copy) return false;
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_ENQUEUE_COPY);
#endif
  memcpy(line_copy, line, line_len + 1);
  pet_p4_protocol_request_t request = {
    .line = line_copy,
    .send_line = send_line,
    .reply_ctx = reply_ctx,
  };
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_ENQUEUE_QUEUE);
#endif
  if (xQueueSend(g_protocol_queue, &request, pdMS_TO_TICKS(20)) == pdTRUE) {
    return true;
  }
  free(line_copy);
  send_line(
    "{\"topic\":\"protocol/error\",\"payload\":{\"ok\":false,\"error\":\"command_queue_busy\"}}",
    reply_ctx
  );
  return false;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline,optimize("Os")))
#endif
static bool protocol_debug_source_bounded(const char *line){
  if(!line||!strstr(line,"debug/"))return false;
  return pet_p4_protocol_source_valid(line);
}

/* Input never waits for storage or a full protocol queue. A failed enqueue
 * leaves the current page/runtime intact; the miniapp owns token retirement. */
static bool enqueue_component_activation(uint64_t token, void *context) {
  (void)context;
  if (!token || !g_protocol_queue) return false;
  pet_p4_protocol_request_t request = {.activation_token = token};
  return xQueueSend(g_protocol_queue, &request, 0) == pdTRUE;
}

static bool enqueue_catalog_cursor(uint64_t token, void *context) {
  (void)context;
  if (!token || !g_protocol_queue) return false;
  pet_p4_protocol_request_t request = {.cursor_token = token};
  return xQueueSend(g_protocol_queue, &request, 0) == pdTRUE;
}

static void run_catalog_cursor(uint64_t token) {
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
#if PET_P4_GAP_PEAK_HOLDER
  g_worker_locked_topic_hash = 0;
  worker_locked_begin();
#endif
  if (pet_p4_miniapp_storage_lane_enter()) {
    (void)pet_p4_miniapp_catalog_cursor_run(token);
    bool left = pet_p4_miniapp_storage_lane_leave();
    configASSERT(left);
    (void)left;
  }
#if PET_P4_GAP_PEAK_HOLDER
  worker_locked_end();
#endif
  xSemaphoreGive(g_state_mutex);
}

static void run_component_activation(uint64_t token) {
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  pet_p4_miniapp_activation_guard(!strcmp(g_state.screen_page, "components")
    && !pet_p4_conversation_active(&g_state) && !g_state.asset_transfer_active
    && !pet_p4_ota_transfer_active());
  bool entered = pet_p4_miniapp_storage_lane_enter();
  pet_p4_miniapp_activation_result_t result = PET_MINIAPP_ACTIVATION_FAILED;
  if (entered) {
    result = pet_p4_miniapp_activation_run(token);
    /* run may lend the state mutex during I/O; navigation can have changed. */
    pet_p4_miniapp_activation_guard(!strcmp(g_state.screen_page, "components")
      && !pet_p4_conversation_active(&g_state) && !g_state.asset_transfer_active
      && !pet_p4_ota_transfer_active());
    if ((result == PET_MINIAPP_ACTIVATION_OPENED
        || result == PET_MINIAPP_ACTIVATION_OPENED_UNSAVED)
        && pet_p4_miniapp_activation_navigation_current(token)) {
      pet_p4_miniapp_sync_stats(&g_state.stats);
      snprintf(g_state.screen_page, sizeof(g_state.screen_page), "app");
      g_state.last_update_ms += 1;
    }
    bool left = pet_p4_miniapp_storage_lane_leave();
    configASSERT(left);
    (void)left;
  } else (void)pet_p4_miniapp_activation_reject_not_started(token);
  xSemaphoreGive(g_state_mutex);
  if (result == PET_MINIAPP_ACTIVATION_FAILED)
    ESP_LOGW(TAG, "component activation failed; previous runtime preserved");
  else if (result == PET_MINIAPP_ACTIVATION_OPENED_UNSAVED)
    ESP_LOGW(TAG, "component opened; navigation selection persistence unavailable");
}

/* Fixed trusted vectors only: never accept bytecode, grants or user state. */
static bool maybe_handle_program_main_probe_line(const char *line,pet_p4_send_line_fn send,void *ctx){
  if(!line||!send||!strstr(line,"debug/program_main_probe"))return false;
  cJSON *root=cJSON_ParseWithOpts(line,NULL,true);
  const cJSON *topic=cJSON_GetObjectItemCaseSensitive(root,"topic");
  if(!cJSON_IsString(topic)||strcmp(topic->valuestring,"debug/program_main_probe")){cJSON_Delete(root);return false;}
  const cJSON *payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
  bool valid=cJSON_IsObject(root)&&cJSON_GetArraySize(root)==2&&cJSON_IsObject(payload)&&!cJSON_GetArraySize(payload);
  cJSON_Delete(root);
  bool stable_reply=(send==transport_send_line&&(!ctx||ctx==&g_uart_route||ctx==&g_usb_serial_jtag_route))
    ||(send==pet_p4_native_usb_send_json_line&&!ctx);
  bool busy=false;
  if(valid&&stable_reply){xSemaphoreTake(g_state_mutex,portMAX_DELAY);busy=g_state.asset_transfer_active
    ||pet_p4_ota_transfer_active()||pet_p4_diagnostics_reboot_pending();xSemaphoreGive(g_state_mutex);}
  if(!valid||!stable_reply||busy){
    const char*error=!valid?"invalid_request":!stable_reply?"unsupported_reply":"transfer_busy";
    char reply[192];snprintf(reply,sizeof(reply),"{\"topic\":\"debug/program_main_result\",\"payload\":{\"ok\":false,\"error\":\"%s\",\"samples\":0}}",error);send(reply,ctx);return true;
  }
  pet_p4_program_main_probe(send,ctx);return true;
}
static bool maybe_handle_program_session_probe_line(const char *line,pet_p4_send_line_fn send,void *ctx){
  if(!line||!strstr(line,"debug/program_session_probe"))return false;
  cJSON *root=cJSON_ParseWithOpts(line,NULL,true);
  const cJSON *topic=cJSON_GetObjectItemCaseSensitive(root,"topic");
  if(!cJSON_IsString(topic)||strcmp(topic->valuestring,"debug/program_session_probe")){cJSON_Delete(root);return false;}
  const cJSON *payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
  bool valid=cJSON_IsObject(root)&&cJSON_GetArraySize(root)==2&&cJSON_IsObject(payload)&&!cJSON_GetArraySize(payload);
  cJSON_Delete(root);
  bool busy=false;
  if(valid){xSemaphoreTake(g_state_mutex,portMAX_DELAY);busy=g_state.asset_transfer_active
    ||pet_p4_ota_transfer_active()||pet_p4_diagnostics_reboot_pending();xSemaphoreGive(g_state_mutex);}
  if(!valid||busy){send(busy?"{\"topic\":\"debug/program_session_result\",\"payload\":{\"ok\":false,\"error\":\"transfer_busy\",\"samples\":0}}":"{\"topic\":\"debug/program_session_result\",\"payload\":{\"ok\":false,\"error\":\"invalid_request\",\"samples\":0}}",ctx);return true;}
  pet_p4_program_session_probe(send,ctx);return true;
}
static bool maybe_handle_program_probe_line(const char *line, pet_p4_send_line_fn send, void *ctx) {
  if (!line || (!strstr(line, "debug/program_probe")&&!strstr(line,"debug/program_publication_probe"))) return false;
  cJSON *root=cJSON_ParseWithOpts(line,NULL,true);
  const cJSON *topic=cJSON_GetObjectItemCaseSensitive(root,"topic");
  bool publication=cJSON_IsString(topic)&&!strcmp(topic->valuestring,"debug/program_publication_probe");
  if (!cJSON_IsString(topic) || (!publication&&strcmp(topic->valuestring,"debug/program_probe"))) {
    cJSON_Delete(root);return false;
  }
  const cJSON *payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
  bool valid=cJSON_IsObject(root) && cJSON_GetArraySize(root)==2
    && cJSON_IsObject(payload) && cJSON_GetArraySize(payload)==0;
  cJSON_Delete(root);
  if (!valid) {
    send(publication?"{\"topic\":\"debug/program_publication_result\",\"payload\":{\"ok\":false,\"error\":\"invalid_request\",\"samples\":0}}":"{\"topic\":\"debug/program_probe_result\",\"payload\":{\"ok\":false,\"error\":\"invalid_request\",\"cases\":[]}}",ctx);
    return true;
  }
  xSemaphoreTake(g_state_mutex,portMAX_DELAY);
  bool busy=g_state.asset_transfer_active || pet_p4_ota_transfer_active()
    || pet_p4_diagnostics_reboot_pending();
  xSemaphoreGive(g_state_mutex);
  if (busy) send(publication?"{\"topic\":\"debug/program_publication_result\",\"payload\":{\"ok\":false,\"error\":\"transfer_busy\",\"samples\":0}}":"{\"topic\":\"debug/program_probe_result\",\"payload\":{\"ok\":false,\"error\":\"transfer_busy\",\"cases\":[]}}",ctx);
  else if(publication)pet_p4_program_publication_probe(send,ctx);
  else pet_p4_program_probe(send,ctx);
  return true;
}

#if PET_P4_QUERY_REPLY_TRACE
static pet_p4_query_timing_t g_query_timing;
#if PET_P4_QUERY_OBSERVATION
static portMUX_TYPE g_query_observation_mux=portMUX_INITIALIZER_UNLOCKED;
static pet_p4_query_observation_t g_query_observation;
bool pet_p4_protocol_query_observation(pet_p4_query_observation_t *out){
  if(!out)return false;
  portENTER_CRITICAL(&g_query_observation_mux);*out=g_query_observation;
  portEXIT_CRITICAL(&g_query_observation_mux);return true;
}
static void query_native_begin(uint8_t kind){
  uint64_t now=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&g_query_observation_mux);pet_p4_query_observation_begin(&g_query_observation,now);
  g_query_observation.native_kind=kind;portEXIT_CRITICAL(&g_query_observation_mux);
}
static void query_native_end(void){
  uint64_t now=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&g_query_observation_mux);pet_p4_query_observation_unlock(&g_query_observation,now);
  pet_p4_query_observation_end(&g_query_observation,now);portEXIT_CRITICAL(&g_query_observation_mux);
}
#endif
bool pet_p4_protocol_query_timing(pet_p4_query_timing_t *out) {
  if (!out || !g_protocol_task || xTaskGetCurrentTaskHandle() != g_protocol_task)
    return false;
  *out = g_query_timing;
  return true;
}
#endif

static bool volume_worker_idle(void) {
  return uxQueueMessagesWaiting(g_protocol_queue)==0 && pet_p4_audio_master_idle();
}
static void volume_worker_idle_commit(void) {
  if (!pet_audio_volume_pending() || !volume_worker_idle()
      || xSemaphoreTake(g_state_mutex,0)!=pdTRUE) return;
  bool idle=!g_state.asset_transfer_active && !pet_p4_ota_transfer_active()
    && !pet_p4_diagnostics_reboot_pending() && !pet_p4_miniapp_storage_busy()
    && !pet_p4_conversation_active(&g_state);
  xSemaphoreGive(g_state_mutex);
  /* No state/stream/model mutex is held across NVS. Existing worker only. */
  pet_audio_volume_idle_commit((uint64_t)esp_timer_get_time()/1000ULL,idle,volume_worker_idle);
}

static void protocol_worker_task(void *arg) {
  (void) arg;
#if PET_P4_RESET_PHASE_TRACE
  (void)pet_reset_phase_bind(PET_RESET_PROTOCOL);
#endif
#if PET_P4_SCHED_TRACE_ENABLE
  bool trace_registered=pet_sched_trace_register(PET_SCHED_TASK_PROTOCOL,xTaskGetCurrentTaskHandle());
  configASSERT(trace_registered);(void)trace_registered;
#endif
#if PET_P4_STORAGE_IO_TRACE_ENABLE
  bool io_registered=pet_storage_io_register(PET_STORAGE_IO_PROTOCOL,xTaskGetCurrentTaskHandle());
  configASSERT(io_registered);(void)io_registered;
#endif
  pet_p4_protocol_request_t request;
  while (true) {
    pet_p4_program_main_probe_reply();
    /* Only wake for snapshot expiry while a pull snapshot actually exists. */
    TickType_t wait=pet_p4_program_main_probe_pending()?pdMS_TO_TICKS(20)
      :g_screenshot.raw?pdMS_TO_TICKS(100):portMAX_DELAY;
    if (pet_audio_volume_pending() && wait>pdMS_TO_TICKS(1000)) wait=pdMS_TO_TICKS(1000);
    if(wait>pdMS_TO_TICKS(1000))wait=pdMS_TO_TICKS(1000);
    asset_idle_worker();
    if (xQueueReceive(g_protocol_queue, &request, wait) != pdTRUE){
      screenshot_poll();volume_worker_idle_commit();continue;
    }
    screenshot_poll();
#if PET_P4_HELLO_ASYNC
    if(request.unsolicited_hello){
      configASSERT(!request.line&&!request.activation_token&&!request.cursor_token&&!request.send_line&&!request.reply_ctx);
#if PET_P4_QUERY_OBSERVATION
      query_native_begin(PET_QUERY_HELLO_NATIVE);
#endif
      run_unsolicited_hello();
#if PET_P4_QUERY_OBSERVATION
      query_native_end();
#endif
      continue;
    }
#endif
    if (request.cursor_token) {
      run_catalog_cursor(request.cursor_token);
      continue;
    }
    if (request.activation_token) {
#if PET_P4_QUERY_OBSERVATION
      query_native_begin(PET_QUERY_ACTIVATION_NATIVE);
#endif
      run_component_activation(request.activation_token);
#if PET_P4_QUERY_OBSERVATION
      query_native_end();
#endif
      continue;
    }
    /* Retired ordinary native JSON must not reach debug/trace/state dispatch. */
    if (request.send_line == pet_p4_native_usb_send_json_line
        && !pet_p4_native_usb_route_current(request.reply_ctx)) {
      free(request.line); continue;
    }
#if PET_P4_QUERY_REPLY_TRACE
    uint64_t query_begin=(uint64_t)esp_timer_get_time();
#if PET_P4_QUERY_OBSERVATION
    portENTER_CRITICAL(&g_query_observation_mux);pet_p4_query_observation_begin(&g_query_observation,query_begin);
    g_query_observation.native_kind=PET_QUERY_DEBUG_NATIVE;portEXIT_CRITICAL(&g_query_observation_mux);
#endif
#endif
    /* Debug handlers run before the ordinary dispatcher. Apply the same
     * nonrecursive source preflight BEFORE any of their cJSON parsers too.
     * Invalid input falls through to the existing invalid_json ACK, not an
     * attempted suffix repair, state change or diagnostic operation. */
    bool bounded_debug_source=protocol_debug_source_bounded(request.line);
    bool handled_debug = bounded_debug_source && (maybe_handle_transport_audit(request.line,request.send_line,request.reply_ctx)
      || pet_p4_sched_trace_rpc_handle(request.line,request.send_line,request.reply_ctx)
      || pet_storage_io_rpc_handle(request.line,request.send_line,request.reply_ctx)
      || maybe_handle_program_probe_line(request.line,request.send_line,request.reply_ctx)
      || maybe_handle_program_session_probe_line(request.line,request.send_line,request.reply_ctx)
      || maybe_handle_program_main_probe_line(request.line,request.send_line,request.reply_ctx)
      || (request.send_line == transport_send_line
          && maybe_handle_debug_line(request.line, request.reply_ctx)));
#if PET_P4_QUERY_OBSERVATION
    if(handled_debug)query_native_end();
    else{portENTER_CRITICAL(&g_query_observation_mux);g_query_observation.native_kind=PET_QUERY_ORDINARY;portEXIT_CRITICAL(&g_query_observation_mux);}
#endif
    if (!handled_debug) {
      pet_p4_query_reply_t query_reply = {0};
#if PET_P4_RESET_PHASE_TRACE
      PET_RESET_PHASE_SCOPE(protocol_state_phase,PET_PHASE_STATE_WAIT);
#endif
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
#if PET_P4_GAP_PEAK_HOLDER
      g_worker_locked_topic_hash=pet_line_topic_hash(request.line);
      worker_locked_begin();
#endif
#if PET_P4_RESET_PHASE_TRACE
      pet_reset_phase_set(PET_PHASE_STATE_HELD);
#endif
#if PET_P4_QUERY_REPLY_TRACE
      uint64_t query_locked = (uint64_t)esp_timer_get_time();
#endif
      bool storage_command = pet_p4_miniapp_storage_lane_enter();
      g_protocol_storage_command_entered=storage_command;
#if PET_P4_QUERY_REPLY_DEFERRED || PET_P4_QUERY_REPLY_TRACE || PET_P4_MUTATION_ACK_DEFERRED
#if PET_P4_MUTATION_ACK_DEFERRED
      pet_p4_protocol_set_deferred_reply(&query_reply);
#endif
      pet_p4_handle_line_with_reply(&g_state, request.line, request.send_line,
        request.reply_ctx, &query_reply);
#if PET_P4_MUTATION_ACK_DEFERRED
      pet_p4_protocol_set_deferred_reply(NULL);
#endif
#else
      pet_p4_handle_line(&g_state, request.line, request.send_line, request.reply_ctx);
#endif
      pet_p4_miniapp_activation_guard(!strcmp(g_state.screen_page, "components")
        && !pet_p4_conversation_active(&g_state) && !g_state.asset_transfer_active
        && !pet_p4_ota_transfer_active());
      if (storage_command) {
        bool storage_released = pet_p4_miniapp_storage_lane_leave();
        configASSERT(storage_released);
        (void)storage_released;
      }
      g_protocol_storage_command_entered=false;
#if PET_P4_GAP_PEAK_HOLDER
      worker_locked_end();
#endif
      xSemaphoreGive(g_state_mutex);
#if PET_P4_GAP_PEAK_HOLDER
      g_worker_locked_topic_hash=0;
#endif
#if PET_P4_RESET_PHASE_TRACE
      pet_reset_phase_set(PET_PHASE_IDLE);
#endif
#if PET_P4_QUERY_REPLY_TRACE
      uint64_t query_unlocked = (uint64_t)esp_timer_get_time();
#if PET_P4_QUERY_OBSERVATION
      portENTER_CRITICAL(&g_query_observation_mux);pet_p4_query_observation_unlock(&g_query_observation,query_unlocked);
      portEXIT_CRITICAL(&g_query_observation_mux);
#endif
#endif
      /* The original route and full serialization remain owned by THIS
       * dispatch. No stack callback/context is registered with media/hosts.
       * TX pacing/drains may wait, but cannot retain the state/diagnostic lock.
       * No queue, replay, second dispatch, ACK duplication or new task. */
      if (query_reply.line) {
        request.send_line(query_reply.line, request.reply_ctx);
        cJSON_free(query_reply.line);
      }
#if PET_P4_QUERY_REPLY_TRACE
#if PET_P4_QUERY_OBSERVATION
      uint64_t query_observed_end=(uint64_t)esp_timer_get_time();
      portENTER_CRITICAL(&g_query_observation_mux);pet_p4_query_observation_end(&g_query_observation,query_observed_end);
      portEXIT_CRITICAL(&g_query_observation_mux);
#endif
      if(query_reply.diagnostic_query)pet_p4_query_timing_record(&g_query_timing,
        query_begin, query_locked, query_unlocked, (uint64_t)esp_timer_get_time(),
        PET_P4_QUERY_REPLY_DEFERRED != 0);
#endif
    }
    free(request.line);
  }
}

static bool consume_byte(
  char ch,
  char *line_buffer,
  size_t *line_used,
  size_t capacity,
  void *reply_ctx
) {
  if (!line_buffer || !line_used || capacity < 2) return false;
  /* A rejected frame owns the rest of its line. Never reinterpret a suffix as
   * a new command after overflow or an embedded C-string terminator. Each
   * transport keeps its own sentinel in line_used until the next newline. */
  /* A final CR after the maximum legal body needs no extra buffer byte.
   * This sentinel admits only the immediately following LF. */
  if(*line_used==capacity+1){
    if(ch=='\n')*line_used=capacity-1;
    else{*line_used=capacity;return false;}
  }
  if (*line_used >= capacity) {
    if (ch == '\n') *line_used = 0;
    return false;
  }
  if (ch == '\0') {
    *line_used = capacity;
    return false;
  }
  if(ch=='\r'&&*line_used+1==capacity){*line_used=capacity+1;return false;}
  if (ch == '\n') {
    /* CR is legal only as the single line terminator in CRLF. Never repair
     * a malformed topic/string by deleting a CR from the message body. */
    if(*line_used&&line_buffer[*line_used-1]=='\r')--*line_used;
    if(memchr(line_buffer,'\r',*line_used)){*line_used=0;return false;}
    line_buffer[*line_used] = '\0';
    if (*line_used > 0) {
      (void) enqueue_protocol_line(
        line_buffer,
        transport_send_line,
        reply_ctx,
        NULL
      );
    }
    *line_used = 0;
    return true; /* line boundary: caller rechecks raw ownership under lock */
  }
  if (*line_used + 1 < capacity) {
    line_buffer[(*line_used)++] = ch;
  } else {
    *line_used = capacity;
  }
  return false;
}

static void mark_usb_protocol_seen(void) {
  portENTER_CRITICAL(&g_rx_health_mutex);
  g_usb_protocol_seen = true;
  portEXIT_CRITICAL(&g_rx_health_mutex);
}

static bool usb_protocol_seen(void) {
  bool seen;
  portENTER_CRITICAL(&g_rx_health_mutex);
  seen = g_usb_protocol_seen;
  portEXIT_CRITICAL(&g_rx_health_mutex);
  return seen;
}

static void consume_transport_bytes(
  const uint8_t *data,
  size_t len,
  char *line_buffer,
  size_t *line_used,
  size_t line_capacity,
  bool *raw_mode,
  void *reply_ctx
) {
  size_t offset = 0;bool check_mode=true;
  while (offset < len) {
    if(check_mode){
      xSemaphoreTake(g_state_mutex,portMAX_DELAY);
      *raw_mode=pet_p4_raw_asset_chunk_active_for_route(transport_send_line,reply_ctx);
      xSemaphoreGive(g_state_mutex);check_mode=false;
    }
    if (*raw_mode) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      pet_appearance_commit_io_t raw_io={raw_asset_rx_io_begin,raw_asset_rx_io_end,raw_asset_rx_current,xTaskGetCurrentTaskHandle()};
      size_t consumed = pet_p4_consume_raw_asset_bytes_scoped(
        &g_state,
        data + offset,
        len - offset,
        transport_send_line,
        reply_ctx, &raw_io
      );
      xSemaphoreGive(g_state_mutex);
      if (consumed > 0) {
        offset += consumed;
      }
      if (consumed == 0) {
        *raw_mode = false;check_mode=true;
      }
      continue;
    }
    if (consume_byte((char) data[offset], line_buffer, line_used, line_capacity, reply_ctx)) {
      check_mode = true;
    }
    offset += 1;
  }
}

static esp_err_t init_storage(void) {
  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = "storage",
    .max_files = PET_P4_SPIFFS_MAX_FILES,
    .format_if_mount_failed = false,
  };
  return esp_vfs_spiffs_register(&conf);
}

static esp_err_t init_usb(void) {
  if (usb_serial_jtag_is_driver_installed()) return ESP_OK;
  usb_serial_jtag_driver_config_t config = {
    .tx_buffer_size = 4096,
    .rx_buffer_size = 32768,
  };
  return usb_serial_jtag_driver_install(&config);
}

static esp_err_t init_uart(void) {
  uart_config_t config = {
    .baud_rate = PET_P4_UART_BAUD,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_RETURN_ON_ERROR(uart_param_config(PET_P4_UART_NUM, &config), TAG, "configure UART0");
  ESP_RETURN_ON_ERROR(
    uart_set_pin(PET_P4_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
    TAG,
    "configure UART0 pins"
  );
  esp_err_t err = uart_driver_install(
    PET_P4_UART_NUM,
    PET_P4_UART_RX_BUFFER_BYTES,
    4096,
    0,
    NULL,
    0
  );
  if (err == ESP_ERR_INVALID_STATE) return ESP_OK;
  return err;
}

static void usb_rx_task(void *arg) {
  (void) arg;
  uint8_t buf[512];
  bool raw_mode = false;
  while (true) {
    int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (len >= 0) mark_rx_healthy(true);
    if (len > 0) {
      mark_usb_protocol_seen();
      consume_transport_bytes(
        buf,
        (size_t) len,
        g_line_buffer,
        &g_line_used,
        PET_P4_PROTOCOL_LINE_BUFFER_BYTES,
        &raw_mode,
        (void *) &g_usb_serial_jtag_route
      );
    }
  }
}

static void uart_rx_task(void *arg) {
  (void) arg;
#if PET_P4_RESET_PHASE_TRACE
  (void)pet_reset_phase_bind(PET_RESET_UART_RX);
#endif
  uint8_t buf[PET_P4_UART_READ_CHUNK_BYTES];
  bool raw_mode = false;
  while (true) {
#if PET_P4_RESET_PHASE_TRACE
    PET_RESET_PHASE_SCOPE(rx_phase,PET_PHASE_RX_WAIT);
#endif
    int len = uart_read_bytes(PET_P4_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
    if (len >= 0) mark_rx_healthy(false);
    if (len > 0) {
#if PET_P4_RESET_PHASE_TRACE
      pet_reset_phase_set(PET_PHASE_RX_DISPATCH);
#endif
      consume_transport_bytes(
        buf,
        (size_t) len,
        g_uart_line_buffer,
        &g_uart_line_used,
        PET_P4_PROTOCOL_LINE_BUFFER_BYTES,
        &raw_mode,
        (void *) &g_uart_route
      );
    }
  }
}

#if PET_P4_GAP_PEAK_CONTEXT
/* Diagnostic wall clock for the main-loop gap context only; the serialized
 * model turn still samples its own now_ms exactly once after the lock. */
static inline int64_t pet_p4_gap_clock_us(void){return esp_timer_get_time();}
#endif
void app_main(void) {
  pet_boot_dma_link_anchor();
  (void)pet_boot_trace_begin();
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_boot();
#endif
#if PET_P4_NATIVE_PANIC_TRACE
  pet_native_panic_boot();
#endif
#if PET_P4_RESET_PHASE_BENCHMARK
  pet_reset_cost_boot();
#endif
#if PET_P4_SCHED_TRACE_ENABLE
  bool trace_registered=pet_sched_trace_register(PET_SCHED_TASK_MAIN,xTaskGetCurrentTaskHandle());
  configASSERT(trace_registered);(void)trace_registered;
#endif
#if PET_P4_STORAGE_IO_TRACE_ENABLE
  bool io_registered=pet_storage_io_register(PET_STORAGE_IO_MAIN,xTaskGetCurrentTaskHandle());
  configASSERT(io_registered);(void)io_registered;
#endif
  unsigned long long last_logged_update = 0;
  unsigned long long last_hello_ms = 0;
  unsigned long long last_render_ms = 0;
  unsigned long long last_transfer_render_ms = 0;
  unsigned long long last_lcd_keepalive_ms = 0;
  unsigned long long last_rx_health_warning_ms = 0;
  bool transfer_was_active = false;
  bool runtime_render_healthy = false;
  char lcd_status[192];
  char i2c_devices[128];
  char board_device_id[PET_P4_DEVICE_ID_MAX];
  esp_err_t backlight_hide_err = pet_p4_lcd_prepare_boot();
  if (backlight_hide_err != ESP_OK) {
    ESP_LOGW(TAG, "LCD boot backlight could not be hidden: %s", esp_err_to_name(backlight_hide_err));
  }
  ESP_ERROR_CHECK(nvs_flash_init());
  g_line_buffer = heap_caps_calloc(1, PET_P4_PROTOCOL_LINE_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_uart_line_buffer = heap_caps_calloc(1, PET_P4_PROTOCOL_LINE_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_ERROR_CHECK(g_line_buffer && g_uart_line_buffer ? ESP_OK : ESP_ERR_NO_MEM);
  g_render_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(g_render_mutex ? ESP_OK : ESP_ERR_NO_MEM);
  g_transport_mutex = xSemaphoreCreateMutex();
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
  ESP_ERROR_CHECK(pet_control_sessions_init(&g_uart_route,&g_usb_serial_jtag_route)?ESP_OK:ESP_ERR_INVALID_STATE);
#endif
  ESP_ERROR_CHECK(pet_native_work_window_init()?ESP_OK:ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(g_transport_mutex ? ESP_OK : ESP_ERR_NO_MEM);
  g_state_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(g_state_mutex ? ESP_OK : ESP_ERR_NO_MEM);
  esp_log_set_vprintf(transport_log_vprintf);
#if PET_COMPONENT_PROGRAM_ENABLED
  if(!pet_p4_program_main_probe_bind_owner())ESP_LOGW(TAG,"main program diagnostic unavailable");
#endif
  esp_err_t storage_err = init_storage();
  if (storage_err != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed; preserving storage for explicit recovery: %s",
             esp_err_to_name(storage_err));
  }
  esp_err_t diagnostics_err = pet_p4_diagnostics_init();
  if (diagnostics_err != ESP_OK) {
    ESP_LOGW(TAG, "diagnostics persistence unavailable; continuing without boot history: %s",
             esp_err_to_name(diagnostics_err));
  }
  esp_err_t device_id_err = build_board_device_id(board_device_id, sizeof(board_device_id));
  (void)pet_boot_trace_mark(PET_BOOT_STORAGE_DONE);
  if (device_id_err != ESP_OK) {
    ESP_LOGE(TAG, "base MAC unavailable; using degraded device id: %s", esp_err_to_name(device_id_err));
  }
  pet_p4_state_init(&g_state, board_device_id);
#if PET_P4_HELLO_ASYNC
  snprintf(g_hello_board_id,sizeof(g_hello_board_id),"%s",board_device_id);
#endif
  pet_p4_boot_identity_start(esp_random(),esp_random());
  (void) pet_bubble_pixels_init();
  g_render_state = (pet_p4_runtime_state_t *) heap_caps_calloc(
    1,
    sizeof(*g_render_state),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!g_render_state) g_render_state = calloc(1, sizeof(*g_render_state));
  ESP_ERROR_CHECK(g_render_state ? ESP_OK : ESP_ERR_NO_MEM);
  g_protocol_queue = xQueueCreate(
    PET_P4_PROTOCOL_QUEUE_DEPTH,
    sizeof(pet_p4_protocol_request_t)
  );
  ESP_ERROR_CHECK(g_protocol_queue ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(
    pet_p4_storage_worker_create(
      protocol_worker_task,
      "pet_p4_protocol",
      PET_P4_PROTOCOL_TASK_STACK_BYTES,
      NULL,
      PET_P4_PROTOCOL_TASK_PRIORITY,
      &g_protocol_task
    ) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM
  );
  esp_err_t miniapp_err = pet_p4_miniapp_init();
  (void)pet_boot_trace_mark(PET_BOOT_CATALOG_DONE);
  if (miniapp_err != ESP_OK && miniapp_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "mini-app init failed: %s", esp_err_to_name(miniapp_err));
  }
  esp_err_t builtin_sync_err = pet_p4_miniapp_sync_builtins();
  (void)pet_boot_trace_mark(PET_BOOT_BUILTINS_DONE);
  if (builtin_sync_err != ESP_OK) {
    ESP_LOGW(TAG, "built-in component sync failed: %s", esp_err_to_name(builtin_sync_err));
  }
  ESP_ERROR_CHECK(pet_p4_miniapp_storage_lane_configure(storage_command_unlock,
    storage_command_lock, storage_command_owner, NULL) ? ESP_OK : ESP_ERR_INVALID_STATE);
  ESP_ERROR_CHECK(pet_p4_miniapp_catalog_cursor_configure(enqueue_catalog_cursor,
    NULL) ? ESP_OK : ESP_ERR_INVALID_STATE);
  ESP_ERROR_CHECK(pet_p4_protocol_asset_query_io_configure(asset_query_io_begin,
    asset_query_io_end,NULL) ? ESP_OK : ESP_ERR_INVALID_STATE);
  ESP_ERROR_CHECK(pet_p4_protocol_asset_query_pending_configure(
    asset_query_worker_pending,NULL) ? ESP_OK : ESP_ERR_INVALID_STATE);
  ESP_ERROR_CHECK(pet_p4_miniapp_activation_configure(enqueue_component_activation,
    NULL) ? ESP_OK : ESP_ERR_INVALID_STATE);
  // Parse the large appearance manifest before any USB/UART receive task can
  // concurrently parse command JSON during boot.
  pet_p4_load_asset_manifest(&g_state);
  // Load cue admission metadata before either receive task can publish audio.
  pet_audio_patch_global_refresh();
  (void)pet_boot_trace_mark(PET_BOOT_MANIFEST_DONE);
  ESP_ERROR_CHECK(init_usb());
  ESP_ERROR_CHECK(init_uart());
  xSemaphoreTake(g_transport_mutex, portMAX_DELAY);
  g_transport_ready = true;
  xSemaphoreGive(g_transport_mutex);
  int64_t font_started=esp_timer_get_time();
  ESP_ERROR_CHECK(pet_p4_font_bitmap_init()?ESP_OK:ESP_ERR_INVALID_STATE);
  ESP_LOGI(TAG,"font bitmap ready packed=%u elapsed_us=%lld",(unsigned)PET_FONT_BITMAP_PACKED,
    (long long)(esp_timer_get_time()-font_started));
  ESP_ERROR_CHECK(pet_p4_system_cues_init()?ESP_OK:ESP_ERR_INVALID_STATE);
  ESP_ERROR_CHECK(pet_p4_input_init());
  esp_err_t lcd_err = pet_p4_lcd_init();
  esp_err_t touch_err = lcd_err == ESP_OK ? pet_p4_touch_init() : ESP_ERR_INVALID_STATE;
  if (touch_err != ESP_OK) {
    ESP_LOGW(TAG, "P4 touchscreen unavailable, keeping buttons and display alive: %s",
             esp_err_to_name(touch_err));
  }
  esp_err_t audio_err = pet_p4_audio_init(board_device_id, transport_send_audio_line, NULL);
  if (audio_err != ESP_OK) {
    ESP_LOGW(TAG, "P4 microphone unavailable, keeping non-voice controls alive: %s",
             esp_err_to_name(audio_err));
  }
  esp_err_t render_err = ESP_ERR_INVALID_STATE;
#if PET_P4_HELLO_ASYNC
  /* A diagnostic bus sweep must finish before revealing the first image, not
   * freeze that visible image before frame 2. Same sweep/driver/timeouts. */
  (void)pet_p4_lcd_scan_i2c(i2c_devices,sizeof(i2c_devices));
#endif
  (void)pet_boot_trace_mark(PET_BOOT_HARDWARE_DONE);
  if (lcd_err == ESP_OK) {
    pet_p4_view_model_t boot_view;
    pet_p4_build_view_model(&g_state, &boot_view);
    render_err = pet_p4_renderer_render(&g_state, &boot_view, 0);
    if (render_err == ESP_OK) {
      render_err = pet_p4_lcd_reveal();
    }
    runtime_render_healthy = render_err == ESP_OK;
    if(runtime_render_healthy)(void)pet_boot_trace_mark(PET_BOOT_FIRST_FRAME);
  } else {
    ESP_LOGW(TAG, "LCD init failed, keep USB runtime alive: %s", esp_err_to_name(lcd_err));
  }
#if !PET_P4_HELLO_ASYNC
  (void) pet_p4_lcd_scan_i2c(i2c_devices, sizeof(i2c_devices));
#endif
  snprintf(
    lcd_status,
    sizeof(lcd_status),
    "{\"topic\":\"debug/lcd\",\"payload\":{\"init\":\"%s\",\"backlight\":\"%s\",\"render\":\"%s\",\"i2c\":\"%s\"}}",
    esp_err_to_name(lcd_err),
    esp_err_to_name(pet_p4_lcd_backlight_status()),
    esp_err_to_name(render_err),
    i2c_devices
  );
  transport_send_line(lcd_status, NULL);
  esp_err_t ota_err = pet_p4_ota_init(&g_state, transport_send_line, NULL);
  if (ota_err != ESP_OK) {
    ESP_LOGW(TAG, "A/B firmware OTA unavailable: %s", esp_err_to_name(ota_err));
  }
  esp_err_t native_usb_err = pet_p4_native_usb_init(
    &g_state,
    g_state_mutex,
    enqueue_protocol_line,
    NULL
  );
  if (native_usb_err != ESP_OK) {
    ESP_LOGW(TAG, "native USB init failed, keeping UART transports alive: %s", esp_err_to_name(native_usb_err));
  }
  // UART0 is the default Waveshare Type-C route, so reserve its task first.
  BaseType_t uart_rx_created = xTaskCreate(
    uart_rx_task,
    "pet_p4_uart_rx",
    PET_P4_TRANSPORT_RX_TASK_STACK_BYTES,
    NULL,
    PET_P4_TRANSPORT_TASK_PRIORITY,
    NULL
  );
  BaseType_t usb_rx_created = xTaskCreate(
    usb_rx_task,
    "pet_p4_usj_rx",
    PET_P4_TRANSPORT_RX_TASK_STACK_BYTES,
    NULL,
    PET_P4_TRANSPORT_TASK_PRIORITY,
    NULL
  );
  if (usb_rx_created != pdPASS || uart_rx_created != pdPASS) {
    ESP_LOGE(TAG, "transport RX task creation failed usb=%ld uart=%ld",
             (long) usb_rx_created, (long) uart_rx_created);
  }
  // Optional DSP must not take RAM needed to create display/USB tasks.
  if (pet_p4_audio_ready() && pet_p4_audio_playback_ready()) (void)pet_p4_afe_init();
  if(usb_rx_created==pdPASS&&uart_rx_created==pdPASS)
    (void)pet_boot_trace_mark(PET_BOOT_TRANSPORT_STARTED);
  ESP_LOGI(TAG, "Pet Manager ESP-P4 USB Serial/JTAG, USB-UART and native USB runtime ready");
#if !PET_P4_HELLO_ASYNC
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  pet_p4_send_hello(&g_state, transport_send_line, NULL);
  xSemaphoreGive(g_state_mutex);
#endif
  pet_p4_audio_send_status();
  if (lcd_err != ESP_OK || render_err != ESP_OK
      || usb_rx_created != pdPASS || uart_rx_created != pdPASS) {
    pet_p4_ota_runtime_failed((unsigned long long) (esp_timer_get_time() / 1000ULL));
  }

  /* All independent SPIFFS actors (asset preparation, file-cue playback and
   * app saves) now participate in the non-blocking activity observer. Package
   * mutation still uses the existing state lane. Adding an unobserved actor
   * requires disabling this optional snapshot optimization until audited. */
  pet_fs_activity_enable_observation();
#if PET_P4_HELLO_ASYNC
  /* AEC and original readiness initialization already completed. Do not emit
   * an early capability snapshot that falsely reports AEC as unavailable. */
  if(enqueue_unsolicited_hello())last_hello_ms=(unsigned long long)(esp_timer_get_time()/1000ULL);
#endif
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_IDLE);
#endif
  while (true) {
    bool render_frame = false;
    bool render_transfer_frame = false;
    bool firmware_transfer_active = false;
    bool keep_lcd_awake = false;
    pet_p4_view_model_t render_view = {0};
#if PET_P4_RESET_PHASE_TRACE
    pet_reset_phase_scope_t main_state_phase=pet_reset_phase_enter(PET_PHASE_STATE_WAIT);
#endif
#if PET_P4_GAP_PEAK_HOLDER
    /* Holder class only (no name lookup on a handle that may be released). */
    TaskHandle_t loop_holder = xSemaphoreGetMutexHolder(g_state_mutex);
    const char *loop_holder_class = !loop_holder ? "" : loop_holder == g_protocol_task ? "protocol" : "other";
    uint32_t loop_holder_topic = loop_holder == g_protocol_task ? g_worker_locked_topic_hash : 0;
#endif
#if PET_P4_GAP_PEAK_CONTEXT
    int64_t loop_wait_started_us = pet_p4_gap_clock_us();
#endif
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
#if PET_P4_GAP_PEAK_CONTEXT
    int64_t loop_held_started_us = pet_p4_gap_clock_us();
    uint32_t loop_wait_us = (uint32_t)(loop_held_started_us - loop_wait_started_us);
#endif
#if PET_P4_GAP_PEAK_HOLDER
    pet_p4_renderer_note_wait_holder(loop_wait_us, loop_holder_class, loop_holder_topic);
#endif
#if PET_P4_RESET_PHASE_TRACE
    pet_reset_phase_set(PET_PHASE_STATE_HELD);
#endif
    /* Another serialized owner may start timers while this task waits. A
     * pre-lock timestamp could precede that new lifecycle and falsely look
     * like a regressing clock. Sample once AFTER acquiring the same boundary;
     * real monotonic-clock regressions must still fail closed. */
    unsigned long long now_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
    pet_p4_diagnostics_process(now_ms, &g_state);
    pet_p4_ota_process(now_ms);
    pet_p4_asset_transfer_process(&g_state, now_ms);
    pet_p4_native_usb_process(now_ms);
#if PET_P4_RESET_PHASE_TRACE
    pet_reset_phase_scope_t program_phase=pet_reset_phase_enter(PET_PHASE_PROGRAM_STEP);
#endif
#if PET_P4_GAP_PEAK_CONTEXT
    int64_t loop_miniapp_started_us = pet_p4_gap_clock_us();
#endif
    pet_p4_miniapp_process(now_ms,!strcmp(g_state.screen_page,"app")&&!pet_p4_conversation_active(&g_state));
    pet_p4_miniapp_host_process(now_ms,!strcmp(g_state.screen_page,"app")&&!pet_p4_conversation_active(&g_state));
#if PET_P4_GAP_PEAK_CONTEXT
    uint32_t loop_miniapp_us = (uint32_t)(pet_p4_gap_clock_us() - loop_miniapp_started_us);
#endif
#if PET_P4_RESET_PHASE_TRACE
    pet_reset_phase_leave(&program_phase);
#endif
    pet_p4_state_process(&g_state, now_ms);
    pet_p4_touch_process(&g_state, transport_send_line, NULL);
    pet_p4_input_process(&g_state, transport_send_line, NULL);
    pet_p4_miniapp_activation_guard(!strcmp(g_state.screen_page, "components")
      && !pet_p4_conversation_active(&g_state) && !g_state.asset_transfer_active
      && !pet_p4_ota_transfer_active());
    pet_p4_miniapp_fruit_process(now_ms,!strcmp(g_state.screen_page,"app")
      && !pet_p4_conversation_active(&g_state) && !g_state.asset_transfer_active && !pet_p4_ota_transfer_active());
    if (!strcmp(g_state.screen_page, "touch_test")) {
      if (now_ms >= g_state.touch_test_until_ms || pet_p4_conversation_active(&g_state)) {
        snprintf(g_state.screen_page, sizeof(g_state.screen_page), "main");
        g_state.touch_test.pointer_down = false;
      } else pet_p4_slice_tick(&g_state.touch_test, now_ms);
    }
    uint8_t instrument_effect=0, instrument_ambience=0;
    bool instrument_open=!strcmp(g_state.screen_page,"app")
      &&pet_p4_miniapp_instrument_config(&instrument_effect,&instrument_ambience);
    bool slicing_open=!strcmp(g_state.screen_page,"app")&&pet_p4_miniapp_slicing_active();
    if((instrument_open||slicing_open) && pet_p4_audio_music_active())
      pet_p4_media_stop_background(transport_send_line,NULL);
    pet_p4_audio_instrument_set(instrument_open&&!g_state.asset_transfer_active
      &&!pet_p4_ota_transfer_active()&&!pet_p4_audio_conversation_active(),
      instrument_effect,instrument_ambience);
    pet_bubble_pixels_maintain(now_ms,
      pet_p4_audio_active() || pet_p4_audio_playback_active() || pet_p4_audio_music_active()
      || pet_p4_audio_conversation_active() || pet_p4_ota_active() || g_state.asset_transfer_active);
    pet_bubble_need_t need;
    if(pet_bubble_pixels_next_need(now_ms,&need)){
      char notice[256];
      int count=snprintf(notice,sizeof(notice),"{\"topic\":\"ui/text-pixels/need\",\"payload\":{\"boot\":\"%s\",\"client\":\"%s\",\"id\":\"%s\",\"style\":%u,\"page\":%u}}",need.boot,need.client,need.id,need.style,need.page);
      if(count>0&&(size_t)count<sizeof(notice))transport_send_line(notice,(void*)need.route);
    }
    /* p4-sfx-v1 cues: same preconditions as the instrument path, no AEC involvement. */
    pet_p4_miniapp_sfx_process(now_ms,!strcmp(g_state.screen_page,"app")&&!g_state.asset_transfer_active
      &&!pet_p4_ota_transfer_active()&&!pet_p4_audio_conversation_active());
#if PET_P4_EVENTS
    {
      /* p4-events-v1 background alarm: same bubble fields as p4-notify-v1 (title/text/"通知"). */
      char alarm_title[64], alarm_text[41];
      if (!pet_p4_conversation_active(&g_state) && pet_p4_miniapp_alarm_notification(alarm_title, alarm_text)) {
        snprintf(g_state.current_title, sizeof(g_state.current_title), "%s", alarm_title);
        snprintf(g_state.current_speech, sizeof(g_state.current_speech), "%s", alarm_text);
        snprintf(g_state.current_status_text, sizeof(g_state.current_status_text), "%s", "通知");
        g_state.last_update_ms = now_ms;
      }
    }
#endif
    {
#if PET_P4_RESET_PHASE_TRACE
      PET_RESET_PHASE_SCOPE(audio_phase,PET_PHASE_AUDIO_PROCESS);
#endif
      pet_p4_audio_process(&g_state, now_ms);
    }
    firmware_transfer_active = pet_p4_ota_transfer_active();
    bool transfer_active = g_state.asset_transfer_active || firmware_transfer_active;
    pet_p4_renderer_set_asset_preparation_suspended(transfer_active || pet_p4_miniapp_storage_busy()
      || pet_p4_appearance_active());
    if (transfer_was_active && !transfer_active) {
      pet_p4_renderer_reset_playback();
    }
    transfer_was_active = transfer_active;
    if (!g_state.desktop_device_id[0] && now_ms - last_hello_ms > 2000ULL) {
#if PET_P4_HELLO_ASYNC
      if(enqueue_unsolicited_hello())last_hello_ms=now_ms;
#else
      pet_p4_send_hello(&g_state, transport_send_line, NULL);
      last_hello_ms = now_ms;
#endif
    }
    if (g_state.last_update_ms != last_logged_update) {
      pet_p4_view_model_t view;
      pet_p4_build_view_model(&g_state, &view);
      // Animation updates can arrive every frame; keep them out of the normal
      // UART protocol stream and never log dialogue/Agent bubble contents.
      ESP_LOGD(
        TAG,
        "view page=%s agent=%s status=%d compact=%d realtime=%d",
        view.page,
        view.agent,
        (int) view.status,
        view.compact_bubble ? 1 : 0,
        view.realtime_conversation ? 1 : 0
      );
      last_logged_update = g_state.last_update_ms;
    }
    if (lcd_err == ESP_OK && now_ms - last_lcd_keepalive_ms >= 1000ULL) {
      keep_lcd_awake = true;
      last_lcd_keepalive_ms = now_ms;
    }
    if (lcd_err == ESP_OK && transfer_active
        && now_ms - last_transfer_render_ms >= 500ULL) {
      render_transfer_frame = true;
      last_transfer_render_ms = now_ms;
    } else if (lcd_err == ESP_OK && !transfer_active
        && now_ms - last_render_ms >= ((!strcmp(g_state.screen_page,"app") && (pet_p4_miniapp_slicing_active() || pet_p4_miniapp_media_surface_animated())) || !strcmp(g_state.screen_page, "touch_test")
          ? 33ULL : PET_P4_MAIN_RENDER_INTERVAL_MS)) {
      memcpy(g_render_state, &g_state, sizeof(*g_render_state));
      pet_p4_build_view_model(g_render_state, &render_view);
      render_view.component_loading = pet_p4_miniapp_activation_pending();
      render_frame = true;
      last_render_ms = now_ms;
    }
    xSemaphoreGive(g_state_mutex);
#if PET_P4_RESET_PHASE_TRACE
    pet_reset_phase_leave(&main_state_phase);
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
    pet_p4_program_main_probe_process();
#endif
#if PET_P4_GAP_PEAK_CONTEXT
    /* Held time ends after the passive probe cleanup that follows the unlock;
     * that cleanup is microseconds and keeps the original unlock adjacency. */
    pet_p4_renderer_note_loop(loop_wait_us,(uint32_t)(pet_p4_gap_clock_us() - loop_held_started_us),loop_miniapp_us);
#endif
    if (keep_lcd_awake) {
      esp_err_t keepalive_err = pet_p4_lcd_keep_awake();
      if (keepalive_err != ESP_OK) {
        ESP_LOGW(TAG, "LCD keepalive failed: %s", esp_err_to_name(keepalive_err));
      }
    }
    if (render_transfer_frame) {
#if PET_P4_RESET_PHASE_TRACE
      PET_RESET_PHASE_SCOPE(transfer_render_phase,PET_PHASE_RENDER);
#endif
      esp_err_t err;
      xSemaphoreTake(g_render_mutex, portMAX_DELAY);
      err = pet_p4_renderer_render_transfer_status(firmware_transfer_active, now_ms);
      xSemaphoreGive(g_render_mutex);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "transfer status render failed: %s", esp_err_to_name(err));
      }
    } else if (render_frame) {
#if PET_P4_RESET_PHASE_TRACE
      PET_RESET_PHASE_SCOPE(render_phase,PET_PHASE_RENDER);
#endif
      esp_err_t err;
      xSemaphoreTake(g_render_mutex, portMAX_DELAY);
      err = pet_p4_renderer_render(g_render_state, &render_view, now_ms);
      xSemaphoreGive(g_render_mutex);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "render failed: %s", esp_err_to_name(err));
        runtime_render_healthy = false;
        pet_p4_ota_runtime_failed(now_ms);
      } else {
        runtime_render_healthy = true;
      }
    }
    bool transports_healthy = rx_tasks_healthy();
    if (!transports_healthy && (!last_rx_health_warning_ms
        || now_ms - last_rx_health_warning_ms >= PET_P4_MAIN_RX_HEALTH_TIMEOUT_MS)) {
      ESP_LOGW(TAG, "transport RX health check failed");
      last_rx_health_warning_ms = now_ms;
    }
    if (lcd_err == ESP_OK && runtime_render_healthy && transports_healthy) {
      pet_p4_ota_runtime_ready(now_ms);
    } else {
      pet_p4_ota_runtime_failed(now_ms);
    }
    vTaskDelay(pdMS_TO_TICKS(PET_P4_MAIN_LOOP_DELAY_MS));
  }
}
