/*
 * [Input] Pet lifecycle/session state, decoded assets, bounded mini-app views,
 *         semantic shapes, and persisted RGB565-alpha sprite frames.
 * [Output] Logical RGB565 P4 frames with aspect-fit asset scaling and an
 *          optional direct-to-panel H.264 path for full-size idle playback,
 *          plus a paginated component card catalog with native icons,
 *          global-binding-neutral navigation hints, modern clean cards/HUDs, and
 *          Session-independent recording feedback on the idle main page,
 *          a lightweight transfer screen that never reads changing assets,
 *          a modal DSH approval overlay above every page,
 *          and reusable bounded H.264 decode storage isolated from immutable asset caches.
 * [Pos] ESP32-P4 display renderer.
 * [Sync] If this file changes, update `firmware/.folder.md` and renderer tests.
 */

#include "pet_p4_renderer.h"
#include "pet_p4_compact_underlay.h"
#include "pet_p4_fs_activity.h"
#include "pet_p4_frame_gate.h"
#include "pet_p4_logical_retention.h"
#include "pet_p4_render_trace.h"
#include "pet_p4_ppa_trace.h"
#include "pet_p4_task_policy.h"
#include "pet_p4_prefetch_lifetime.h"
#include "pet_p4_h264_packed.h"
#ifndef PET_P4_H264_PREFETCH
#define PET_P4_H264_PREFETCH 0
#endif
#include "pet_p4_bubble_pixels.h"

#include <limits.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
/* Routine renderer success/trace logs are DEBUG only. Both production targets
 * compile at INFO (maximum INFO), so frames and frame-tail metrics never wait
 * for serialized protocol TX merely to announce normal rendering. Keep WARN/
 * ERROR, numeric fault/lifetime metrics and all frame/gap clocks unchanged. */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "libs/tjpgd/tjpgd.h"
#include "lvgl.h"
#include "miniz.h"

#include "pet_p4_lcd.h"
#include "pet_p4_rgb565_span.h"
#include "pet_p4_rgb565_blend.h"
#include "pet_p4_rgb565_rotate.h"
#include "pet_p4_h264_pack_word.h"
#include "pet_p4_miniapp.h"
#if PET_SERVICE_BRIDGE_ENABLED
#include "pet_p4_app_host.h"
#endif
#include "pet_p4_center_layout.h"
#include "../assets/component-center/catalog.h"
#include "pet_p4_image.h"
#include "pet_p4_app_glyphs.h"
#include "esp_attr.h"
#include "pet_p4_media.h"
#include "pet_p4_audio.h"
#include "pet_p4_instrument_core.h"
#include "pet_p4_behavior.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_resources_device.h"

#define PET_P4_ASSET_WIDTH 640
static bool g_last_native_valid;
static uint32_t g_lcd_submitted, g_lcd_skipped;
static portMUX_TYPE g_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static pet_p4_render_metrics_t g_metrics;
#if PET_P4_APP_CLEAR_OPAQUE_CENTER
/* Renderer-serialized numeric counters, copied into the lifetime metrics. */
static uint32_t g_clear_center_frames,g_clear_full_frames;
#endif
#if PET_P4_APP_COLD_FAST
static uint32_t g_cold_fast_image_nodes,g_cold_fast_panel_nodes,g_cold_fast_clear_skips;
#endif
#if PET_P4_APP_PPA_COPY
static uint32_t g_ppa_copies,g_ppa_copy_fallbacks,g_ppa_copy_mismatches;static bool g_ppa_copy_disabled;
#if PET_P4_H264_WORKER_PACK
/* Render-thread owned; written only at take from job-local worker values. */
static uint32_t g_worker_packs,g_worker_pack_fallbacks,g_worker_pack_max_us,g_worker_pack_last_us;
#endif
#endif
#if PET_P4_GAP_PEAK_CONTEXT
/* Written by the main task between frames, consumed by its own next frame. */
static pet_render_gap_context_t g_loop_context;
void pet_p4_renderer_note_loop(uint32_t wait_us,uint32_t held_us,uint32_t miniapp_us){
  if(wait_us>g_loop_context.wait_max_us)g_loop_context.wait_max_us=wait_us;
  if(held_us>g_loop_context.held_max_us)g_loop_context.held_max_us=held_us;
  if(miniapp_us>g_loop_context.miniapp_max_us)g_loop_context.miniapp_max_us=miniapp_us;
  pet_p4_render_trace_add(&g_loop_context.held_total_us,held_us);
  pet_render_counter_increment(&g_loop_context.iterations);
}
#if PET_P4_GAP_PEAK_HOLDER
/* Called before note_loop raises wait_max_us, so a wait equal to or above the
 * current maximum re-attributes the context; a shorter one leaves it. */
void pet_p4_renderer_note_wait_holder(uint32_t wait_us,const char *holder,uint32_t topic_hash){
  if(wait_us<g_loop_context.wait_max_us)return;
  g_loop_context.wait_topic_hash=topic_hash;
  strncpy(g_loop_context.wait_holder,holder?holder:"",sizeof(g_loop_context.wait_holder)-1);
  g_loop_context.wait_holder[sizeof(g_loop_context.wait_holder)-1]=0;
}
#endif
#endif
pet_p4_render_metrics_t pet_p4_renderer_metrics(void) {
  portENTER_CRITICAL(&g_metrics_lock);
  pet_p4_render_metrics_t result = g_metrics;
  portEXIT_CRITICAL(&g_metrics_lock);
  return result;
}
#define PET_P4_ASSET_HEIGHT 480
#define PET_P4_MAX_MJPEG_FRAMES PET_P4_ASSET_CATALOG_MAX_FRAMES
#define PET_P4_MAX_JPEG_BYTES (512 * 1024)
#define PET_P4_TJPGD_WORK_BUFFER_BYTES 4096
#define PET_P4_HARDWARE_JPEG_TIMEOUT_MS 40
#define PET_P4_ASSET_DECODE_BACKOFF_MS 500ULL
#define PET_P4_RENDER_INTERVAL_MS 50ULL
#define PET_P4_RENDER_FRAME_BUDGET_US 66667U
#define PET_P4_RENDER_PERF_WINDOW 100U
#define PET_P4_RENDER_PERF_GAP_RESET_US 2000000LL
#define PET_P4_PPA_BUFFER_ALIGNMENT 64U
#define PET_P4_ASSET_CACHE_SLOTS 2U
#define PET_P4_ASSET_CACHE_MAX_FILE_BYTES (2U * 1024U * 1024U)
#define PET_P4_ASSET_PREPARE_CHUNK_BYTES 16384U
#ifndef PET_P4_ASSET_PREPARE_STACK_BYTES
#define PET_P4_ASSET_PREPARE_STACK_BYTES 4096U
#endif
#define PET_P4_ASSET_PREPARE_PRIORITY 1U
#define PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER 1U
#define PET_P4_H264_PRIMARY_TASK_PRIORITY PET_P4_H264_TASK_PRIORITY
#define PET_P4_H264_PACK_TASK_STACK_BYTES 3072U
#define PET_P4_H264_PACK_TASK_PRIORITY PET_P4_H264_TASK_PRIORITY
#if defined(CONFIG_ESP_H264_DUAL_TASK)
_Static_assert(CONFIG_ESP_H264_DUAL_TASK_PRIORITY == PET_P4_H264_SECONDARY_TASK_PRIORITY,
  "H264 secondary worker priority must match the reviewed policy");
_Static_assert(CONFIG_ESP_H264_DUAL_TASK_CORE == 1,
  "Secondary decode scheduling requires the dedicated second core");
#endif
#define P4_BOOT_DIAGNOSTIC_MS 3000ULL
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define PET_P4_SESSION_CARD_X 42
#define PET_P4_SESSION_CARD_WIDTH 556
#define PET_P4_SESSION_CARD_HEIGHT 120
#define PET_P4_SESSION_CARD_RADIUS 14
#define PET_P4_SESSION_CARD_GAP 8
#define PET_P4_SESSION_CARD_BOTTOM 460
#define PET_P4_COMPACT_TEXT_OPTICAL_Y (-2)
#define PET_P4_APPROVAL_CARD_WIDTH 520
#define PET_P4_APPROVAL_CARD_HEIGHT 352
#define PET_P4_APPROVAL_CARD_RADIUS 20
#define PET_P4_APPROVAL_CARD_X ((PET_P4_UI_WIDTH - PET_P4_APPROVAL_CARD_WIDTH) / 2)
#define PET_P4_APPROVAL_CARD_Y ((PET_P4_UI_HEIGHT - PET_P4_APPROVAL_CARD_HEIGHT) / 2)
#define PET_P4_PAGE_INDICATOR_X 294
#define PET_P4_PAGE_INDICATOR_Y 16
#define PET_P4_PAGE_INDICATOR_WIDTH 52
#define PET_P4_PAGE_INDICATOR_HEIGHT 20
#define PET_P4_PAGE_INDICATOR_TITLE_GAP 16
#define PET_P4_SESSION_CACHE_SENTINEL 0x0001U
#define PET_P4_SESSION_CACHE_MAX_RUNS 4U

#ifdef CONFIG_ESP_H264_DUAL_TASK
#define PET_P4_H264_DECODER_MODE "software-dual"
#else
#define PET_P4_H264_DECODER_MODE "software-single"
#endif
#define PET_P4_NATIVE_OUTPUT_BUFFER_COUNT 2

static const char *TAG = "pet-p4-renderer";

extern const lv_font_t pet_p4_font_cn_16;
#include "pet_p4_font_native32.h"

static uint16_t *g_framebuffer;
static uint16_t *g_native_framebuffer;
static uint16_t *g_native_framebuffers[PET_P4_NATIVE_OUTPUT_BUFFER_COUNT];
static unsigned int g_native_framebuffer_index;
static uint8_t *g_jpeg_input;
static uint16_t *g_jpeg_output;
static uint8_t *g_tjpgd_workbuf;
static jpeg_decoder_handle_t g_jpeg_decoder;
static ppa_client_handle_t g_ppa_srm_client;
/* Diagnostic state outlives every ISR callback. user_data is an opaque numeric
 * cookie, never a pointer to a caller's stack or an image buffer. */
static DRAM_ATTR pet_ppa_trace_state_t g_ppa_trace;
static DRAM_ATTR portMUX_TYPE g_ppa_trace_mux=portMUX_INITIALIZER_UNLOCKED;
#include "pet_p4_ppa_queue_probe.inc"
static void ppa_trace_frame_begin(void) {
  portENTER_CRITICAL(&g_ppa_trace_mux);
  pet_ppa_trace_frame_begin(&g_ppa_trace);
  portEXIT_CRITICAL(&g_ppa_trace_mux);
}
static void ppa_trace_frame_snapshot(pet_ppa_trace_frame_t *out) {
  portENTER_CRITICAL(&g_ppa_trace_mux);
  pet_ppa_trace_snapshot(&g_ppa_trace,out);
  portEXIT_CRITICAL(&g_ppa_trace_mux);
}
static bool IRAM_ATTR ppa_trace_done(ppa_client_handle_t client,ppa_event_data_t *event,void *user_data) {
  (void)client;(void)event;
  uint32_t cookie=(uint32_t)(uintptr_t)user_data;
  uint64_t now_us=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL_ISR(&g_ppa_trace_mux);
  pet_ppa_trace_callback(&g_ppa_trace,cookie,now_us);
  portEXIT_CRITICAL_ISR(&g_ppa_trace_mux);
  return false; /* No extra task notification, yield, log, or ownership change. */
}
static esp_err_t ppa_trace_srm(pet_ppa_trace_op_t kind,ppa_srm_oper_config_t *config) {
  uint64_t started_us=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&g_ppa_trace_mux);
  uint32_t cookie=pet_ppa_trace_begin(&g_ppa_trace,kind,started_us);
  portEXIT_CRITICAL(&g_ppa_trace_mux);
  config->user_data=(void *)(uintptr_t)cookie;
#if PET_P4_PPA_QUEUE_PROBE
  ppa_queue_scope_begin(g_ppa_srm_client,cookie);
#endif
  /* The caller's mode, buffers and all image parameters are unchanged. Never
   * hold the diagnostic lock while the original blocking transaction runs. */
  esp_err_t result=ppa_do_scale_rotate_mirror(g_ppa_srm_client,config);
#if PET_P4_PPA_QUEUE_PROBE
  ppa_queue_scope_end(cookie);
#endif
  uint64_t returned_us=(uint64_t)esp_timer_get_time();
  portENTER_CRITICAL(&g_ppa_trace_mux);
  pet_ppa_trace_return(&g_ppa_trace,cookie,result==ESP_OK,returned_us);
  portEXIT_CRITICAL(&g_ppa_trace_mux);
  return result;
}
static size_t g_jpeg_input_capacity;
static size_t g_jpeg_output_capacity;
static bool g_jpeg_input_dma_compatible;
static bool g_jpeg_output_dma_compatible;
static bool g_jpeg_decoder_init_attempted;
static bool g_framebuffer_ppa_compatible;
static bool g_ppa_init_attempted;
static bool g_native_framebuffer_ppa_compatible;
static bool g_logged_hardware_decoder;
static bool g_logged_hardware_fallback;
static bool g_logged_h264_decoder;
static bool g_logged_h264_ppa;
static bool g_logged_h264_ppa_fallback;
static bool g_logged_ppa_scaling;
static bool g_logged_ppa_scale_fallback;
static bool g_logged_ppa_rotation;
static bool g_logged_ppa_fallback;
static char g_decoder_fs_path[80];
static char g_rendered_family[PET_P4_ASSET_FAMILY_MAX];
static pet_p4_behavior_t g_behavior;
static EXT_RAM_BSS_ATTR pet_p4_miniapp_sprite_pack_t g_miniapp_sprites;
// Only the main render task uses these snapshots. Keep large views off its
// bounded stack; package/legacy media rendering shares one snapshot per frame.
static EXT_RAM_BSS_ATTR pet_p4_miniapp_render_snapshot_t g_miniapp_render_snapshot;
static EXT_RAM_BSS_ATTR pet_p4_media_view_t g_media_view_snapshot;
static bool g_logged_asset;
static bool g_logged_missing_asset;
static bool g_logged_asset_failure;
static bool g_framebuffer_initialized;
static unsigned long long g_asset_decode_blocked_until_ms;
static char g_last_render_page[PET_P4_PAGE_MAX];
static uint32_t g_last_decode_us;
static pet_p4_render_trace_t g_render_trace;
static uint32_t g_render_trace_frame;
static unsigned int g_asset_cache_revision;
static uint64_t g_asset_renderer_storage_epoch;
static uint16_t *g_session_overlay_cache;
static uint64_t g_session_overlay_signature;
static bool g_session_overlay_cache_valid;
static bool g_last_realtime_conversation;
/* Reset when leaving/clearing main. A pending first appearance must not paint
 * its status bubble over retained component-center pixels. */
static bool g_main_appearance_ready;
/* Only the compact panel ROI is retained, never another full frame. */
static EXT_RAM_BSS_ATTR pet_compact_underlay_t g_main_compact_underlay;
static bool g_main_compact_frame_clean;
_Static_assert(PET_P4_UI_WIDTH == PET_COMPACT_FRAME_WIDTH, "compact underlay stride");
_Static_assert(PET_P4_UI_HEIGHT == PET_COMPACT_ROI_Y + PET_COMPACT_ROI_HEIGHT + 16,
  "compact underlay geometry");
static bool g_last_approval_visible;
static bool g_session_overlay_cache_allocation_failed;
static uint8_t g_session_overlay_row_run_count[PET_P4_UI_HEIGHT];
static uint16_t g_session_overlay_row_run_start[PET_P4_UI_HEIGHT][PET_P4_SESSION_CACHE_MAX_RUNS];
static uint16_t g_session_overlay_row_run_end[PET_P4_UI_HEIGHT][PET_P4_SESSION_CACHE_MAX_RUNS];
static esp_h264_dec_handle_t g_h264_decoder;
static bool g_h264_decoder_open;
static size_t g_h264_stream_offset;
// tinyh264 removes emulation-prevention bytes in place. Never give it the cache.
static uint8_t *g_h264_work_stream;
static size_t g_h264_work_capacity;
static size_t g_h264_work_size;
static int g_h264_decoded_frame = -1;
static uint8_t *g_h264_output;
static uint32_t g_h264_output_size;
static uint8_t *g_h264_ppa_input;
static size_t g_h264_ppa_input_capacity;
static TaskHandle_t g_h264_pack_task;
static SemaphoreHandle_t g_h264_pack_done;
static bool g_h264_pack_task_init_attempted;
static char g_h264_fs_path[80];

typedef struct {
  const uint8_t *plane_y;
  const uint8_t *plane_u;
  const uint8_t *plane_v;
  uint8_t *packed;
  size_t packed_stride;
  int src_w;
  int src_stride;
  int row_begin;
  int row_end;
} pet_p4_h264_pack_job_t;

static pet_p4_h264_pack_job_t g_h264_pack_job;

typedef struct {
  uint8_t *bytes;
  size_t size;
  unsigned long long last_used_ms;
  char fs_path[80];
  bool loading, finished, failed, h264, single_slice_valid, raw_source;
#if PET_P4_H264_PREFETCH
  uint8_t decoder_readers; /* exact immutable-source pins; mux protected */
#endif
  int source_slot;
  uint32_t raw_offset;
  uint64_t storage_epoch;
  pet_resource_owner_t owner;
  pet_resource_lease_t buffer_lease, prepare_lease;
} pet_p4_asset_cache_entry_t;

static pet_p4_asset_cache_entry_t g_asset_cache[PET_P4_ASSET_CACHE_SLOTS];
static portMUX_TYPE g_asset_prepare_mux=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t g_asset_prepare_task;
static int g_asset_prepare_slot=-1;
static bool g_asset_cache_pending;
static uint64_t g_asset_prepare_cancel_epoch;
static bool g_asset_prepare_suspended;
static uint64_t g_asset_prepare_storage_epoch;
#if PET_P4_H264_PREFETCH
typedef struct {
  pet_prefetch_ticket_t ticket;
  pet_resource_owner_t owner;
  pet_resource_lease_t lease;
  pet_p4_asset_cache_entry_t *source;
  uint64_t cancel_epoch;
  char path[80];
  int width,height,frame;
  bool restart,success;
  uint32_t output_size,decode_us,wall_us;
  uint64_t enqueued_us;
  uint8_t *output;
#if PET_P4_H264_WORKER_PACK
  /* Back buffer assigned by the render thread at offer; packed by the worker
   * in WORKING only; adopted by the render thread at take. Job-local results. */
  uint8_t *packed;size_t packed_capacity;
  int packed_stride,packed_storage_h;bool packed_ok;uint32_t pack_us;
#endif
} pet_h264_prefetch_job_t;
static pet_prefetch_lifetime_t g_h264_prefetch_lifetime;
static pet_h264_prefetch_job_t g_h264_prefetch_job;
/* Packed-current is renderer-owned, never decoder output or worker metadata. */
static pet_h264_packed_view_t g_h264_packed_current;
static int g_h264_presented_frame=-1;
static bool g_h264_reset_requested;
static bool g_h264_program_window;
static bool h264_prefetch_worker(void);
#if PET_P4_KEYFRAME_BG
static bool keyframe_worker(void);
/* Caller holds g_asset_prepare_mux; no nested lock or wait. */
static bool keyframe_working_locked(void);
#endif
static void h264_prefetch_cancel_locked(void);
static bool h264_prefetch_reset(void);
_Static_assert(sizeof(StackType_t)==1,"ESP-IDF P4 task stack/HWM units are bytes");
#endif

bool pet_p4_renderer_program_window_begin(void){
#if PET_P4_H264_PREFETCH
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool idle=!g_h264_program_window&&g_h264_prefetch_lifetime.phase!=PET_PREFETCH_WORKING
    &&g_h264_prefetch_lifetime.phase!=PET_PREFETCH_CONSUMING;
#if PET_P4_KEYFRAME_BG
  idle=idle&&!keyframe_working_locked();
#endif
  if(idle)g_h264_program_window=true;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  return idle;
#else
  return true;
#endif
}
void pet_p4_renderer_program_window_end(void){
#if PET_P4_H264_PREFETCH
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool held=g_h264_program_window;g_h264_program_window=false;
  TaskHandle_t worker=g_asset_prepare_task;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(held&&worker)xTaskNotifyGive(worker);
#endif
}

static bool h264_stream_has_single_slice_access_units(
  const uint8_t *stream, size_t stream_size,
  const pet_p4_asset_cache_entry_t *job
);

typedef struct {
  uint32_t samples;
  int64_t started_us;
  int64_t last_ended_us;
  uint64_t asset_us;
  uint64_t decode_us;
  uint64_t h264_pack_us;
  uint64_t h264_convert_us;
  uint64_t overlay_us;
  uint64_t rotate_us;
  uint64_t lcd_us;
  uint64_t total_us;
  uint32_t over_budget;
  uint32_t total_samples_us[PET_P4_RENDER_PERF_WINDOW];
  pet_p4_render_peak_t peak;
#if PET_P4_RENDER_LCD_SPLIT_PROBE && !PET_P4_RETAIN_PEAK_PHASES
  /* A retained boot peak already owns the split. Do not duplicate a second
   * window-peak record in that private mode; the dedicated LCD probe keeps
   * its original window logging unchanged. */
  uint32_t peak_compare_us;
  bool peak_compare_valid;
  pet_lcd_observation_t peak_lcd;
#endif
} pet_p4_render_perf_t;

static pet_p4_render_perf_t g_render_perf;
#if PET_P4_LOGICAL_COMPARE
static bool g_app_logical_same;
_Static_assert(PET_P4_NATIVE_OUTPUT_BUFFER_COUNT>=2,"Logical reuse needs distinct native buffers");
#endif
#if PET_P4_APP_RETAIN_LOGICAL
static pet_logical_retention_t g_app_final_retention;
static pet_logical_common_plan_t g_app_common_plan;
static bool g_app_may_retain,g_app_final_reused,g_app_final_eligible;
#endif
#if PET_P4_RENDER_LCD_SPLIT_PROBE
static uint32_t g_last_compare_us;
static bool g_last_compare_valid;
static pet_lcd_observation_t g_last_lcd_observation;
#endif
static int64_t g_last_slow_render_log_us;
static int64_t g_last_frame_gap_log_us;
static int64_t g_previous_render_ended_us;
static uint32_t g_last_h264_pack_us;
static uint32_t g_last_h264_convert_us;

static unsigned long long renderer_monotonic_ms(void) {
  return (unsigned long long) (esp_timer_get_time() / 1000ULL);
}

static bool asset_decode_is_in_backoff(void) {
  return g_asset_decode_blocked_until_ms > renderer_monotonic_ms();
}

static void note_asset_decode_failure(void) {
  unsigned long long now_ms = renderer_monotonic_ms();
  g_asset_decode_blocked_until_ms = now_ms + PET_P4_ASSET_DECODE_BACKOFF_MS;
}

static void note_asset_decode_success(void) {
  g_asset_decode_blocked_until_ms = 0;
}

static uint32_t elapsed_us_clamped(int64_t started_us, int64_t ended_us) {
  if (started_us < 0 || ended_us <= started_us) return 0;
  uint64_t elapsed = (uint64_t)ended_us - (uint64_t)started_us;
  return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t) elapsed;
}

static uint32_t perf_average_tenths_ms(uint64_t total_us, uint32_t samples) {
  if (samples == 0) return 0;
  return (uint32_t) (total_us / ((uint64_t) samples * 100ULL));
}

#if PET_P4_APP_NODE_TRACE
_Static_assert(PET_SURFACE_NODES==PET_APP_TRACE_NODES,"node trace must cover every surface node");
typedef struct {uint32_t *out;int64_t started;} app_node_scope_t;
static app_node_scope_t app_node_scope_begin(uint32_t *out){
  return (app_node_scope_t){out,esp_timer_get_time()};
}
static void app_node_scope_end(app_node_scope_t *scope){
  if(!pet_app_draw_trace_add(scope->out,scope->started,esp_timer_get_time())
      &&g_render_trace.app_nodes.invalid_clock<UINT32_MAX)g_render_trace.app_nodes.invalid_clock++;
}
static void app_node_trace_log(const pet_p4_render_trace_t *trace){
  if(!trace->app_draw_started_us)return;
#if PET_P4_QUOTA_V3
  char line[1536]; /* 64 full-width slots plus the 64-bit mask header */
#else
  char line[1152]; /* 48 full-width slots plus the 64-bit mask header */
#endif
  if(pet_app_draw_trace_format(&trace->app_nodes,trace->frame,line,sizeof(line)))ESP_LOGD(TAG,"%s",line);
  else ESP_LOGW(TAG,"P4 app nodes truncated frame=%u",(unsigned)trace->frame);
#if PET_P4_APP_RETAIN_LOGICAL
  if(trace->logical_retained)ESP_LOGD(TAG,"P4 final logical retained frame=%u",(unsigned)trace->frame);
#endif
}
#define APP_NODE_SCOPE(name,field) app_node_scope_t name __attribute__((cleanup(app_node_scope_end)))=app_node_scope_begin(&g_render_trace.app_nodes.field)
#define APP_NODE_MARK(index) (g_render_trace.app_nodes.nodes_mask|=UINT64_C(1)<<(index))
#define APP_NODE_HIT() (g_render_trace.app_nodes.cache_hit=1)
#else
#define APP_NODE_SCOPE(name,field)
#define APP_NODE_MARK(index) ((void)0)
#define APP_NODE_HIT() ((void)0)
#define app_node_trace_log(trace) ((void)0)
#endif
static void record_render_performance(
  int64_t started_us,
  int64_t ended_us,
  uint32_t asset_us,
  uint32_t overlay_us,
  uint32_t rotate_us,
  uint32_t lcd_us
) {
  uint32_t total_us = elapsed_us_clamped(started_us, ended_us);
  portENTER_CRITICAL(&g_metrics_lock);
  g_metrics.last_us = total_us;
  g_metrics.last_decode_us = g_last_decode_us;
  g_metrics.submitted_frames = g_lcd_submitted;
  g_metrics.unchanged_frames = g_lcd_skipped;
#if PET_P4_RETAIN_PEAK_PHASES
  if(total_us>g_metrics.lifetime.max_us){
    g_metrics.lifetime.asset_us=asset_us;g_metrics.lifetime.decode_us=g_last_decode_us;
    g_metrics.lifetime.pack_us=g_last_h264_pack_us;g_metrics.lifetime.convert_us=g_last_h264_convert_us;
#if PET_P4_LOGICAL_COMPARE
    g_metrics.lifetime.logical_compare_us=g_render_trace.logical_compare_us;
#if PET_P4_APP_RETAIN_LOGICAL
    g_metrics.lifetime.logical_retained=g_render_trace.logical_retained;
#endif
#endif
    g_metrics.lifetime.overlay_us=overlay_us;g_metrics.lifetime.rotate_us=rotate_us;g_metrics.lifetime.lcd_us=lcd_us;
    g_metrics.lifetime.content_us=g_render_trace.content_us;g_metrics.lifetime.common_us=g_render_trace.common_overlay_us;
    g_metrics.lifetime.bubble_select_us=g_render_trace.bubble_select_us;
    g_metrics.lifetime.pinned_blit_us=g_render_trace.pinned_blit_us;
    g_metrics.lifetime.bubble_draw_us=g_render_trace.bubble_draw_us;
    g_metrics.lifetime.session_cache_fill_us=g_render_trace.session_cache_fill_us;
    g_metrics.lifetime.session_cache_draw_us=g_render_trace.session_cache_draw_us;
    g_metrics.lifetime.session_cache_scan_us=g_render_trace.session_cache_scan_us;
    g_metrics.lifetime.session_cache_copy_us=g_render_trace.session_cache_copy_us;
    g_metrics.lifetime.voice_us=g_render_trace.voice_us;
    g_metrics.lifetime.resolve_us=g_render_trace.resolve_us;g_metrics.lifetime.cache_us=g_render_trace.cache_us;
    g_metrics.lifetime.scale_us=g_render_trace.scale_us;
#if PET_P4_APP_PHASE_TRACE
    g_metrics.lifetime.app_snapshot_us=g_render_trace.app_snapshot_us;
    g_metrics.lifetime.app_snapshot_wait_us=g_render_trace.app_snapshot_wait_us;
    g_metrics.lifetime.app_draw_us=g_render_trace.app_draw_us;
#endif
#if PET_P4_APP_NODE_TRACE
    g_metrics.lifetime.app_nodes=g_render_trace.app_nodes;
#endif
#if PET_P4_RENDER_LCD_SPLIT_PROBE
    g_metrics.lifetime.compare_us=g_last_compare_us;g_metrics.lifetime.compare_valid=g_last_compare_valid;
    g_metrics.lifetime.lcd_submit_us=g_last_lcd_observation.submit_us;g_metrics.lifetime.lcd_wait_us=g_last_lcd_observation.wait_us;
    g_metrics.lifetime.lcd_valid=g_last_lcd_observation.valid;g_metrics.lifetime.lcd_submitted=g_last_lcd_observation.submitted;
    g_metrics.lifetime.lcd_waited=g_last_lcd_observation.refresh_waited;
#endif
  }
#endif
#if PET_P4_FRAME_IO_OVERLAP
  {
    /* A widget operation lane (install/delete/activate or save flush) was
     * active, started or completed inside this frame's wall window. This is a
     * numeric correlation for diagnosis only, not CPU time or a cause. */
    bool overlapped=false;
    for(unsigned i=0;i<2;i++){
      const pet_p4_render_operation_observation_t *before=&g_render_trace.operation_before[i],*after=&g_render_trace.operation_after[i];
      if(((before->phase|after->phase)>>16)&1U)overlapped=true;
      if(after->seq!=before->seq||g_render_trace.completed_after[i]!=g_render_trace.completed_before[i])overlapped=true;
    }
    uint32_t nodes_us=pet_app_draw_trace_nodes_total(&g_render_trace.app_nodes);
    pet_render_overlap_record(&g_metrics.lifetime,overlapped,g_render_trace.page==PET_RENDER_PAGE_APP,total_us,g_render_trace.frame,
      g_render_trace.app_draw_us,rotate_us,lcd_us,g_render_trace.app_nodes.clear_us,nodes_us,overlay_us,
      total_us>g_metrics.lifetime.max_us);
  }
#endif
#if PET_P4_APP_CLEAR_OPAQUE_CENTER
  g_metrics.lifetime.clear_center_frames=g_clear_center_frames;
  g_metrics.lifetime.clear_full_frames=g_clear_full_frames;
#endif
#if PET_P4_APP_COLD_FAST
  g_metrics.lifetime.cold_fast_image_nodes=g_cold_fast_image_nodes;
  g_metrics.lifetime.cold_fast_panel_nodes=g_cold_fast_panel_nodes;
  g_metrics.lifetime.cold_fast_clear_skips=g_cold_fast_clear_skips;
#endif
#if PET_P4_APP_PPA_COPY
  g_metrics.lifetime.ppa_copies=g_ppa_copies;g_metrics.lifetime.ppa_copy_fallbacks=g_ppa_copy_fallbacks;g_metrics.lifetime.ppa_copy_mismatches=g_ppa_copy_mismatches;
#endif
#if PET_P4_H264_WORKER_PACK
  g_metrics.lifetime.worker_packs=g_worker_packs;g_metrics.lifetime.worker_pack_fallbacks=g_worker_pack_fallbacks;
  g_metrics.lifetime.worker_pack_max_us=g_worker_pack_max_us;g_metrics.lifetime.worker_pack_last_us=g_worker_pack_last_us;
#endif
#if PET_P4_GAP_PEAK_CONTEXT
  {
    pet_render_gap_context_t context=g_loop_context;
    context.frame=g_render_trace.frame;
#if PET_P4_GAP_PEAK_HOLDER
    context.at_us=(uint64_t)ended_us;
#endif
    context.lane0_phase=g_render_trace.operation_before[0].phase;context.lane1_phase=g_render_trace.operation_before[1].phase;
    pet_render_gap_context_record(&g_metrics.lifetime,g_render_trace.gap_us,&context);
    memset(&g_loop_context,0,sizeof(g_loop_context));
  }
#endif
  pet_render_lifetime_record(&g_metrics.lifetime,total_us,g_render_trace.gap_us,
    g_render_trace.frame,g_render_trace.page,started_us,ended_us,
    PET_P4_RENDER_FRAME_BUDGET_US,100000U);
  portEXIT_CRITICAL(&g_metrics_lock);
  /* Execution time alone hides time spent waiting between frames, including
   * protocol/state-lock work. Report the gap separately; it is not CPU time
   * and is not attributed to the operation observed at the next frame start. */
  if (g_render_trace.gap_us > 100000U
      && (!g_last_frame_gap_log_us || ended_us-g_last_frame_gap_log_us >= 2000000LL)) {
    ESP_LOGD(TAG, "P4 frame gap frame=%u gap_us=%u",
      (unsigned)g_render_trace.frame, (unsigned)g_render_trace.gap_us);
    ESP_LOGD(TAG, PET_RENDER_CONTEXT_FORMAT, PET_RENDER_CONTEXT_VALUES(g_render_trace));
#if PET_P4_APP_NODE_TRACE
    app_node_trace_log(&g_render_trace);
#endif
    g_last_frame_gap_log_us=ended_us;
  }
  if (total_us > 100000U
      && (!g_last_slow_render_log_us
          || ended_us - g_last_slow_render_log_us >= 2000000LL)) {
    ESP_LOGW(
      TAG,
      "P4 slow render total=%u.%ums asset=%u.%u decode=%u.%u pack=%u.%u convert=%u.%u "
      "overlay=%u.%u rotate=%u.%u lcd=%u.%u" PET_RENDER_TRACE_FORMAT,
      total_us / 1000U,
      (total_us % 1000U) / 100U,
      asset_us / 1000U,
      (asset_us % 1000U) / 100U,
      g_last_decode_us / 1000U,
      (g_last_decode_us % 1000U) / 100U,
      g_last_h264_pack_us / 1000U,
      (g_last_h264_pack_us % 1000U) / 100U,
      g_last_h264_convert_us / 1000U,
      (g_last_h264_convert_us % 1000U) / 100U,
      overlay_us / 1000U,
      (overlay_us % 1000U) / 100U,
      rotate_us / 1000U,
      (rotate_us % 1000U) / 100U,
      lcd_us / 1000U,
      (lcd_us % 1000U) / 100U,
      PET_RENDER_TRACE_VALUES(g_render_trace)
    );
    if (g_render_trace.start_us) {
      ESP_LOGD(TAG, PET_RENDER_CONTEXT_FORMAT, PET_RENDER_CONTEXT_VALUES(g_render_trace));
#if PET_P4_APP_NODE_TRACE
      app_node_trace_log(&g_render_trace);
#endif
      ESP_LOGD(TAG, PET_RENDER_PPA_FORMAT, PET_RENDER_PPA_VALUES(g_render_trace));
    }
    g_last_slow_render_log_us = ended_us;
  }
  if (total_us > PET_P4_RENDER_PERF_GAP_RESET_US) {
    portENTER_CRITICAL(&g_metrics_lock);
    pet_render_counter_increment(&g_metrics.lifetime.window_resets);
    portEXIT_CRITICAL(&g_metrics_lock);
    memset(&g_render_perf, 0, sizeof(g_render_perf));
    return;
  }
  if (g_render_perf.samples > 0
      && started_us - g_render_perf.last_ended_us > PET_P4_RENDER_PERF_GAP_RESET_US) {
    portENTER_CRITICAL(&g_metrics_lock);
    pet_render_counter_increment(&g_metrics.lifetime.window_resets);
    portEXIT_CRITICAL(&g_metrics_lock);
    memset(&g_render_perf, 0, sizeof(g_render_perf));
  }
  if (g_render_perf.samples == 0) g_render_perf.started_us = started_us;
#if PET_P4_RENDER_LCD_SPLIT_PROBE && !PET_P4_RETAIN_PEAK_PHASES
  if(!g_render_perf.peak.valid||total_us>g_render_perf.peak.total_us){
    g_render_perf.peak_compare_us=g_last_compare_us;g_render_perf.peak_compare_valid=g_last_compare_valid;g_render_perf.peak_lcd=g_last_lcd_observation;
  }
#endif
  pet_p4_render_peak_record(
    &g_render_perf.peak, &g_render_trace, total_us, asset_us, g_last_decode_us,
    g_last_h264_pack_us, g_last_h264_convert_us, overlay_us, rotate_us, lcd_us
  );
  uint32_t index = g_render_perf.samples;
  if (index >= PET_P4_RENDER_PERF_WINDOW) index = PET_P4_RENDER_PERF_WINDOW - 1;
  g_render_perf.total_samples_us[index] = total_us;
  g_render_perf.samples += 1;
  g_render_perf.asset_us += asset_us;
  g_render_perf.decode_us += g_last_decode_us;
  g_render_perf.h264_pack_us += g_last_h264_pack_us;
  g_render_perf.h264_convert_us += g_last_h264_convert_us;
  g_render_perf.overlay_us += overlay_us;
  g_render_perf.rotate_us += rotate_us;
  g_render_perf.lcd_us += lcd_us;
  g_render_perf.total_us += total_us;
  g_render_perf.last_ended_us = ended_us;
  if (total_us > PET_P4_RENDER_FRAME_BUDGET_US) g_render_perf.over_budget += 1;
  if (g_render_perf.samples < PET_P4_RENDER_PERF_WINDOW) return;

  uint32_t sorted[PET_P4_RENDER_PERF_WINDOW];
  memcpy(sorted, g_render_perf.total_samples_us, sizeof(sorted));
  for (uint32_t i = 1; i < PET_P4_RENDER_PERF_WINDOW; i += 1) {
    uint32_t value = sorted[i];
    uint32_t j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      j -= 1;
    }
    sorted[j] = value;
  }

  int64_t window_us = ended_us - g_render_perf.started_us;
  uint32_t fps_tenths = window_us > 0
    ? (uint32_t) (((uint64_t) PET_P4_RENDER_PERF_WINDOW * 10000000ULL) / (uint64_t) window_us)
    : 0;
  uint32_t total_tenths = perf_average_tenths_ms(g_render_perf.total_us, g_render_perf.samples);
  uint32_t asset_tenths = perf_average_tenths_ms(g_render_perf.asset_us, g_render_perf.samples);
  uint32_t decode_tenths = perf_average_tenths_ms(g_render_perf.decode_us, g_render_perf.samples);
  uint32_t pack_tenths = perf_average_tenths_ms(g_render_perf.h264_pack_us, g_render_perf.samples);
  uint32_t convert_tenths = perf_average_tenths_ms(g_render_perf.h264_convert_us, g_render_perf.samples);
  uint32_t overlay_tenths = perf_average_tenths_ms(g_render_perf.overlay_us, g_render_perf.samples);
  uint32_t rotate_tenths = perf_average_tenths_ms(g_render_perf.rotate_us, g_render_perf.samples);
  uint32_t lcd_tenths = perf_average_tenths_ms(g_render_perf.lcd_us, g_render_perf.samples);
  uint32_t p95_tenths = sorted[94] / 100U;
  uint32_t max_tenths = sorted[PET_P4_RENDER_PERF_WINDOW - 1] / 100U;
  portENTER_CRITICAL(&g_metrics_lock);
  g_metrics.p95_us = sorted[94];
  g_metrics.max_us = sorted[PET_P4_RENDER_PERF_WINDOW - 1];
  g_metrics.window_samples = PET_P4_RENDER_PERF_WINDOW;
  pet_render_counter_increment(&g_metrics.lifetime.completed_windows);
  g_metrics.lifetime.window_end_frame=g_render_trace.frame;
  portEXIT_CRITICAL(&g_metrics_lock);
  ESP_LOGD(
    TAG,
    "P4 render perf fps=%u.%u total=%u.%ums p95=%u.%ums max=%u.%ums "
    "asset=%u.%u decode=%u.%u pack=%u.%u convert=%u.%u overlay=%u.%u rotate=%u.%u lcd=%u.%u over66=%u/%u",
    fps_tenths / 10U,
    fps_tenths % 10U,
    total_tenths / 10U,
    total_tenths % 10U,
    p95_tenths / 10U,
    p95_tenths % 10U,
    max_tenths / 10U,
    max_tenths % 10U,
    asset_tenths / 10U,
    asset_tenths % 10U,
    decode_tenths / 10U,
    decode_tenths % 10U,
    pack_tenths / 10U,
    pack_tenths % 10U,
    convert_tenths / 10U,
    convert_tenths % 10U,
    overlay_tenths / 10U,
    overlay_tenths % 10U,
    rotate_tenths / 10U,
    rotate_tenths % 10U,
    lcd_tenths / 10U,
    lcd_tenths % 10U,
    g_render_perf.over_budget,
    PET_P4_RENDER_PERF_WINDOW
  );
  /* Same frame as max, not a mixture of independently averaged/maxed stages. */
  const pet_p4_render_peak_t *peak = &g_render_perf.peak;
  ESP_LOGD(
    TAG,
    "P4 render peak total_us=%u asset_us=%u decode_us=%u pack_us=%u convert_us=%u "
    "overlay_us=%u rotate_us=%u lcd_us=%u" PET_RENDER_TRACE_FORMAT,
    (unsigned int) peak->total_us, (unsigned int) peak->asset_us,
    (unsigned int) peak->decode_us, (unsigned int) peak->pack_us,
    (unsigned int) peak->convert_us, (unsigned int) peak->overlay_us,
    (unsigned int) peak->rotate_us, (unsigned int) peak->lcd_us,
    PET_RENDER_TRACE_VALUES(peak->trace)
  );
  if (peak->trace.start_us) {
    ESP_LOGD(TAG, PET_RENDER_CONTEXT_FORMAT, PET_RENDER_CONTEXT_VALUES(peak->trace));
#if PET_P4_APP_NODE_TRACE
    app_node_trace_log(&peak->trace);
#endif
    ESP_LOGD(TAG, PET_RENDER_PPA_FORMAT, PET_RENDER_PPA_VALUES(peak->trace));
  }
#if PET_P4_RENDER_LCD_SPLIT_PROBE && !PET_P4_RETAIN_PEAK_PHASES
  ESP_LOGD(TAG,"P4 lcd split frame=%u compare_us=%u compare_valid=%u submit_us=%u wait_us=%u valid=%u submitted=%u waited=%u",
    (unsigned)peak->trace.frame,(unsigned)g_render_perf.peak_compare_us,(unsigned)g_render_perf.peak_compare_valid,(unsigned)g_render_perf.peak_lcd.submit_us,
    (unsigned)g_render_perf.peak_lcd.wait_us,(unsigned)g_render_perf.peak_lcd.valid,
    (unsigned)g_render_perf.peak_lcd.submitted,(unsigned)g_render_perf.peak_lcd.refresh_waited);
#endif
  memset(&g_render_perf, 0, sizeof(g_render_perf));
}

static bool asset_prepare_audio_busy(void) {
  return pet_p4_audio_active() || pet_p4_audio_playback_active()
    || pet_p4_audio_conversation_active() || pet_p4_audio_music_active();
}

static bool asset_prepare_current(const pet_p4_asset_cache_entry_t *job) {
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool suspended = g_asset_prepare_suspended;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  bool audio_blocked=asset_prepare_audio_busy();
#if PET_P4_CONVERSATION_VIDEO
  if(job->prepare_lease.kind==PET_RESOURCE_CACHED_VIDEO)
    audio_blocked=audio_blocked&&(!pet_p4_audio_conversation_video_ready()||pet_p4_audio_music_active());
#endif
  if (suspended || audio_blocked) {
    pet_resources_device_revoke(job->owner);
    return false;
  }
  return pet_resources_device_current(job->prepare_lease)
    && pet_resources_device_current(job->buffer_lease);
}

/* Only the renderer mutates metadata/reclaims entries. The worker has exclusive
 * ownership of its private buffer until finished is published under this mux.
 * No allocation, I/O, broker call, or wait is made while holding the mux. */
static bool asset_cache_entry_finished(const pet_p4_asset_cache_entry_t *entry) {
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool finished = entry->finished;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  return finished;
}

static void asset_cache_release(pet_p4_asset_cache_entry_t *entry) {
  portENTER_CRITICAL(&g_asset_prepare_mux);
  pet_p4_asset_cache_entry_t previous = *entry;
  bool in_flight = previous.loading && !previous.finished;
#if PET_P4_H264_PREFETCH
  in_flight = in_flight || previous.decoder_readers;
#endif
  if (!in_flight) memset(entry, 0, sizeof(*entry));
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  pet_resources_device_revoke(previous.owner);
  if (in_flight) return;
  if (previous.bytes) heap_caps_free(previous.bytes);
  (void) pet_resources_device_finish(previous.buffer_lease);
  (void) pet_resources_device_finish(previous.prepare_lease);
}

/* May be called by a protocol worker before mutating storage, without taking
 * the render mutex. No cache memory/decoder changes and no waiting for I/O. */
void pet_p4_renderer_cancel_asset_preparation(void) {
  pet_resource_owner_t owners[PET_P4_ASSET_CACHE_SLOTS] = {0};
  portENTER_CRITICAL(&g_asset_prepare_mux);
  g_asset_prepare_cancel_epoch++;
#if PET_P4_H264_PREFETCH
  h264_prefetch_cancel_locked();
  pet_resource_owner_t codec_owner=g_h264_prefetch_job.owner;
#endif
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i++)
    if (g_asset_cache[i].loading) owners[i] = g_asset_cache[i].owner;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
#if PET_P4_H264_PREFETCH
  pet_resources_device_revoke(codec_owner);
#endif
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i++)
    pet_resources_device_revoke(owners[i]);
}

void pet_p4_renderer_set_asset_preparation_suspended(bool suspended) {
  pet_resource_owner_t owners[PET_P4_ASSET_CACHE_SLOTS] = {0};
  portENTER_CRITICAL(&g_asset_prepare_mux);
  if (suspended && !g_asset_prepare_suspended) g_asset_prepare_storage_epoch++;
  g_asset_prepare_suspended = suspended;
  if (suspended) {
    g_asset_prepare_cancel_epoch++;
#if PET_P4_H264_PREFETCH
    h264_prefetch_cancel_locked();
#endif
    for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i++)
      if (g_asset_cache[i].loading) owners[i] = g_asset_cache[i].owner;
  }
#if PET_P4_H264_PREFETCH
  pet_resource_owner_t codec_owner=suspended?g_h264_prefetch_job.owner:(pet_resource_owner_t){0};
#endif
  portEXIT_CRITICAL(&g_asset_prepare_mux);
#if PET_P4_H264_PREFETCH
  pet_resources_device_revoke(codec_owner);
#endif
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i++)
    pet_resources_device_revoke(owners[i]);
}

/* Renderer-owned physical reclamation is separate from cancelling the shared
 * decoder. A clip may own that decoder while a previous ordinary read finishes. */
static void release_inactive_asset_prepares(void) {
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i++) {
    pet_p4_asset_cache_entry_t *entry = &g_asset_cache[i];
    if (entry->loading) asset_cache_release(entry);
  }
}
static void cancel_pending_asset_prepares(void) {
  pet_p4_renderer_cancel_asset_preparation();
  release_inactive_asset_prepares();
}

static void clear_asset_stream_cache(void) {
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i++) {
    asset_cache_release(&g_asset_cache[i]);
  }
}

/* A single priority-1 task owns all filesystem work. The captured raw slot/base
 * and SPIFFS path never consult mutable active-slot metadata from this task. */
static void prepare_asset_entry(pet_p4_asset_cache_entry_t *entry) {
  pet_p4_asset_cache_entry_t job = *entry;
  uint8_t *bytes = NULL;
  FILE *file = NULL;
  pet_fs_activity_t file_activity __attribute__((cleanup(pet_fs_activity_end)))={0};
  bool ok = asset_prepare_current(&job), valid = !job.h264;
  int64_t started_us = esp_timer_get_time();
  if (ok) {
    bytes = heap_caps_malloc(job.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ok = bytes != NULL;
  }
  if (ok && !job.raw_source) {
    file_activity=pet_fs_activity_begin();
    file = fopen(job.fs_path, "rb");
    ok = file != NULL;
  }
  for (size_t offset = 0; ok && offset < job.size;) {
    if (!asset_prepare_current(&job)) { ok = false; break; }
    size_t length = job.size - offset;
    if (length > PET_P4_ASSET_PREPARE_CHUNK_BYTES) length = PET_P4_ASSET_PREPARE_CHUNK_BYTES;
    ok = job.raw_source
      ? pet_p4_raw_assets_read_range(job.source_slot, job.raw_offset, job.size, offset, bytes + offset, length)
      : fread(bytes + offset, 1, length, file) == length;
    offset += length;
    vTaskDelay(1);
  }
  if (file) fclose(file);
  pet_fs_activity_end(&file_activity);
  uint32_t read_us = elapsed_us_clamped(started_us, esp_timer_get_time());
  int64_t validate_started_us = esp_timer_get_time();
  ok = ok && asset_prepare_current(&job);
  bool read_ok = ok;
  if (ok && job.h264) valid = h264_stream_has_single_slice_access_units(bytes, job.size, &job);
  bool current = asset_prepare_current(&job);
  ok = ok && valid && current;
  uint32_t validate_us = elapsed_us_clamped(validate_started_us, esp_timer_get_time());
  /* Finish only after all physical I/O/validation has returned. The buffer lease
   * stays occupied until the renderer observes publication and frees it. */
  (void) pet_resources_device_finish(job.prepare_lease);
  portENTER_CRITICAL(&g_asset_prepare_mux);
  entry->bytes = bytes;
  entry->single_slice_valid = valid;
  entry->failed = !ok;
  entry->finished = true;
  g_asset_prepare_slot = -1;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  /* Job-local values only: after publication the renderer may reuse entry. */
  ESP_LOGD(TAG, "P4 asset prepare bytes=%u read_us=%u validate_us=%u raw=%u readOK=%u valid=%u ok=%u current=%u",
    (unsigned) job.size, (unsigned) read_us, (unsigned) validate_us,
    job.raw_source ? 1U : 0U, read_ok ? 1U : 0U, valid ? 1U : 0U,
    ok ? 1U : 0U, current ? 1U : 0U);
}

static void asset_prepare_task(void *unused) {
  (void) unused;
  for (;;) {
    (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      portENTER_CRITICAL(&g_asset_prepare_mux);
      int slot = g_asset_prepare_slot;
      portEXIT_CRITICAL(&g_asset_prepare_mux);
      if (slot >= 0 && slot < (int) PET_P4_ASSET_CACHE_SLOTS) {
        prepare_asset_entry(&g_asset_cache[slot]);
        continue;
      }
#if PET_P4_H264_PREFETCH
      if (h264_prefetch_worker()) continue;
#endif
#if PET_P4_KEYFRAME_BG
      if (keyframe_worker()) continue;
#endif
      break;
    }
  }
}

static pet_p4_asset_cache_entry_t *load_asset_stream_cache(
  const char *logical_path,
  const char *fs_path,
  size_t expected_size,
  bool h264,
  int source_slot,
  uint64_t request_epoch,
  uint64_t storage_epoch
) {
  unsigned long long now_ms = renderer_monotonic_ms();
  portENTER_CRITICAL(&g_asset_prepare_mux);
  uint64_t cancel_epoch = g_asset_prepare_cancel_epoch;
  bool suspended = g_asset_prepare_suspended;
  bool stale = request_epoch != cancel_epoch || storage_epoch != g_asset_prepare_storage_epoch;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  g_asset_cache_pending = false;
  g_render_trace.cache_hit = PET_RENDER_CACHE_MISS;
  if (stale) { g_asset_cache_pending = true; return NULL; }
  if (!fs_path || !fs_path[0] || expected_size == 0
      || expected_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES) return NULL;
  bool failed_match = false;
  pet_p4_asset_cache_entry_t *hit = NULL;
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i += 1) {
    pet_p4_asset_cache_entry_t *entry = &g_asset_cache[i];
    if (entry->owner.generation && (entry->storage_epoch != storage_epoch || entry->source_slot != source_slot)) {
      asset_cache_release(entry);
    }
    bool match = entry->size == expected_size && entry->h264 == h264
      && strcmp(entry->fs_path, fs_path) == 0;
    if (entry->loading && !asset_cache_entry_finished(entry)) {
      if (!match || asset_prepare_audio_busy()) pet_resources_device_revoke(entry->owner);
      g_asset_cache_pending = true;
      continue;
    }
    portENTER_CRITICAL(&g_asset_prepare_mux);
    entry->loading = false; /* acquire publication above */
    portEXIT_CRITICAL(&g_asset_prepare_mux);
    bool current = entry->owner.generation && pet_resources_device_current(entry->buffer_lease);
    if (entry->failed || (entry->owner.generation && !current)) {
      failed_match = failed_match || (match && entry->failed && current);
      asset_cache_release(entry);
      continue;
    }
    if (match && entry->bytes && current) {
      entry->last_used_ms = now_ms;
      hit = entry;
    }
  }
  if (hit) {
    g_render_trace.cache_hit = PET_RENDER_CACHE_HIT;
    g_asset_cache_pending = false;
    return hit;
  }
  if (failed_match) { g_asset_cache_pending = false; return NULL; }
  if (g_asset_cache_pending) return NULL;
  /* Busy/protected resources are deferred work, never decoder failures. */
  g_asset_cache_pending = true;
  if (suspended || asset_prepare_audio_busy()) return NULL;
  if (!g_asset_prepare_task && xTaskCreate(asset_prepare_task, "p4-asset-prep",
        PET_P4_ASSET_PREPARE_STACK_BYTES, NULL, PET_P4_ASSET_PREPARE_PRIORITY,
        &g_asset_prepare_task) != pdPASS) return NULL;
  unsigned int selected = 0;
  for (unsigned int i = 0; i < PET_P4_ASSET_CACHE_SLOTS; i += 1) {
    if (!g_asset_cache[i].bytes) {
      selected = i;
      break;
    }
    if (g_asset_cache[i].last_used_ms < g_asset_cache[selected].last_used_ms) {
      selected = i;
    }
  }
  pet_p4_asset_cache_entry_t *entry = &g_asset_cache[selected];
  asset_cache_release(entry);
#if PET_P4_H264_PREFETCH
  /* Revocation can leave a physically pinned source occupied. Never reuse it. */
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool still_pinned=entry->decoder_readers!=0;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(still_pinned)return NULL;
#endif
  uint32_t mask = PET_RESOURCE_MASK(PET_RESOURCE_ASSET_BUFFER) | PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE);
  if (pet_resources_device_owner_open(mask, &entry->owner) != PET_RESOURCE_OK) return NULL;
  if (pet_resources_device_acquire(entry->owner, PET_RESOURCE_ASSET_BUFFER, expected_size,
        &entry->buffer_lease) != PET_RESOURCE_OK
      || pet_resources_device_acquire(entry->owner, PET_RESOURCE_ASSET_PREPARE, 1,
        &entry->prepare_lease) != PET_RESOURCE_OK) {
    asset_cache_release(entry);
    return NULL;
  }
  entry->size = expected_size;
  entry->h264 = h264;
  entry->last_used_ms = now_ms;
  snprintf(entry->fs_path, sizeof(entry->fs_path), "%s", fs_path);
  entry->source_slot = source_slot;
  entry->storage_epoch = storage_epoch;
  entry->raw_source = pet_p4_raw_assets_supports_path(logical_path)
    && pet_p4_raw_assets_locate(entry->source_slot, logical_path, expected_size, &entry->raw_offset);
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool cancelled = g_asset_prepare_suspended || cancel_epoch != g_asset_prepare_cancel_epoch;
  if (!cancelled) {
    entry->loading = true;
    g_asset_prepare_slot = (int) selected;
  }
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if (cancelled) { asset_cache_release(entry); return NULL; }
  xTaskNotifyGive(g_asset_prepare_task);
  return NULL;
}

#if PET_P4_KEYFRAME_BG
static void keyframe_engine_lock(void);
static void keyframe_engine_unlock(void);
#endif
static bool decode_jpeg_frame_hardware_into(
  const uint8_t *input,
  size_t input_size,
  uint16_t *output,
  size_t output_capacity,
  int *out_width,
  int *out_height,
  int *out_stride,
  esp_err_t *out_error
);
static bool decode_jpeg_frame_hardware(
  const uint8_t *input,
  size_t input_size,
  int *out_width,
  int *out_height,
  int *out_stride,
  esp_err_t *out_error
) {
  return decode_jpeg_frame_hardware_into(input,input_size,g_jpeg_output,g_jpeg_output_capacity,out_width,out_height,out_stride,out_error);
}
static bool decode_jpeg_frame_hardware_into(
  const uint8_t *input,
  size_t input_size,
  uint16_t *output,
  size_t output_capacity,
  int *out_width,
  int *out_height,
  int *out_stride,
  esp_err_t *out_error
) {
  jpeg_decode_picture_info_t picture_info = {0};
  jpeg_decode_cfg_t decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
  };
  uint32_t decoded_size = 0;
  esp_err_t err;

  if (out_error) *out_error = ESP_ERR_INVALID_STATE;
  if (!g_jpeg_decoder || !input || input_size == 0 || !output) return false;
  if (input_size > UINT32_MAX || output_capacity > UINT32_MAX) {
    if (out_error) *out_error = ESP_ERR_INVALID_SIZE;
    return false;
  }

  err = jpeg_decoder_get_info(input, (uint32_t) input_size, &picture_info);
  if (err != ESP_OK) {
    if (out_error) *out_error = err;
    return false;
  }
  if (picture_info.width == 0 || picture_info.height == 0
      || picture_info.width > PET_P4_ASSET_WIDTH || picture_info.height > PET_P4_ASSET_HEIGHT) {
    if (out_error) *out_error = ESP_ERR_INVALID_SIZE;
    return false;
  }

#if PET_P4_KEYFRAME_BG
  keyframe_engine_lock(); /* one hardware engine shared with the keyframe worker */
#endif
  err = jpeg_decoder_process(
    g_jpeg_decoder,
    &decode_cfg,
    input,
    (uint32_t) input_size,
    (uint8_t *) output,
    (uint32_t) output_capacity,
    &decoded_size
  );
#if PET_P4_KEYFRAME_BG
  keyframe_engine_unlock();
#endif
  if (err != ESP_OK) {
    if (out_error) *out_error = err;
    return false;
  }
  uint32_t mcu_width =
    picture_info.sample_method == JPEG_DOWN_SAMPLING_YUV420
      || picture_info.sample_method == JPEG_DOWN_SAMPLING_YUV422
      ? 16U : 8U;
  uint32_t mcu_height =
    picture_info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16U : 8U;
  uint32_t decoded_stride =
    (picture_info.width + mcu_width - 1U) / mcu_width * mcu_width;
  uint32_t decoded_rows =
    (picture_info.height + mcu_height - 1U) / mcu_height * mcu_height;
  if (decoded_size < decoded_stride * decoded_rows * sizeof(uint16_t)) {
    if (out_error) *out_error = ESP_ERR_INVALID_SIZE;
    return false;
  }

  if (out_width) *out_width = (int) picture_info.width;
  if (out_height) *out_height = (int) picture_info.height;
  if (out_stride) *out_stride = (int) decoded_stride;
  if (out_error) *out_error = ESP_OK;
  return true;
}

typedef struct {
  char family[PET_P4_ASSET_FAMILY_MAX];
  char path[128];
  uint32_t frame_sizes[PET_P4_MAX_MJPEG_FRAMES];
  uint32_t stream_bytes;
  int frames;
  int fps;
  uint32_t frame_duration_ms;
  uint32_t duration_ms;
  int width;
  int height;
  pet_p4_asset_codec_t codec;
} pet_p4_asset_selection_t;

typedef struct {
  const uint8_t *input;
  size_t input_size;
  size_t input_offset;
  uint16_t *output;
  size_t output_pixels;
  int width;
  int height;
} pet_p4_tjpgd_session_t;

static void blend_px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static size_t tjpgd_input_func(JDEC *jd, uint8_t *buff, size_t ndata) {
  pet_p4_tjpgd_session_t *session = jd ? (pet_p4_tjpgd_session_t *) jd->device : NULL;
  if (!session || session->input_offset >= session->input_size) return 0;
  size_t available = session->input_size - session->input_offset;
  size_t count = ndata < available ? ndata : available;
  if (buff && count > 0) {
    memcpy(buff, session->input + session->input_offset, count);
  }
  session->input_offset += count;
  return count;
}

static int tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect) {
  pet_p4_tjpgd_session_t *session = jd ? (pet_p4_tjpgd_session_t *) jd->device : NULL;
  const uint8_t *src = (const uint8_t *) bitmap;
  if (!session || !session->output || !src || !rect) return 0;

  int rect_w = (int) rect->right - (int) rect->left + 1;
  int rect_h = (int) rect->bottom - (int) rect->top + 1;
  if (rect_w <= 0 || rect_h <= 0) return 0;
  for (int y = 0; y < rect_h; y += 1) {
    int dst_y = (int) rect->top + y;
    if (dst_y < 0 || dst_y >= session->height) continue;
    for (int x = 0; x < rect_w; x += 1) {
      int dst_x = (int) rect->left + x;
      if (dst_x < 0 || dst_x >= session->width) continue;
      size_t dst_index = (size_t) dst_y * (size_t) session->width + (size_t) dst_x;
      if (dst_index >= session->output_pixels) return 0;
      size_t src_index = ((size_t) y * (size_t) rect_w + (size_t) x) * 3U;
      session->output[dst_index] = rgb565(src[src_index + 2], src[src_index + 1], src[src_index]);
    }
  }
  return 1;
}

static bool decode_jpeg_frame_tjpgd_into(const uint8_t *input, size_t input_size, uint16_t *output, size_t output_pixels, uint8_t *workbuf, int *out_width, int *out_height, JRESULT *out_error);
static bool decode_jpeg_frame_tjpgd(const uint8_t *input, size_t input_size, int *out_width, int *out_height, JRESULT *out_error) {
  return decode_jpeg_frame_tjpgd_into(input,input_size,g_jpeg_output,g_jpeg_output_capacity / sizeof(uint16_t),g_tjpgd_workbuf,out_width,out_height,out_error);
}
static bool decode_jpeg_frame_tjpgd_into(const uint8_t *input, size_t input_size, uint16_t *output, size_t output_pixels, uint8_t *workbuf, int *out_width, int *out_height, JRESULT *out_error) {
  JDEC decoder;
  pet_p4_tjpgd_session_t session = {
    .input = input,
    .input_size = input_size,
    .output = output,
    .output_pixels = output_pixels,
  };
  if (out_error) *out_error = JDR_OK;
  if (!input || input_size == 0 || !workbuf || !output) {
    if (out_error) *out_error = JDR_PAR;
    return false;
  }

  JRESULT rc = jd_prepare(&decoder, tjpgd_input_func, workbuf, PET_P4_TJPGD_WORK_BUFFER_BYTES, &session);
  if (rc != JDR_OK) {
    if (out_error) *out_error = rc;
    return false;
  }
  session.width = decoder.width;
  session.height = decoder.height;
  if (session.width <= 0 || session.height <= 0 ||
      (size_t) session.width * (size_t) session.height > session.output_pixels) {
    if (out_error) *out_error = JDR_MEM1;
    return false;
  }

  rc = jd_decomp(&decoder, tjpgd_output_func, 0);
  if (rc != JDR_OK) {
    if (out_error) *out_error = rc;
    return false;
  }
  if (out_width) *out_width = session.width;
  if (out_height) *out_height = session.height;
  return true;
}

static uint16_t blend_rgb565(uint16_t dst, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  uint8_t dr = (uint8_t) (((dst >> 11) & 0x1F) * 255 / 31);
  uint8_t dg = (uint8_t) (((dst >> 5) & 0x3F) * 255 / 63);
  uint8_t db = (uint8_t) ((dst & 0x1F) * 255 / 31);
  uint8_t nr = (uint8_t) ((r * alpha + dr * (255 - alpha)) / 255);
  uint8_t ng = (uint8_t) ((g * alpha + dg * (255 - alpha)) / 255);
  uint8_t nb = (uint8_t) ((b * alpha + db * (255 - alpha)) / 255);
  return rgb565(nr, ng, nb);
}

static struct {bool active;int x,y,right,bottom;} g_app_clip;
#if PET_P4_RASTER_EXACT
/* All raster calls share the existing single native renderer serialization.
 * No stack-sized row, allocation, DMA buffer or asynchronous scratch owner. */
static DRAM_ATTR pet_rgb565_blend_t g_blend_lookup;
#if PET_P4_GLYPH_AXIS_INTERNAL
/* The SAME512-byte axis scratch, not another allocation or texture. */
static DRAM_ATTR pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];
#else
static EXT_RAM_BSS_ATTR pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];
#endif
#endif
#if PET_P4_GLYPH_ROWS
/* Only two source rows, not a scaled glyph/frame or retained bitmap pointer.
 * Same serialized native renderer owner as the existing axis scratch. */
static DRAM_ATTR float g_glyph_rows[2][66];
static DRAM_ATTR int32_t g_glyph_sample_y;
static EXT_RAM_BSS_ATTR char g_app_label_rows[8][192];
#endif
static bool app_clip_contains(int x,int y){return !g_app_clip.active||(x>=g_app_clip.x&&y>=g_app_clip.y&&x<g_app_clip.right&&y<g_app_clip.bottom);}
static void put_px(int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= PET_P4_UI_WIDTH || y >= PET_P4_UI_HEIGHT) return;
  if(!app_clip_contains(x,y))return;
  if (!g_framebuffer) return;
  g_framebuffer[y * PET_P4_UI_WIDTH + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > PET_P4_UI_WIDTH ? PET_P4_UI_WIDTH : x + w;
  int y1 = y + h > PET_P4_UI_HEIGHT ? PET_P4_UI_HEIGHT : y + h;
  if(g_app_clip.active){if(x0<g_app_clip.x)x0=g_app_clip.x;if(y0<g_app_clip.y)y0=g_app_clip.y;if(x1>g_app_clip.right)x1=g_app_clip.right;if(y1>g_app_clip.bottom)y1=g_app_clip.bottom;}
  if (x0 >= x1 || y0 >= y1) return;
  if (!g_framebuffer) return;
#if PET_P4_RENDER_SPAN_FILL
  if(x1-x0>=32&&y1-y0>=4){
    pet_rgb565_fill_rows(g_framebuffer+(size_t)y0*PET_P4_UI_WIDTH+x0,PET_P4_UI_WIDTH,(size_t)(x1-x0),(size_t)(y1-y0),color);
    return;
  }
#endif
  for (int yy = y0; yy < y1; yy += 1) {
    uint16_t *row = g_framebuffer + yy * PET_P4_UI_WIDTH;
    for (int xx = x0; xx < x1; xx += 1) row[xx] = color;
  }
}

static void fill_ellipse(int cx, int cy, int rx, int ry, uint16_t color) {
  long long rx2 = (long long) rx * rx;
  long long ry2 = (long long) ry * ry;
  long long limit = rx2 * ry2;
  for (int y = -ry; y <= ry; y += 1) {
    for (int x = -rx; x <= rx; x += 1) {
      if ((long long) x * x * ry2 + (long long) y * y * rx2 <= limit) {
        put_px(cx + x, cy + y, color);
      }
    }
  }
}

static void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  int min_x = x0 < x1 ? x0 : x1;
  int max_x = x0 > x1 ? x0 : x1;
  int min_y = y0 < y1 ? y0 : y1;
  int max_y = y0 > y1 ? y0 : y1;
  if (x2 < min_x) min_x = x2;
  if (x2 > max_x) max_x = x2;
  if (y2 < min_y) min_y = y2;
  if (y2 > max_y) max_y = y2;

  int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
  if (area == 0) return;
  for (int y = min_y; y <= max_y; y += 1) {
    for (int x = min_x; x <= max_x; x += 1) {
      int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
      int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
      int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
      if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
          (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
        put_px(x, y, color);
      }
    }
  }
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    put_px(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static const uint8_t *font5x7(char ch) {
  static const uint8_t glyphs[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7f,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08},{0x00,0x00,0x00,0x00,0x00}
  };
  unsigned char c = (unsigned char) ch;
  if (c < 32 || c > 127) c = '?';
  return glyphs[c - 32];
}

static void draw_glyph5x7(int x, int y, char ch, uint16_t color, int scale) {
  const uint8_t *glyph = font5x7(ch);
  for (int col = 0; col < 5; col += 1) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; row += 1) {
      if ((bits >> row) & 1) {
        fill_rect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

static uint32_t utf8_next(const char **cursor) {
  const unsigned char *s = (const unsigned char *) *cursor;
  uint32_t cp = 0;
  if (!s || !s[0]) return 0;
  if (s[0] < 0x80) {
    cp = s[0];
    *cursor += 1;
  } else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
    cp = ((uint32_t) (s[0] & 0x1F) << 6) | (uint32_t) (s[1] & 0x3F);
    *cursor += 2;
  } else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
    cp = ((uint32_t) (s[0] & 0x0F) << 12) | ((uint32_t) (s[1] & 0x3F) << 6) | (uint32_t) (s[2] & 0x3F);
    *cursor += 3;
  } else if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
    cp = ((uint32_t) (s[0] & 0x07) << 18) | ((uint32_t) (s[1] & 0x3F) << 12) |
         ((uint32_t) (s[2] & 0x3F) << 6) | (uint32_t) (s[3] & 0x3F);
    *cursor += 4;
  } else {
    cp = '?';
    *cursor += 1;
  }
  return cp;
}

static uint32_t utf8_peek(const char *cursor) {
  return utf8_next(&cursor);
}

static uint8_t glyph_alpha_at(const uint8_t *bitmap, const lv_font_glyph_dsc_t *glyph, int x, int y) {
  if (!bitmap || !glyph || x < 0 || y < 0 || x >= glyph->box_w || y >= glyph->box_h) return 0;
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A8) {
    int stride = glyph->stride ? glyph->stride : glyph->box_w;
    return bitmap[y * stride + x];
  }
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A4) {
    int pixel_index = y * glyph->box_w + x;
    int byte_index = glyph->stride ? y * glyph->stride + x / 2 : pixel_index / 2;
    uint8_t byte = bitmap[byte_index];
    uint8_t nibble = (pixel_index & 1) ? (byte & 0x0F) : (byte >> 4);
    return (uint8_t) (nibble * 17);
  }
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A2) {
    int pixel_index = y * glyph->box_w + x;
    int byte_index = glyph->stride ? y * glyph->stride + x / 4 : pixel_index / 4;
    uint8_t byte = bitmap[byte_index];
    uint8_t shift = (uint8_t) ((3 - (pixel_index & 3)) * 2);
    return (uint8_t) (((byte >> shift) & 0x03) * 85);
  }
  if (glyph->format == LV_FONT_GLYPH_FORMAT_A1) {
    int pixel_index = y * glyph->box_w + x;
    int byte_index = glyph->stride ? y * glyph->stride + x / 8 : pixel_index / 8;
    uint8_t byte = bitmap[byte_index];
    return (byte & (0x80 >> (pixel_index & 7))) ? 255 : 0;
  }
  return 0;
}

static int glyph_width(uint32_t codepoint, uint32_t next_codepoint) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return 0;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    return codepoint >= 0x2300 ? 16 : 10;
  }
  return glyph.adv_w > 0 ? glyph.adv_w : glyph.box_w;
}

/* Native32 changes ink only. Layout continues to use glyph_width/font16. */
static inline bool scaled_glyph_dsc(uint32_t cp,uint32_t next,lv_font_glyph_dsc_t *glyph,int *scale){
#if PET_FONT_NATIVE_32
  if(*scale==2&&pet_p4_font_native32_has_glyph(cp)
      &&lv_font_get_glyph_dsc(&pet_p4_font_cn_32,glyph,cp,next)){
    *scale=1;return true;
  }
#else
  (void)scale;
#endif
  return lv_font_get_glyph_dsc(&pet_p4_font_cn_16,glyph,cp,next);
}
static inline int scaled_glyph_baseline(const lv_font_glyph_dsc_t *glyph,int scale){
#if PET_FONT_NATIVE_32
  return (glyph->resolved_font->line_height-glyph->resolved_font->base_line)*scale;
#else
  (void)glyph;
  return (pet_p4_font_cn_16.line_height-pet_p4_font_cn_16.base_line)*scale;
#endif
}

static bool glyph_visual_bounds(
  uint32_t codepoint,
  uint32_t next_codepoint,
  int scale,
  int *top,
  int *bottom
) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return false;
  if (scale < 1) scale = 1;
  if (!scaled_glyph_dsc(codepoint, next_codepoint, &glyph, &scale)) {
    if (codepoint == 0x00B7) {
      *top = 6 * scale;
      *bottom = 8 * scale;
    } else if (codepoint >= 0x2300) {
      *top = scale;
      *bottom = 13 * scale;
    } else {
      *top = 2 * scale;
      *bottom = 9 * scale - 1;
    }
    return true;
  }
  if (glyph.box_h <= 0 || glyph.box_w <= 0) return false;
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  int first_ink_row = glyph.box_h;
  int last_ink_row = -1;
  if (bitmap) {
    for (int y = 0; y < glyph.box_h; y += 1) {
      for (int x = 0; x < glyph.box_w; x += 1) {
        if (!glyph_alpha_at(bitmap, &glyph, x, y)) continue;
        if (y < first_ink_row) first_ink_row = y;
        if (y > last_ink_row) last_ink_row = y;
      }
    }
  }
  /* Capture resolved-font and geometry before a release callback may invalidate it. */
  int baseline = scaled_glyph_baseline(&glyph, scale);
  int glyph_top = baseline - glyph.box_h * scale - glyph.ofs_y * scale;
  lv_font_glyph_release_draw_data(&glyph);
  if (last_ink_row < first_ink_row) return false;
  *top = glyph_top + first_ink_row * scale;
  *bottom = glyph_top + (last_ink_row + 1) * scale - 1;
  return true;
}

static void text_visual_bounds(const char *text, int scale, int *top, int *bottom) {
  const char *cursor = text ? text : "";
  bool found = false;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    int glyph_top = 0;
    int glyph_bottom = 0;
    if (glyph_visual_bounds(
          codepoint,
          utf8_peek(next_cursor),
          scale,
          &glyph_top,
          &glyph_bottom
        )) {
      if (!found || glyph_top < *top) *top = glyph_top;
      if (!found || glyph_bottom > *bottom) *bottom = glyph_bottom;
      found = true;
    }
    cursor = next_cursor;
  }
  if (!found) {
    *top = 0;
    *bottom = pet_p4_font_cn_16.line_height * (scale > 0 ? scale : 1) - 1;
  }
}

static int utf8_text_width(const char *text, int byte_limit, int scale) {
  const char *cursor = text ? text : "";
  const char *end = byte_limit >= 0 ? cursor + byte_limit : NULL;
  int width = 0;
  if (scale < 1) scale = 1;
  while (*cursor && (!end || cursor < end)) {
    const char *next_cursor = cursor;
    uint32_t cp = utf8_next(&next_cursor);
    if (!cp || (end && next_cursor > end)) break;
    int glyph_advance = glyph_width(cp, utf8_peek(next_cursor));
    if (glyph_advance > 0) width += glyph_advance * scale + scale;
    cursor = next_cursor;
  }
  return width;
}

static void draw_lvgl_glyph(uint32_t codepoint, uint32_t next_codepoint, int x, int y, uint16_t color, int scale) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return;
  if (!scaled_glyph_dsc(codepoint, next_codepoint, &glyph, &scale)) {
    if (scale < 1) scale = 1;
    if (codepoint == 0x00B7) {
      fill_ellipse(x + 4 * scale, y + 7 * scale, scale, scale, color);
    } else if (codepoint >= 0x2300) {
      int cx = x + 7 * scale;
      int cy = y + 7 * scale;
      draw_line(cx, y + scale, cx, y + 13 * scale, color);
      draw_line(x + scale, cy, x + 13 * scale, cy, color);
      fill_ellipse(cx, cy, 3 * scale, 3 * scale, color);
    } else {
      draw_glyph5x7(x, y + 2 * scale, '?', color, scale);
    }
    return;
  }
  if (scale < 1) scale = 1;
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  if (!bitmap) {
    lv_font_glyph_release_draw_data(&glyph);
    return;
  }
  int baseline = y + scaled_glyph_baseline(&glyph, scale);
  int gx0 = x + glyph.ofs_x * scale;
  int gy0 = baseline - glyph.box_h * scale - glyph.ofs_y * scale;
  uint8_t r = (uint8_t) (((color >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t) (((color >> 5) & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t) ((color & 0x1F) * 255 / 31);
  for (int gy = 0; gy < glyph.box_h; gy += 1) {
    for (int gx = 0; gx < glyph.box_w; gx += 1) {
      uint8_t alpha = glyph_alpha_at(bitmap, &glyph, gx, gy);
      if (alpha) {
        for (int sy = 0; sy < scale; sy += 1) {
          for (int sx = 0; sx < scale; sx += 1) {
            blend_px(gx0 + gx * scale + sx, gy0 + gy * scale + sy, r, g, b, alpha);
          }
        }
      }
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
}

static int medium_glyph_width(uint32_t codepoint, uint32_t next_codepoint) {
  int width = glyph_width(codepoint, next_codepoint);
  return width > 0 ? (width * 3 + 1) / 2 : 0;
}

static bool medium_glyph_visual_bounds(
  uint32_t codepoint,
  uint32_t next_codepoint,
  int *top,
  int *bottom
) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return false;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    if (codepoint == 0x00B7) {
      *top = 8;
      *bottom = 12;
    } else if (codepoint >= 0x2300) {
      *top = 2;
      *bottom = 18;
    } else {
      *top = 3;
      *bottom = 16;
    }
    return true;
  }
  if (glyph.box_h <= 0 || glyph.box_w <= 0) return false;
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  int scaled_height = (glyph.box_h * 3 + 1) / 2;
  int first_ink_row = scaled_height;
  int last_ink_row = -1;
  if (bitmap) {
    for (int dy = 0; dy < scaled_height; dy += 1) {
      int source_y = dy * 2 / 3;
      if (source_y >= glyph.box_h) source_y = glyph.box_h - 1;
      for (int x = 0; x < glyph.box_w; x += 1) {
        if (!glyph_alpha_at(bitmap, &glyph, x, source_y)) continue;
        if (dy < first_ink_row) first_ink_row = dy;
        if (dy > last_ink_row) last_ink_row = dy;
      }
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
  if (last_ink_row < first_ink_row) return false;
  int baseline = ((pet_p4_font_cn_16.line_height - pet_p4_font_cn_16.base_line) * 3 + 1) / 2;
  int glyph_top = baseline - scaled_height - glyph.ofs_y * 3 / 2;
  *top = glyph_top + first_ink_row;
  *bottom = glyph_top + last_ink_row;
  return true;
}

static void text_visual_bounds_medium(const char *text, int *top, int *bottom) {
  const char *cursor = text ? text : "";
  bool found = false;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    int glyph_top = 0;
    int glyph_bottom = 0;
    if (medium_glyph_visual_bounds(
          codepoint,
          utf8_peek(next_cursor),
          &glyph_top,
          &glyph_bottom
        )) {
      if (!found || glyph_top < *top) *top = glyph_top;
      if (!found || glyph_bottom > *bottom) *bottom = glyph_bottom;
      found = true;
    }
    cursor = next_cursor;
  }
  if (!found) {
    *top = 0;
    *bottom = (pet_p4_font_cn_16.line_height * 3 + 1) / 2 - 1;
  }
}

static int utf8_text_width_medium(const char *text) {
  const char *cursor = text ? text : "";
  int width = 0;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    int glyph_advance = medium_glyph_width(codepoint, utf8_peek(next_cursor));
    if (glyph_advance > 0) width += glyph_advance + 1;
    cursor = next_cursor;
  }
  return width;
}

static void draw_lvgl_glyph_medium(
  uint32_t codepoint,
  uint32_t next_codepoint,
  int x,
  int y,
  uint16_t color
) {
  lv_font_glyph_dsc_t glyph = {0};
  if (codepoint == 0xFE0F || codepoint == 0x200D) return;
  if (!lv_font_get_glyph_dsc(&pet_p4_font_cn_16, &glyph, codepoint, next_codepoint)) {
    if (codepoint == 0x00B7) {
      fill_ellipse(x + 6, y + 10, 2, 2, color);
    } else if (codepoint >= 0x2300) {
      draw_line(x + 10, y + 2, x + 10, y + 18, color);
      draw_line(x + 2, y + 10, x + 18, y + 10, color);
      fill_ellipse(x + 10, y + 10, 4, 4, color);
    } else {
      draw_glyph5x7(x, y + 3, '?', color, 2);
    }
    return;
  }
  glyph.req_raw_bitmap = 1;
  const uint8_t *bitmap = (const uint8_t *) glyph.resolved_font->get_glyph_bitmap(&glyph, NULL);
  if (!bitmap) {
    lv_font_glyph_release_draw_data(&glyph);
    return;
  }
  int baseline = y + ((pet_p4_font_cn_16.line_height - pet_p4_font_cn_16.base_line) * 3 + 1) / 2;
  int gx0 = x + glyph.ofs_x * 3 / 2;
  int gy0 = baseline - (glyph.box_h * 3 + 1) / 2 - glyph.ofs_y * 3 / 2;
  int scaled_w = (glyph.box_w * 3 + 1) / 2;
  int scaled_h = (glyph.box_h * 3 + 1) / 2;
  uint8_t r = (uint8_t) (((color >> 11) & 0x1F) * 255 / 31);
  uint8_t g = (uint8_t) (((color >> 5) & 0x3F) * 255 / 63);
  uint8_t b = (uint8_t) ((color & 0x1F) * 255 / 31);
  for (int dy = 0; dy < scaled_h; dy += 1) {
    int source_y = dy * 2 / 3;
    if (source_y >= glyph.box_h) source_y = glyph.box_h - 1;
    for (int dx = 0; dx < scaled_w; dx += 1) {
      int source_x = dx * 2 / 3;
      if (source_x >= glyph.box_w) source_x = glyph.box_w - 1;
      uint8_t alpha = glyph_alpha_at(bitmap, &glyph, source_x, source_y);
      if (alpha) blend_px(gx0 + dx, gy0 + dy, r, g, b, alpha);
    }
  }
  lv_font_glyph_release_draw_data(&glyph);
}

static const char *draw_text_line_medium(
  const char *text,
  int x,
  int y,
  int max_px,
  uint16_t color,
  bool ellipsis
) {
  const char *cursor = text ? text : "";
  const char *last_fit = cursor;
  int pen_x = x;
  int used_px = 0;
  bool needs_ellipsis = ellipsis && utf8_text_width_medium(cursor) > max_px;
  int reserve_px = needs_ellipsis ? utf8_text_width_medium("...") : 0;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t codepoint = utf8_next(&next_cursor);
    uint32_t next_codepoint = utf8_peek(next_cursor);
    int glyph_advance = medium_glyph_width(codepoint, next_codepoint);
    int advance = glyph_advance > 0 ? glyph_advance + 1 : 0;
    if (used_px + advance + reserve_px > max_px) break;
    draw_lvgl_glyph_medium(codepoint, next_codepoint, pen_x, y, color);
    pen_x += advance;
    used_px += advance;
    cursor = next_cursor;
    last_fit = cursor;
  }
  if (needs_ellipsis && *cursor) {
    const char *dots = "...";
    while (*dots) {
      const char *next_cursor = dots;
      uint32_t codepoint = utf8_next(&next_cursor);
      uint32_t next_codepoint = utf8_peek(next_cursor);
      draw_lvgl_glyph_medium(codepoint, next_codepoint, pen_x, y, color);
      pen_x += medium_glyph_width(codepoint, next_codepoint) + 1;
      dots = next_cursor;
    }
  }
  return last_fit;
}

static int append_display_char(char *out, int used, int max_chars, const char **cursor) {
  const unsigned char *s = (const unsigned char *) *cursor;
  if (!s || !s[0] || used >= max_chars) return used;
  if (s[0] < 0x80) {
    out[used++] = (char) s[0];
    *cursor += 1;
  } else {
    out[used++] = '?';
    int skip = 1;
    if ((s[0] & 0xE0) == 0xC0) skip = 2;
    else if ((s[0] & 0xF0) == 0xE0) skip = 3;
    else if ((s[0] & 0xF8) == 0xF0) skip = 4;
    *cursor += skip;
  }
  return used;
}

static const char *draw_text_line(
  const char *text,
  int x,
  int y,
  int max_px,
  uint16_t color,
  int scale,
  bool ellipsis
) {
  if (scale < 1) scale = 1;
  const char *cursor = text ? text : "";
  const char *last_fit = cursor;
  int pen_x = x;
  int used_px = 0;
  int ellipsis_px = utf8_text_width("...", -1, scale);
  bool needs_ellipsis = ellipsis && utf8_text_width(cursor, -1, scale) > max_px;
  int reserve_px = needs_ellipsis ? ellipsis_px : 0;
  while (*cursor) {
    const char *next_cursor = cursor;
    uint32_t cp = utf8_next(&next_cursor);
    uint32_t next_cp = utf8_peek(next_cursor);
    int glyph_advance = glyph_width(cp, next_cp);
    int adv = glyph_advance > 0 ? glyph_advance * scale + scale : 0;
    if (used_px + adv + reserve_px > max_px) break;
    draw_lvgl_glyph(cp, next_cp, pen_x, y, color, scale);
    pen_x += adv;
    used_px += adv;
    cursor = next_cursor;
    last_fit = cursor;
  }
  if (needs_ellipsis && *cursor) {
    const char *dots = "...";
    while (*dots) {
      const char *next_cursor = dots;
      uint32_t cp = utf8_next(&next_cursor);
      uint32_t next_cp = utf8_peek(next_cursor);
      draw_lvgl_glyph(cp, next_cp, pen_x, y, color, scale);
      pen_x += glyph_width(cp, next_cp) * scale + scale;
      dots = next_cursor;
    }
  }
  cursor = last_fit;
  return cursor;
}

static int text_y_in_box(const char *text, int scale, int box_y, int box_h) {
  int top = 0;
  int bottom = 0;
  text_visual_bounds(text, scale, &top, &bottom);
  return (box_y * 2 + box_h - 1 - top - bottom) / 2;
}

static int text_y_in_box_medium(const char *text, int box_y, int box_h) {
  int top = 0;
  int bottom = 0;
  text_visual_bounds_medium(text, &top, &bottom);
  return (box_y * 2 + box_h - 1 - top - bottom) / 2;
}

static const char *draw_text_line_vcenter(
  const char *text,
  int x,
  int box_y,
  int box_h,
  int max_px,
  uint16_t color,
  int scale,
  bool ellipsis
) {
  return draw_text_line(
    text,
    x,
    text_y_in_box(text, scale, box_y, box_h),
    max_px,
    color,
    scale,
    ellipsis
  );
}

static const char *draw_text_line_medium_vcenter(
  const char *text,
  int x,
  int box_y,
  int box_h,
  int max_px,
  uint16_t color,
  bool ellipsis
) {
  return draw_text_line_medium(
    text,
    x,
    text_y_in_box_medium(text, box_y, box_h),
    max_px,
    color,
    ellipsis
  );
}

static int display_char_count(const char *text, int limit) {
  int count = 0;
  const unsigned char *s = (const unsigned char *) (text ? text : "");
  while (*s && count < limit) {
    if (*s < 0x80) s += 1;
    else if ((*s & 0xE0) == 0xC0) s += 2;
    else if ((*s & 0xF0) == 0xE0) s += 3;
    else if ((*s & 0xF8) == 0xF0) s += 4;
    else s += 1;
    count += 1;
  }
  return count;
}

static void fill_round_rect(int x, int y, int w, int h, int radius, uint16_t color) {
  fill_rect(x + radius, y, w - radius * 2, h, color);
  fill_rect(x, y + radius, w, h - radius * 2, color);
  fill_ellipse(x + radius, y + radius, radius, radius, color);
  fill_ellipse(x + w - radius - 1, y + radius, radius, radius, color);
  fill_ellipse(x + radius, y + h - radius - 1, radius, radius, color);
  fill_ellipse(x + w - radius - 1, y + h - radius - 1, radius, radius, color);
}

static void blend_px(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  if (x < 0 || y < 0 || x >= PET_P4_UI_WIDTH || y >= PET_P4_UI_HEIGHT) return;
  if(!app_clip_contains(x,y))return;
  if (!g_framebuffer) return;
  uint16_t *px = &g_framebuffer[y * PET_P4_UI_WIDTH + x];
#if PET_P4_ALPHA_FAST
  /* Both endpoint identities are exhaustive RGB565 tests. Do not execute
   * six channel divisions for an opaque glyph's interior. */
  if(!alpha)return;
  if(alpha==255){*px=rgb565(r,g,b);return;}
#endif
  *px = blend_rgb565(*px, r, g, b, alpha);
}

static void blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w > PET_P4_UI_WIDTH ? PET_P4_UI_WIDTH : x + w;
  int y1 = y + h > PET_P4_UI_HEIGHT ? PET_P4_UI_HEIGHT : y + h;
#if PET_P4_RASTER_EXACT
  if(g_app_clip.active){if(x0<g_app_clip.x)x0=g_app_clip.x;if(y0<g_app_clip.y)y0=g_app_clip.y;if(x1>g_app_clip.right)x1=g_app_clip.right;if(y1>g_app_clip.bottom)y1=g_app_clip.bottom;}
  if(!g_framebuffer)return;
  if(x1>x0&&y1>y0&&(x1-x0)*(y1-y0)>=1024){
    pet_rgb565_blend_prepare(&g_blend_lookup,r,g,b,alpha);
    pet_rgb565_blend_rows(g_framebuffer+(size_t)y0*PET_P4_UI_WIDTH+x0,
      PET_P4_UI_WIDTH,(size_t)(x1-x0),(size_t)(y1-y0),&g_blend_lookup);
    return;
  }
#endif
  if (x0 >= x1 || y0 >= y1) return;
  for (int yy = y0; yy < y1; yy += 1) {
    for (int xx = x0; xx < x1; xx += 1) blend_px(xx, yy, r, g, b, alpha);
  }
}

static void blend_ellipse(int cx, int cy, int rx, int ry, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  long long rx2 = (long long) rx * rx;
  long long ry2 = (long long) ry * ry;
  long long limit = rx2 * ry2;
  for (int y = -ry; y <= ry; y += 1) {
    for (int x = -rx; x <= rx; x += 1) {
      if ((long long) x * x * ry2 + (long long) y * y * rx2 <= limit) {
        blend_px(cx + x, cy + y, r, g, b, alpha);
      }
    }
  }
}

static void blend_round_rect(int x, int y, int w, int h, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
  if (w <= 0 || h <= 0 || !alpha) return;
  if (radius < 0) radius = 0;
  if (radius > w / 2) radius = w / 2;
  if (radius > h / 2) radius = h / 2;
  if (!radius) { blend_rect(x, y, w, h, r, g, b, alpha); return; }
#if PET_P4_RASTER_EXACT
  /* Three DISJOINT straight regions; keep the original AA corner formula.
   * Alpha is non-idempotent, so no pixel may be painted by two regions. */
  blend_rect(x+radius,y,w-2*radius,h,r,g,b,alpha);
  blend_rect(x,y+radius,radius,h-2*radius,r,g,b,alpha);
  blend_rect(x+w-radius,y+radius,radius,h-2*radius,r,g,b,alpha);
  for(int corner=0;corner<4;corner++){
    int cx=corner&1?x+w-radius:x,cy=corner&2?y+h-radius:y;
    int left=cx<0?0:cx,top=cy<0?0:cy;
    int right=cx+radius>PET_P4_UI_WIDTH?PET_P4_UI_WIDTH:cx+radius;
    int bottom=cy+radius>PET_P4_UI_HEIGHT?PET_P4_UI_HEIGHT:cy+radius;
    for(int yy=top;yy<bottom;yy++){
      float dy=fmaxf(0,fabsf(yy+.5f-(y+h*.5f))-(h*.5f-radius));
      for(int xx=left;xx<right;xx++){
        float dx=fmaxf(0,fabsf(xx+.5f-(x+w*.5f))-(w*.5f-radius));
        float coverage=dx>0&&dy>0?fminf(1,fmaxf(0,radius+.5f-sqrtf(dx*dx+dy*dy))):1;
        uint8_t a=(uint8_t)(alpha*coverage);if(a)blend_px(xx,yy,r,g,b,a);
      }
    }
  }
#else
  // Alpha compositing is not idempotent: overlapping rectangles/full circles
  // produce dark seams. Cover each destination pixel exactly once. Straight
  // edges need no square root; only the four bounded corner regions do.
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + w > PET_P4_UI_WIDTH ? PET_P4_UI_WIDTH : x + w;
  int y1 = y + h > PET_P4_UI_HEIGHT ? PET_P4_UI_HEIGHT : y + h;
  for (int yy = y0; yy < y1; yy++) {
    float dy = fmaxf(0, fabsf(yy + .5f - (y + h * .5f)) - (h * .5f - radius));
    for (int xx = x0; xx < x1; xx++) {
      float dx = fmaxf(0, fabsf(xx + .5f - (x + w * .5f)) - (w * .5f - radius));
      float coverage = dx > 0 && dy > 0 ? fminf(1, fmaxf(0, radius + .5f - sqrtf(dx * dx + dy * dy))) : 1;
      uint8_t a = (uint8_t) (alpha * coverage);
      if (a) blend_px(xx, yy, r, g, b, a);
    }
  }
#endif
}

static void draw_home_white_panel(int x, int y, int w, int h, int radius, uint16_t outline) {
  // The panel is opaque, so a preblended solid shadow gives the same visible
  // edge without read/modify/write alpha blending tens of thousands of PSRAM
  // pixels every frame.
  fill_round_rect(x + 2, y + 4, w, h, radius, rgb565(55, 58, 57));
  fill_round_rect(x, y, w, h, radius, outline);
  fill_round_rect(x + 2, y + 2, w - 4, h - 4, radius - 2, rgb565(255, 255, 255));
}

static void draw_session_card_panel(
  int x,
  int y,
  int w,
  int h,
  int radius,
  bool selected,
  uint16_t outline
) {
  if (!selected) {
    draw_home_white_panel(x, y, w, h, radius, outline);
    return;
  }

  uint16_t selection = rgb565(25, 151, 99);
  uint16_t selected_surface = rgb565(237, 249, 242);
  fill_round_rect(x - 2, y - 2, w + 4, h + 4, radius + 2, rgb565(173, 220, 193));
  fill_round_rect(x + 2, y + 5, w, h, radius, rgb565(46, 50, 48));
  fill_round_rect(x, y, w, h, radius, selection);
  fill_round_rect(x + 4, y + 4, w - 8, h - 8, radius - 4, selected_surface);
}

/* Renderer-only timing; includes underlying mutex waits and all timer overhead.
 * Child select/blit/voice stages are subsets of the inclusive draw stages. */
static void renderer_bubble_select(const char *owner, unsigned style, const char *title,
    const char *body, uint64_t now, pet_bubble_text_view_t *out) {
  int64_t started = esp_timer_get_time();
  pet_bubble_pixels_select(owner, style, title, body, now, out);
  pet_p4_render_trace_add(&g_render_trace.bubble_select_us,
    elapsed_us_clamped(started, esp_timer_get_time()));
}
static bool renderer_bubble_draw_pinned(const pet_bubble_text_view_t *view,
    unsigned style, unsigned page, uint16_t *framebuffer, int x, int y,
    int clip_width, uint16_t color) {
  int64_t started = esp_timer_get_time();
  bool drawn = pet_bubble_pixels_draw_pinned(view, style, page, framebuffer,
    x, y, clip_width, color);
  pet_p4_render_trace_add(&g_render_trace.pinned_blit_us,
    elapsed_us_clamped(started, esp_timer_get_time()));
  return drawn;
}

static void draw_card_body_lines(
  const char *body,
  int x,
  int first_line_y,
  int max_px,
  uint16_t color,
  const pet_bubble_text_view_t *host_pixels
) {
  if (host_pixels && host_pixels->pixels && renderer_bubble_draw_pinned(host_pixels, PET_BUBBLE_BODY_CLIPPED, 0, g_framebuffer,
      x, first_line_y, max_px, color)) return;
  const char *second_line = draw_text_line_medium_vcenter(
    body,
    x,
    first_line_y,
    27,
    max_px,
    color,
    false
  );
  while (second_line && (*second_line == ' ' || *second_line == '\t')) second_line += 1;
  if (second_line && second_line[0]) {
    draw_text_line_medium_vcenter(second_line, x, first_line_y + 27, 27, max_px, color, true);
  }
}

static void fill_round_rect_outline(
  int x,
  int y,
  int w,
  int h,
  int radius,
  uint16_t fill,
  uint16_t outline
) {
  fill_round_rect(x, y, w, h, radius, outline);
  fill_round_rect(x + 2, y + 2, w - 4, h - 4, radius - 2, fill);
}

static void draw_status_marker(pet_p4_view_status_t status, unsigned long long now_ms, int cx, int cy) {
  uint16_t white = rgb565(245, 245, 245);
  uint16_t green = rgb565(0, 190, 95);
  uint16_t red = rgb565(225, 64, 64);
  uint16_t gray = rgb565(82, 90, 105);
  uint16_t amber = rgb565(232, 157, 38);
  uint16_t teal = rgb565(0, 176, 164);

  if (status == PET_P4_VIEW_STATUS_IDLE) return;
  if (status == PET_P4_VIEW_STATUS_DONE) {
    fill_ellipse(cx, cy, 15, 15, green);
    draw_line(cx - 7, cy, cx - 2, cy + 6, white);
    draw_line(cx - 2, cy + 6, cx + 8, cy - 7, white);
    return;
  }
  if (status == PET_P4_VIEW_STATUS_ERROR) {
    fill_ellipse(cx, cy, 15, 15, red);
    draw_line(cx - 6, cy - 6, cx + 6, cy + 6, white);
    draw_line(cx + 6, cy - 6, cx - 6, cy + 6, white);
    return;
  }
  if (status == PET_P4_VIEW_STATUS_STOPPED) {
    fill_ellipse(cx, cy, 15, 15, gray);
    fill_rect(cx - 5, cy - 5, 10, 10, white);
    return;
  }
  if (status == PET_P4_VIEW_STATUS_WAITING) {
    fill_ellipse(cx, cy, 15, 15, amber);
    fill_ellipse(cx - 6, cy, 2, 2, white);
    fill_ellipse(cx, cy, 2, 2, white);
    fill_ellipse(cx + 6, cy, 2, 2, white);
    return;
  }
  int phase = (int) ((now_ms / 150ULL) % 8ULL);
  for (int i = 0; i < 8; i += 1) {
    int dx[] = {0, 9, 13, 9, 0, -9, -13, -9};
    int dy[] = {-13, -9, 0, 9, 13, 9, 0, -9};
    uint16_t c = i == phase ? teal : gray;
    fill_ellipse(cx + dx[i], cy + dy[i], 3, 3, c);
  }
}

static uint16_t view_status_color(pet_p4_view_status_t status) {
  if (status == PET_P4_VIEW_STATUS_DONE) return rgb565(85, 193, 133);
  if (status == PET_P4_VIEW_STATUS_ERROR) return rgb565(235, 89, 82);
  if (status == PET_P4_VIEW_STATUS_STOPPED) return rgb565(120, 126, 132);
  if (status == PET_P4_VIEW_STATUS_WAITING) return rgb565(255, 163, 31);
  if (status == PET_P4_VIEW_STATUS_WORKING) return rgb565(96, 196, 255);
  return rgb565(170, 176, 173);
}

static bool select_asset(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  pet_p4_asset_selection_t *out
) {
  const pet_p4_asset_entry_t *entry;
  int index;
  if (!state || !out) return false;
  index = pet_p4_behavior_select(
    &g_behavior,
    &state->asset_catalog,
    pet_p4_state_effective_lifecycle(state, now_ms),
    state->asset_revision,
    now_ms
  );
  if (index < 0 || index >= state->asset_catalog.count) return false;
  entry = &state->asset_catalog.entries[index];
  memset(out, 0, sizeof(*out));
  snprintf(out->family, sizeof(out->family), "%s", entry->family);
  snprintf(out->path, sizeof(out->path), "%s", entry->path);
  out->codec = entry->codec;
  out->stream_bytes = entry->stream_bytes;
  out->frames = entry->frames;
  out->fps = entry->fps;
  out->frame_duration_ms = entry->frame_duration_ms;
  out->duration_ms = entry->duration_ms;
  out->width = entry->width;
  out->height = entry->height;
  for (int i = 0; i < entry->frames && i < PET_P4_MAX_MJPEG_FRAMES; i += 1) {
    out->frame_sizes[i] = entry->frame_sizes[i];
  }
  return out->path[0] && out->frames > 0;
}

static bool asset_fit_layout(
  int src_w,
  int src_h,
  int *dst_x,
  int *dst_y,
  int *dst_w,
  int *dst_h,
  float *out_scale
) {
  if (src_w <= 0 || src_h <= 0 || !dst_x || !dst_y || !dst_w || !dst_h) {
    return false;
  }
  uint32_t scale_x_sixteenths =
    (((uint32_t) PET_P4_UI_WIDTH + 1U) * 16U - 1U) / (uint32_t) src_w;
  uint32_t scale_y_sixteenths =
    (((uint32_t) PET_P4_UI_HEIGHT + 1U) * 16U - 1U) / (uint32_t) src_h;
  uint32_t scale_sixteenths =
    scale_x_sixteenths < scale_y_sixteenths ? scale_x_sixteenths : scale_y_sixteenths;
  if (scale_sixteenths == 0) return false;
  if (scale_sixteenths > 255U) scale_sixteenths = 255U;

  *dst_w = (int) (((uint32_t) src_w * scale_sixteenths) / 16U);
  *dst_h = (int) (((uint32_t) src_h * scale_sixteenths) / 16U);
  if (*dst_w <= 0 || *dst_h <= 0
      || *dst_w > PET_P4_UI_WIDTH || *dst_h > PET_P4_UI_HEIGHT) {
    return false;
  }
  *dst_x = (PET_P4_UI_WIDTH - *dst_w) / 2;
  *dst_y = (PET_P4_UI_HEIGHT - *dst_h) / 2;
  if (out_scale) *out_scale = (float) scale_sixteenths / 16.0f;
  return true;
}

static bool scale_asset_frame_ppa(int src_w, int src_h, int src_stride) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  float scale;
  if (!g_ppa_srm_client || !g_framebuffer_ppa_compatible || src_stride < src_w
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, &scale)) {
    return false;
  }

  ppa_srm_oper_config_t scaling = {
    .in = {
      .buffer = g_jpeg_output,
      .pic_w = (uint32_t) src_stride,
      .pic_h = (uint32_t) src_h,
      .block_w = (uint32_t) src_w,
      .block_h = (uint32_t) src_h,
      .block_offset_x = 0,
      .block_offset_y = 0,
      .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .out = {
      .buffer = g_framebuffer,
      .buffer_size = PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
      .pic_w = PET_P4_UI_WIDTH,
      .pic_h = PET_P4_UI_HEIGHT,
      .block_offset_x = (uint32_t) dst_x,
      .block_offset_y = (uint32_t) dst_y,
      .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
    .scale_x = scale,
    .scale_y = scale,
    .rgb_swap = false,
    .byte_swap = false,
    .mode = PPA_TRANS_MODE_BLOCKING,
  };
  esp_err_t err = ppa_trace_srm(PET_PPA_TRACE_SCALE, &scaling);
  if (err == ESP_OK) {
    if (!g_logged_ppa_scaling) {
      ESP_LOGD(
        TAG,
        "P4 asset scale=PPA source=%dx%d stride=%d output=%dx%d offset=%d,%d factor=%.3f",
        src_w,
        src_h,
        src_stride,
        dst_w,
        dst_h,
        dst_x,
        dst_y,
        (double) scale
      );
      g_logged_ppa_scaling = true;
    }
    return true;
  }
  if (!g_logged_ppa_scale_fallback) {
    ESP_LOGW(TAG, "P4 PPA asset scaling failed (%s); using CPU fallback", esp_err_to_name(err));
    g_logged_ppa_scale_fallback = true;
  }
  return false;
}

static void scale_asset_frame_cpu(int src_w, int src_h, int src_stride) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  if (src_stride < src_w
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, NULL)) {
    return;
  }
  for (int y = 0; y < dst_h; y += 1) {
    int sy = (y * src_h) / dst_h;
    const uint16_t *source = g_jpeg_output + sy * src_stride;
    uint16_t *target = g_framebuffer + (dst_y + y) * PET_P4_UI_WIDTH + dst_x;
    for (int x = 0; x < dst_w; x += 1) {
      target[x] = source[(x * src_w) / dst_w];
    }
  }
}

static void scale_asset_frame_to_display(int src_w, int src_h, int src_stride) {
  if (src_w == PET_P4_UI_WIDTH && src_h == PET_P4_UI_HEIGHT && src_stride == src_w) {
    memcpy(
      g_framebuffer,
      g_jpeg_output,
      PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t)
    );
    return;
  }
  memset(g_framebuffer, 0, PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t));
  if (!scale_asset_frame_ppa(src_w, src_h, src_stride)) {
    g_render_trace.fallback |= PET_RENDER_FALLBACK_JPEG_SCALE_CPU;
    scale_asset_frame_cpu(src_w, src_h, src_stride);
  }
}

static void reset_h264_decoder(void) {
  if (g_h264_decoder) {
    if (g_h264_decoder_open) {
      (void) esp_h264_dec_close(g_h264_decoder);
    }
    (void) esp_h264_dec_del(g_h264_decoder);
  }
  g_h264_decoder = NULL;
  g_h264_decoder_open = false;
  g_h264_stream_offset = 0;
  g_h264_work_size = 0;
  g_h264_decoded_frame = -1;
  g_h264_output = NULL;
  g_h264_output_size = 0;
  g_h264_fs_path[0] = '\0';
}

/* Only render-path callers use this wrapper: out-of-frame playback resets must
 * not contaminate the next frame's diagnostics. No reset policy is changed. */
static void reset_h264_decoder_traced(pet_p4_render_trace_t *trace){
  int64_t started_us=esp_timer_get_time();reset_h264_decoder();
  pet_p4_render_trace_add(&trace->reset_us,elapsed_us_clamped(started_us,esp_timer_get_time()));
}
static void reset_h264_decoder_measured(void) {
#if PET_P4_H264_PREFETCH
  int64_t started_us = esp_timer_get_time();
  (void)h264_prefetch_reset();
  pet_p4_render_trace_add(&g_render_trace.reset_us,
    elapsed_us_clamped(started_us, esp_timer_get_time()));
#else
  reset_h264_decoder_traced(&g_render_trace);
#endif
}

void pet_p4_renderer_reset_playback(void) {
  cancel_pending_asset_prepares();
  g_last_native_valid = false;
  pet_p4_behavior_init(&g_behavior);
#if PET_P4_H264_PREFETCH
  (void)h264_prefetch_reset();
#else
  reset_h264_decoder();
#endif
  g_decoder_fs_path[0] = '\0';
}

static bool h264_stream_has_single_slice_access_units(
  const uint8_t *stream,
  size_t stream_size,
  const pet_p4_asset_cache_entry_t *job
) {
  size_t cursor = 0;
  size_t next_check = PET_P4_ASSET_PREPARE_CHUNK_BYTES;
  unsigned int access_units = 0;
  unsigned int slices_in_access_unit = 0;
  if (!stream || stream_size < 5) return false;
  while (cursor + 3 < stream_size) {
    if (cursor >= next_check) {
      vTaskDelay(1);
      if (!asset_prepare_current(job)) return false;
      next_check = cursor + PET_P4_ASSET_PREPARE_CHUNK_BYTES;
    }
    size_t prefix_size = 0;
    if (cursor + 4 <= stream_size
        && stream[cursor] == 0 && stream[cursor + 1] == 0
        && stream[cursor + 2] == 0 && stream[cursor + 3] == 1) {
      prefix_size = 4;
    } else if (stream[cursor] == 0 && stream[cursor + 1] == 0
               && stream[cursor + 2] == 1) {
      prefix_size = 3;
    }
    if (prefix_size == 0) {
      cursor += 1;
      continue;
    }
    size_t payload_start = cursor + prefix_size;
    if (payload_start >= stream_size) break;
    uint8_t nal_type = stream[payload_start] & 0x1fU;
    if (nal_type == 9U) {
      if (access_units > 0 && slices_in_access_unit != 1U) return false;
      access_units += 1U;
      slices_in_access_unit = 0;
    } else if (nal_type == 1U || nal_type == 5U) {
      if (access_units == 0) return false;
      slices_in_access_unit += 1U;
      if (slices_in_access_unit > 1U) return false;
    }
    cursor = payload_start + 1U;
  }
  return access_units > 0 && slices_in_access_unit == 1U;
}

static bool start_h264_decoder_role(const char *fs_path,const uint8_t *stream,size_t stream_size,
  pet_p4_render_trace_t *trace,const pet_p4_asset_cache_entry_t *job) {
  esp_h264_dec_cfg_sw_t config = {
    .pic_type = ESP_H264_RAW_FMT_I420,
  };
  reset_h264_decoder_traced(trace);
  if (!stream || stream_size == 0 || stream_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES) {
    return false;
  }
  int64_t copy_started_us = esp_timer_get_time();
  if (g_h264_work_capacity < stream_size) {
    uint8_t *resized = heap_caps_realloc(
      g_h264_work_stream, stream_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!resized) {
      pet_p4_render_trace_add(&trace->copy_us,
        elapsed_us_clamped(copy_started_us, esp_timer_get_time()));
      ESP_LOGW(TAG, "P4 H264 working buffer allocation failed bytes=%u", (unsigned int) stream_size);
      return false;
    }
    g_h264_work_stream = resized;
    g_h264_work_capacity = stream_size;
  }
  // Refresh once per playback/seek, not on every frame. Reuse the allocation
  // across loops and families, but never reuse the decoder-mutated contents.
  if(job){
    for(size_t offset=0;offset<stream_size;){
      if(!asset_prepare_current(job))return false;
      size_t length=stream_size-offset;
      if(length>PET_P4_ASSET_PREPARE_CHUNK_BYTES)length=PET_P4_ASSET_PREPARE_CHUNK_BYTES;
      memcpy(g_h264_work_stream+offset,stream+offset,length);offset+=length;
    }
  }else memcpy(g_h264_work_stream, stream, stream_size);
  g_h264_work_size = stream_size;
  pet_p4_render_trace_add(&trace->copy_us,
    elapsed_us_clamped(copy_started_us, esp_timer_get_time()));
  int64_t init_started_us = esp_timer_get_time();
  esp_h264_err_t err = esp_h264_dec_sw_new(&config, &g_h264_decoder);
  if (err != ESP_H264_ERR_OK || !g_h264_decoder) {
    pet_p4_render_trace_add(&trace->init_us,
      elapsed_us_clamped(init_started_us, esp_timer_get_time()));
    ESP_LOGW(TAG, "P4 H264 decoder create failed error=%d", (int) err);
    reset_h264_decoder_traced(trace);
    return false;
  }
  err = esp_h264_dec_open(g_h264_decoder);
  pet_p4_render_trace_add(&trace->init_us,
    elapsed_us_clamped(init_started_us, esp_timer_get_time()));
  if (err != ESP_H264_ERR_OK) {
    ESP_LOGW(TAG, "P4 H264 decoder open failed error=%d", (int) err);
    reset_h264_decoder_traced(trace);
    return false;
  }
  g_h264_decoder_open = true;
  snprintf(g_h264_fs_path, sizeof(g_h264_fs_path), "%s", fs_path ? fs_path : "");
  return true;
}

static bool decode_next_h264_frame_role(
  int width,
  int height,
  bool render_caller,
  uint32_t *decode_us,
  const pet_p4_asset_cache_entry_t *job
) {
  unsigned int steps = 0;
  size_t expected_output = (size_t) width * (size_t) height * 3U / 2U;
  if (!g_h264_decoder || !g_h264_decoder_open || !g_h264_work_stream
      || g_h264_work_size == 0 || width <= 0 || height <= 0) {
    return false;
  }
  while (g_h264_stream_offset < g_h264_work_size && steps++ < 64U) {
    if(job&&!asset_prepare_current(job))return false;
    size_t remaining = g_h264_work_size - g_h264_stream_offset;
    esp_h264_dec_in_frame_t input = {
      .raw_data = {
        .buffer = g_h264_work_stream + g_h264_stream_offset,
        .len = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t) remaining,
      },
    };
    esp_h264_dec_out_frame_t output = {0};
    bool boosted = render_caller && pet_p4_render_can_boost(uxTaskPriorityGet(NULL), ESP_TASK_MAIN_PRIO);
    if (boosted) {
      vTaskPrioritySet(NULL, PET_P4_H264_PRIMARY_TASK_PRIORITY);
    }
    int64_t decode_started_us = esp_timer_get_time();
    esp_h264_err_t err = esp_h264_dec_process(g_h264_decoder, &input, &output);
    pet_p4_render_trace_add(decode_us,elapsed_us_clamped(decode_started_us,esp_timer_get_time()));
    if (boosted) {
      vTaskPrioritySet(NULL, ESP_TASK_MAIN_PRIO);
    }
    if (err != ESP_H264_ERR_OK || input.consume == 0
        || input.consume > input.raw_data.len) {
      ESP_LOGW(
        TAG,
        "P4 H264 decode failed error=%d offset=%u remaining=%u consume=%u",
        (int) err,
        (unsigned int) g_h264_stream_offset,
        (unsigned int) remaining,
        (unsigned int) input.consume
      );
      return false;
    }
    g_h264_stream_offset += input.consume;
    if (output.out_size == 0) continue;
    if (!output.outbuf || output.out_size < expected_output) {
      ESP_LOGW(
        TAG,
        "P4 H264 output invalid size=%u expected=%u",
        (unsigned int) output.out_size,
        (unsigned int) expected_output
      );
      return false;
    }
    g_h264_output = output.outbuf;
    g_h264_output_size = output.out_size;
    g_h264_decoded_frame += 1;
    return true;
  }
  return false;
}

#if !PET_P4_H264_PREFETCH
static bool start_h264_decoder(const char *fs_path,const uint8_t *stream,size_t stream_size){
  return start_h264_decoder_role(fs_path,stream,stream_size,&g_render_trace,NULL);
}
static bool decode_next_h264_frame(int width,int height){
  return decode_next_h264_frame_role(width,height,true,&g_last_decode_us,NULL);
}
#endif

static bool resolve_h264_i420_layout(
  int visible_w,
  int visible_h,
  uint32_t output_size,
  int *stride,
  int *storage_h
) {
  size_t luma_bytes;
  int coded_w;
  size_t coded_h;
  if (visible_w <= 0 || visible_h <= 0 || !stride || !storage_h
      || (visible_w & 1) != 0 || (visible_h & 1) != 0
      || output_size == 0 || (output_size % 3U) != 0) {
    return false;
  }
  luma_bytes = (size_t) output_size * 2U / 3U;
  coded_w = (visible_w + 15) & ~15;
  if (coded_w < visible_w || luma_bytes % (size_t) coded_w != 0) {
    return false;
  }
  coded_h = luma_bytes / (size_t) coded_w;
  if (coded_h < (size_t) visible_h || coded_h > INT_MAX
      || (coded_h & 1U) != 0) {
    return false;
  }
  *stride = coded_w;
  *storage_h = (int) coded_h;
  return true;
}

static void pack_h264_i420_rows(const pet_p4_h264_pack_job_t *job) {
  for (int y = job->row_begin; y < job->row_end; y += 1) {
    const int src_w = job->src_w;
    const uint8_t *source_y = job->plane_y
      + (size_t) y * (size_t) job->src_stride;
    const uint8_t *source_chroma = ((y & 1) == 0 ? job->plane_u : job->plane_v)
      + (size_t) (y / 2) * (size_t) (job->src_stride / 2);
    uint8_t *target = job->packed + (size_t) y * job->packed_stride;
    int x = 0;
    for (; x + 7 < src_w; x += 8) {
#if PET_P4_H264_PACK_WORD_IO
      /* Pointer alignment is checked BEFORE any compiler assumption. Keep
       * the original byte-safe block for unaligned planes/rows and tails. */
      if (pet_h264_pack8_word_io(target, source_y + x, source_chroma + x / 2)) {
        target += 12;
        continue;
      }
#endif
      uint32_t y_lo;
      uint32_t y_hi;
      uint32_t chroma;
      uint32_t packed0;
      uint32_t packed1;
      uint32_t packed2;
      memcpy(&y_lo, source_y + x, sizeof(y_lo));
      memcpy(&y_hi, source_y + x + 4, sizeof(y_hi));
      memcpy(&chroma, source_chroma + x / 2, sizeof(chroma));
      packed0 = (chroma & 0x000000ffU)
              | ((y_lo & 0x0000ffffU) << 8)
              | ((chroma & 0x0000ff00U) << 16);
      packed1 = (y_lo >> 16)
              | (chroma & 0x00ff0000U)
              | ((y_hi & 0x000000ffU) << 24);
      packed2 = ((y_hi >> 8) & 0x000000ffU)
              | ((chroma >> 16) & 0x0000ff00U)
              | (y_hi & 0xffff0000U);
      memcpy(target, &packed0, sizeof(packed0));
      memcpy(target + 4, &packed1, sizeof(packed1));
      memcpy(target + 8, &packed2, sizeof(packed2));
      target += 12;
    }
    for (; x < src_w; x += 2) {
      *target++ = source_chroma[x / 2];
      *target++ = source_y[x];
      *target++ = source_y[x + 1];
    }
  }
}

static void h264_pack_worker(void *unused) {
  (void) unused;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    pack_h264_i420_rows(&g_h264_pack_job);
    xSemaphoreGive(g_h264_pack_done);
  }
}

static bool pack_h264_i420_for_ppa(
  const uint8_t *i420,
  int src_w,
  int src_h,
  int src_stride,
  int storage_h,
  const uint8_t **packed
) {
  if (!i420 || !packed || src_w <= 0 || src_h <= 0
      || src_stride < src_w || storage_h < src_h
      || (src_w & 1) != 0 || (src_h & 1) != 0
      || (src_stride & 1) != 0 || (storage_h & 1) != 0) {
    return false;
  }
  size_t storage_luma_bytes = (size_t) src_stride * (size_t) storage_h;
  size_t packed_luma_bytes = (size_t) src_w * (size_t) storage_h;
  size_t required = packed_luma_bytes * 3U / 2U;
  if (!g_h264_ppa_input || g_h264_ppa_input_capacity < required) {
    if (g_h264_ppa_input) heap_caps_free(g_h264_ppa_input);
    g_h264_ppa_input = (uint8_t *) heap_caps_aligned_calloc(
      PET_P4_PPA_BUFFER_ALIGNMENT,
      required,
      sizeof(uint8_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
    );
    g_h264_ppa_input_capacity = g_h264_ppa_input ? required : 0;
  }
  if (!g_h264_ppa_input) return false;

  pet_p4_h264_pack_job_t primary_job = {
    .plane_y = i420,
    .plane_u = i420 + storage_luma_bytes,
    .plane_v = i420 + storage_luma_bytes + storage_luma_bytes / 4U,
    .packed = g_h264_ppa_input,
    .packed_stride = (size_t) src_w * 3U / 2U,
    .src_w = src_w,
    .src_stride = src_stride,
    .row_begin = 0,
    .row_end = storage_h,
  };
  if (g_h264_pack_task && g_h264_pack_done && storage_h >= 2) {
    int split_row = storage_h / 2;
    g_h264_pack_job = primary_job;
    g_h264_pack_job.row_begin = split_row;
    primary_job.row_end = split_row;
    while (xSemaphoreTake(g_h264_pack_done, 0) == pdTRUE) {}
    bool boosted = pet_p4_render_can_boost(uxTaskPriorityGet(NULL), ESP_TASK_MAIN_PRIO);
    if (boosted) {
      vTaskPrioritySet(NULL, PET_P4_H264_PACK_TASK_PRIORITY);
    }
    xTaskNotifyGive(g_h264_pack_task);
    pack_h264_i420_rows(&primary_job);
    xSemaphoreTake(g_h264_pack_done, portMAX_DELAY);
    if (boosted) {
      vTaskPrioritySet(NULL, ESP_TASK_MAIN_PRIO);
    }
  } else {
    pack_h264_i420_rows(&primary_job);
  }
  *packed = g_h264_ppa_input;
  return true;
}

static void clear_framebuffer_outside_rect(int x, int y, int width, int height) {
  const size_t row_bytes = PET_P4_UI_WIDTH * sizeof(uint16_t);
  if (!g_framebuffer) return;
  if (x < 0 || y < 0 || width <= 0 || height <= 0
      || x + width > PET_P4_UI_WIDTH || y + height > PET_P4_UI_HEIGHT) {
    memset(g_framebuffer, 0, PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t));
    return;
  }
  if (y > 0) memset(g_framebuffer, 0, (size_t) y * row_bytes);
  if (y + height < PET_P4_UI_HEIGHT) {
    memset(
      g_framebuffer + (size_t) (y + height) * PET_P4_UI_WIDTH,
      0,
      (size_t) (PET_P4_UI_HEIGHT - y - height) * row_bytes
    );
  }
  if (x > 0 || x + width < PET_P4_UI_WIDTH) {
    for (int row = y; row < y + height; row += 1) {
      uint16_t *line = g_framebuffer + (size_t) row * PET_P4_UI_WIDTH;
      if (x > 0) memset(line, 0, (size_t) x * sizeof(uint16_t));
      if (x + width < PET_P4_UI_WIDTH) {
        memset(
          line + x + width,
          0,
          (size_t) (PET_P4_UI_WIDTH - x - width) * sizeof(uint16_t)
        );
      }
    }
  }
}

static bool scale_h264_frame_ppa(
  const uint8_t *packed_yuv420,
  int src_w,
  int src_h,
  int storage_h
) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  float scale;
  if (!packed_yuv420 || !g_ppa_srm_client) {
    return false;
  }
  if (!g_framebuffer_ppa_compatible
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, &scale)) {
    return false;
  }
  if (dst_x != 0 || dst_y != 0
      || dst_w != PET_P4_UI_WIDTH || dst_h != PET_P4_UI_HEIGHT) {
    clear_framebuffer_outside_rect(dst_x, dst_y, dst_w, dst_h);
  }
  ppa_srm_oper_config_t scaling = {
    .in = {
      .buffer = (void *) packed_yuv420,
      .pic_w = (uint32_t) src_w,
      .pic_h = (uint32_t) storage_h,
      .block_w = (uint32_t) src_w,
      .block_h = (uint32_t) src_h,
      .block_offset_x = 0,
      .block_offset_y = 0,
      .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
      .yuv_range = PPA_COLOR_RANGE_LIMIT,
      .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT709,
    },
    .out = {
      .buffer = g_framebuffer,
      .buffer_size = PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
      .pic_w = PET_P4_UI_WIDTH,
      .pic_h = PET_P4_UI_HEIGHT,
      .block_offset_x = (uint32_t) dst_x,
      .block_offset_y = (uint32_t) dst_y,
      .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
    .scale_x = scale,
    .scale_y = scale,
    .rgb_swap = false,
    .byte_swap = false,
    .mode = PPA_TRANS_MODE_BLOCKING,
  };
  int64_t convert_started_us = esp_timer_get_time();
  esp_err_t err = ppa_trace_srm(PET_PPA_TRACE_CONVERT, &scaling);
  g_last_h264_convert_us += elapsed_us_clamped(convert_started_us, esp_timer_get_time());
  if (err == ESP_OK) {
    if (!g_logged_h264_ppa) {
      ESP_LOGD(
        TAG,
        "P4 H264 convert=PPA source=%dx%d output=%dx%d offset=%d,%d factor=%.3f",
        src_w,
        src_h,
        dst_w,
        dst_h,
        dst_x,
        dst_y,
        (double) scale
      );
      g_logged_h264_ppa = true;
    }
    return true;
  }
  if (!g_logged_h264_ppa_fallback) {
    ESP_LOGW(TAG, "P4 H264 PPA conversion failed (%s); using CPU fallback", esp_err_to_name(err));
    g_logged_h264_ppa_fallback = true;
  }
  return false;
}

static bool h264_frame_supports_ppa_yuv420(int width, int storage_h) {
  return width > 0 && storage_h > 0
    && (width % 16) == 0
    && (storage_h % 16) == 0;
}

static uint8_t clamp_u8(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return (uint8_t) value;
}

static void scale_h264_frame_cpu(
  const uint8_t *i420,
  int src_w,
  int src_h,
  int src_stride,
  int storage_h
) {
  int dst_x;
  int dst_y;
  int dst_w;
  int dst_h;
  if (!i420 || src_stride < src_w || storage_h < src_h
      || !asset_fit_layout(src_w, src_h, &dst_x, &dst_y, &dst_w, &dst_h, NULL)) {
    return;
  }
  const uint8_t *plane_y = i420;
  size_t storage_luma_bytes = (size_t) src_stride * (size_t) storage_h;
  const uint8_t *plane_u = plane_y + storage_luma_bytes;
  const uint8_t *plane_v = plane_u + storage_luma_bytes / 4U;
  memset(g_framebuffer, 0, PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t));
  for (int y = 0; y < dst_h; y += 1) {
    int sy = y * src_h / dst_h;
    uint16_t *target = g_framebuffer + (dst_y + y) * PET_P4_UI_WIDTH + dst_x;
    for (int x = 0; x < dst_w; x += 1) {
      int sx = x * src_w / dst_w;
      int luma = (int) plane_y[sy * src_stride + sx] - 16;
      int u = (int) plane_u[(sy / 2) * (src_stride / 2) + sx / 2] - 128;
      int v = (int) plane_v[(sy / 2) * (src_stride / 2) + sx / 2] - 128;
      if (luma < 0) luma = 0;
      int red = (298 * luma + 459 * v + 128) >> 8;
      int green = (298 * luma - 55 * u - 136 * v + 128) >> 8;
      int blue = (298 * luma + 541 * u + 128) >> 8;
      target[x] = rgb565(clamp_u8(red), clamp_u8(green), clamp_u8(blue));
    }
  }
}

#include "pet_p4_h264_prefetch.inc"

#if !PET_P4_H264_PREFETCH
static bool render_h264_asset_frame(
  const pet_p4_asset_selection_t *asset,
  const char *fs_path,
  const pet_p4_asset_cache_entry_t *stream,
  unsigned long long cycle_elapsed_ms
) {
  int target_frame;
  int i420_stride;
  int i420_storage_h;
  const uint8_t *packed_yuv420 = NULL;
  if (!asset || !fs_path || !stream || asset->frames <= 0
      || asset->width <= 0 || asset->height <= 0) {
    return false;
  }
  /* Immutable cache entries are validated once by the low-priority preparer.
   * validate_us remains render-thread wall time (zero), never worker CPU time. */
  if (!stream->single_slice_valid) return false;
  target_frame = (int) (
    (cycle_elapsed_ms * (unsigned long long) asset->frames)
    / (unsigned long long) asset->duration_ms
  );
  if (target_frame >= asset->frames) target_frame = asset->frames - 1;
  g_render_trace.restart_reason |= pet_p4_render_restart_reason(
    g_h264_decoder != NULL, strcmp(g_h264_fs_path, fs_path) == 0,
    target_frame < g_h264_decoded_frame
  );
  if (!g_h264_decoder || strcmp(g_h264_fs_path, fs_path) != 0
      || target_frame < g_h264_decoded_frame) {
    if (!start_h264_decoder(fs_path, stream->bytes, stream->size)) return false;
  }
  unsigned int decoded_in_pass = 0;
  int64_t decode_outer_started_us = esp_timer_get_time();
  while (g_h264_decoded_frame < target_frame
         && decoded_in_pass < PET_P4_H264_MAX_DECODE_FRAMES_PER_RENDER) {
    if (!decode_next_h264_frame(asset->width, asset->height)) {
      pet_p4_render_trace_add(&g_render_trace.decode_outer_us,
        elapsed_us_clamped(decode_outer_started_us, esp_timer_get_time()));
      return false;
    }
    decoded_in_pass += 1U;
    if ((decoded_in_pass % 8U) == 0U) vTaskDelay(1);
  }
  pet_p4_render_trace_add(&g_render_trace.decode_outer_us,
    elapsed_us_clamped(decode_outer_started_us, esp_timer_get_time()));
  if (!g_h264_output
      || g_h264_output_size < (uint32_t) (asset->width * asset->height * 3 / 2)) {
    return false;
  }
  int64_t scale_started_us = esp_timer_get_time();
  if (!resolve_h264_i420_layout(
        asset->width,
        asset->height,
        g_h264_output_size,
        &i420_stride,
        &i420_storage_h
      )) {
    pet_p4_render_trace_add(&g_render_trace.scale_us,
      elapsed_us_clamped(scale_started_us, esp_timer_get_time()));
    ESP_LOGW(
      TAG,
      "P4 H264 output layout invalid visible=%dx%d bytes=%u",
      asset->width,
      asset->height,
      (unsigned int) g_h264_output_size
    );
    return false;
  }
  bool converted = false;
  if (h264_frame_supports_ppa_yuv420(asset->width, i420_storage_h)) {
    int64_t pack_started_us = esp_timer_get_time();
    bool packed_ok = pack_h264_i420_for_ppa(
      g_h264_output,
      asset->width,
      asset->height,
      i420_stride,
      i420_storage_h,
      &packed_yuv420
    );
    g_last_h264_pack_us += elapsed_us_clamped(pack_started_us, esp_timer_get_time());
    if (packed_ok) {
      converted = scale_h264_frame_ppa(
        packed_yuv420,
        asset->width,
        asset->height,
        i420_storage_h
      );
    }
  }
  if (!converted) {
    g_render_trace.fallback |= PET_RENDER_FALLBACK_H264_CPU;
    scale_h264_frame_cpu(
      g_h264_output,
      asset->width,
      asset->height,
      i420_stride,
      i420_storage_h
    );
  }
  pet_p4_render_trace_add(&g_render_trace.scale_us,
    elapsed_us_clamped(scale_started_us, esp_timer_get_time()));
  if (!g_logged_h264_decoder) {
    ESP_LOGD(
      TAG,
      "P4 H264 decoder=%s output=I420 visible=%dx%d storage=%dx%d bytes=%u",
      PET_P4_H264_DECODER_MODE,
      asset->width,
      asset->height,
      i420_stride,
      i420_storage_h,
      (unsigned int) g_h264_output_size
    );
    g_logged_h264_decoder = true;
  }
  return true;
}
#endif

#if PET_P4_CONVERSATION_VIDEO
/* Render-mutex owned metadata, never a retained cache pointer. If a listening
 * or speaking clip is not cached, continue the last valid animation rather
 * than doing filesystem reads during conversation. Replacement/storage edits
 * invalidate it; a revoked or physically unfinished buffer is never reused. */
static struct {
  pet_p4_asset_selection_t asset;
  char fs_path[80];
  uint64_t storage_epoch,cancel_epoch;
  unsigned int revision;
  int source_slot;
  bool valid;
} g_conversation_animation;
static pet_p4_asset_cache_entry_t *conversation_animation_cached(unsigned int revision,
    int source_slot,uint64_t request_epoch,uint64_t storage_epoch){
  portENTER_CRITICAL(&g_asset_prepare_mux);
  bool current=!g_asset_prepare_suspended&&request_epoch==g_asset_prepare_cancel_epoch
    &&storage_epoch==g_asset_prepare_storage_epoch;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  if(!current)return NULL;
  if(!g_conversation_animation.valid||g_conversation_animation.revision!=revision
      ||g_conversation_animation.source_slot!=source_slot
      ||g_conversation_animation.cancel_epoch!=request_epoch
      ||g_conversation_animation.storage_epoch!=storage_epoch)return NULL;
  for(unsigned i=0;i<PET_P4_ASSET_CACHE_SLOTS;i++){
    pet_p4_asset_cache_entry_t *entry=&g_asset_cache[i];
    if(entry->bytes&&entry->h264&&!entry->failed&&asset_cache_entry_finished(entry)
        &&entry->storage_epoch==storage_epoch&&entry->source_slot==source_slot
        &&entry->size==g_conversation_animation.asset.stream_bytes
        &&!strcmp(entry->fs_path,g_conversation_animation.fs_path)
        &&pet_resources_device_current(entry->buffer_lease))return entry;
  }
  return NULL;
}
#endif
static bool render_asset_pet_frame(const pet_p4_runtime_state_t *state, const pet_p4_view_model_t *view, unsigned long long now_ms) {
  char fs_path[80];
  pet_p4_asset_selection_t asset;
  portENTER_CRITICAL(&g_asset_prepare_mux);
  uint64_t request_epoch = g_asset_prepare_cancel_epoch;
  uint64_t storage_epoch = g_asset_prepare_storage_epoch;
  portEXIT_CRITICAL(&g_asset_prepare_mux);
  int source_slot = pet_p4_asset_active_slot();
  int64_t resolve_started_us = esp_timer_get_time();
  if (!select_asset(state, now_ms, &asset)) {
    pet_p4_render_trace_add(&g_render_trace.resolve_us,
      elapsed_us_clamped(resolve_started_us, esp_timer_get_time()));
    if (!g_logged_asset_failure) {
      ESP_LOGW(TAG, "P4 asset select failed manifest_len=%u family_count=%u status=%d",
               state && state->asset_manifest_json[0] ? (unsigned int) strlen(state->asset_manifest_json) : 0,
               state ? state->asset_family_count : 0,
               view ? (int) view->status : -1);
      g_logged_asset_failure = true;
    }
    return false;
  }
  if (!pet_p4_asset_fs_path_for_slot(source_slot, asset.path, false, fs_path, sizeof(fs_path))) {
    pet_p4_render_trace_add(&g_render_trace.resolve_us,
      elapsed_us_clamped(resolve_started_us, esp_timer_get_time()));
    if (!g_logged_asset_failure) {
      ESP_LOGW(TAG, "P4 asset path rejected: %s", asset.path);
      g_logged_asset_failure = true;
    }
    return false;
  }
  pet_p4_render_trace_add(&g_render_trace.resolve_us,
    elapsed_us_clamped(resolve_started_us, esp_timer_get_time()));
  g_render_trace.stream_size = asset.stream_bytes;
  if (asset.codec == PET_P4_ASSET_CODEC_H264) {
#ifdef CONFIG_ESP_H264_DUAL_TASK
    g_render_trace.backend = PET_RENDER_BACKEND_H264_DUAL;
#else
    g_render_trace.backend = PET_RENDER_BACKEND_H264_SINGLE;
#endif
  } else {
    g_render_trace.backend = PET_RENDER_BACKEND_JPEG_HARDWARE;
  }
  if ((state && g_asset_cache_revision != state->asset_revision)
      || g_asset_renderer_storage_epoch != storage_epoch) {
    g_render_trace.restart_reason |= PET_RENDER_RESTART_REVISION;
    reset_h264_decoder_measured();
    int64_t cache_started_us = esp_timer_get_time();
    clear_asset_stream_cache();
    pet_p4_render_trace_add(&g_render_trace.cache_us,
      elapsed_us_clamped(cache_started_us, esp_timer_get_time()));
    if (state) g_asset_cache_revision = state->asset_revision;
    g_asset_renderer_storage_epoch = storage_epoch;
    g_decoder_fs_path[0] = '\0';
  }
  if (strcmp(g_decoder_fs_path, fs_path) != 0) {
    snprintf(g_decoder_fs_path, sizeof(g_decoder_fs_path), "%s", fs_path);
    note_asset_decode_success();
  }
  if (asset_decode_is_in_backoff()) {
    return false;
  }

  size_t stream_size = asset.stream_bytes;
  if (stream_size == 0 || stream_size > PET_P4_ASSET_CACHE_MAX_FILE_BYTES) return false;
  int64_t cache_started_us = esp_timer_get_time();
  pet_p4_asset_cache_entry_t *stream = load_asset_stream_cache(asset.path, fs_path, stream_size,
    asset.codec == PET_P4_ASSET_CODEC_H264, source_slot, request_epoch, storage_epoch);
  pet_p4_render_trace_add(&g_render_trace.cache_us,
    elapsed_us_clamped(cache_started_us, esp_timer_get_time()));
#if PET_P4_CONVERSATION_VIDEO
  if(!stream&&state&&pet_p4_audio_conversation_video_ready()){
    stream=conversation_animation_cached(state->asset_revision,source_slot,request_epoch,storage_epoch);
    if(stream){
      asset=g_conversation_animation.asset;
      snprintf(fs_path,sizeof(fs_path),"%s",g_conversation_animation.fs_path);
      g_asset_cache_pending=false;g_render_trace.cache_hit=PET_RENDER_CACHE_HIT;
      g_render_trace.stream_size=asset.stream_bytes;
    }
  }
#endif
  if (!stream) {
    if (g_asset_cache_pending) return false;
    if (!g_logged_asset_failure) {
      ESP_LOGW(
        TAG,
        "P4 asset PSRAM cache unavailable logical=%s fs=%s bytes=%u",
        asset.path,
        fs_path,
        (unsigned int) stream_size
      );
      g_logged_asset_failure = true;
    }
    note_asset_decode_failure();
    return false;
  }

  unsigned long long playback_duration_ms = asset.duration_ms;
  if (playback_duration_ms == 0 && asset.frame_duration_ms > 0) {
    playback_duration_ms = (unsigned long long) asset.frame_duration_ms * (unsigned long long) asset.frames;
  } else if (playback_duration_ms == 0 && asset.fps > 0) {
    playback_duration_ms = ((unsigned long long) asset.frames * 1000ULL) / (unsigned long long) asset.fps;
  }
  if (playback_duration_ms == 0) playback_duration_ms = PET_P4_RENDER_INTERVAL_MS * (unsigned long long) asset.frames;
  unsigned long long playback_elapsed_ms = now_ms >= g_behavior.selected_since_ms
    ? now_ms - g_behavior.selected_since_ms : 0;
  unsigned long long cycle_elapsed_ms = playback_elapsed_ms % playback_duration_ms;
  g_last_decode_us = 0;
  if (asset.codec == PET_P4_ASSET_CODEC_H264) {
#if PET_P4_H264_PREFETCH
    bool rendered=render_h264_asset_frame_async(&asset,fs_path,stream,playback_duration_ms);
#else
    bool rendered = render_h264_asset_frame(&asset, fs_path, stream, cycle_elapsed_ms);
#endif
    if (!rendered) {
      if(g_asset_cache_pending)return false; /* native work is not an asset error */
      if (!g_logged_asset_failure) {
        ESP_LOGW(
          TAG,
          "P4 H264 render failed logical=%s frame=%d offset=%u bytes=%u",
          asset.path,
          g_h264_decoded_frame,
          (unsigned int) g_h264_stream_offset,
          (unsigned int) stream->size
        );
        g_logged_asset_failure = true;
      }
      note_asset_decode_failure();
      return false;
    }
    if (!g_logged_asset || strcmp(g_rendered_family, asset.family) != 0) {
      ESP_LOGD(TAG, "rendering P4 family=%s asset=%s", asset.family, asset.path);
      snprintf(g_rendered_family, sizeof(g_rendered_family), "%s", asset.family);
      g_logged_asset = true;
    }
    note_asset_decode_success();
#if PET_P4_CONVERSATION_VIDEO
    if(state){
      g_conversation_animation.asset=asset;
      snprintf(g_conversation_animation.fs_path,sizeof(g_conversation_animation.fs_path),"%s",fs_path);
      g_conversation_animation.revision=state->asset_revision;
      g_conversation_animation.source_slot=source_slot;
      g_conversation_animation.storage_epoch=storage_epoch;
      g_conversation_animation.cancel_epoch=request_epoch;
      g_conversation_animation.valid=true;
    }
#endif
    return true;
  }
#if PET_P4_H264_PREFETCH
  if(!h264_prefetch_reset()){g_asset_cache_pending=true;return false;}
#else
  if (g_h264_decoder) {
    g_render_trace.restart_reason |= PET_RENDER_RESTART_CODEC;
    reset_h264_decoder_measured();
  }
#endif
  int frame_index = (int) (
    (cycle_elapsed_ms * (unsigned long long) asset.frames) / playback_duration_ms
  );
  if (frame_index >= asset.frames) frame_index = asset.frames - 1;
  size_t offset = 0;
  for (int i = 0; i < frame_index; i += 1) offset += asset.frame_sizes[i];
  uint32_t frame_size = asset.frame_sizes[frame_index];
  if (frame_size == 0 || frame_size > PET_P4_MAX_JPEG_BYTES
      || offset > stream->size || frame_size > stream->size - offset) {
    return false;
  }
  int64_t copy_started_us = esp_timer_get_time();
  memcpy(g_jpeg_input, stream->bytes + offset, frame_size);
  pet_p4_render_trace_add(&g_render_trace.copy_us,
    elapsed_us_clamped(copy_started_us, esp_timer_get_time()));

  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_stride = 0;
  esp_err_t hardware_decode_err = ESP_ERR_INVALID_STATE;
  JRESULT decode_err = JDR_OK;
  int64_t decode_started_us = esp_timer_get_time();
  bool decoded_with_hardware = decode_jpeg_frame_hardware(
    g_jpeg_input,
    frame_size,
    &decoded_width,
    &decoded_height,
    &decoded_stride,
    &hardware_decode_err
  );
  bool decoded = decoded_with_hardware;
  if (!decoded) {
    g_render_trace.backend = PET_RENDER_BACKEND_JPEG_TJPGD;
    g_render_trace.fallback |= PET_RENDER_FALLBACK_JPEG_TJPGD;
    decoded = decode_jpeg_frame_tjpgd(
      g_jpeg_input,
      frame_size,
      &decoded_width,
      &decoded_height,
      &decode_err
    );
    if (decoded) decoded_stride = decoded_width;
    if (decoded && g_jpeg_decoder && !g_logged_hardware_fallback) {
      ESP_LOGW(
        TAG,
        "P4 hardware JPEG decode failed (%s); using TJPGD fallback",
        esp_err_to_name(hardware_decode_err)
      );
      g_logged_hardware_fallback = true;
    }
  }
  g_last_decode_us = elapsed_us_clamped(decode_started_us, esp_timer_get_time());
  g_render_trace.decode_outer_us = g_last_decode_us;
  if (!decoded) {
    if (!g_logged_asset_failure) {
      ESP_LOGW(
        TAG,
        "P4 JPEG decode failed logical=%s frame=%d size=%u out_cap=%u hardware=%s tjpgd=%d",
        asset.path,
        frame_index,
        frame_size,
        (unsigned int) g_jpeg_output_capacity,
        esp_err_to_name(hardware_decode_err),
        (int) decode_err
      );
      g_logged_asset_failure = true;
    }
    note_asset_decode_failure();
    return false;
  }
  if (decoded_with_hardware && !g_logged_hardware_decoder) {
    ESP_LOGD(
      TAG,
      "P4 JPEG decoder=hardware output=RGB565 size=%dx%d",
      decoded_width,
      decoded_height
    );
    g_logged_hardware_decoder = true;
  }

  int src_w = decoded_width;
  int src_h = decoded_height;
  if (src_w <= 0 || src_h <= 0) return false;
  if (src_w > PET_P4_ASSET_WIDTH) src_w = PET_P4_ASSET_WIDTH;
  if (src_h > PET_P4_ASSET_HEIGHT) src_h = PET_P4_ASSET_HEIGHT;
  int64_t scale_started_us = esp_timer_get_time();
  scale_asset_frame_to_display(src_w, src_h, decoded_stride);
  pet_p4_render_trace_add(&g_render_trace.scale_us,
    elapsed_us_clamped(scale_started_us, esp_timer_get_time()));
  if (!g_logged_asset || strcmp(g_rendered_family, asset.family) != 0) {
    ESP_LOGD(TAG, "rendering P4 family=%s asset=%s", asset.family, asset.path);
    snprintf(g_rendered_family, sizeof(g_rendered_family), "%s", asset.family);
    g_logged_asset = true;
  }
  note_asset_decode_success();
  return true;
}

/* Called after the appearance attempt and before any main-page overlays. */
static void main_compact_underlay_prepare(bool drew_asset, bool compact_requested) {
  g_main_compact_frame_clean = drew_asset;
  if (drew_asset) pet_compact_underlay_forget(&g_main_compact_underlay);
  else pet_compact_underlay_restore(&g_main_compact_underlay, g_framebuffer);
  /* Once another overlay owns this frame, no later pending compact may capture
   * its pixels. Wait for the next clean appearance instead. */
  if (!compact_requested) pet_compact_underlay_forget(&g_main_compact_underlay);
}

static void draw_bubble(const pet_p4_runtime_state_t *state,
    const pet_p4_view_model_t *view, unsigned long long now_ms) {
  if (view && !view->show_bubble) return;
  int64_t content_started = esp_timer_get_time();
  bool compact = view ? view->compact_bubble : true;
  pet_bubble_text_view_t text = {0};
  char owner[160];
  snprintf(owner, sizeof(owner), "%s:%s", view && view->realtime_conversation ? "conversation" : "bubble",
    state ? (view && view->realtime_conversation ? state->conversation_id : state->current_session_id) : "");
  const char *requested_title = view && view->title ? view->title : "";
  const char *requested_body = view && view->body ? view->body : "";
  renderer_bubble_select(owner, compact ? PET_BUBBLE_COMPACT
      : view && view->realtime_conversation ? PET_BUBBLE_BODY : PET_BUBBLE_BODY_CLIPPED,
    compact ? "" : requested_title, compact && !requested_body[0] ? requested_title : requested_body,
    now_ms, &text);
  if (!compact) {
    int x = PET_P4_SESSION_CARD_X;
    int y = PET_P4_SESSION_CARD_BOTTOM - PET_P4_SESSION_CARD_HEIGHT;
    int w = PET_P4_SESSION_CARD_WIDTH;
    int h = PET_P4_SESSION_CARD_HEIGHT;
    const char *title = text.title;
    const char *body = text.body;
    uint16_t ink = rgb565(28, 31, 31);
    uint16_t muted = rgb565(55, 61, 59);
    bool realtime = view && view->realtime_conversation;
    uint16_t selection = realtime ? rgb565(42, 119, 225) : rgb565(25, 151, 99);
    pet_p4_view_status_t status = view ? view->status : PET_P4_VIEW_STATUS_IDLE;
    if (!realtime) draw_session_card_panel(x, y, w, h, PET_P4_SESSION_CARD_RADIUS, true, selection);
    if (realtime) {
      // Same card geometry, surface and shadow; replace only the selected outline.
      fill_round_rect(x - 2, y - 2, w + 4, h + 4, PET_P4_SESSION_CARD_RADIUS + 2, rgb565(170, 205, 250));
      fill_round_rect(x + 2, y + 5, w, h, PET_P4_SESSION_CARD_RADIUS, rgb565(46, 50, 48));
      fill_round_rect(x, y, w, h, PET_P4_SESSION_CARD_RADIUS, selection);
      fill_round_rect(x + 4, y + 4, w - 8, h - 8, PET_P4_SESSION_CARD_RADIUS - 4, rgb565(237, 249, 242));
    }
    fill_round_rect(x + 10, y + 14, 7, h - 28, 2, selection);
    unsigned host_pages = realtime ? pet_bubble_pixels_pages(PET_BUBBLE_BODY, body) : 0;
    uint64_t caption_since = text.since_ms > view->caption_since_ms ? text.since_ms : view->caption_since_ms;
    unsigned host_page = host_pages ? (unsigned)(((now_ms >= caption_since ? now_ms-caption_since : 0)/2800ULL)%host_pages) : 0;
    if (realtime && host_pages) pet_bubble_pixels_pin_page(&text,host_page);
    if (!(text.pixels && renderer_bubble_draw_pinned(&text, PET_BUBBLE_TITLE, 0, g_framebuffer,
        x + 30, y + 10, w - 82, ink)))
      draw_text_line_vcenter(title, x + 30, y + 10, 38, w - 82, ink, 2, true);
    if (body[0]) {
      if (realtime) {
        bool host_drawn = host_pages && text.pixels && renderer_bubble_draw_pinned(&text,
          PET_BUBBLE_BODY, host_page, g_framebuffer, x + 30, y + 50, w - 60, muted);
        if (!host_drawn) {
          // Roll through pairs of wrapped lines without allocating or cutting UTF-8.
          const char *pages[32];
          unsigned int count = 0;
          const char *cursor = body;
          while (*cursor && count < 32) {
            pages[count++] = cursor;
            for (int line = 0; line < 2 && *cursor; line++) {
              int width = 0;
              const char *start = cursor;
              while (*cursor) {
                const char *next = cursor;
                uint32_t cp = utf8_next(&next);
                int advance = medium_glyph_width(cp, utf8_peek(next));
                advance = advance > 0 ? advance + 1 : 0;
                if (width + advance > w - 60) break;
                width += advance;
                cursor = next;
              }
              if (cursor == start) { utf8_next(&cursor); }
            }
          }
          unsigned long long elapsed = now_ms >= view->caption_since_ms ? now_ms - view->caption_since_ms : 0;
          unsigned int page = count > 0 ? (unsigned int) ((elapsed / 2800ULL) % count) : 0;
          draw_card_body_lines(count ? pages[page] : body, x + 30, y + 50, w - 60, muted, NULL);
        }
      } else draw_card_body_lines(body, x + 30, y + 50, w - 60, muted, &text);
    }
    if (realtime) draw_text_line_vcenter(view->conversation_history ? "回看" : "对话", x + w - 78, y + 14, 30, 64, selection, 1, true);
    else draw_status_marker(status, now_ms, x + w - 26, y + 26);
    pet_bubble_pixels_release(&text);
    pet_p4_render_trace_add(&g_render_trace.bubble_draw_us,
      elapsed_us_clamped(content_started, esp_timer_get_time()));
    return;
  }

  int h = 56;
  int x = 20;
  int y = PET_P4_UI_HEIGHT - h - 20;
  int w = 220;
  int text_x = x + 24;
  uint16_t ink = rgb565(28, 31, 31);
  uint16_t outline = rgb565(216, 221, 218);
  uint16_t accent = view_status_color(view ? view->status : PET_P4_VIEW_STATUS_IDLE);
  const char *body = text.body;
  w = 72 + utf8_text_width(body, -1, 2);
  if (w < 160) w = 160;
  if (w > 420) w = 420;
  if (w & 1) w += 1;
  if (!pet_compact_underlay_begin(&g_main_compact_underlay, g_framebuffer,
      (unsigned)(w + 2), g_main_compact_frame_clean)) {
    pet_bubble_pixels_release(&text);
    pet_p4_render_trace_add(&g_render_trace.bubble_draw_us,
      elapsed_us_clamped(content_started, esp_timer_get_time()));
    return;
  }
  draw_home_white_panel(x, y, w, h, 16, outline);
  fill_round_rect(x + 10, y + 14, 4, h - 28, 2, accent);
  if (!(text.pixels && renderer_bubble_draw_pinned(&text, PET_BUBBLE_COMPACT, 0, g_framebuffer,
      text_x, y + (h - 38) / 2 + PET_P4_COMPACT_TEXT_OPTICAL_Y, w - 72, ink))) draw_text_line(
    body,
    text_x,
    text_y_in_box(body, 2, y, h) + PET_P4_COMPACT_TEXT_OPTICAL_Y,
    w - 72,
    ink,
    2,
    true
  );
  draw_status_marker(
    view ? view->status : PET_P4_VIEW_STATUS_IDLE,
    now_ms,
    x + w - 28,
    y + 28
  );
  pet_bubble_pixels_release(&text);
    pet_p4_render_trace_add(&g_render_trace.bubble_draw_us,
      elapsed_us_clamped(content_started, esp_timer_get_time()));
}

static void draw_touch_feedback_pixels(int x,int y) {
  uint16_t white = rgb565(255, 255, 255);
  uint16_t teal = rgb565(0, 170, 162);
  fill_ellipse(x, y, 13, 13, white);
  fill_ellipse(x, y, 9, 9, teal);
  fill_ellipse(x, y, 3, 3, white);
}
static void draw_touch_feedback(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!pet_p4_state_touch_feedback_active(state, now_ms)) return;
  draw_touch_feedback_pixels(state->local_touch_x,state->local_touch_y);
}


static void draw_text_right(
  const char *text,
  int right_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
);
static void draw_text_center(
  const char *text,
  int center_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
);

// Geometric icons do not depend on emoji or private-use font glyphs.
static void draw_component_icon(const char *id, int x, int y, uint16_t ink, uint16_t tile) {
  fill_round_rect(x, y, 44, 44, 12, tile);
  if (strcmp(id, "stock-watchlist") == 0) {
    fill_round_rect(x+10, y+25, 5, 10, 2, ink);
    fill_round_rect(x+20, y+19, 5, 16, 2, ink);
    fill_round_rect(x+30, y+9, 5, 26, 2, ink);
  } else if (strcmp(id, "music-player") == 0) {
    fill_rect(x+18, y+11, 3, 22, ink);
    fill_rect(x+31, y+8, 3, 22, ink);
    for (int t=0; t<4; ++t) draw_line(x+18, y+11+t, x+33, y+8+t, ink);
    fill_ellipse(x+14, y+32, 6, 4, ink);
    fill_ellipse(x+27, y+29, 6, 4, ink);
  } else if (strcmp(id, "upcoming-todos") == 0) {
    for (int row=0; row<3; ++row) {
      const int top=y+11+row*10;
      fill_round_rect(x+20, top+2, 15, 3, 1, ink);
      for (int t=0; t<2; ++t) {
        draw_line(x+9, top+2+t, x+12, top+5+t, ink);
        draw_line(x+12, top+5+t, x+17, top+t, ink);
      }
    }
  } else if (strcmp(id, "computer-status") == 0) {
    fill_round_rect(x+8, y+10, 28, 20, 3, ink);
    fill_round_rect(x+11, y+13, 22, 14, 1, tile);
    fill_rect(x+20, y+30, 4, 5, ink);
    fill_round_rect(x+14, y+35, 16, 3, 1, ink);
  } else {
    for (int row=0; row<2; ++row) for (int col=0; col<2; ++col)
      fill_round_rect(x+9+col*15, y+9+row*15, 11, 11, 3, ink);
  }
}

static const char *component_card_caption(const char *id) {
  for(size_t i=0;i<CENTER_ART_COUNT;i++)
    if(!strcmp(id,center_art_catalog[i].id))return center_art_catalog[i].caption;
  if (strcmp(id, "stock-watchlist") == 0) return "实时行情";
  if (strcmp(id, "music-player") == 0) return "音乐与歌词";
  if (strcmp(id, "upcoming-todos") == 0) return "记录与管理";
  if (strcmp(id, "computer-status") == 0) return "电脑实时状态";
  return "打开体验";
}

/* Engineering touch/slicing diagnostic; final game assets belong to PetUI. */
static void render_touch_test_page(const pet_p4_runtime_state_t *state) {
  const pet_p4_slice_t *s = &state->touch_test;
  uint16_t ink = rgb565(236, 241, 230), accent = rgb565(148, 226, 151);
  fill_rect(0, 0, 640, 480, rgb565(17, 30, 29));
  fill_round_rect(16, 14, 608, 64, 16, rgb565(31, 49, 44));
  draw_text_line("触控切水果测试", 32, 30, 320, ink, 2, true);
  char score[64];
  snprintf(score, sizeof(score), "%u  /  MISS %u", s->score, s->misses);
  draw_text_line(score, 430, 36, 180, accent, 1, true);
  if (!s->finished) {
    int x = (int)s->x, y = (int)s->y;
    unsigned age = s->sliced && s->now_ms >= s->cut_ms ? (unsigned)(s->now_ms - s->cut_ms) : 0;
    if (s->sliced) {
      int gap = 5 + (int)age / 12;
      fill_ellipse(x - gap, y + (int)age / 20, 18, 32, rgb565(244, 139, 62));
      fill_ellipse(x + gap, y + (int)age / 15, 18, 32, rgb565(254, 184, 77));
      fill_ellipse(x - gap + 3, y + (int)age / 20, 11, 25, rgb565(255, 211, 120));
    } else {
      fill_ellipse(x + 2, y + 4, 35, 34, rgb565(114, 64, 29));
      fill_ellipse(x, y, 34, 34, rgb565(243, 147, 57));
      fill_ellipse(x - 10, y - 10, 12, 17, rgb565(255, 182, 77));
      fill_ellipse(x + 8, y - 35, 13, 5, accent);
    }
  }
  for (unsigned i = 1; i < s->trail_count; i++) {
    if (s->trail[i].start) continue;
    const pet_p4_slice_point_t *a = &s->trail[i-1], *b = &s->trail[i];
    draw_line(a->x, a->y, b->x, b->y, accent);
    draw_line(a->x, a->y + 1, b->x, b->y + 1, ink);
  }
  if (s->finished) draw_text_line("本轮结束 · 按 SW1 重新开始", 110, 215, 480, ink, 2, true);
  /* A stationary finger must remain visible too; a short line-only trail
   * cannot distinguish coordinate mismatch from an unresponsive controller. */
  if (s->pointer_down) {
    fill_ellipse(s->pointer_x, s->pointer_y, 9, 9, ink);
    fill_ellipse(s->pointer_x, s->pointer_y, 5, 5, rgb565(255, 99, 66));
    draw_line(s->pointer_x - 18, s->pointer_y, s->pointer_x + 18, s->pointer_y, ink);
    draw_line(s->pointer_x, s->pointer_y - 18, s->pointer_x, s->pointer_y + 18, ink);
    char xy[48];
    snprintf(xy, sizeof(xy), "X %d  Y %d", s->pointer_x, s->pointer_y);
    draw_text_line(xy, 32, 90, 300, accent, 1, true);
  }
  draw_text_line("滑动切开水果 · SW1 重开 · 全局键退出", 48, 450, 552, accent, 1, true);
}

extern const uint8_t center_bg_jpg[] asm("_binary_center_background_jpg_start");
extern const uint8_t center_bg_jpg_end[] asm("_binary_center_background_jpg_end");
extern const uint8_t center_thumbs_z[] asm("_binary_center_thumbs_deflate_start");
extern const uint8_t center_thumbs_z_end[] asm("_binary_center_thumbs_deflate_end");
static uint16_t *center_background, *center_thumbnails;
static bool center_art_attempted;
static void center_art_release(void) {
  if(center_background)heap_caps_free(center_background);
  if(center_thumbnails)heap_caps_free(center_thumbnails);
  center_background=center_thumbnails=NULL;
  center_art_attempted=false;
}
static void center_art_prepare(void) {
  if(center_art_attempted)return;
  center_art_attempted=true;
  size_t length=(size_t)(center_bg_jpg_end-center_bg_jpg);
  if(g_jpeg_input&&g_jpeg_output&&length<=g_jpeg_input_capacity) {
    memcpy(g_jpeg_input,center_bg_jpg,length);
    int w=0,h=0,stride=0;esp_err_t error=ESP_OK;JRESULT fallback=JDR_OK;
    bool ok=decode_jpeg_frame_hardware(g_jpeg_input,length,&w,&h,&stride,&error);
    if(!ok){ok=decode_jpeg_frame_tjpgd(g_jpeg_input,length,&w,&h,&fallback);stride=w;}
    if(ok&&w==640&&h==480) {
      center_background=heap_caps_malloc(640*480*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
      if(center_background)for(int y=0;y<480;y++)memcpy(center_background+y*640,g_jpeg_output+y*stride,1280);
    }
  }
  size_t expected=CENTER_ART_COUNT*64*48*2;
  center_thumbnails=heap_caps_malloc(expected,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  tinfl_decompressor *inflater=calloc(1,sizeof(*inflater));
  if(center_thumbnails&&inflater) {
    tinfl_init(inflater);size_t consumed=center_thumbs_z_end-center_thumbs_z,decoded=expected;
    tinfl_status status=tinfl_decompress(inflater,center_thumbs_z,&consumed,(uint8_t*)center_thumbnails,
      (uint8_t*)center_thumbnails,&decoded,TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if(status!=TINFL_STATUS_DONE||decoded!=expected||consumed!=(size_t)(center_thumbs_z_end-center_thumbs_z)) {
      heap_caps_free(center_thumbnails);center_thumbnails=NULL;
    }
  }else if(center_thumbnails){heap_caps_free(center_thumbnails);center_thumbnails=NULL;}
  free(inflater);
}
static bool center_draw_thumbnail(const char *id,int x,int y) {
  if(!center_thumbnails)return false;
  for(size_t i=0;i<CENTER_ART_COUNT;i++)if(!strcmp(id,center_art_catalog[i].id)&&center_art_catalog[i].art) {
    const uint16_t *pixels=center_thumbnails+i*64*48;
    for(int row=0;row<48;row++)for(int col=0;col<64;col++) {
      int dx=col<7?7-col:col>56?col-56:0;
      int dy=row<7?7-row:row>40?row-40:0;
      if(dx*dx+dy*dy<=49)g_framebuffer[(y+row)*640+x+col]=pixels[row*64+col];
    }
    return true;
  }
  return false;
}
static void render_component_center_page(bool loading) {
  const uint16_t background = rgb565(224, 196, 162);
  const uint16_t panel = rgb565(255, 247, 232);
  const uint16_t outline = rgb565(231, 207, 174);
  const uint16_t selected_panel = rgb565(141, 75, 49);
  const uint16_t ink = rgb565(68, 45, 32);
  const uint16_t muted = rgb565(115, 88, 67);
  const uint16_t orange = rgb565(184, 97, 49);
  const size_t count = pet_p4_miniapp_catalog_count();
  const size_t requested = pet_p4_miniapp_catalog_selected();
  const size_t selected = count && requested < count ? requested : 0;
  const size_t per_page = PET_CENTER_PAGE_SIZE;
  const size_t start = selected / per_page * per_page;
  const size_t pages = (count + per_page - 1) / per_page;

  center_art_prepare();
  if(center_background)memcpy(g_framebuffer,center_background,640*480*2);
  else fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  draw_text_line("组件中心", 26, 14, 300, ink, 2, true);
  draw_text_line(loading ? "正在准备组件，可返回取消" : "轻点打开 · 左右滑动翻页",26,54,440,muted,1,true);
  char count_text[24];
  snprintf(count_text, sizeof(count_text), "已安装 %u 个", (unsigned int) count);
  fill_round_rect(474,22,142,30,15,panel);
  draw_text_center(count_text,545,text_y_in_box(count_text,1,22,30),130,muted,1);

  if (count == 0) {
    fill_round_rect(24, 86, 592, 306, 20, panel);
    draw_component_icon("", 298, 142, orange, selected_panel);
    draw_text_center("还没有组件", 320, 210, 536, ink, 2);
    draw_text_center("在 PC 组件中心添加后，即可在这里打开", 320, 265, 536, muted, 1);
  } else {
    size_t end = count < start + per_page ? count : start + per_page;
    for (size_t index = start; index < end; index += 1) {
      pet_p4_miniapp_catalog_entry_t item = {0};
      if (!pet_p4_miniapp_catalog_get(index, &item)) continue;
      const bool focused = index == selected;
      const int slot = (int) (index - start);
      const int x = PET_CENTER_LEFT + (slot % PET_CENTER_COLUMNS) * PET_CENTER_STEP_X;
      const int y = PET_CENTER_TOP + (slot / PET_CENTER_COLUMNS) * PET_CENTER_STEP_Y;
      const uint16_t surface = focused ? selected_panel : panel;
      const uint16_t card_ink=focused?rgb565(255,248,231):ink;
      const uint16_t card_muted=focused?rgb565(244,216,184):muted;
      fill_round_rect(x, y+4, 288, 146, 18, rgb565(187,151,114));
      fill_round_rect_outline(x, y, 288, 146, 18, surface, focused ? orange : outline);
      if (focused) fill_round_rect_outline(x+1, y+1, 286, 144, 17, surface, orange);
      if(!center_draw_thumbnail(item.widget_id,x+18,y+14))
        draw_component_icon(item.widget_id, x+18, y+16, focused ? card_ink : orange,
          focused ? rgb565(168,100,67) : rgb565(242,224,196));
      if (item.active) {
        fill_round_rect(x+194, y+23, 76, 24, 12, rgb565(226, 246, 237));
        draw_text_center("已打开", x+232, text_y_in_box("已打开", 1, y+23, 24), 64,
          rgb565(33, 138, 92), 1);
      } else {
        char number[12];
        snprintf(number, sizeof(number), "%02u", (unsigned int)index+1);
        draw_text_right(number, x+268, y+28, 48, card_muted, 1);
      }
      draw_text_line(
        item.title[0] ? item.title : item.widget_id, x+18, y+72, 252, card_ink, 2, true
      );
      draw_text_line(component_card_caption(item.widget_id), x+18, y+116, 226, card_muted, 1, true);
      if (focused) {
        draw_line(x+260, y+117, x+265, y+122, card_ink);
        draw_line(x+265, y+122, x+260, y+127, card_ink);
      }
    }
    for (size_t page=0; page<pages; ++page) {
      /* Equal-size circular markers; selection changes colour, not a pill
       * spanning two apparent dots. Keep the entire row centred. */
      const int dot_x = 320 - (int)((pages-1)*18)/2 + (int)page*18;
      fill_ellipse(dot_x, 415, 3, 3,
        page == selected/per_page ? selected_panel : rgb565(181,149,117));
    }
  }
  fill_round_rect(24,435,592,34,14,panel);
  draw_text_center("摇杆选择    确认键打开    全局键返回", 320, 444, 580, muted, 1);
}

static void draw_connection_banner_pixels(const char *label,uint16_t dot) {
  uint16_t background = rgb565(11, 14, 13);
  uint16_t text = rgb565(250, 241, 204);
  int banner_h = 32;
  int banner_w = utf8_text_width(label, -1, 1) + 48;
  if (banner_w < 200) banner_w = 200;
  if (banner_w > 272) banner_w = 272;
  if (banner_w & 1) banner_w += 1;
  int banner_x = PET_P4_UI_WIDTH - banner_w - 20;
  int banner_y = 18;
  fill_round_rect(banner_x, banner_y, banner_w, banner_h, 12, background);
  fill_ellipse(banner_x + 16, banner_y + banner_h / 2, 4, 4, dot);
  draw_text_line_vcenter(
    label,
    banner_x + 30,
    banner_y,
    banner_h,
    banner_w - 44,
    text,
    1,
    true
  );
}
static void draw_connection_banner(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state) return;
  bool connected = pet_p4_state_host_connected(state, now_ms);
  const char *label = NULL;
  uint16_t dot = rgb565(0, 170, 162);
  if (connected && state->asset_family_count > 0) return;
  if (connected) label = "正在同步宠物形象";
  else if (state->desktop_device_id[0]) {label = "客户端未连接";dot = rgb565(224, 79, 72);}
  else if (state->native_usb_mounted || state->host_last_seen_ms > 0) {label = "正在连接 Pet Manager";dot = rgb565(242, 168, 45);}
  else {label = "连接 Pet Manager 以开始";dot = rgb565(242, 168, 45);}
  draw_connection_banner_pixels(label,dot);
}
#if PET_P4_APP_RETAIN_LOGICAL
static void draw_common_connection(uint8_t banner){
  switch(banner){
    case 1:draw_connection_banner_pixels("正在同步宠物形象",rgb565(0,170,162));break;
    case 2:draw_connection_banner_pixels("客户端未连接",rgb565(224,79,72));break;
    case 3:draw_connection_banner_pixels("正在连接 Pet Manager",rgb565(242,168,45));break;
    case 4:draw_connection_banner_pixels("连接 Pet Manager 以开始",rgb565(242,168,45));break;
    default:break;
  }
}
#endif

static void draw_text_right(
  const char *text,
  int right_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
) {
  int width = utf8_text_width(text ? text : "", -1, scale);
  if (width > max_px) {
    draw_text_line(text, right_x - max_px, y, max_px, color, scale, true);
    return;
  }
  draw_text_line(text, right_x - width, y, max_px, color, scale, false);
}

static void draw_text_center(
  const char *text,
  int center_x,
  int y,
  int max_px,
  uint16_t color,
  int scale
) {
  int width = utf8_text_width(text ? text : "", -1, scale);
  if (width > max_px) {
    draw_text_line(text, center_x - max_px / 2, y, max_px, color, scale, true);
    return;
  }
  draw_text_line(text, center_x - width / 2, y, max_px, color, scale, false);
}

static int fitted_text_scale(const char *text, int max_px, int preferred, int minimum) {
  for (int scale = preferred; scale > minimum; scale -= 1) {
    if (utf8_text_width(text ? text : "", -1, scale) <= max_px) return scale;
  }
  return minimum;
}

static uint16_t miniapp_state_color(const char *state) {
  if (state && (strstr(state, "error") || strstr(state, "failed"))) return rgb565(235, 89, 82);
  if (state && (strstr(state, "pause") || strstr(state, "wait"))) return rgb565(255, 163, 31);
  return rgb565(85, 193, 133);
}

static uint16_t session_status_color(const char *status) {
  if (status && strcmp(status, "done") == 0) return view_status_color(PET_P4_VIEW_STATUS_DONE);
  if (status && strcmp(status, "error") == 0) return view_status_color(PET_P4_VIEW_STATUS_ERROR);
  if (status && strcmp(status, "stopped") == 0) return view_status_color(PET_P4_VIEW_STATUS_STOPPED);
  if (status && strcmp(status, "waiting_user") == 0) return view_status_color(PET_P4_VIEW_STATUS_WAITING);
  if (status && strcmp(status, "speaking") == 0) return view_status_color(PET_P4_VIEW_STATUS_WORKING);
  if (status && strcmp(status, "working") == 0) return view_status_color(PET_P4_VIEW_STATUS_WORKING);
  return view_status_color(PET_P4_VIEW_STATUS_IDLE);
}

static pet_p4_view_status_t session_view_status(const char *status) {
  if (status && strcmp(status, "done") == 0) return PET_P4_VIEW_STATUS_DONE;
  if (status && strcmp(status, "error") == 0) return PET_P4_VIEW_STATUS_ERROR;
  if (status && strcmp(status, "stopped") == 0) return PET_P4_VIEW_STATUS_STOPPED;
  if (status && strcmp(status, "waiting_user") == 0) return PET_P4_VIEW_STATUS_WAITING;
  if (status && strcmp(status, "speaking") == 0) return PET_P4_VIEW_STATUS_WORKING;
  if (status && strcmp(status, "working") == 0) return PET_P4_VIEW_STATUS_WORKING;
  return PET_P4_VIEW_STATUS_IDLE;
}

static void draw_voice_waveform(int x, int y, int width, uint16_t color) {
  static const int heights[] = {4, 7, 11, 16, 9, 5, 8, 13, 6, 4};
  int cursor = x;
  int segments = (int) (sizeof(heights) / sizeof(heights[0]));
  int step = width / segments;
  for (int i = 0; i < segments; i += 1) {
    int next = cursor + step;
    int y1 = y + (i > 0 ? heights[i - 1] : 0);
    int y2 = y + heights[i];
    draw_line(cursor, y1, next, y2, color);
    cursor = next;
  }
}

static void draw_voice_input_row(int x, int y) {
  int64_t content_started = esp_timer_get_time();
  uint16_t ink = rgb565(28, 31, 31);
  uint16_t muted = rgb565(92, 98, 96);
  draw_voice_waveform(x + 30, y, 230, rgb565(255, 163, 31));
  fill_ellipse(x + 288, y + 13, 7, 11, ink);
  fill_rect(x + 286, y + 22, 5, 7, ink);
  draw_line(x + 279, y + 18, x + 279, y + 25, ink);
  draw_line(x + 279, y + 25, x + 297, y + 25, ink);
  draw_line(x + 297, y + 25, x + 297, y + 18, ink);
  draw_text_line_vcenter("LISTENING", x + 324, y + 11, 30, 180, muted, 1, true);
  pet_p4_render_trace_add(&g_render_trace.voice_us,
    elapsed_us_clamped(content_started, esp_timer_get_time()));
}

static void draw_standalone_voice_overlay(void) {
  const int height = 76;
  const int x = PET_P4_SESSION_CARD_X;
  const int y = PET_P4_SESSION_CARD_BOTTOM - height;
  draw_session_card_panel(x, y, PET_P4_SESSION_CARD_WIDTH, height,
    PET_P4_SESSION_CARD_RADIUS, true, rgb565(216, 221, 218));
  fill_round_rect(x + 10, y + 14, 7, height - 28, 2, rgb565(25, 151, 99));
  draw_voice_input_row(x, y + 18);
}

static void session_text(const pet_p4_session_queue_item_t *item, uint64_t now_ms,
    pet_bubble_text_view_t *out) {
  char owner[160];
  snprintf(owner, sizeof(owner), "session:%s", item->id);
  renderer_bubble_select(owner, PET_BUBBLE_BODY_CLIPPED, item->title,
    item->content[0] ? item->content : item->state, now_ms, out);
}

/* Keep cache identity and both draw passes on the same visible card window. */
static void session_overlay_visible_window(const pet_p4_runtime_state_t *state,
    unsigned int *out_selected, unsigned int *out_start, unsigned int *out_visible) {
  unsigned int selected = state->current_session_index > 0
    ? state->current_session_index - 1
    : 0;
  if (selected >= state->session_queue_count) selected = 0;
  unsigned int visible = state->session_queue_count < 3 ? state->session_queue_count : 3;
  unsigned int start = 0;
  if (state->session_queue_count > visible) {
    if (selected >= state->session_queue_count - 1) start = state->session_queue_count - visible;
    else if (selected > 0) start = selected - 1;
  }
  *out_selected = selected;
  *out_start = start;
  *out_visible = visible;
}

static void draw_session_queue_uncached(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  bool draw_working_markers
) {
  if (!state || state->session_queue_count == 0) return;
  int64_t content_started = esp_timer_get_time();


  uint16_t ink = rgb565(28, 31, 31);
  uint16_t muted = rgb565(55, 61, 59);
  uint16_t card_outline = rgb565(216, 221, 218);
  uint16_t selection = rgb565(25, 151, 99);
  unsigned int selected, start, visible;
  session_overlay_visible_window(state, &selected, &start, &visible);
  int block_height = (int) visible * PET_P4_SESSION_CARD_HEIGHT
    + ((int) visible - 1) * PET_P4_SESSION_CARD_GAP;
  int first_y = PET_P4_SESSION_CARD_BOTTOM - block_height;
  for (unsigned int row = 0; row < visible; row += 1) {
    unsigned int item_index = start + row;
    const pet_p4_session_queue_item_t *item = &state->session_queue[item_index];
    pet_bubble_text_view_t text = {0};
    session_text(item, now_ms, &text);
    bool is_selected = item_index == selected;
    int x = PET_P4_SESSION_CARD_X;
    int y = first_y + (int) row * (PET_P4_SESSION_CARD_HEIGHT + PET_P4_SESSION_CARD_GAP);
    int w = PET_P4_SESSION_CARD_WIDTH;
    int h = PET_P4_SESSION_CARD_HEIGHT;
    uint16_t status = session_status_color(item->state);
    draw_session_card_panel(
      x,
      y,
      w,
      h,
      PET_P4_SESSION_CARD_RADIUS,
      is_selected,
      card_outline
    );
    fill_round_rect(x + 10, y + 14, is_selected ? 7 : 4, h - 28, 2, is_selected ? selection : status);
    pet_p4_view_status_t marker_status = session_view_status(item->state);
    if (draw_working_markers || marker_status != PET_P4_VIEW_STATUS_WORKING) {
      draw_status_marker(marker_status, now_ms, x + w - 26, y + 26);
    }
    if (!(text.pixels && renderer_bubble_draw_pinned(&text, PET_BUBBLE_TITLE, 0, g_framebuffer,
        x + 30, y + 10, w - 82, ink)))
      draw_text_line_vcenter(text.title, x + 30, y + 10, 38, w - 82, ink, 2, true);

    if (is_selected && state->session_voice_active) {
      draw_voice_input_row(x, y + 62);
    } else {
      draw_card_body_lines(text.body, x + 30, y + 50, w - 60, muted, &text);
    }
    pet_bubble_pixels_release(&text);
  }
  pet_p4_render_trace_add(&g_render_trace.session_cache_draw_us,
    elapsed_us_clamped(content_started, esp_timer_get_time()));
}

static uint64_t session_cache_hash_bytes(uint64_t hash, const void *bytes, size_t size) {
  const uint8_t *cursor = (const uint8_t *) bytes;
  for (size_t i = 0; i < size; i += 1) {
    hash ^= cursor[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t session_cache_hash_text(uint64_t hash, const char *text) {
  const char *value = text ? text : "";
  return session_cache_hash_bytes(hash, value, strlen(value) + 1);
}

static uint64_t session_overlay_signature(const pet_p4_runtime_state_t *state) {
  uint64_t hash = 1469598103934665603ULL;
  uint32_t text_revision = pet_bubble_pixels_revision();
  hash = session_cache_hash_bytes(hash, &text_revision, sizeof(text_revision));
  hash = session_cache_hash_bytes(hash, &state->session_queue_count, sizeof(state->session_queue_count));
  hash = session_cache_hash_bytes(hash, &state->current_session_index, sizeof(state->current_session_index));
  hash = session_cache_hash_bytes(hash, &state->session_voice_active, sizeof(state->session_voice_active));
  unsigned int selected, start, visible;
  session_overlay_visible_window(state, &selected, &start, &visible);
  (void)selected; /* current_session_index above retains its complete dependency. */
  for (unsigned int i = start; i < start + visible && i < PET_P4_SESSION_QUEUE_MAX; i += 1) {
    const pet_p4_session_queue_item_t *item = &state->session_queue[i];
    hash = session_cache_hash_text(hash, item->id);
    hash = session_cache_hash_text(hash, item->title);
    hash = session_cache_hash_text(hash, item->content);
    hash = session_cache_hash_text(hash, item->state);
  }
  return hash;
}

static void scan_session_overlay_cache_runs(void) {
  int64_t content_started = esp_timer_get_time();
  memset(g_session_overlay_row_run_count, 0, sizeof(g_session_overlay_row_run_count));
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 1) {
    const uint16_t *row = g_session_overlay_cache + y * PET_P4_UI_WIDTH;
    int x = 0;
    while (x < PET_P4_UI_WIDTH) {
      while (x < PET_P4_UI_WIDTH && row[x] == PET_P4_SESSION_CACHE_SENTINEL) x += 1;
      if (x >= PET_P4_UI_WIDTH) break;
      int start = x;
      while (x < PET_P4_UI_WIDTH && row[x] != PET_P4_SESSION_CACHE_SENTINEL) x += 1;
      uint8_t run = g_session_overlay_row_run_count[y];
      if (run >= PET_P4_SESSION_CACHE_MAX_RUNS) {
        uint8_t last = PET_P4_SESSION_CACHE_MAX_RUNS - 1;
        g_session_overlay_row_run_end[y][last] = (uint16_t) x;
        continue;
      }
      g_session_overlay_row_run_start[y][run] = (uint16_t) start;
      g_session_overlay_row_run_end[y][run] = (uint16_t) x;
      g_session_overlay_row_run_count[y] = run + 1;
    }
  }
  pet_p4_render_trace_add(&g_render_trace.session_cache_scan_us,
    elapsed_us_clamped(content_started, esp_timer_get_time()));
}

static bool rebuild_session_overlay_cache(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  uint64_t signature
) {
  if (!g_session_overlay_cache && !g_session_overlay_cache_allocation_failed) {
    g_session_overlay_cache = (uint16_t *) heap_caps_malloc(
      PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!g_session_overlay_cache) {
      g_session_overlay_cache_allocation_failed = true;
      ESP_LOGW(TAG, "P4 session overlay cache unavailable; using direct drawing");
    }
  }
  if (!g_session_overlay_cache) return false;

  uint16_t *output = g_framebuffer;
  g_framebuffer = g_session_overlay_cache;
  int64_t fill_started = esp_timer_get_time();
  for (size_t i = 0; i < (size_t) PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT; i += 1) {
    g_session_overlay_cache[i] = PET_P4_SESSION_CACHE_SENTINEL;
  }
  pet_p4_render_trace_add(&g_render_trace.session_cache_fill_us,
    elapsed_us_clamped(fill_started, esp_timer_get_time()));
  draw_session_queue_uncached(state, now_ms, false);
  g_framebuffer = output;
  scan_session_overlay_cache_runs();
  g_session_overlay_signature = signature;
  g_session_overlay_cache_valid = true;
  memset(&g_render_perf, 0, sizeof(g_render_perf));
  ESP_LOGD(
    TAG,
    "P4 session overlay cache rebuilt sessions=%u selected=%u",
    state->session_queue_count,
    state->current_session_index
  );
  return true;
}

static void copy_session_overlay_cache(void) {
  int64_t content_started = esp_timer_get_time();
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 1) {
    const uint16_t *source = g_session_overlay_cache + y * PET_P4_UI_WIDTH;
    uint16_t *target = g_framebuffer + y * PET_P4_UI_WIDTH;
    for (uint8_t run = 0; run < g_session_overlay_row_run_count[y]; run += 1) {
      uint16_t start = g_session_overlay_row_run_start[y][run];
      uint16_t end = g_session_overlay_row_run_end[y][run];
      memcpy(target + start, source + start, (size_t) (end - start) * sizeof(uint16_t));
    }
  }
  pet_p4_render_trace_add(&g_render_trace.session_cache_copy_us,
    elapsed_us_clamped(content_started, esp_timer_get_time()));
}

static void draw_session_queue_working_markers(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  unsigned int selected, start, visible;
  session_overlay_visible_window(state, &selected, &start, &visible);
  int block_height = (int) visible * PET_P4_SESSION_CARD_HEIGHT
    + ((int) visible - 1) * PET_P4_SESSION_CARD_GAP;
  int first_y = PET_P4_SESSION_CARD_BOTTOM - block_height;
  for (unsigned int row = 0; row < visible; row += 1) {
    const pet_p4_session_queue_item_t *item = &state->session_queue[start + row];
    if (session_view_status(item->state) != PET_P4_VIEW_STATUS_WORKING) continue;
    int y = first_y + (int) row * (PET_P4_SESSION_CARD_HEIGHT + PET_P4_SESSION_CARD_GAP);
    draw_status_marker(
      PET_P4_VIEW_STATUS_WORKING,
      now_ms,
      PET_P4_SESSION_CARD_X + PET_P4_SESSION_CARD_WIDTH - 26,
      y + 26
    );
  }
}

static void draw_session_queue(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state || state->session_queue_count == 0) {
    g_session_overlay_cache_valid = false;
    return;
  }
  uint64_t signature = session_overlay_signature(state);
  if ((!g_session_overlay_cache_valid || signature != g_session_overlay_signature)
      && !rebuild_session_overlay_cache(state, now_ms, signature)) {
    draw_session_queue_uncached(state, now_ms, true);
    return;
  }
  copy_session_overlay_cache();
  draw_session_queue_working_markers(state, now_ms);
}

typedef struct {
  uint16_t background;
  uint16_t background_alt;
  uint16_t surface;
  uint16_t ink;
  uint16_t primary;
  uint16_t secondary;
  uint16_t light;
  uint16_t shadow;
} miniapp_pixel_palette_t;

typedef struct {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint16_t color;
} miniapp_tool_accent_t;

static miniapp_pixel_palette_t miniapp_pixel_palette(const char *name) {
  miniapp_pixel_palette_t palette = {
    .background = rgb565(90, 215, 255),
    .background_alt = rgb565(112, 225, 255),
    .surface = rgb565(255, 243, 184),
    .ink = rgb565(43, 35, 103),
    .primary = rgb565(255, 79, 139),
    .secondary = rgb565(255, 216, 61),
    .light = rgb565(255, 253, 240),
    .shadow = rgb565(103, 80, 164),
  };
  if (name && strcmp(name, "sunset") == 0) {
    palette.background = rgb565(255, 155, 115);
    palette.background_alt = rgb565(255, 176, 132);
    palette.surface = rgb565(255, 234, 182);
    palette.ink = rgb565(85, 39, 95);
    palette.primary = rgb565(233, 63, 134);
    palette.secondary = rgb565(255, 210, 63);
    palette.shadow = rgb565(132, 63, 114);
  } else if (name && strcmp(name, "mint") == 0) {
    palette.background = rgb565(120, 230, 189);
    palette.background_alt = rgb565(151, 239, 204);
    palette.surface = rgb565(244, 255, 208);
    palette.ink = rgb565(23, 63, 95);
    palette.primary = rgb565(255, 95, 119);
    palette.secondary = rgb565(255, 209, 102);
    palette.shadow = rgb565(44, 122, 120);
  } else if (name && strcmp(name, "arcade") == 0) {
    palette.background = rgb565(70, 59, 143);
    palette.background_alt = rgb565(83, 70, 163);
    palette.surface = rgb565(255, 240, 168);
    palette.ink = rgb565(32, 25, 79);
    palette.primary = rgb565(255, 79, 184);
    palette.secondary = rgb565(60, 242, 210);
    palette.shadow = rgb565(33, 25, 73);
  } else if (name && strcmp(name, "ocean") == 0) {
    palette.background = rgb565(24, 70, 106);
    palette.background_alt = rgb565(39, 101, 139);
    palette.surface = rgb565(224, 246, 249);
    palette.ink = rgb565(9, 35, 52);
    palette.primary = rgb565(36, 196, 212);
    palette.secondary = rgb565(255, 196, 71);
    palette.light = rgb565(248, 254, 255);
    palette.shadow = rgb565(18, 67, 84);
  } else if (name && strcmp(name, "forest") == 0) {
    palette.background = rgb565(74, 132, 92);
    palette.background_alt = rgb565(106, 161, 111);
    palette.surface = rgb565(246, 244, 220);
    palette.ink = rgb565(29, 55, 35);
    palette.primary = rgb565(255, 111, 70);
    palette.secondary = rgb565(248, 207, 89);
    palette.light = rgb565(255, 253, 240);
    palette.shadow = rgb565(43, 87, 61);
  } else if (name && strcmp(name, "ember") == 0) {
    palette.background = rgb565(58, 25, 31);
    palette.background_alt = rgb565(91, 33, 36);
    palette.surface = rgb565(255, 232, 193);
    palette.ink = rgb565(35, 17, 21);
    palette.primary = rgb565(255, 91, 54);
    palette.secondary = rgb565(255, 190, 69);
    palette.light = rgb565(255, 249, 235);
    palette.shadow = rgb565(103, 38, 42);
  } else if (name && strcmp(name, "mono") == 0) {
    palette.background = rgb565(214, 219, 218);
    palette.background_alt = rgb565(232, 235, 233);
    palette.surface = rgb565(255, 255, 255);
    palette.ink = rgb565(27, 31, 30);
    palette.primary = rgb565(12, 126, 117);
    palette.secondary = rgb565(238, 95, 76);
    palette.light = rgb565(250, 252, 251);
    palette.shadow = rgb565(112, 122, 119);
  }
  return palette;
}

static miniapp_tool_accent_t miniapp_tool_accent(const char *palette_name) {
  miniapp_tool_accent_t accent = {
    .red = 246,
    .green = 183,
    .blue = 60,
    .color = rgb565(246, 183, 60),
  };
  if (palette_name && strcmp(palette_name, "sunset") == 0) {
    accent.red = 255;
    accent.green = 102;
    accent.blue = 92;
  } else if (palette_name && strcmp(palette_name, "mint") == 0) {
    accent.red = 87;
    accent.green = 217;
    accent.blue = 255;
  } else if (palette_name && strcmp(palette_name, "arcade") == 0) {
    accent.red = 170;
    accent.green = 140;
    accent.blue = 255;
  } else if (palette_name && strcmp(palette_name, "ocean") == 0) {
    accent.red = 36;
    accent.green = 196;
    accent.blue = 212;
  } else if (palette_name && strcmp(palette_name, "forest") == 0) {
    accent.red = 255;
    accent.green = 111;
    accent.blue = 70;
  } else if (palette_name && strcmp(palette_name, "ember") == 0) {
    accent.red = 255;
    accent.green = 91;
    accent.blue = 54;
  } else if (palette_name && strcmp(palette_name, "mono") == 0) {
    accent.red = 12;
    accent.green = 126;
    accent.blue = 117;
  }
  accent.color = rgb565(accent.red, accent.green, accent.blue);
  return accent;
}

static void draw_miniapp_pixel_panel(
  int x,
  int y,
  int width,
  int height,
  const miniapp_pixel_palette_t *palette
) {
  fill_rect(x + 6, y + 6, width, height, palette->shadow);
  fill_rect(x, y, width, height, palette->ink);
  fill_rect(x + 4, y + 4, width - 8, height - 8, palette->surface);
}

static void draw_miniapp_pixel_game_panel(
  int x,
  int y,
  int width,
  int height,
  const miniapp_pixel_palette_t *palette
) {
  fill_rect(x, y, width, height, palette->ink);
  fill_rect(x + 2, y + 2, width - 4, height - 4, palette->background_alt);
}

static void draw_miniapp_clean_card(
  int x,
  int y,
  int width,
  int height,
  uint16_t surface,
  const miniapp_pixel_palette_t *palette
) {
  fill_round_rect(x + 3, y + 5, width, height, 16, palette->shadow);
  fill_round_rect(x, y, width, height, 16, surface);
  if (width > 36 && height > 12) {
    fill_round_rect(x + 14, y + 3, width - 28, 3, 2, palette->light);
  }
}

static void draw_miniapp_pixel_sprite(
  const char *name,
  int x,
  int y,
  int cell,
  const miniapp_pixel_palette_t *palette
) {
  static const char *const target[] = {
    "000222000", "002111200", "021222120", "212111212", "212131212",
    "212111212", "021222120", "002111200", "000222000",
  };
  static const char *const trophy[] = {
    "001111100", "211111112", "221111122", "022111220", "002111200",
    "000111000", "000111000", "001111100", "011111110",
  };
  static const char *const star[] = {
    "000100000", "000100000", "010111010", "001111100", "111141111",
    "001111100", "010111010", "000101000", "000100000",
  };
  static const char *const bolt[] = {
    "000111000", "001111000", "011110000", "111111000", "001111100",
    "000111110", "000011100", "000011000", "000010000",
  };
  static const char *const coffee[] = {
    "001010000", "000101000", "011111020", "011111022", "011111020",
    "011111000", "001111000", "000000000", "011111110",
  };
  static const char *const timer[] = {
    "000111000", "001111100", "000111000", "011111110", "110000011",
    "110030011", "110000011", "011111110", "001111100",
  };
  static const char *const droplet[] = {
    "000010000", "000111000", "001111100", "011111110", "111111111",
    "111111111", "011111110", "001111100", "000111000",
  };
  static const char *const gauge[] = {
    "001111100", "011111110", "110000011", "100000001", "100030001",
    "100010001", "110111011", "011111110", "001111100",
  };
  static const char *const blocks[] = {
    "110022200", "110002000", "000002000", "033333000", "000330000",
    "000330044", "000000044", "111122220", "111122220",
  };
  static const char *const snake[] = {
    "000000000", "011100000", "010100000", "010111000", "000001000",
    "000001330", "000000303", "000222300", "000222000",
  };
  static const char *const flappy[] = {
    "000000000", "000000000", "000011100", "000111110", "001114440",
    "011112220", "001111100", "000011000", "000000000",
  };
  static const char *const mole_ready[] = {
    "000000000000000", "000000000000000", "000000000000000",
    "000000000000000", "000000000000000", "000000000000000",
    "033000330003300", "333303333033330", "033000330003300",
  };
  static const char *const mole_left[] = {
    "100100000000000", "111100000000000", "141400000000000",
    "122100000000000", "011000000000000", "011000000000000",
    "033000330003300", "333303333033330", "033000330003300",
  };
  static const char *const mole_center[] = {
    "000001001000000", "000001111000000", "000001414000000",
    "000001221000000", "000000110000000", "000000110000000",
    "033000330003300", "333303333033330", "033000330003300",
  };
  static const char *const mole_right[] = {
    "000000000010010", "000000000011110", "000000000014140",
    "000000000012210", "000000000001100", "000000000001100",
    "033000330003300", "333303333033330", "033000330003300",
  };
  const char *const *rows = target;
  int columns = 9;
  if (name && strcmp(name, "trophy") == 0) rows = trophy;
  else if (name && strcmp(name, "star") == 0) rows = star;
  else if (name && strcmp(name, "bolt") == 0) rows = bolt;
  else if (name && strcmp(name, "coffee") == 0) rows = coffee;
  else if (name && strcmp(name, "timer") == 0) rows = timer;
  else if (name && strcmp(name, "droplet") == 0) rows = droplet;
  else if (name && strcmp(name, "gauge") == 0) rows = gauge;
  else if (name && strcmp(name, "blocks") == 0) rows = blocks;
  else if (name && strcmp(name, "snake") == 0) rows = snake;
  else if (name && strcmp(name, "flappy") == 0) rows = flappy;
  else if (name && strcmp(name, "mole-ready") == 0) {
    rows = mole_ready;
    columns = 15;
  } else if (name && strcmp(name, "mole-left") == 0) {
    rows = mole_left;
    columns = 15;
  } else if (name && strcmp(name, "mole-center") == 0) {
    rows = mole_center;
    columns = 15;
  } else if (name && strcmp(name, "mole-right") == 0) {
    rows = mole_right;
    columns = 15;
  }
  for (int row = 0; row < 9; row += 1) {
    for (int column = 0; column < columns; column += 1) {
      uint16_t color;
      switch (rows[row][column]) {
        case '1': color = palette->primary; break;
        case '2': color = palette->secondary; break;
        case '3': color = palette->ink; break;
        case '4': color = palette->light; break;
        default: continue;
      }
      fill_rect(x + column * cell, y + row * cell, cell, cell, color);
    }
  }
}

static uint16_t miniapp_game_tone_color(
  uint8_t tone,
  const miniapp_pixel_palette_t *palette
) {
  if (tone == 1) return palette->primary;
  if (tone == 2) return palette->secondary;
  if (tone == 3) return palette->ink;
  if (tone == 4) return palette->light;
  return palette->background_alt;
}

static bool draw_miniapp_entity_sprite(
  const pet_p4_game_entity_t *entity,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  int x,
  int y,
  int width,
  int height
) {
  if (!entity || !sprites || entity->sprite_index < 0
      || entity->sprite_index >= sprites->count) return false;
  const pet_p4_miniapp_sprite_t *sprite = &sprites->items[entity->sprite_index];
  const size_t frame_bytes = (size_t) sprite->frame_width * sprite->frame_height * 3U;
  const uint64_t now_ms = (uint64_t) esp_timer_get_time() / 1000ULL;
  const uint8_t frame = sprite->frames > 1
    ? (uint8_t) (((now_ms * sprite->fps) / 1000ULL) % sprite->frames)
    : 0;
  const size_t frame_offset = (size_t) sprite->data_offset + (size_t) frame * frame_bytes;
  if (sprite->frame_width == 0 || sprite->frame_height == 0
      || frame_offset + frame_bytes > sprites->data_length) return false;
  for (int target_y = 0; target_y < height; target_y += 1) {
    const int source_y = target_y * sprite->frame_height / height;
    for (int target_x = 0; target_x < width; target_x += 1) {
      const int source_x = target_x * sprite->frame_width / width;
      const size_t offset = frame_offset
        + ((size_t) source_y * sprite->frame_width + (size_t) source_x) * 3U;
      const uint16_t color = (uint16_t) sprites->pixels[offset]
        | ((uint16_t) sprites->pixels[offset + 1] << 8U);
      const uint8_t alpha = sprites->pixels[offset + 2];
      if (alpha == 0) continue;
      if (alpha == 255) {
        put_px(x + target_x, y + target_y, color);
      } else {
        const uint8_t red = (uint8_t) (((color >> 11) & 0x1fU) * 255U / 31U);
        const uint8_t green = (uint8_t) (((color >> 5) & 0x3fU) * 255U / 63U);
        const uint8_t blue = (uint8_t) ((color & 0x1fU) * 255U / 31U);
        blend_px(x + target_x, y + target_y, red, green, blue, alpha);
      }
    }
  }
  return true;
}

static void draw_miniapp_game_entity(
  const pet_p4_game_entity_t *entity,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  int grid_x,
  int grid_y,
  int cell,
  bool clean_style,
  const miniapp_pixel_palette_t *palette
) {
  if (!entity || !entity->active) return;
  int inset = clean_style ? 2 : 1;
  int x = grid_x + entity->x * cell + inset;
  int y = grid_y + entity->y * cell + inset;
  int width = entity->width * cell - inset * 2;
  int height = entity->height * cell - inset * 2;
  if (width < 3 || height < 3) return;
  int cx = x + width / 2;
  int cy = y + height / 2;
  uint16_t color = miniapp_game_tone_color(entity->tone, palette);
  uint16_t detail = entity->tone == 4 ? palette->ink : palette->light;

  if (draw_miniapp_entity_sprite(entity, sprites, x, y, width, height)) return;

  switch (entity->shape) {
    case PET_P4_GAME_SHAPE_PLAYER_SHIP: {
      int shoulder_y = y + height * 2 / 3;
      fill_triangle(cx, y, x + width - 1, y + height - 1, x, y + height - 1, color);
      fill_rect(
        cx - (width > 12 ? 2 : 1),
        y + height / 4,
        width > 12 ? 5 : 3,
        height * 3 / 4,
        color
      );
      fill_ellipse(
        cx,
        shoulder_y - height / 4,
        width > 12 ? 2 : 1,
        height > 12 ? 3 : 1,
        detail
      );
      fill_rect(cx - 1, y + height - 2, 3, 2, palette->secondary);
      break;
    }
    case PET_P4_GAME_SHAPE_ENEMY_SHIP:
      fill_triangle(x, y, x + width - 1, y, cx, y + height - 1, color);
      fill_rect(
        cx - (width > 12 ? 2 : 1),
        y,
        width > 12 ? 5 : 3,
        height * 3 / 4,
        color
      );
      fill_ellipse(
        cx,
        y + height / 3,
        width > 12 ? 2 : 1,
        height > 12 ? 3 : 1,
        detail
      );
      break;
    case PET_P4_GAME_SHAPE_BULLET: {
      int bullet_width = width / 3;
      if (bullet_width < 3) bullet_width = 3;
      if (bullet_width > 9) bullet_width = 9;
      fill_round_rect(
        cx - bullet_width / 2,
        y,
        bullet_width,
        height,
        bullet_width / 2,
        color
      );
      if (bullet_width >= 5 && height >= 7) fill_rect(cx, y + 2, 1, height - 4, detail);
      break;
    }
    case PET_P4_GAME_SHAPE_STAR: {
      int rx = width / 2;
      int ry = height / 2;
      fill_triangle(cx, y, cx + rx / 3, cy, cx - rx / 3, cy, color);
      fill_triangle(cx, y + height - 1, cx - rx / 3, cy, cx + rx / 3, cy, color);
      fill_triangle(x, cy, cx, cy - ry / 3, cx, cy + ry / 3, color);
      fill_triangle(x + width - 1, cy, cx, cy + ry / 3, cx, cy - ry / 3, color);
      fill_ellipse(cx, cy, rx / 3 + 1, ry / 3 + 1, detail);
      break;
    }
    case PET_P4_GAME_SHAPE_PADDLE: {
      int radius = (width < height ? width : height) / 3;
      if (radius < 2) radius = 2;
      fill_round_rect(x, y, width, height, radius, color);
      if (width > height && height >= 6) fill_rect(x + 3, y + 2, width - 6, 2, detail);
      break;
    }
    case PET_P4_GAME_SHAPE_BALL:
    case PET_P4_GAME_SHAPE_CIRCLE:
      fill_ellipse(cx, cy, width / 2, height / 2, color);
      if (width >= 8 && height >= 8) {
        fill_ellipse(cx - width / 5, cy - height / 5, 2, 2, detail);
      }
      break;
    case PET_P4_GAME_SHAPE_CAPSULE: {
      int radius = (width < height ? width : height) / 2;
      fill_round_rect(x, y, width, height, radius, color);
      if (width >= 8 && height >= 8) {
        fill_round_rect(x + width / 5, y + height / 5, width / 3, height / 5, 2, detail);
      }
      break;
    }
    case PET_P4_GAME_SHAPE_TRIANGLE:
      fill_triangle(cx, y, x + width - 1, y + height - 1, x, y + height - 1, color);
      break;
    case PET_P4_GAME_SHAPE_DIAMOND:
      fill_triangle(cx, y, x + width - 1, cy, cx, cy, color);
      fill_triangle(x, cy, cx, cy, cx, y + height - 1, color);
      fill_triangle(cx, cy, x + width - 1, cy, cx, y + height - 1, color);
      break;
    case PET_P4_GAME_SHAPE_HEART:
      fill_ellipse(x + width / 3, y + height / 3, width / 4, height / 4, color);
      fill_ellipse(x + width * 2 / 3, y + height / 3, width / 4, height / 4, color);
      fill_triangle(x, y + height / 3, x + width - 1, y + height / 3, cx, y + height - 1, color);
      break;
    case PET_P4_GAME_SHAPE_CLOUD:
      fill_round_rect(x, y + height / 3, width, height * 2 / 3, height / 4, color);
      fill_ellipse(x + width / 3, y + height / 3, width / 4, height / 3, color);
      fill_ellipse(x + width * 2 / 3, y + height / 3, width / 3, height / 3, color);
      break;
    case PET_P4_GAME_SHAPE_COIN:
      fill_ellipse(cx, cy, width / 2, height / 2, color);
      fill_ellipse(cx, cy, width / 3, height / 3, detail);
      fill_ellipse(cx, cy, width / 5, height / 5, color);
      break;
    case PET_P4_GAME_SHAPE_CHARACTER:
      fill_ellipse(cx, y + height / 4, width / 4, height / 4, color);
      fill_round_rect(x + width / 4, y + height / 2, width / 2, height / 2, width / 6, color);
      if (width >= 8 && height >= 8) {
        fill_ellipse(cx - width / 10, y + height / 5, 1, 1, detail);
        fill_ellipse(cx + width / 10, y + height / 5, 1, 1, detail);
      }
      break;
    case PET_P4_GAME_SHAPE_RECT:
    default:
      if (clean_style) {
        int radius = (width < height ? width : height) / 5;
        if (radius < 2) radius = 2;
        if (radius > 6) radius = 6;
        fill_round_rect(x, y, width, height, radius, color);
      } else {
        fill_rect(x, y, width, height, color);
      }
      break;
  }
}

static void draw_miniapp_game_grid(
  const pet_p4_game_frame_t *game,
  const pet_p4_miniapp_sprite_pack_t *sprites,
  int panel_x,
  int panel_y,
  int panel_width,
  int panel_height,
  const miniapp_pixel_palette_t *palette,
  bool clean_style
) {
  if (!game || game->kind == PET_P4_GAME_NONE || game->width == 0 || game->height == 0) return;
  bool flappy_game = game->kind == PET_P4_GAME_FLAPPY;
  uint16_t grid_outline = flappy_game ? rgb565(22, 53, 79) : palette->ink;
  uint16_t empty = flappy_game ? rgb565(143, 216, 247) : palette->background_alt;
  uint16_t primary = flappy_game ? rgb565(55, 168, 73) : palette->primary;
  uint16_t secondary = flappy_game ? rgb565(155, 228, 91) : palette->secondary;
  uint16_t ink = flappy_game ? rgb565(243, 165, 31) : palette->ink;
  uint16_t light = flappy_game ? rgb565(255, 241, 166) : palette->light;
  int cell = (panel_width - 4) / game->width;
  int height_cell = (panel_height - 4) / game->height;
  if (height_cell < cell) cell = height_cell;
  if (cell < 6) cell = 6;
  int max_cell = (
    game->kind == PET_P4_GAME_SNAKE || game->kind == PET_P4_GAME_FLAPPY
  ) ? 28 : 21;
  if (cell > max_cell) cell = max_cell;
  int grid_width = cell * game->width;
  int grid_height = cell * game->height;
  int grid_x = panel_x + (panel_width - grid_width) / 2;
  int grid_y = panel_y + (panel_height - grid_height) / 2;
  if (clean_style) {
    fill_round_rect(grid_x - 2, grid_y - 2, grid_width + 4, grid_height + 4, 8, empty);
  } else {
    fill_rect(grid_x - 2, grid_y - 2, grid_width + 4, grid_height + 4, grid_outline);
  }
  for (int y = 0; y < game->height; y += 1) {
    for (int x = 0; x < game->width; x += 1) {
      int index = y * game->width + x;
      uint8_t tone = game->cells[index];
      if (game->kind == PET_P4_GAME_GOMOKU) {
        /* packed gomoku cell: black -> ink, white -> light, bare cursor -> secondary */
        uint8_t stone = tone & PET_P4_GAME_GOMOKU_STONE_MASK;
        tone = stone == 1 ? 3 : stone == 2 ? 4 : (tone & PET_P4_GAME_GOMOKU_CURSOR) ? 2 : 0;
      }
      uint16_t color = empty;
      if (tone == 1) color = primary;
      else if (tone == 2) color = secondary;
      else if (tone == 3) color = ink;
      else if (tone == 4) color = light;
      if (!clean_style || tone != 0) {
        int gap = clean_style ? 0 : 1;
        fill_rect(
          grid_x + x * cell + gap,
          grid_y + y * cell + gap,
          cell - gap * 2,
          cell - gap * 2,
          color
        );
      }
    }
  }
  if (game->kind == PET_P4_GAME_BOUNDED) {
    for (int index = 0; index < game->entity_count; index += 1) {
      draw_miniapp_game_entity(
        &game->entities[index], sprites, grid_x, grid_y, cell, clean_style, palette
      );
    }
  }
}

static bool miniapp_tool_badge_visible(const char *badge) {
  return badge && badge[0]
    && strcmp(badge, "0") != 0
    && strcmp(badge, "—") != 0
    && strcmp(badge, "-") != 0;
}

static void draw_miniapp_tool_keyboard_icon(
  int x,
  int y,
  uint16_t accent,
  uint16_t background
) {
  fill_rect(x, y, 22, 14, accent);
  fill_rect(x + 2, y + 2, 18, 10, background);
  for (int key_y = 0; key_y < 2; key_y += 1) {
    for (int key_x = 0; key_x < 4; key_x += 1) {
      fill_rect(x + 4 + key_x * 4, y + 4 + key_y * 4, 2, 2, accent);
    }
  }
}

static void render_pixel_tool_miniapp_page(const pet_p4_miniapp_view_t *app) {
  const uint16_t background = rgb565(5, 7, 6);
  const uint16_t panel = rgb565(11, 15, 14);
  const uint16_t line = rgb565(34, 41, 37);
  const uint16_t ink = rgb565(243, 241, 233);
  const uint16_t muted = rgb565(143, 152, 147);
  const uint16_t progress_background = rgb565(32, 37, 34);
  const uint16_t shadow = rgb565(0, 0, 0);
  const miniapp_tool_accent_t accent = miniapp_tool_accent(app->visual_palette);
  const char *title = app->title[0] ? app->title : app->widget_id;
  const bool show_badge = miniapp_tool_badge_visible(app->badge);
  const bool show_progress = app->progress_percent >= 0;
  const bool show_footer = app->footer[0] != '\0';

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 10) {
    fill_rect(0, y, PET_P4_UI_WIDTH, 1, rgb565(8, 10, 9));
  }
  fill_rect(26, 0, 96, 4, accent.color);

  fill_rect(30, 28, 50, 50, shadow);
  fill_rect(26, 24, 50, 50, accent.color);
  fill_rect(28, 26, 46, 46, background);
  blend_rect(28, 26, 46, 46, accent.red, accent.green, accent.blue, 33);
  miniapp_pixel_palette_t icon_palette = {
    .background = background,
    .background_alt = background,
    .surface = background,
    .ink = accent.color,
    .primary = accent.color,
    .secondary = accent.color,
    .light = accent.color,
    .shadow = shadow,
  };
  draw_miniapp_pixel_sprite(app->visual_sprite, 38, 36, 3, &icon_palette);
  draw_text_line(title, 90, 30, 188, ink, 2, true);

  int badge_width = 0;
  int badge_x = 612;
  if (show_badge) {
    badge_width = utf8_text_width(app->badge, -1, 1) + 22;
    if (badge_width < 44) badge_width = 44;
    if (badge_width > 82) badge_width = 82;
    badge_x = 612 - badge_width;
    fill_round_rect(badge_x, 28, badge_width, 42, 21, accent.color);
    draw_text_center(
      app->badge,
      badge_x + badge_width / 2,
      text_y_in_box(app->badge, 1, 28, 42),
      badge_width - 12,
      background,
      1
    );
  }
  if (app->headline[0]) {
    int status_right = show_badge ? badge_x - 14 : 612;
    int status_width = status_right - 370;
    if (status_width > 36) {
      draw_text_right(app->headline, status_right, 39, status_width, accent.color, 1);
    }
  }

  int content_bottom = 460;
  int footer_y = -1;
  int progress_y = -1;
  if (show_footer) {
    footer_y = content_bottom - 26;
    content_bottom = footer_y - 10;
  }
  if (show_progress) {
    progress_y = content_bottom - 36;
    content_bottom = progress_y - 10;
  }

  bool show_metric = app->metric_label[0] || app->metric_value[0];
  const int metric_x = 26;
  const int metric_y = 94;
  const int metric_width = 588;
  const int metric_height = content_bottom - metric_y;
  if (show_metric && metric_height > 80) {
    fill_rect(metric_x, metric_y, metric_width, metric_height, line);
    fill_rect(metric_x + 2, metric_y + 2, metric_width - 4, metric_height - 4, panel);
    fill_rect(metric_x + 2, metric_y + 2, 6, metric_height - 4, accent.color);

    if (app->metric_label[0]) {
      draw_miniapp_pixel_sprite(app->visual_sprite, 52, metric_y + 24, 1, &icon_palette);
      draw_text_line(app->metric_label, 70, metric_y + 20, 500, muted, 1, true);
    }

    const char *metric_value = app->metric_value[0] ? app->metric_value : "—";
    int value_max_width = app->metric_unit[0] ? 430 : 520;
    int value_scale = fitted_text_scale(metric_value, value_max_width, 5, 2);
    int value_width = utf8_text_width(metric_value, -1, value_scale);
    int value_box_y = metric_y + (app->metric_label[0] ? 58 : 28);
    int value_box_height = metric_height - (app->metric_label[0] ? 74 : 44);
    if (value_box_height < 42) value_box_height = 42;
    int value_y = text_y_in_box(metric_value, value_scale, value_box_y, value_box_height);
    draw_text_line(metric_value, 52, value_y, value_max_width, ink, value_scale, true);
    if (app->metric_unit[0]) {
      int unit_x = 52 + value_width + 14;
      if (unit_x > 520) unit_x = 520;
      draw_text_line(
        app->metric_unit,
        unit_x,
        value_y + (value_scale - 1) * 12,
        82,
        muted,
        1,
        true
      );
    }
  }

  if (show_progress) {
    char percent[12];
    snprintf(percent, sizeof(percent), "%d%%", app->progress_percent);
    draw_text_line(
      app->progress_label[0] ? app->progress_label : "进度",
      26,
      progress_y,
      420,
      muted,
      1,
      true
    );
    draw_text_right(percent, 614, progress_y, 90, ink, 1);
    fill_rect(26, progress_y + 26, 588, 8, progress_background);
    int progress_width = (int) ((uint32_t) 588U * (uint32_t) app->progress_percent / 100U);
    if (progress_width > 0) fill_rect(26, progress_y + 26, progress_width, 8, accent.color);
  }

  if (show_footer) {
    draw_miniapp_tool_keyboard_icon(26, footer_y + 4, accent.color, background);
    draw_text_line(app->footer, 58, footer_y + 2, 556, muted, 1, true);
  }
}

static void render_pixel_miniapp_page(const pet_p4_miniapp_view_t *app) {
  miniapp_pixel_palette_t palette = miniapp_pixel_palette(app->visual_palette);
  memset(&g_miniapp_sprites, 0, sizeof(g_miniapp_sprites));
  (void) pet_p4_miniapp_get_sprites(&g_miniapp_sprites);
  bool clean_style = strcmp(app->visual_style, "clean") == 0;
  bool native_game = app->game.kind != PET_P4_GAME_NONE;
  bool blocks_game = app->game.kind == PET_P4_GAME_BLOCKS;
  bool native_progress = native_game && app->progress_percent >= 0;
  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, palette.background);
  if (clean_style) {
    blend_rect(0, 0, PET_P4_UI_WIDTH, 168, 255, 255, 255, 18);
    blend_rect(0, 360, PET_P4_UI_WIDTH, 120, 0, 0, 0, 10);
    fill_round_rect(28, 0, 108, 7, 4, palette.primary);
  } else {
    for (int y = 0; y < PET_P4_UI_HEIGHT; y += 32) {
      for (int x = 0; x < PET_P4_UI_WIDTH; x += 32) {
        uint16_t color = (((x / 32) + (y / 32)) & 1)
          ? palette.background_alt : palette.background;
        fill_rect(x, y, 32, 32, color);
      }
    }
  }

  if (clean_style) {
    draw_miniapp_clean_card(28, 18, 584, 62, palette.surface, &palette);
  } else {
    draw_miniapp_pixel_panel(28, 18, 584, 62, &palette);
  }
  draw_text_line(app->title[0] ? app->title : app->widget_id,
                 44, 33, 360, palette.ink, 2, true);
  const char *time_text = native_game && app->headline[0]
    ? app->headline
    : (app->note[0] ? app->note : app->eyebrow);
  if (time_text[0]) {
    if (clean_style) fill_round_rect(452, 32, 140, 32, 16, palette.secondary);
    else fill_rect(452, 32, 140, 32, palette.secondary);
    draw_text_center(time_text, 522, text_y_in_box(time_text, 1, 32, 32),
                     128, palette.ink, 1);
  }

  bool scoreboard_first = !native_game && strcmp(app->visual_layout, "scoreboard") == 0;
  int play_x = scoreboard_first ? 248 : 28;
  int play_w = native_game ? 462 : (scoreboard_first ? 364 : 430);
  int score_x = native_game ? 504 : (scoreboard_first ? 28 : 470);
  int score_w = native_game ? 108 : (scoreboard_first ? 202 : 142);
  int stage_y = native_game ? (blocks_game && !native_progress ? 84 : 88) : 104;
  int stage_h = native_game
    ? (native_progress ? 298 : (blocks_game ? 350 : 318))
    : 248;
  int score_h = native_game ? 132 : (scoreboard_first ? stage_h : 160);
  if (native_game) {
    if (clean_style) {
      draw_miniapp_clean_card(
        play_x, stage_y, play_w, stage_h, palette.background_alt, &palette
      );
    } else {
      draw_miniapp_pixel_game_panel(play_x, stage_y, play_w, stage_h, &palette);
    }
  } else {
    if (clean_style) {
      draw_miniapp_clean_card(play_x, stage_y, play_w, stage_h, palette.surface, &palette);
    } else {
      draw_miniapp_pixel_panel(play_x, stage_y, play_w, stage_h, &palette);
    }
  }
  if (clean_style) {
    draw_miniapp_clean_card(score_x, stage_y, score_w, score_h, palette.surface, &palette);
  } else {
    draw_miniapp_pixel_panel(score_x, stage_y, score_w, score_h, &palette);
  }

  const char *headline_text = app->headline[0] ? app->headline : "READY";
  if (native_game) {
    draw_miniapp_game_grid(
      &app->game,
      &g_miniapp_sprites,
      play_x + 2,
      stage_y + 2,
      play_w - 4,
      stage_h - 4,
      &palette,
      clean_style
    );
  } else {
    if (app->eyebrow[0]) {
      draw_text_center(app->eyebrow, play_x + play_w / 2, 120, play_w - 24,
                       palette.ink, 1);
    }
    bool wide_sprite = strncmp(app->visual_sprite, "mole-", 5) == 0;
    int sprite_cell = wide_sprite ? 14 : 16;
    int sprite_size = sprite_cell * (wide_sprite ? 15 : 9);
    draw_miniapp_pixel_sprite(
      app->visual_sprite,
      play_x + (play_w - sprite_size) / 2,
      wide_sprite ? 144 : 140,
      sprite_cell,
      &palette
    );
    draw_text_center(headline_text,
                     play_x + play_w / 2,
                     text_y_in_box(headline_text, 2, 302, 38),
                     play_w - 32,
                     palette.primary,
                     2);
  }

  const char *metric_label = app->metric_label[0] ? app->metric_label : "SCORE";
  bool has_badge = app->badge[0] != '\0';
  int badge_size = native_game ? 26 : 30;
  int score_label_x = score_x + 12;
  int score_label_y = stage_y + 14;
  int score_label_w = score_w - 24 - (has_badge ? badge_size + 8 : 0);
  if (clean_style) {
    fill_round_rect(score_label_x, score_label_y, score_label_w, 26, 8, palette.ink);
  } else {
    fill_rect(score_label_x, score_label_y, score_label_w, 26, palette.ink);
  }
  draw_text_center(metric_label,
                   score_label_x + score_label_w / 2,
                   text_y_in_box(metric_label, 1, score_label_y, 26),
                   score_label_w - 8,
                   palette.light,
                   1);
  const char *metric_value = app->metric_value[0] ? app->metric_value : "0";
  int value_y = stage_y + (native_game ? 48 : (scoreboard_first ? 62 : 52));
  int value_h = native_game ? 58 : (scoreboard_first ? 102 : 62);
  int value_max_scale = native_game ? 3 : (scoreboard_first ? 5 : 4);
  int value_scale = fitted_text_scale(metric_value, score_w - 24, value_max_scale, 2);
  draw_text_center(metric_value,
                   score_x + score_w / 2,
                   text_y_in_box(metric_value, value_scale, value_y, value_h),
                   score_w - 24,
                   palette.primary,
                   value_scale);
  if (app->metric_unit[0]) {
    draw_text_center(app->metric_unit, score_x + score_w / 2,
                     text_y_in_box(app->metric_unit, 1, stage_y + score_h - 30, 24),
                     score_w - 24, palette.ink, 1);
  }
  if (has_badge) {
    int badge_x = score_x + score_w - badge_size - 10;
    int badge_y = stage_y + 12;
    if (clean_style) {
      fill_ellipse(
        badge_x + badge_size / 2,
        badge_y + badge_size / 2,
        badge_size / 2,
        badge_size / 2,
        palette.secondary
      );
    } else {
      fill_rect(badge_x, badge_y, badge_size, badge_size, palette.secondary);
    }
    draw_text_center(app->badge, badge_x + badge_size / 2,
                     text_y_in_box(app->badge, 1, badge_y, badge_size),
                     badge_size - 4, palette.ink, 1);
  }

  int progress_y = native_game ? 394 : 374;
  if (app->progress_percent >= 0) {
    int progress_width = (int) ((uint32_t) 584U * (uint32_t) app->progress_percent / 100U);
    if (clean_style) fill_round_rect(28, progress_y, 584, 14, 7, palette.shadow);
    else fill_rect(28, progress_y, 584, 14, palette.ink);
    if (progress_width > 0) {
      if (clean_style) {
        fill_round_rect(
          32,
          progress_y + 4,
          progress_width > 576 ? 576 : progress_width,
          6,
          3,
          palette.primary
        );
      } else {
        fill_rect(
          32,
          progress_y + 4,
          progress_width > 576 ? 576 : progress_width,
          6,
          palette.primary
        );
      }
    }
  }

  bool maximized_blocks = blocks_game && !native_progress;
  int footer_y = maximized_blocks ? 442 : (native_game ? 414 : 406);
  int footer_h = maximized_blocks ? 26 : (native_game ? 46 : 54);
  if (clean_style) {
    fill_round_rect_outline(
      28, footer_y, 584, footer_h, 8, palette.surface, palette.shadow
    );
  } else {
    draw_miniapp_pixel_panel(28, footer_y, 584, footer_h, &palette);
  }
  if (app->footer[0]) {
    draw_text_center(app->footer, 320,
                     text_y_in_box(app->footer, native_game ? 1 : 2,
                                   footer_y + 4, footer_h - 8),
                     548, palette.ink, native_game ? 1 : 2);
  }
}


static void render_data_miniapp_page(const pet_p4_miniapp_view_t *app) {
  const pet_p4_data_view_t *data = &app->data;
  const uint16_t bg = rgb565(7, 12, 18), ink = rgb565(236, 243, 251), muted = rgb565(147, 165, 182);
  const uint16_t accent = rgb565(76, 167, 250);
  const bool todos = strcmp(app->data_source, "todos.upcoming") == 0;
  fill_rect(0, 0, 640, 480, bg);
  draw_text_line(app->title, 24, 18, data->date[0] ? 346 : 490, ink, 2, true);
  if (data->date[0]) {
    if (todos && strlen(data->date) == 10) draw_text_line(data->date + 5, 450, 22, 90, muted, 1, true);
    else draw_text_line(data->date, 384, 22, 156, muted, 1, true);
  }
  char page[16]; snprintf(page, sizeof(page), "%u/%u", data->page + 1, data->pages);
  draw_text_line(page, 554, 22, 70, accent, 1, true);
  fill_rect(24, 57, 592, 1, rgb565(43, 59, 76));
  for (int i = 0; i < data->count; i++) {
    const pet_p4_data_row_t *r = &data->rows[i];
    int y = 74 + i * 61;
    uint16_t tone = data->stale ? muted : r->tone > 0 ? rgb565(255, 100, 102) : r->tone < 0 ? rgb565(79, 212, 149) : ink;
    if (todos) {
      tone = data->stale ? muted : r->tone < 0 ? rgb565(79, 212, 149) : accent;
      char title[sizeof(r->label) + sizeof(r->meta)];
      snprintf(title, sizeof(title), "%s%s", r->label, r->meta);
      draw_text_line(title, 24, y, 592, tone, 2, true);
      draw_text_line(r->value, 24, y + 33, 420, muted, 1, true);
      draw_text_line(r->detail, 500, y + 33, 116, tone, 1, true);
    } else {
    draw_text_line(r->label, 24, y, 235, ink, 1, true);
    draw_text_line(r->value, 270, y, 176, tone, 2, true);
    draw_text_line(r->detail, 465, y + 2, 153, tone, 1, true);
    draw_text_line(r->meta, 24, y + 29, 592, muted, 1, true);
    }
    fill_rect(24, y + 53, 592, 1, rgb565(28, 40, 51));
  }
  if (!data->count) draw_text_line("等待数据", 24, 135, 590, ink, 2, true);
  const char *status = !data->received ? "请连接 PC 并打开 Pet Manager" : data->stale ? "数据已过期，请检查 PC 或 USB 连接" : data->message;
  draw_text_line(status, 24, 391, 592, data->stale || !strcmp(data->status, "error") ? rgb565(255, 186, 86) : muted, 1, true);
  draw_text_line(app->footer, 24, 437, 592, accent, 1, true);
}

// Semantic icons are geometric primitives, not font/Unicode glyphs. The board's
// compact Chinese font does not contain the browser preview's icon font.
static void draw_media_note(int x,int y,uint16_t color) {
  fill_rect(x+14,y,3,22,color);fill_rect(x+16,y+1,9,3,color);
  fill_ellipse(x+10,y+22,7,5,color);
}
static void draw_media_volume(int x,int y,uint16_t color) {
  fill_rect(x,y+8,5,10,color);
  for(int i=0;i<7;i++)fill_rect(x+5+i,y+8-i,1,10+i*2,color);
  for(int d=0;d<2;d++) {
    draw_line(x+16+d,y+6,x+20+d,y+12,color);draw_line(x+20+d,y+12,x+16+d,y+19,color);
    draw_line(x+21+d,y+2,x+27+d,y+12,color);draw_line(x+27+d,y+12,x+21+d,y+23,color);
  }
}
static void draw_media_queue(int x,int y,uint16_t color) {
  for(int row=0;row<3;row++)fill_rect(x,y+row*7,16,2,color);
  fill_rect(x+23,y+3,2,17,color);fill_rect(x+24,y+3,5,2,color);
  fill_ellipse(x+20,y+20,5,3,color);
}

static EXT_RAM_BSS_ATTR pet_p4_miniapp_sprite_pack_t g_surface_images;
#include "pet_p4_app_icons.h"
static EXT_RAM_BSS_ATTR uint8_t g_surface_indices[PET_SURFACE_IMAGE_PIXELS];
// Runtime-only cache: flash still stores the same compact P4I1 images. A static
// full-screen background must not be resampled/alpha-blended 307200 times/frame.
#if PET_P4_APP_PPA_COPY
#define PET_SURFACE_BACKGROUND_ALIGN __attribute__((aligned(64)))  /* PPA source/destination cache-line alignment */
#else
#define PET_SURFACE_BACKGROUND_ALIGN
#endif
static EXT_RAM_BSS_ATTR PET_SURFACE_BACKGROUND_ALIGN uint16_t g_surface_background[640*480];
static uint16_t g_surface_colors[PET_SURFACE_IMAGES][256];
static uint8_t g_surface_alpha[PET_SURFACE_IMAGES][256];
#if PET_P4_APP_COLD_FAST
static bool g_surface_opaque[PET_SURFACE_IMAGES];
#endif
static bool g_surface_background_valid;
static uint8_t g_surface_background_page;
static uint32_t g_surface_revision, g_surface_offsets[PET_P4_MINIAPP_SPRITE_MAX];
static uint64_t g_surface_play_ms,g_surface_last_ms;
static bool g_surface_was_playing;
#include "pet_p4_capability_draw.inc"
#if PET_P4_APP_COLD_FAST
/* Pure span helpers; the host exactness test compiles them against the loops they replace. */
static inline void cold_fast_blend_span(uint16_t *px,int count,uint8_t r,uint8_t g,uint8_t b,uint8_t alpha){
  for(int i=0;i<count;i++)px[i]=blend_rgb565(px[i],r,g,b,alpha);
}
/* blend_rgb565 is channel-separable, so one 5/6/5-bit table per channel, built
 * by calling blend_rgb565 itself, reproduces it exactly for a constant colour+alpha. */
typedef struct {uint16_t r[32],g[64],b[32];} cold_fast_blend_lut_t;
static inline void cold_fast_blend_lut(cold_fast_blend_lut_t *lut,uint8_t r,uint8_t g,uint8_t b,uint8_t alpha){
  for(unsigned v=0;v<64;v++){
    uint16_t out=blend_rgb565((uint16_t)(((v&31)<<11)|(v<<5)|(v&31)),r,g,b,alpha);
    if(v<32){lut->r[v]=(uint16_t)(out&0xF800);lut->b[v]=(uint16_t)(out&0x001F);}
    lut->g[v]=(uint16_t)(out&0x07E0);
  }
}
static inline void cold_fast_blend_span_lut(uint16_t *px,int count,const cold_fast_blend_lut_t *lut){
  for(int i=0;i<count;i++){uint16_t v=px[i];px[i]=(uint16_t)(lut->r[v>>11]|lut->g[(v>>5)&63]|lut->b[v&31]);}
}
static inline void cold_fast_image_span(uint16_t *out,const uint8_t *row,const uint16_t *colors,const uint8_t *alpha,const uint8_t *palette,int count){
  for(int i=0;i<count;i++){
    unsigned index=row[i],a=alpha[index];
    if(!a)continue;
    if(a==255){out[i]=colors[index];continue;}
    const uint8_t *p=palette+4*index;uint16_t bg=out[i];
    unsigned r=((bg>>11)&31)*255/31,g=((bg>>5)&63)*255/63,b=(bg&31)*255/31;
    out[i]=rgb565((p[0]*a+r*(255-a))/255,(p[1]*a+g*(255-a))/255,(p[2]*a+b*(255-a))/255);
  }
}
static inline bool cold_fast_palette_opaque(const uint8_t *palette){
  for(int color=0;color<256;color++)if(palette[4*color+3]!=255)return false;
  return true;
}
/* Opacity over the indices the frame actually uses (unused palette entries do not matter). */
static inline bool cold_fast_indices_opaque(const uint8_t *indices,size_t count,const uint8_t *alpha){
  for(size_t i=0;i<count;i++)if(alpha[indices[i]]!=255)return false;
  return true;
}
/* Only the cover==1 interior of a valid, unstroked rounded panel. Corners
 * continue through the original SDF below. Alpha interpolation keeps its signed
 * integer truncation; horizontal tiles compute it once per column, not per pixel.
 * Scratch is bounded (64 alpha bytes or the existing 256-byte channel LUT). */
static void cold_fast_panel_interior(const pet_surface_node_t *n,int x0,int y0,int x1,int y1,
    uint8_t r,uint8_t g,uint8_t b){
  if(x0>=x1||y0>=y1)return;
  if(n->vertical||n->opacity==n->opacity_end){
    cold_fast_blend_lut_t lut;
    int last=-1;
    for(int y=y0;y<y1;y++){
      int a=n->opacity+(n->opacity_end-n->opacity)*(n->vertical?y:0)/(n->vertical&&n->h>1?n->h-1:1);
      if(!a)continue;
      uint16_t *row=g_framebuffer+(n->y+y)*640+n->x+x0;
      if(a==255){for(int x=x0;x<x1;x++)row[x-x0]=n->color;continue;}
      if(a!=last){cold_fast_blend_lut(&lut,r,g,b,(uint8_t)a);last=a;}
      cold_fast_blend_span_lut(row,x1-x0,&lut);
    }
    return;
  }
  for(int tile=x0;tile<x1;tile+=64){
    int count=x1-tile;if(count>64)count=64;
    uint8_t alpha[64];
    for(int x=0;x<count;x++)alpha[x]=(uint8_t)(n->opacity+(n->opacity_end-n->opacity)*(tile+x)/(n->w>1?n->w-1:1));
    for(int y=y0;y<y1;y++){
      uint16_t *row=g_framebuffer+(n->y+y)*640+n->x+tile;
      for(int x=0;x<count;x++){
        unsigned a=alpha[x];
        if(a==255)row[x]=n->color;
        else if(a)row[x]=blend_rgb565(row[x],r,g,b,(uint8_t)a);
      }
    }
  }
}
/* Set when the clear was skipped under a full-screen first JPEG node; the JPEG
 * branch clears it on success or paints the background itself on failure. */
static bool g_cold_fast_jpeg_pending;
/* Tentative forward declaration of the renderer's own static (defined below next to the label code). */
static const pet_surface_t *g_app_text_surface;
#endif
/* Track C fragment: big-image tiles and clip playback (flag-gated, shares these statics). */
#include "pet_p4_renderer_ext.inc"

/* Render-thread handoff: a hidden/absent clip is polled on EVERY frame until
 * its worker and decoder really release the source. Never retag an old view
 * with a store-generation lookup performed after the model lock was dropped. */
static bool renderer_component_media_frame(const pet_p4_miniapp_render_snapshot_t *snapshot){
#if PET_P4_BIG_IMAGE || PET_P4_CLIP
  renderer_ext_media_snapshot(snapshot?snapshot->media_generation:0);
#endif
#if PET_P4_CLIP
  bool clip_visible=false;
  if(snapshot&&snapshot->view.active&&snapshot->view.app_surface&&snapshot->surface_active){
    for(unsigned i=0;i<snapshot->surface.count;i++){
      const pet_surface_node_t *n=&snapshot->surface.nodes[i];
      if(n->type!=PET_SURFACE_NODE_CLIP||n->hidden||!pet_surface_node_on_page(n,snapshot->view.surface_page))continue;
      pet_surface_node_t projected;
      const pet_surface_viewport_t *viewport=pet_surface_node_viewport(&snapshot->surface,n);
      if(pet_surface_project(viewport,n,viewport?viewport->offset:0,&projected)){
        clip_visible=true;break;
      }
    }
  }
  /* Generation retirement cannot depend on draw: a clip may have scrolled out
   * of its viewport, so no node renderer would see its stale source token. */
  if(!clip_visible||(g_clip_lease.token&&g_clip_lease.generation!=snapshot->media_generation))
    return renderer_ext_clip_retire();
#else
  (void)snapshot;
#endif
  return true;
}
#if PET_SERVICE_BRIDGE_ENABLED
static bool draw_service_image(const pet_surface_node_t*n,const char*id,uint64_t now){
  if(!n->image_bind[0])return false;pet_bridge_image_lease_t lease;
  if(!pet_app_host_image_borrow(id,n->image_bind,now,&lease))return false;
  for(int dy=0;dy<n->h;dy++){
    int y=n->y+dy;if(y<0||y>=480)continue;unsigned sy=(unsigned)((uint32_t)dy*lease.height/n->h);
    for(int dx=0;dx<n->w;dx++){int x=n->x+dx;if(x<0||x>=640||!app_clip_contains(x,y))continue;unsigned sx=(unsigned)((uint32_t)dx*lease.width/n->w);g_framebuffer[y*640+x]=lease.pixels[sy*lease.width+sx];}
  }pet_app_host_image_release(&lease);
#if PET_P4_APP_COLD_FAST
  /* A pending clear belongs to this first full-screen JPEG fallback node.
   * A successful service image has already overwritten the same whole frame. */
  g_cold_fast_jpeg_pending=false;
#endif
  return true;
}
#endif
static void draw_surface_image(const pet_surface_node_t *n) {
#if PET_P4_BIG_IMAGE
  /* A parsed big-image node has no ordinary image slot. Prefer a real
   * palette/JPEG image even if a synthetic node was zero-initialized. */
  if(n->image<0&&n->big>=0){renderer_ext_draw_big(n);return;}
#endif
  if(n->image<0||n->image>=g_surface_images.count)return;
  const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[n->image];
  if(im->fps!=0)return;
  const uint8_t *packed=g_surface_images.pixels+im->data_offset;
  if(im->data_length>=16&&packed[2]=='J'){
    // The parser only permits this as the first full-screen static node. Decode
    // once into the existing background cache, never on each animation frame.
    int w=0,h=0,stride=0;bool ok=false;
#if PET_P4_KEYFRAME_BG
    /* Keyframe set: the worker-decoded current frame is copied; frame 0 before the
     * worker ran (or a cache holding another frame) decodes exactly as before. */
    if(g_app_text_surface&&g_app_text_surface->keyframes>1){
      if(keyframe_blit(g_app_text_surface,&im)){ok=true;w=640;h=480;}
      else packed=g_surface_images.pixels+im->data_offset;
    }
    if(!ok)
#endif
    if(pet_image_jpeg(packed,im->data_length)&&g_jpeg_input&&im->data_length-12<=g_jpeg_input_capacity){
      memcpy(g_jpeg_input,packed+12,im->data_length-12);
      esp_err_t err;JRESULT fallback;
      ok=decode_jpeg_frame_hardware(g_jpeg_input,im->data_length-12,&w,&h,&stride,&err);
      if(!ok){ok=decode_jpeg_frame_tjpgd(g_jpeg_input,im->data_length-12,&w,&h,&fallback);stride=w;}
    }
    if(ok&&w==640&&h==480&&stride)for(int y=0;y<480;y++)memcpy(g_framebuffer+y*640,g_jpeg_output+y*stride,1280);
#if PET_P4_APP_COLD_FAST
    /* The clear was skipped for this node: a successful full-screen decode wrote every
     * pixel; any other outcome paints the background exactly where the clear would have. */
    if(g_cold_fast_jpeg_pending){g_cold_fast_jpeg_pending=false;if(!(ok&&w==640&&h==480)&&g_app_text_surface)fill_rect(0,0,640,480,g_app_text_surface->background);}
#endif
    return;
  }
  const uint8_t *palette=g_surface_images.pixels+im->data_offset+12;
  const uint8_t *indices=g_surface_indices+g_surface_offsets[n->image];
  float angle=n->rotate_ms?(float)(g_surface_play_ms%n->rotate_ms)*6.2831853f/n->rotate_ms:0;
  float co=cosf(angle),si=sinf(angle);
  int iw=im->frame_width,ih=im->frame_height;
  int stride=iw,ox=0,oy=0;
  if(n->tile_cols){iw/=n->tile_cols;ih/=n->tile_rows;ox=n->tile_index*iw;oy=n->tile_row*ih;}
  // Q16 affine steps: trigonometry/divisions only once per image, never per
  // pixel. Keep the source PNG's existing nearest-neighbour sampling contract.
  const float unit=65536.0f;
  int32_t xx=(int32_t)lroundf(co*iw/n->w*unit),xy=(int32_t)lroundf(si*iw/n->h*unit);
  int32_t yx=(int32_t)lroundf(-si*ih/n->w*unit),yy=(int32_t)lroundf(co*ih/n->h*unit);
  int32_t row_x=(int32_t)lroundf((co*(.5f/n->w-.5f)+si*(.5f/n->h-.5f)+.5f)*iw*unit);
  int32_t row_y=(int32_t)lroundf((-si*(.5f/n->w-.5f)+co*(.5f/n->h-.5f)+.5f)*ih*unit);
  const uint16_t *colors=g_surface_colors[n->image];const uint8_t *alpha=g_surface_alpha[n->image];
#if PET_P4_APP_COLD_FAST
  if(!n->rotate_ms&&!n->tile_cols&&n->w==iw&&n->h==ih&&g_framebuffer){
    /* Identity sampling: xx==yy==65536, xy==yx==0, row_x==row_y==32768, so the
     * loop below reads indices[dy*stride+dx]; same three per-pixel cases. */
    int dx0=n->x<0?-n->x:0,dx1=n->w;if(n->x+dx1>640)dx1=640-n->x;
    if(g_app_clip.active){if(n->x+dx0<g_app_clip.x)dx0=g_app_clip.x-n->x;if(n->x+dx1>g_app_clip.right)dx1=g_app_clip.right-n->x;}
    for(int dy=0;dy<n->h;dy++){
      int py=n->y+dy;if(py<0||py>=480||(g_app_clip.active&&(py<g_app_clip.y||py>=g_app_clip.bottom)))continue;
      if(dx1>dx0)cold_fast_image_span(g_framebuffer+py*640+n->x+dx0,indices+dy*stride+dx0,colors,alpha,palette,dx1-dx0);
    }
    pet_render_counter_increment(&g_cold_fast_image_nodes);return;
  }
#endif
  for(int dy=0;dy<n->h;dy++,row_x+=xy,row_y+=yy){
    int py=n->y+dy;if(py<0||py>=480||(g_app_clip.active&&(py<g_app_clip.y||py>=g_app_clip.bottom)))continue;
    int32_t sx=row_x,sy=row_y;uint16_t *out=g_framebuffer+(n->y+dy)*640+n->x;
    for(int dx=0;dx<n->w;dx++,sx+=xx,sy+=yx){
      if(n->x+dx<0||n->x+dx>=640||!app_clip_contains(n->x+dx,py))continue;
      if(sx<0||sy<0||sx>=(iw<<16)||sy>=(ih<<16))continue;
      unsigned index=indices[((sy>>16)+oy)*stride+(sx>>16)+ox],a=alpha[index];
      if(!a)continue;
      if(a==255){out[dx]=colors[index];continue;}
      const uint8_t *p=palette+4*index;uint16_t bg=out[dx];
      unsigned r=((bg>>11)&31)*255/31,g=((bg>>5)&63)*255/63,b=(bg&31)*255/31;
      out[dx]=rgb565((p[0]*a+r*(255-a))/255,(p[1]*a+g*(255-a))/255,(p[2]*a+b*(255-a))/255);
    }
  }
}
static void load_surface_images(unsigned long long now_ms) {
  if(g_miniapp_render_snapshot.images_changed){
    g_surface_background_valid=false;
    size_t used=0;
    for(int i=0;i<g_surface_images.count;i++){
      const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[i];g_surface_offsets[i]=used;
      if(pet_image_jpeg(g_surface_images.pixels+im->data_offset,im->data_length))continue;
      size_t pixels=(size_t)im->frame_width*im->frame_height;
      if(im->fps!=0||used+pixels>sizeof(g_surface_indices)||!pet_image_decode(g_surface_images.pixels+im->data_offset,im->data_length,g_surface_indices+used,sizeof(g_surface_indices)-used)){
        g_surface_images.count=0;break;
      }
      const uint8_t *palette=g_surface_images.pixels+im->data_offset+12;
      for(int color=0;color<256;color++){
        const uint8_t *p=palette+4*color;
        g_surface_colors[i][color]=rgb565(p[0],p[1],p[2]);g_surface_alpha[i][color]=p[3];
      }
#if PET_P4_APP_COLD_FAST
      g_surface_opaque[i]=cold_fast_palette_opaque(palette)||cold_fast_indices_opaque(g_surface_indices+g_surface_offsets[i],pixels,g_surface_alpha[i]);
#endif
      used+=pixels;
    }
    g_surface_play_ms=0;g_surface_last_ms=now_ms;g_surface_was_playing=false;
  }
}
static void draw_app_panel(const pet_surface_node_t *n) {
  if(!n->opacity&&!n->opacity_end&&!n->stroke_width)return;
  if(!n->radius&&!n->stroke_width&&n->opacity==255&&n->opacity_end==255){fill_rect(n->x,n->y,n->w,n->h,n->color);return;}
  const uint8_t r=((n->color>>11)&31)*255/31,g=((n->color>>5)&63)*255/63,b=(n->color&31)*255/31;
  const int radius=n->radius;
  int x0=n->x<0?-n->x:0,y0=n->y<0?-n->y:0,x1=n->w,y1=n->h;
  if(n->x+x1>640)x1=640-n->x;
  if(n->y+y1>480)y1=480-n->y;
  if(g_app_clip.active){if(n->x+x0<g_app_clip.x)x0=g_app_clip.x-n->x;if(n->y+y0<g_app_clip.y)y0=g_app_clip.y-n->y;if(n->x+x1>g_app_clip.right)x1=g_app_clip.right-n->x;if(n->y+y1>g_app_clip.bottom)y1=g_app_clip.bottom-n->y;}
#if PET_P4_APP_COLD_FAST
  if(!radius&&!n->stroke_width&&n->opacity==n->opacity_end&&n->opacity&&n->opacity!=255&&g_framebuffer&&x1>x0&&y1>y0){
    /* Radius 0: the SDF cover is 1 on every pixel of the clipped rectangle, so the
     * loop below is blend_px(x,y,r,g,b,opacity) everywhere; same blend_rgb565. */
    cold_fast_blend_lut_t lut;cold_fast_blend_lut(&lut,r,g,b,n->opacity);
    for(int y=y0;y<y1;y++)cold_fast_blend_span_lut(g_framebuffer+(n->y+y)*640+n->x+x0,x1-x0,&lut);
    pet_render_counter_increment(&g_cold_fast_panel_nodes);return;
  }
#endif
  const bool solid=n->opacity==255&&n->opacity_end==255&&!n->stroke_width
    &&radius>0&&radius<=n->w/2&&radius<=n->h/2;
  bool span_interior=false;
#if PET_P4_APP_COLD_FAST
  span_interior=!solid&&!n->stroke_width&&radius<=n->w/2&&radius<=n->h/2
    &&g_framebuffer&&x1>x0&&y1>y0;
  if(span_interior){
    /* Three disjoint rectangles are strictly outside the radius-square corners.
     * Their original SDF coverage is exactly 1, including radius zero. */
    cold_fast_panel_interior(n,x0,y0>radius?y0:radius,x1,y1<n->h-radius?y1:n->h-radius,r,g,b);
    cold_fast_panel_interior(n,x0>radius?x0:radius,y0,x1<n->w-radius?x1:n->w-radius,y1<radius?y1:radius,r,g,b);
    cold_fast_panel_interior(n,x0>radius?x0:radius,y0>n->h-radius?y0:n->h-radius,x1<n->w-radius?x1:n->w-radius,y1,r,g,b);
    pet_render_counter_increment(&g_cold_fast_panel_nodes);
  }
#endif
  if(solid){
    // Outside the four radius-square corners the original SDF has cover==1.
    // Fill disjoint opaque spans; corners retain the exact antialiasing below.
    fill_rect(n->x,n->y+radius,n->w,n->h-2*radius,n->color);
    fill_rect(n->x+radius,n->y,n->w-2*radius,radius,n->color);
    fill_rect(n->x+radius,n->y+n->h-radius,n->w-2*radius,radius,n->color);
  }
#if PET_P4_APP_PANEL_MIRROR
  if(solid&&x0==0&&y0==0&&x1==n->w&&y1==n->h){
    /* Integer/half-integer geometry is exactly representable here. The four
     * unclipped SDF corners have identical coverage, not approximated circles.
     * Gradients, strokes, clipping and malformed geometry retain the old path. */
    for(int y=0;y<radius;y++){
      float qy=fabsf(y+.5f-n->h*.5f)-(n->h*.5f-radius);
      for(int x=0;x<radius;x++){
        float qx=fabsf(x+.5f-n->w*.5f)-(n->w*.5f-radius);
        float d=(qx>0&&qy>0?sqrtf(qx*qx+qy*qy):(qx>qy?qx:qy))-radius;
        float cover=.5f-d;if(cover<=0)continue;if(cover>1)cover=1;
        int xx=n->x+x,rx=n->x+n->w-1-x,yy=n->y+y,by=n->y+n->h-1-y;
        if(cover==1){put_px(xx,yy,n->color);put_px(rx,yy,n->color);put_px(xx,by,n->color);put_px(rx,by,n->color);}
        else{int alpha=(int)(255*cover);blend_px(xx,yy,r,g,b,alpha);blend_px(rx,yy,r,g,b,alpha);blend_px(xx,by,r,g,b,alpha);blend_px(rx,by,r,g,b,alpha);}
      }
    }
    return;
  }
#endif
  for(int y=y0;y<y1;y++){
    if((solid||span_interior)&&y>=radius&&y<n->h-radius)continue;
    float qy=fabsf(y+.5f-n->h*.5f)-(n->h*.5f-radius);
    for(int x=x0;x<x1;x++){
    if((solid||span_interior)&&x>=radius&&x<n->w-radius){x=n->w-radius-1;continue;}
    float qx=fabsf(x+.5f-n->w*.5f)-(n->w*.5f-radius);
    // The rounded-box SDF needs sqrt only in the four corner squares.
    float d=(qx>0&&qy>0?sqrtf(qx*qx+qy*qy):(qx>qy?qx:qy))-radius;
    float cover=.5f-d;if(cover<=0)continue;if(cover>1)cover=1;
    int pos=n->vertical?y:x,len=n->vertical?n->h:n->w;
    int a=n->opacity+(n->opacity_end-n->opacity)*pos/(len>1?len-1:1);
    if(a==255&&cover==1)put_px(n->x+x,n->y+y,n->color);
    else if(a)blend_px(n->x+x,n->y+y,r,g,b,(int)(a*cover));
    if(n->stroke_width){float edge=fminf(1,fmaxf(0,d+n->stroke_width+.5f))*cover;uint16_t c=n->stroke;if(edge>0)blend_px(n->x+x,n->y+y,((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31,(int)(edge*255));}
  }}
}
static void draw_app_icon(int icon,int x,int y,int size,uint16_t color){
  if(icon<0||icon>=PET_APP_ICON_COUNT)return;
  float scale=size/48.0f;
  for(int py=0;py<size;py++)for(int px=0;px<size;px++){
    float best=1e6f;
    for(int s=0;s<pet_app_icons[icon].count;s++){
      const uint8_t *v=pet_app_icon_segments[pet_app_icons[icon].start+s];
      float ax=v[0]*scale,ay=v[1]*scale,dx=(v[2]-v[0])*scale,dy=(v[3]-v[1])*scale;
      float den=dx*dx+dy*dy,t=den?((px+.5f-ax)*dx+(py+.5f-ay)*dy)/den:0;t=fminf(1,fmaxf(0,t));
      float ex=px+.5f-ax-t*dx,ey=py+.5f-ay-t*dy,dist=ex*ex+ey*ey;if(dist<best)best=dist;
    }
    float alpha=fminf(1,fmaxf(0,size*.9f/24+.5f-sqrtf(best)));
    if(alpha>0)blend_px(x+px,y+py,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,(int)(alpha*255));
  }
}
static const pet_surface_t *g_app_text_surface;
static const pet_surface_font_t *app_font_glyph(uint32_t cp,int *index){
  if(!g_app_text_surface)return NULL;
  for(int f=0;f<g_app_text_surface->font_count;f++){
    const pet_surface_font_t *font=&g_app_text_surface->fonts[f];
    if(font->image>=g_surface_images.count)continue;
    for(int i=0;i<font->count;i++)if(font->chars[i]==cp){*index=i;return font;}
  }return NULL;
}
static int app_glyph_width(uint32_t cp,uint32_t next){
  int i;const pet_surface_font_t *f=app_font_glyph(cp,&i);if(f)return f->metrics[i][0];
  int width=pet_app_symbol_advance(cp);return 2*(width?width:glyph_width(cp,next));
}
static int app_text_width(const char *s,int px){
  int units=0;while(s&&*s){uint32_t cp=utf8_next(&s);units+=app_glyph_width(cp,utf8_peek(s));}return (units*px+16)/32;
}
static void app_text_bounds(const char *s,int *top,int *bottom){
  bool found=false;
  while(s&&*s){uint32_t cp=utf8_next(&s);int t=0,b=15;
    int i;const pet_surface_font_t *f=app_font_glyph(cp,&i);
    if(f){t=f->metrics[i][1];b=f->metrics[i][2];if(t==b)continue;}
    else {if(pet_app_symbol_advance(cp))pet_app_symbol_bounds(cp,&t,&b);
      else if(!glyph_visual_bounds(cp,utf8_peek(s),1,&t,&b))continue;t*=2;b=(b+1)*2;}
    if(!found||t<*top)*top=t;if(!found||b>*bottom)*bottom=b;found=true;
  }
  if(!found){*top=0;*bottom=32;}
}
static float app_font_sample(const pet_surface_font_t *f,int index,int x,int y){
  int cw=f->cell_w?f->cell_w:32,ch=f->cell_h?f->cell_h:32;
  if(x<0||x>=cw||y<0||y>=ch)return 0;
  const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[f->image];int columns=im->frame_width/cw;
  size_t at=g_surface_offsets[f->image]+(index/columns*ch+y)*im->frame_width+index%columns*cw+x;
  return g_surface_alpha[f->image][g_surface_indices[at]];
}
static float app_glyph_sample(const uint8_t *bitmap,const lv_font_glyph_dsc_t *g,int x,int y){
  return x<0||y<0||x>=g->box_w||y>=g->box_h?0:glyph_alpha_at(bitmap,g,x,y);
}
#if PET_P4_GLYPH_ROWS
static void app_prepare_glyph_rows(const pet_surface_font_t *font,int index,
    const uint8_t *bitmap,const lv_font_glyph_dsc_t *glyph,int width,int iy){
  if(g_glyph_sample_y==iy)return;
  g_glyph_rows[0][0]=g_glyph_rows[1][0]=0;
  g_glyph_rows[0][width+1]=g_glyph_rows[1][width+1]=0;
  for(int x=0;x<width;x++){
    g_glyph_rows[0][x+1]=font?app_font_sample(font,index,x,iy):app_glyph_sample(bitmap,glyph,x,iy);
    g_glyph_rows[1][x+1]=font?app_font_sample(font,index,x,iy+1):app_glyph_sample(bitmap,glyph,x,iy+1);
  }
  g_glyph_sample_y=iy;
}
static inline float app_glyph_row_alpha(int ix,float fx,float fy){
  return (g_glyph_rows[0][ix+1]*(1-fx)+g_glyph_rows[0][ix+2]*fx)*(1-fy)
    +(g_glyph_rows[1][ix+1]*(1-fx)+g_glyph_rows[1][ix+2]*fx)*fy;
}
#endif
#if PET_P4_GLYPH_PIXEL_FAST
/* Private exact store path: framebuffer, node and viewport clipping are
 * intersected once per glyph, then every visible pixel takes the SAME uint8_t
 * alpha conversion, opaque rgb565 word and blend_rgb565 word that blend_px
 * produces under PET_P4_ALPHA_FAST. The float coverage expressions and the
 * two-row source refills are untouched. No new scratch, buffer or cache. */
typedef struct {int x0,x1,y0,y1;} glyph_pixel_bounds_t;
static inline bool glyph_pixel_bounds(const pet_surface_node_t *clip,int left,int right,int top,int bottom,glyph_pixel_bounds_t *out){
  int x0=left,x1=right,y0=top,y1=bottom;
  if(x0<clip->x)x0=clip->x;
  if(x1>clip->x+clip->w)x1=clip->x+clip->w;
  if(y0<clip->y)y0=clip->y;
  if(y1>clip->y+clip->h)y1=clip->y+clip->h;
  if(x0<0)x0=0;
  if(y0<0)y0=0;
  if(x1>PET_P4_UI_WIDTH)x1=PET_P4_UI_WIDTH;
  if(y1>PET_P4_UI_HEIGHT)y1=PET_P4_UI_HEIGHT;
  if(g_app_clip.active){if(x0<g_app_clip.x)x0=g_app_clip.x;if(y0<g_app_clip.y)y0=g_app_clip.y;if(x1>g_app_clip.right)x1=g_app_clip.right;if(y1>g_app_clip.bottom)y1=g_app_clip.bottom;}
  *out=(glyph_pixel_bounds_t){x0,x1,y0,y1};
  return g_framebuffer&&x0<x1&&y0<y1;
}
static inline void glyph_pixel_store(uint16_t *px,uint8_t r,uint8_t g,uint8_t b,uint16_t opaque,float a){
  uint8_t alpha=(uint8_t)(int)a;
  if(!alpha)return;
  if(alpha==255){*px=opaque;return;}
  *px=blend_rgb565(*px,r,g,b,alpha);
}
#endif
static void draw_app_glyph(uint32_t cp,uint32_t next,float x,float y,int px,uint16_t color,const pet_surface_node_t *clip){
  if(cp==0xFE0F||cp==0x200D)return;
  int index;const pet_surface_font_t *font=app_font_glyph(cp,&index);
  if(font){
    float scale=px/(float)(font->cell_h?font->cell_h:32);
    int cw=font->cell_w?font->cell_w:32;
#if PET_P4_GLYPH_PIXEL_FAST
    const uint8_t cr=((color>>11)&31)*255/31,cg=((color>>5)&63)*255/63,cb=(color&31)*255/31;
    const uint16_t opaque=rgb565(cr,cg,cb);
    int left=(int)floorf(x),right=(int)ceilf(x+cw*scale);
    glyph_pixel_bounds_t bounds;
    if(!glyph_pixel_bounds(clip,left,right,(int)floorf(y),(int)ceilf(y+px),&bounds))return;
    for(int tile=left;tile<right;tile+=PET_RASTER_AXIS_TILE){
      unsigned count=(unsigned)(right-tile);if(count>PET_RASTER_AXIS_TILE)count=PET_RASTER_AXIS_TILE;
      int c0=bounds.x0-tile,c1=bounds.x1-tile;if(c0<0)c0=0;if(c1>(int)count)c1=(int)count;
      if(c0>=c1)continue;
      g_glyph_sample_y=INT32_MIN;
      pet_raster_axis_prepare(g_glyph_axis,tile,count,x,scale);
      for(int yy=bounds.y0;yy<bounds.y1;yy++){
        float sy=(yy+.5f-y)/scale-.5f;int iy=(int)floorf(sy);float fy=sy-iy;
        if(cw<=64)app_prepare_glyph_rows(font,index,NULL,NULL,cw,iy);
        uint16_t *row=g_framebuffer+(size_t)yy*PET_P4_UI_WIDTH+tile;
        for(int col=c0;col<c1;col++){
          int ix=g_glyph_axis[col].index;float fx=g_glyph_axis[col].fraction;
          float a;
          if(cw<=64){if(ix< -1||ix>=cw)continue;a=app_glyph_row_alpha(ix,fx,fy);}else
          a=(app_font_sample(font,index,ix,iy)*(1-fx)+app_font_sample(font,index,ix+1,iy)*fx)*(1-fy)+(app_font_sample(font,index,ix,iy+1)*(1-fx)+app_font_sample(font,index,ix+1,iy+1)*fx)*fy;
          if(clip->weight)a=fminf(255,a*1.15f);
          if(a>0)glyph_pixel_store(row+col,cr,cg,cb,opaque,a);
        }
      }
    }
    return;
#elif PET_P4_RASTER_EXACT
    int left=(int)floorf(x),right=(int)ceilf(x+cw*scale);
    for(int tile=left;tile<right;tile+=PET_RASTER_AXIS_TILE){
#if PET_P4_GLYPH_ROWS
      g_glyph_sample_y=INT32_MIN;
#endif
      unsigned count=(unsigned)(right-tile);if(count>PET_RASTER_AXIS_TILE)count=PET_RASTER_AXIS_TILE;
      pet_raster_axis_prepare(g_glyph_axis,tile,count,x,scale);
      for(int yy=(int)floorf(y);yy<(int)ceilf(y+px);yy++){
        if(yy<clip->y||yy>=clip->y+clip->h)continue;
        float sy=(yy+.5f-y)/scale-.5f;int iy=(int)floorf(sy);float fy=sy-iy;
#if PET_P4_GLYPH_ROWS
        if(cw<=64)app_prepare_glyph_rows(font,index,NULL,NULL,cw,iy);
#endif
        for(unsigned col=0;col<count;col++){
          int xx=tile+(int)col;if(xx<clip->x||xx>=clip->x+clip->w)continue;
          int ix=g_glyph_axis[col].index;float fx=g_glyph_axis[col].fraction;
          float a;
#if PET_P4_GLYPH_ROWS
          if(cw<=64){if(ix< -1||ix>=cw)continue;a=app_glyph_row_alpha(ix,fx,fy);}else
#endif
          a=(app_font_sample(font,index,ix,iy)*(1-fx)+app_font_sample(font,index,ix+1,iy)*fx)*(1-fy)+(app_font_sample(font,index,ix,iy+1)*(1-fx)+app_font_sample(font,index,ix+1,iy+1)*fx)*fy;
          if(clip->weight)a=fminf(255,a*1.15f);
          if(a>0)blend_px(xx,yy,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,(int)a);
        }
      }
    }
    return;
#else
    for(int yy=(int)floorf(y);yy<(int)ceilf(y+px);yy++)for(int xx=(int)floorf(x);xx<(int)ceilf(x+cw*scale);xx++){
      if(xx<clip->x||yy<clip->y||xx>=clip->x+clip->w||yy>=clip->y+clip->h)continue;
      float sx=(xx+.5f-x)/scale-.5f,sy=(yy+.5f-y)/scale-.5f;int ix=(int)floorf(sx),iy=(int)floorf(sy);float fx=sx-ix,fy=sy-iy;
      float a=(app_font_sample(font,index,ix,iy)*(1-fx)+app_font_sample(font,index,ix+1,iy)*fx)*(1-fy)+(app_font_sample(font,index,ix,iy+1)*(1-fx)+app_font_sample(font,index,ix+1,iy+1)*fx)*fy;
      if(clip->weight)a=fminf(255,a*1.15f);
      if(a>0)blend_px(xx,yy,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,(int)a);
    }
    return;
#endif
  }
  if(pet_app_symbol_advance(cp)){
    float scale=px/16.0f;int width=cp==0x2103?8:pet_app_symbol_advance(cp);
    for(int yy=(int)floorf(y);yy<(int)ceilf(y+px);yy++)for(int xx=(int)floorf(x);xx<(int)ceilf(x+width*scale);xx++){
      if(xx<clip->x||yy<clip->y||xx>=clip->x+clip->w||yy>=clip->y+clip->h)continue;
      float a=pet_app_symbol_coverage(cp,(xx+.5f-x)/scale,(yy+.5f-y)/scale,scale);
      if(a>0)blend_px(xx,yy,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,(int)(a*255));
    }
    if(cp!=0x2103)return;
    x+=8*scale;cp='C';
  }
  lv_font_glyph_dsc_t g={0};
  if(!lv_font_get_glyph_dsc(&pet_p4_font_cn_16,&g,cp,next)&&!lv_font_get_glyph_dsc(&pet_p4_font_cn_16,&g,'?',0))return;
  g.req_raw_bitmap=1;const uint8_t *bitmap=g.resolved_font->get_glyph_bitmap(&g,NULL);
  if(!bitmap){lv_font_glyph_release_draw_data(&g);return;}
  float scale=px/16.0f,gx=x+g.ofs_x*scale,gy=y+(pet_p4_font_cn_16.line_height-pet_p4_font_cn_16.base_line-g.box_h-g.ofs_y)*scale;
  int left=(int)floorf(gx)-1,top=(int)floorf(gy)-1,right=(int)ceilf(gx+g.box_w*scale)+1,bottom=(int)ceilf(gy+g.box_h*scale)+1;
#if PET_P4_GLYPH_PIXEL_FAST
  const uint8_t cr=((color>>11)&31)*255/31,cg=((color>>5)&63)*255/63,cb=(color&31)*255/31;
  const uint16_t opaque=rgb565(cr,cg,cb);
  glyph_pixel_bounds_t bounds;
  if(glyph_pixel_bounds(clip,left,right,top,bottom,&bounds)){
    for(int tile=left;tile<right;tile+=PET_RASTER_AXIS_TILE){
      unsigned count=(unsigned)(right-tile);if(count>PET_RASTER_AXIS_TILE)count=PET_RASTER_AXIS_TILE;
      int c0=bounds.x0-tile,c1=bounds.x1-tile;if(c0<0)c0=0;if(c1>(int)count)c1=(int)count;
      if(c0>=c1)continue;
      g_glyph_sample_y=INT32_MIN;
      pet_raster_axis_prepare(g_glyph_axis,tile,count,gx,scale);
      for(int yy=bounds.y0;yy<bounds.y1;yy++){
        float sy=(yy+.5f-gy)/scale-.5f;int iy=(int)floorf(sy);float fy=sy-iy;
        if(g.box_w<=64)app_prepare_glyph_rows(NULL,0,bitmap,&g,g.box_w,iy);
        uint16_t *row=g_framebuffer+(size_t)yy*PET_P4_UI_WIDTH+tile;
        for(int col=c0;col<c1;col++){
          int ix=g_glyph_axis[col].index;float fx=g_glyph_axis[col].fraction;
          float a;
          if(g.box_w<=64){if(ix< -1||ix>=g.box_w)continue;a=app_glyph_row_alpha(ix,fx,fy);}else
          a=(app_glyph_sample(bitmap,&g,ix,iy)*(1-fx)+app_glyph_sample(bitmap,&g,ix+1,iy)*fx)*(1-fy)+(app_glyph_sample(bitmap,&g,ix,iy+1)*(1-fx)+app_glyph_sample(bitmap,&g,ix+1,iy+1)*fx)*fy;
          if(clip->weight)a=fminf(255,a*1.18f);
          if(a>0)glyph_pixel_store(row+col,cr,cg,cb,opaque,a);
        }
      }
    }
  }
#elif PET_P4_RASTER_EXACT
  for(int tile=left;tile<right;tile+=PET_RASTER_AXIS_TILE){
#if PET_P4_GLYPH_ROWS
    g_glyph_sample_y=INT32_MIN;
#endif
    unsigned count=(unsigned)(right-tile);if(count>PET_RASTER_AXIS_TILE)count=PET_RASTER_AXIS_TILE;
    pet_raster_axis_prepare(g_glyph_axis,tile,count,gx,scale);
    for(int yy=top;yy<bottom;yy++){
      if(yy<clip->y||yy>=clip->y+clip->h)continue;
      float sy=(yy+.5f-gy)/scale-.5f;int iy=(int)floorf(sy);float fy=sy-iy;
#if PET_P4_GLYPH_ROWS
      if(g.box_w<=64)app_prepare_glyph_rows(NULL,0,bitmap,&g,g.box_w,iy);
#endif
      for(unsigned col=0;col<count;col++){
        int xx=tile+(int)col;if(xx<clip->x||xx>=clip->x+clip->w)continue;
        int ix=g_glyph_axis[col].index;float fx=g_glyph_axis[col].fraction;
        float a;
#if PET_P4_GLYPH_ROWS
        if(g.box_w<=64){if(ix< -1||ix>=g.box_w)continue;a=app_glyph_row_alpha(ix,fx,fy);}else
#endif
        a=(app_glyph_sample(bitmap,&g,ix,iy)*(1-fx)+app_glyph_sample(bitmap,&g,ix+1,iy)*fx)*(1-fy)+(app_glyph_sample(bitmap,&g,ix,iy+1)*(1-fx)+app_glyph_sample(bitmap,&g,ix+1,iy+1)*fx)*fy;
        if(clip->weight)a=fminf(255,a*1.18f);
        if(a>0)blend_px(xx,yy,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,(int)a);
      }
    }
  }
#else
  for(int yy=top;yy<bottom;yy++)for(int xx=left;xx<right;xx++){
    if(xx<clip->x||yy<clip->y||xx>=clip->x+clip->w||yy>=clip->y+clip->h)continue;
    float sx=(xx+.5f-gx)/scale-.5f,sy=(yy+.5f-gy)/scale-.5f;
    int ix=(int)floorf(sx),iy=(int)floorf(sy);float fx=sx-ix,fy=sy-iy;
    float a=(app_glyph_sample(bitmap,&g,ix,iy)*(1-fx)+app_glyph_sample(bitmap,&g,ix+1,iy)*fx)*(1-fy)+(app_glyph_sample(bitmap,&g,ix,iy+1)*(1-fx)+app_glyph_sample(bitmap,&g,ix+1,iy+1)*fx)*fy;
    if(clip->weight)a=fminf(255,a*1.18f);
    if(a>0)blend_px(xx,yy,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,(int)a);
  }
#endif
  lv_font_glyph_release_draw_data(&g);
}
static void draw_app_label(const pet_surface_node_t *n,const char *text,uint16_t color){
#if PET_P4_GLYPH_ROWS
  char (*rows)[192]=g_app_label_rows;memset(rows,0,sizeof(g_app_label_rows));
#else
  char rows[8][192]={{0}};
#endif
  int count=0,px=n->font_size,lh=n->line_height;
  int icon_space=n->type==8&&n->icon>=0?n->icon_size+((text&&*text)?n->gap:0):0;
  int width=n->w-2*n->padding-icon_space;if(width<1)return;
  int max=n->type==8?1:n->h/lh;if(max<1)max=1;if(max>8)max=8;
  const char *cursor=text?text:"";
  do{
    int bytes=0,units=0;
    while(*cursor){const char *end=cursor;uint32_t cp=utf8_next(&end);int step=app_glyph_width(cp,utf8_peek(end));size_t len=end-cursor;
      if((units+step)*px>width*32||bytes+(int)len>=188)break;
      memcpy(rows[count]+bytes,cursor,len);bytes+=len;units+=step;cursor=end;
    }
    if(*cursor&&count==max-1){
      int dots=app_text_width("...",px);
      while(bytes>0&&app_text_width(rows[count],px)+dots>width){do{bytes--;}while(bytes>0&&((unsigned char)rows[count][bytes]&0xc0)==0x80);rows[count][bytes]=0;}
      if(width>=dots)strcat(rows[count],"...");
    }
    count++;
    if(!bytes)break;
  }while(*cursor&&count<max);
  int block=count*lh,y=n->y+(n->valign==1?(n->h-block)/2:n->valign==2?n->h-block:0);
  for(int row=0;row<count;row++){
    int tw=app_text_width(rows[row],px),total=tw+(row==0?icon_space:0);
    int x=n->x+n->padding+(n->align==1?(n->w-2*n->padding-total)/2:n->align==2?n->w-2*n->padding-total:0);
    if(row==0&&icon_space){draw_app_icon(n->icon,x,n->y+(n->h-n->icon_size)/2,n->icon_size,color);x+=icon_space;}
    int ink_top=0,ink_bottom=15;app_text_bounds(rows[row],&ink_top,&ink_bottom);
    float yy=y+row*lh+(lh-(ink_bottom-ink_top)*px/32.0f)/2-ink_top*px/32.0f;
    float xx=x;const char *p=rows[row];while(*p){uint32_t cp=utf8_next(&p),next=utf8_peek(p);draw_app_glyph(cp,next,xx,yy,px,color,n);xx+=app_glyph_width(cp,next)*px/32.0f;}
  }
}
/* Bounded game viewport: same collision grid and gameplay core, independent of
 * its artwork. Empty cells preserve the cached package background. */
static void draw_app_game(const pet_surface_t *s,const pet_p4_game_frame_t *g){
  if(!s->game_view||!g->width||!g->height)return;
  int cell=s->game_w/g->width;if(s->game_h/g->height<cell)cell=s->game_h/g->height;
  if(cell<8)return;
  int ox=s->game_x+(s->game_w-cell*g->width)/2,oy=s->game_y+(s->game_h-cell*g->height)/2;
  for(int y=0;y<g->height;y++)for(int x=0;x<g->width;x++){
    int tone=g->cells[y*g->width+x],px=ox+x*cell,py=oy+y*cell,cx=px+cell/2,cy=py+cell/2;
    if(!tone)continue;
    if(g->kind==PET_P4_GAME_SNAKE){
      if(tone==2){
        fill_ellipse(cx,cy+1,cell/3,cell/3,rgb565(221,99,74));
        fill_ellipse(cx+cell/6,py+cell/5,cell/6,cell/12,rgb565(73,118,76));
        fill_ellipse(cx-cell/8,cy-cell/9,cell/12,cell/12,rgb565(255,220,176));
      }else{
        fill_round_rect(px+1,py+2,cell-2,cell-3,cell/3,rgb565(67,123,86));
        fill_round_rect(px+3,py+3,cell-6,cell/3,cell/7,rgb565(118,166,112));
        if(tone==3){fill_ellipse(cx-cell/5,cy-cell/8,cell/9,cell/9,rgb565(255,249,226));fill_ellipse(cx+cell/5,cy-cell/8,cell/9,cell/9,rgb565(255,249,226));fill_ellipse(cx-cell/5,cy-cell/8,1,2,rgb565(35,60,44));fill_ellipse(cx+cell/5,cy-cell/8,1,2,rgb565(35,60,44));}
      }
    }else if(g->kind==PET_P4_GAME_FLAPPY){
      if(tone==3){
        fill_ellipse(cx,cy,cell/2-1,cell/2-2,rgb565(242,179,78));
        fill_ellipse(cx,cy+cell/5,cell/3,cell/5,rgb565(255,231,170));
        fill_ellipse(cx+cell/5,cy-cell/6,cell/7,cell/7,rgb565(255,254,240));
        fill_ellipse(cx+cell/4,cy-cell/6,1,2,rgb565(63,55,50));
        fill_round_rect(px+cell-6,cy,6,4,2,rgb565(214,115,66));
      }else if(tone==4){
        fill_ellipse(px+cell*3/4,cy+cell/8,cell/4,cell/5,rgb565(219,146,70));
      }else{
        fill_rect(px,py,cell,cell,rgb565(94,143,126));
        fill_rect(px+2,py,cell/5,cell,rgb565(148,185,150));
        if(tone==2){fill_round_rect(px,py,cell,cell,cell/6,rgb565(69,115,100));fill_rect(px+3,py+3,cell-6,3,rgb565(164,197,161));}
      }
    }else if(g->kind==PET_P4_GAME_GOMOKU){
      /* Board lines live in the package background; only stones, cursor,
       * last-move dot and the winning line are drawn here. */
      int stone=tone&PET_P4_GAME_GOMOKU_STONE_MASK,r=cell/2-2;if(r<3)r=3;
      if(tone&PET_P4_GAME_GOMOKU_WIN)fill_round_rect(px+1,py+1,cell-2,cell-2,cell/3,rgb565(236,186,66));
      if(stone){
        fill_ellipse(cx+1,cy+1,r,r,rgb565(70,48,24));
        fill_ellipse(cx,cy,r,r,stone==1?rgb565(28,28,34):rgb565(246,242,230));
        fill_ellipse(cx-r/3,cy-r/3,r/4+1,r/4+1,stone==1?rgb565(96,96,108):rgb565(255,255,255));
        if(tone&PET_P4_GAME_GOMOKU_LAST)fill_ellipse(cx,cy,r/4+1,r/4+1,rgb565(224,72,56));
      }
      if(tone&PET_P4_GAME_GOMOKU_CURSOR){
        uint16_t edge=rgb565(230,96,48);
        fill_rect(px,py,cell,2,edge);fill_rect(px,py+cell-2,cell,2,edge);
        fill_rect(px,py,2,cell,edge);fill_rect(px+cell-2,py,2,cell,edge);
      }
    }else{
      const uint16_t colors[]={0,rgb565(131,122,211),rgb565(98,184,204),rgb565(224,157,117),rgb565(173,206,144)};
      fill_round_rect(px+1,py+1,cell-2,cell-2,cell/5,colors[tone<=4?tone:1]);
      blend_rect(px+3,py+3,cell-6,2,255,255,255,100);
      blend_rect(px+3,py+cell-4,cell-6,2,0,0,0,45);
    }
  }
}
static void app_data_label(const char *text,int x,int y,int w,int h,int size,uint16_t color){
  pet_surface_node_t n={.x=x,.y=y,.w=w,.h=h,.font_size=size,.line_height=(size*5+3)/4,.icon=-1};
  draw_app_label(&n,text,color);
}
/* Clip scrolling platform primitives, rather than dropping a whole platform
 * when only one edge has crossed the viewport. No additional framebuffer. */
static void draw_scene_capsule(int x,int y,int w,int h,uint16_t color,const pet_surface_scene_view_t *v){
  x+=2;y+=2;w-=4;h-=4;if(w<1||h<1)return;
  int r=(w<h?w:h)/2;
  for(int yy=0;yy<h;yy++){
    if(y+yy<v->y||y+yy>=v->y+v->h)continue;
    for(int xx=0;xx<w;xx++){
      if(x+xx<v->x||x+xx>=v->x+v->w)continue;
      int dx=xx<r?r-xx:xx>=w-r?xx-(w-r-1):0;
      int dy=yy<r?r-yy:yy>=h-r?yy-(h-r-1):0;
      if(dx*dx+dy*dy<=r*r)put_px(x+xx,y+yy,color);
    }
  }
}
/* B bodies-only rotation; existing image/node paths and caches are unchanged. */
#if PET_P4_BODIES
#include "pet_p4_bodies_raster.h"
static void draw_app_body(const pet_p4_game_entity_t *e,const pet_surface_scene_sprite_t *sprite,
  int x,int y,int cell,const pet_surface_scene_view_t *v,const miniapp_pixel_palette_t *palette,unsigned long long now){
  int w=e->width*cell,h=e->height*cell,cx=x+w/2,cy=y+h/2,reach=(w+h+1)/2;
  int left=cx-reach,right=cx+reach,top=cy-reach,bottom=cy+reach;
  if(left<v->x)left=v->x;if(top<v->y)top=v->y;if(right>v->x+v->w)right=v->x+v->w;if(bottom>v->y+v->h)bottom=v->y+v->h;
  const pet_p4_miniapp_sprite_t *im=NULL;int iw=0,frame=0;
  if(sprite){
    if(sprite->image>=g_surface_images.count||!sprite->columns)return;
    im=&g_surface_images.items[sprite->image];if(im->fps||im->frame_width%sprite->columns)return;
    iw=im->frame_width/sprite->columns;frame=(now*sprite->fps/1000)%sprite->columns;
  }
  for(int py=top;py<bottom;py++)for(int px=left;px<right;px++){
    int sx,sy;if(!body_source_pixel(px,py,cx,cy,w,h,e->angle_q8/256,&sx,&sy))continue;
    uint16_t color=miniapp_game_tone_color(e->tone,palette);unsigned alpha=255;
    if(im){
      unsigned at=g_surface_offsets[sprite->image]+(unsigned)(sy*im->frame_height/h)*im->frame_width+frame*iw+(unsigned)(sx*iw/w);
      unsigned index=g_surface_indices[at];color=g_surface_colors[sprite->image][index];alpha=g_surface_alpha[sprite->image][index];
    }else{
      if(sx<2||sy<2||sx>=w-2||sy>=h-2)continue;
      int dx=2*sx-w,dy=2*sy-h;
      if(e->shape==PET_P4_GAME_SHAPE_CIRCLE||e->shape==PET_P4_GAME_SHAPE_BALL||e->shape==PET_P4_GAME_SHAPE_COIN){
        if((int64_t)dx*dx*h*h+(int64_t)dy*dy*w*w>(int64_t)w*w*h*h)continue;
      }else if(e->shape==PET_P4_GAME_SHAPE_TRIANGLE){if((int64_t)(dx<0?-dx:dx)*h>(int64_t)w*sy)continue;}
      else if(e->shape==PET_P4_GAME_SHAPE_DIAMOND){if((int64_t)(dx<0?-dx:dx)*h+(int64_t)(dy<0?-dy:dy)*w>(int64_t)w*h)continue;}
      else if(e->shape==PET_P4_GAME_SHAPE_CAPSULE){
        int radius=w<h?w/2:h/2,qx=sx<radius?radius-sx:sx>=w-radius?sx-(w-radius-1):0,qy=sy<radius?radius-sy:sy>=h-radius?sy-(h-radius-1):0;
        if(qx*qx+qy*qy>radius*radius)continue;
      }
    }
    if(alpha==255)put_px(px,py,color);
    else if(alpha)blend_px(px,py,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,alpha);
  }
}
#endif
static void draw_app_scene(const pet_surface_t *s,const pet_p4_game_frame_t *g,unsigned long long now){
  const pet_surface_scene_view_t *v=&s->scene_view;
  if(!v->enabled||g->kind!=PET_P4_GAME_BOUNDED||!g->width||!g->height)return;
  int cell=v->w/g->width;if(v->h/g->height<cell)cell=v->h/g->height;
  if(cell<1)return;
  int ox=v->x+(v->w-cell*g->width)/2,oy=v->y+(v->h-cell*g->height)/2;
  /* Preserve the caller's clip and intersect it with the scene. A sprite
   * crossing one edge must keep its visible pixels, rather than disappear. */
  const bool previous_clip_active=g_app_clip.active;
  const int previous_clip[4]={g_app_clip.x,g_app_clip.y,g_app_clip.right,g_app_clip.bottom};
  if(!g_app_clip.active){g_app_clip.x=v->x;g_app_clip.y=v->y;g_app_clip.right=v->x+v->w;g_app_clip.bottom=v->y+v->h;}
  else{
    if(g_app_clip.x<v->x)g_app_clip.x=v->x;if(g_app_clip.y<v->y)g_app_clip.y=v->y;
    if(g_app_clip.right>v->x+v->w)g_app_clip.right=v->x+v->w;if(g_app_clip.bottom>v->y+v->h)g_app_clip.bottom=v->y+v->h;
  }
  g_app_clip.active=true;
  miniapp_pixel_palette_t palette={.background=s->background,.surface=s->background,.ink=v->colors[0],.primary=v->colors[1],.secondary=v->colors[2],.light=v->colors[3],.shadow=v->colors[4]};
  // Grid cells include entity occupancy, so only the entity renderer owns moving
  // shapes here. The scene's collision geometry remains unchanged.
  for(int i=0;i<g->entity_count;i++){
    const pet_p4_game_entity_t *e=&g->entities[i];if(!e->active)continue;
    const pet_surface_scene_sprite_t *sprite=NULL;
    for(int j=0;j<v->count;j++)if(!strcmp(v->sprites[j].entity,e->id))sprite=&v->sprites[j];
    int px=ox+((g->motion||g->bodies)?e->x_q8*cell/256:e->x*cell),py=oy+((g->motion||g->bodies)?e->y_q8*cell/256:e->y*cell);
#if PET_P4_BODIES
    if(g->bodies&&e->body&&e->rolling){draw_app_body(e,sprite,px,py,cell,v,&palette,now);continue;}
#endif
    if(g->motion&&!sprite&&e->shape==PET_P4_GAME_SHAPE_CAPSULE){
      draw_scene_capsule(px,py,e->width*cell,e->height*cell,miniapp_game_tone_color(e->tone,&palette),v);continue;
    }
    if(px>=v->x+v->w||py>=v->y+v->h||px+e->width*cell<=v->x||py+e->height*cell<=v->y)continue;
    if(sprite){
      pet_surface_node_t n={.x=px,.y=py,.w=e->width*cell,.h=e->height*cell,.image=sprite->image,.tile_cols=sprite->columns,.tile_rows=1,.tile_index=(now*sprite->fps/1000)%sprite->columns};
#if PET_P4_BIG_IMAGE
      /* This synthetic palette sprite bypasses the parser's -1 sentinel. */
      n.big=-1;
#endif
      draw_surface_image(&n);
    }else {pet_p4_game_entity_t placed=*e;placed.x=placed.y=0;draw_miniapp_game_entity(&placed,NULL,px,py,cell,true,&palette);}
  }
  g_app_clip.active=previous_clip_active;g_app_clip.x=previous_clip[0];g_app_clip.y=previous_clip[1];
  g_app_clip.right=previous_clip[2];g_app_clip.bottom=previous_clip[3];
}
static void draw_app_data(const pet_surface_t *s,const pet_p4_miniapp_view_t *app){
  const pet_surface_data_view_t *v=&s->data_view;if(!v->enabled)return;
  const pet_p4_data_view_t *d=&app->data;
  const uint16_t ink=v->colors[0],muted=v->colors[1],accent=v->colors[2],done=v->colors[3],card=v->colors[4];
  char page[32];snprintf(page,sizeof(page),"%u / %u",d->page+1,d->pages?d->pages:1);
  app_data_label(d->date[0]?(v->todos&&strlen(d->date)==10?d->date+5:d->date):"等待连接",v->x,v->y,v->w-110,24,14,muted);
  app_data_label(page,v->x+v->w-85,v->y,85,24,14,muted);
  const int row_h=(v->h-58)/5;
  for(int i=0;i<d->count&&i<5;i++){
    const pet_p4_data_row_t *r=&d->rows[i];int y=v->y+28+i*row_h;
    bool quotes=!strcmp(app->data_source,"stocks.watchlist");
    uint16_t tone=d->stale?muted:v->todos?(r->tone<0?done:accent):quotes?(r->tone>0?rgb565(255,100,102):r->tone<0?rgb565(79,212,149):ink):ink;
    fill_round_rect(v->x,y,v->w,row_h-5,10,card);
    if(v->todos){
      char title[sizeof(r->label)+sizeof(r->meta)];snprintf(title,sizeof(title),"%s%s",r->label,r->meta);
      fill_round_rect(v->x+10,y+10,3,row_h-25,1,tone);
      app_data_label(title,v->x+24,y+4,v->w-38,27,20,tone);
      app_data_label(r->value,v->x+24,y+30,v->w-146,21,13,muted);
      app_data_label(r->detail,v->x+v->w-112,y+30,100,21,13,tone);
    }else{
      app_data_label(r->label,v->x+14,y+5,v->w/2-22,23,16,muted);
      app_data_label(r->value,v->x+v->w/2,y+3,v->w/2-14,28,22,tone);
      app_data_label(r->meta,v->x+14,y+30,v->w/2-22,21,12,muted);
      app_data_label(r->detail,v->x+v->w/2,y+32,v->w/2-14,19,12,quotes?tone:muted);
    }
  }
  if(!d->count)app_data_label(d->received?"暂无内容":"等待 PC 数据",v->x+18,v->y+110,v->w-36,40,24,ink);
  const char *status=!d->received?"连接 Pet Manager 后显示":d->stale?"数据已过期，请检查连接":d->message;
  app_data_label(status,v->x,v->y+v->h-24,v->w,24,13,muted);
}
typedef struct {int image;const pet_side_art_t*art;} side_draw_context_t;
static void draw_side_cell(void*opaque,int x,int y,int w,int h,unsigned frame,bool flip){
  const side_draw_context_t*c=opaque;const pet_side_art_t*a=c->art;
  if(c->image<0||c->image>=g_surface_images.count||frame>=a->columns*a->rows||w<=0||h<=0||w>96||h>96)return;
  const pet_p4_miniapp_sprite_t*im=&g_surface_images.items[c->image];
  if(im->frame_width!=a->columns*16||im->frame_height!=a->rows*16)return;
  const uint8_t*indices=g_surface_indices+g_surface_offsets[c->image];
  const uint8_t*palette=g_surface_images.pixels+im->data_offset+12;
  int ox=(frame%a->columns)*16,oy=(frame/a->columns)*16;
  int left=x>a->x?x:a->x,top=y>a->y?y:a->y,right=x+w<a->x+a->w?x+w:a->x+a->w,bottom=y+h<a->y+a->h?y+h:a->y+a->h;
  uint8_t sample_x[96];for(int px=left;px<right;px++){int sx=(px-x)*16/w;sample_x[px-left]=(uint8_t)(flip?15-sx:sx);}
  for(int py=top;py<bottom;py++){
    int sy=oy+(py-y)*16/h;uint16_t*out=g_framebuffer+py*640;
    for(int px=left;px<right;px++){
      int sx=sample_x[px-left];
      unsigned index=indices[sy*im->frame_width+ox+sx],alpha=g_surface_alpha[c->image][index];if(!alpha)continue;
      if(alpha==255){out[px]=g_surface_colors[c->image][index];continue;}
      const uint8_t*p=palette+4*index;uint16_t bg=out[px];
      unsigned r=((bg>>11)&31)*255/31,g=((bg>>5)&63)*255/63,b=(bg&31)*255/31;
      out[px]=rgb565((p[0]*alpha+r*(255-alpha))/255,(p[1]*alpha+g*(255-alpha))/255,(p[2]*alpha+b*(255-alpha))/255);
    }
  }
}
static void draw_app_side(void){
  if(!g_miniapp_render_snapshot.side_active)return;
  const pet_side_scene_t *scene=&g_miniapp_render_snapshot.scene;
  const pet_side_art_t *art=&g_miniapp_render_snapshot.art;
  int image=-1;for(int i=0;i<g_surface_images.count;i++)if(!strcmp(g_surface_images.items[i].id,art->image))image=i;
  if(image<0)return;
  fill_rect(art->x,art->y,art->w,art->h,art->sky);
  side_draw_context_t context={.image=image,.art=art};pet_side_draw(scene,art,draw_side_cell,&context);
}
#include "pet_p4_app_cache.h"
#if PET_CANVAS_ENABLED
static void canvas_rect(void *context,int x,int y,int w,int h,uint16_t color){
  (void)context;fill_rect(x,y,w,h,color);
}
static void draw_app_canvas(const pet_surface_t *surface,unsigned page){
  if(surface->canvas.count&&surface->canvas_bindings_ready&&(surface->canvas.page==4||surface->canvas.page==page))pet_canvas_draw(&surface->canvas,canvas_rect,NULL);
}
#else
#define draw_app_canvas(surface,page) ((void)(surface),(void)(page))
#endif
static EXT_RAM_BSS_ATTR pet_app_frame_key_t g_app_frame_key;
#if PET_P4_APP_DIRTY_CACHE
#include "pet_p4_app_dirty.h"
/* Prefix bytes belong to an exact captured image/resource revision even when
 * both packs contain zero images. A visually unchanged empty image pack alone
 * must not authorize another dynamic application's old static prefix. */
static uint32_t g_app_background_revision;
#if PET_P4_APP_CACHE_PROGRESSIVE
#include "pet_p4_app_cache_progressive.h"
static pet_app_cache_progressive_t g_app_cache_progressive;
#endif
static void app_dirty_clip(const pet_app_dirty_rect_t *r){
  if(!g_app_clip.active){g_app_clip.active=true;g_app_clip.x=r->x;g_app_clip.y=r->y;g_app_clip.right=r->right;g_app_clip.bottom=r->bottom;}
  else{
    if(g_app_clip.x<r->x)g_app_clip.x=r->x;if(g_app_clip.y<r->y)g_app_clip.y=r->y;
    if(g_app_clip.right>r->right)g_app_clip.right=r->right;if(g_app_clip.bottom>r->bottom)g_app_clip.bottom=r->bottom;
  }
}
#if PET_P4_APP_COLD_FAST
/* True when the first node drawn on this page is a static, unrotated, untiled,
 * full-screen palette image whose palette is entirely opaque: it overwrites every
 * framebuffer pixel, so the clear it would cover is pure waste. */
static bool cold_fast_opaque_first_image(const pet_surface_t *s,uint8_t page){
  for(unsigned i=0;i<s->count;i++){
    const pet_surface_node_t *n=&s->nodes[i];
    if(n->hidden||!pet_surface_node_on_page(n,page))continue;
    if(n->type!=6||n->in_view||n->floating||n->x||n->y||n->w!=640||n->h!=480||n->rotate_ms||n->tile_cols)return false;
    if(n->image<0||n->image>=g_surface_images.count)return false;
    const pet_p4_miniapp_sprite_t *im=&g_surface_images.items[n->image];
    if(im->fps!=0)return false;
    if(pet_image_jpeg(g_surface_images.pixels+im->data_offset,im->data_length)){g_cold_fast_jpeg_pending=true;return true;}
    return g_surface_opaque[n->image];
  }
  return false;
}
#endif
static void app_clear_uncovered(const pet_surface_t *surface,uint8_t page,bool cache_frame){
  pet_app_dirty_rect_t r;
#if PET_P4_APP_CLEAR_OPAQUE_CENTER
  const bool center=cache_frame&&pet_app_clear_center_supported(surface);
#else
  const bool center=cache_frame&&pet_app_dirty_supported(surface);
#endif
  const bool skipped=center&&pet_app_opaque_center(surface,page,&r);
#if PET_P4_APP_CLEAR_OPAQUE_CENTER
  pet_render_counter_increment(skipped?&g_clear_center_frames:&g_clear_full_frames);
#endif
  if(skipped){
    fill_rect(0,0,640,r.y,surface->background);
    fill_rect(0,r.bottom,640,480-r.bottom,surface->background);
    fill_rect(0,r.y,r.x,r.bottom-r.y,surface->background);
    fill_rect(r.right,r.y,640-r.right,r.bottom-r.y,surface->background);
  }else fill_rect(0,0,640,480,surface->background);
}
#endif
#if PET_P4_KEYFRAME_BG
static void keyframe_invalidate_frame_cache(void){
  g_surface_background_valid=false;g_app_frame_key.valid=false;
#if PET_P4_APP_CACHE_PROGRESSIVE
  /* The logical surface key does not encode which JPEG keyframe is shown.
   * Old populated rows/retained pixels cannot be continued under a new frame. */
  pet_app_cache_progressive_cancel(&g_app_cache_progressive);
#endif
}
#endif
#if PET_P4_APP_PPA_COPY
/* 1:1 PPA SRM copy of one 640x480 RGB565 frame: identical bytes, DMA instead of
 * the CPU. Rows 0/159/320/479 of the first 8 copies are compared with the source;
 * any mismatch or PPA error disables the path for this boot (CPU copy). */
static void app_frame_copy_ppa(void *to,const void *from,size_t bytes){
  uint16_t *dst=(uint16_t*)to;const uint16_t *src=(const uint16_t*)from;
  if(bytes==640*480*sizeof(uint16_t)&&!g_ppa_copy_disabled&&g_ppa_srm_client&&g_framebuffer_ppa_compatible&&(((uintptr_t)dst|(uintptr_t)src)&63u)==0){
    ppa_srm_oper_config_t copy={
      .in={.buffer=src,.pic_w=640,.pic_h=480,.block_w=640,.block_h=480,.block_offset_x=0,.block_offset_y=0,.srm_cm=PPA_SRM_COLOR_MODE_RGB565},
      .out={.buffer=dst,.buffer_size=640*480*sizeof(uint16_t),.pic_w=640,.pic_h=480,.block_offset_x=0,.block_offset_y=0,.srm_cm=PPA_SRM_COLOR_MODE_RGB565},
      .rotation_angle=PPA_SRM_ROTATION_ANGLE_0,.scale_x=1.0f,.scale_y=1.0f,.rgb_swap=false,.byte_swap=false,.mode=PPA_TRANS_MODE_BLOCKING};
    if(ppa_trace_srm(PET_PPA_TRACE_SCALE,&copy)==ESP_OK){
      pet_render_counter_increment(&g_ppa_copies);
      if(g_ppa_copies<=8){
        static const int rows[4]={0,159,320,479};
        for(int i=0;i<4;i++)if(memcmp(dst+rows[i]*640,src+rows[i]*640,640*sizeof(uint16_t))){
          pet_render_counter_increment(&g_ppa_copy_mismatches);g_ppa_copy_disabled=true;pet_app_frame_copy(dst,src,bytes);return;
        }
      }
      return;
    }
    pet_render_counter_increment(&g_ppa_copy_fallbacks);g_ppa_copy_disabled=true;
  }
  pet_app_frame_copy(dst,src,bytes);
}
/* From here on (the APP page renderer below) every clean-frame copy routes through the PPA helper. */
#define pet_app_frame_copy(to,from,bytes) app_frame_copy_ppa((to),(from),(bytes))
#endif
#if PET_P4_CELLS_V1
/* p4-cells-v1 node: one whole-pixel tile per cell. With an atlas the frame index is the
 * cell value (0 draws nothing); without one, 1 = disc in color, 2 = disc in foreground,
 * >=3 = square in stroke. The cursor is a 2 px outline around (cursor_col, cursor_row).
 * Bounded by 256 cells; no heap, same primitives as the other app nodes. */
static void draw_app_cells(const pet_surface_node_t *n,const pet_cells_t *grid){
  if(!grid||!grid->enabled||!grid->cols||!grid->rows)return;
  int tw=n->w/grid->cols,th=n->h/grid->rows;if(tw<1||th<1)return;
  int inset=tw<8||th<8?1:2;
  for(unsigned y=0;y<grid->rows;y++)for(unsigned x=0;x<grid->cols;x++){
    int v=grid->values[y*grid->cols+x];if(!v)continue;
    int px=n->x+(int)x*tw,py=n->y+(int)y*th;
    if(n->image>=0){
      if(v>=n->tile_cols)continue;
      pet_surface_node_t tile=*n;tile.x=px;tile.y=py;tile.w=tw;tile.h=th;tile.tile_cols=n->tile_cols;tile.tile_rows=1;tile.tile_row=0;tile.tile_index=(uint8_t)v;tile.floating=false;tile.rotate_ms=0;
      draw_surface_image(&tile);
    }else{
      int w=tw-2*inset,h=th-2*inset;if(w<1||h<1)continue;
      if(v==1)fill_round_rect(px+inset,py+inset,w,h,(w<h?w:h)/2,n->color);
      else if(v==2)fill_round_rect(px+inset,py+inset,w,h,(w<h?w:h)/2,n->foreground);
      else fill_rect(px+inset,py+inset,w,h,n->stroke);
    }
  }
  if(n->cursor_col>=0&&n->cursor_row>=0&&n->cursor_col<grid->cols&&n->cursor_row<grid->rows){
    int px=n->x+n->cursor_col*tw,py=n->y+n->cursor_row*th;
    fill_rect(px,py,tw,2,n->cursor_color);fill_rect(px,py+th-2,tw,2,n->cursor_color);
    fill_rect(px,py,2,th,n->cursor_color);fill_rect(px+tw-2,py,2,th,n->cursor_color);
  }
}
#endif
static bool render_package_app(unsigned long long now_ms,const pet_p4_miniapp_view_t *app) {
  if(!g_miniapp_render_snapshot.surface_active)return false;
#if PET_P4_APP_CACHE_POPULATION_BUDGET && !PET_P4_APP_CACHE_PROGRESSIVE
  int64_t cache_population_started=esp_timer_get_time();
#endif
  const pet_surface_t *surface=&g_miniapp_render_snapshot.surface;
  {APP_NODE_SCOPE(images_scope,images_us);load_surface_images(now_ms);}
  g_app_text_surface=surface;
#if PET_P4_KEYFRAME_BG
  keyframe_advance(surface,now_ms);
#endif
  const char *values[]={app->title,app->eyebrow,app->headline,app->metric_label,app->metric_value,app->metric_unit,app->badge,app->note,app->footer};
  /* Only the resolved package layer is cached. These runtime layers can change
   * without changing surface nodes/dashboard values, so they must stay live. */
  bool cache_frame,cache_match;
  /* Media progress lives outside the resolved surface/dashboard key: a big
   * image can still be filling tiles, and a clip can present another frame.
   * Until that progress is part of an exact ready key, neither whole/final
   * frames nor a static prefix may suppress their per-frame renderer work. */
  bool dynamic_media=false;
  {APP_NODE_SCOPE(key_scope,key_us);
#if PET_P4_BIG_IMAGE || PET_P4_CLIP
  for(unsigned i=0;i<surface->count;i++){
    const pet_surface_node_t *n=&surface->nodes[i];
#if PET_P4_BIG_IMAGE
    if(n->type==6&&n->big>=0){dynamic_media=true;break;}
#endif
#if PET_P4_CLIP
    if(n->type==PET_SURFACE_NODE_CLIP){dynamic_media=true;break;}
#endif
  }
#endif
  cache_frame=!dynamic_media&&pet_app_frame_eligible_at(surface,app->surface_page)&&!app->slicing&&!app->media&&!app->instrument
    &&app->game.kind==PET_P4_GAME_NONE&&!app->data.enabled&&!app->data_source[0]
    &&!g_miniapp_render_snapshot.side_active;
#if PET_P4_APP_CACHE_PROGRESSIVE
  if(!cache_frame&&g_app_cache_progressive.active){
    pet_app_cache_progressive_cancel(&g_app_cache_progressive);
    g_surface_background_valid=false;
  }
#endif
  cache_match=cache_frame&&g_surface_background_valid&&pet_app_frame_matches(&g_app_frame_key,surface,values,app->surface_page,app->progress_percent);
#if PET_P4_LOGICAL_COMPARE
  /* Hint ONLY. The final complete logical/native pixel comparison remains
   * mandatory, including all live overlays. A staged key is never a texture. */
  g_app_logical_same=cache_frame&&((g_surface_background_valid&&g_app_frame_key.valid)
      ||(g_app_cache_progressive.active&&g_app_cache_progressive.revision==g_surface_revision))
    &&pet_app_frame_content_matches(&g_app_frame_key,surface,values,app->surface_page,app->progress_percent);
#endif
  }
#if PET_P4_APP_RETAIN_LOGICAL
  if(g_app_may_retain&&g_app_logical_same&&!g_miniapp_render_snapshot.confirmation_active){
    /* This is the PREVIOUS complete final logical frame, not the partially
     * populated clean texture. Exact base key AND immutable common plan match.
     * Never copy a banner/modal/touch marker into the clean texture. */
    g_app_final_reused=true;g_render_trace.logical_retained=true;
    if(g_app_cache_progressive.active&&pet_logical_common_empty(g_app_common_plan)){
      APP_NODE_SCOPE(remember_scope,remember_us);
      pet_app_cache_progressive_copy(&g_app_cache_progressive,&g_app_frame_key,g_surface_background,g_framebuffer);
      g_surface_background_valid=g_app_frame_key.valid;
    }
#if PET_P4_APP_NODE_TRACE
    g_render_trace.app_nodes.cache_deferred=g_app_cache_progressive.active;
#endif
    goto app_overlays;
  }
#endif
  if(cache_match){
    APP_NODE_HIT();
    {APP_NODE_SCOPE(copy_scope,cache_copy_us);pet_app_frame_copy(g_framebuffer,g_surface_background,sizeof(g_surface_background));}goto app_overlays;
  }
#if PET_P4_APP_DIRTY_CACHE
  pet_app_dirty_rect_t dirty={0};
  bool partial=cache_frame&&g_surface_background_valid
    &&pet_app_dirty_plan(&g_app_frame_key,surface,values,app->surface_page,app->progress_percent,&dirty);
  if(!partial&&g_app_frame_key.valid){g_surface_background_valid=false;g_app_frame_key.valid=false;}
#else
  if(g_app_frame_key.valid){g_surface_background_valid=false;g_app_frame_key.valid=false;}
#endif
  // Cache the static prefix, including tiled artwork and its translucent shade.
  // Theme overlays are cached per page too; do not blend a full-screen wash on
  // every frame. Actionable/dynamic nodes always remain live.
  int prefix=0;
  /* Draw order is independent of whether media may be cached. Keeping the
   * real static prefix places scene/canvas layers above its background. */
  while(prefix<surface->count){const pet_surface_node_t *n=&surface->nodes[prefix];if(
#if PET_P4_BIG_IMAGE
    (n->type==6&&n->big>=0)||
#endif
#if PET_SERVICE_BRIDGE_ENABLED
    n->image_bind[0]||
#endif
    n->in_view||n->floating||n->action[0]||n->tile_cols||n->show_var[0]||n->state_mask||n->host_mask||(n->type!=6&&n->type!=0))break;prefix++;}
  int first=0;
#if PET_P4_APP_DIRTY_CACHE
  if(partial){
    {APP_NODE_SCOPE(copy_scope,cache_copy_us);pet_app_frame_copy(g_framebuffer,g_surface_background,sizeof(g_surface_background));}
    APP_NODE_SCOPE(clear_scope,clear_us);g_app_clip.active=false;app_dirty_clip(&dirty);
    fill_rect(dirty.x,dirty.y,dirty.right-dirty.x,dirty.bottom-dirty.y,surface->background);
  }else if(!dynamic_media&&!cache_frame&&prefix&&g_surface_background_valid&&g_surface_background_page==app->surface_page
      &&g_app_background_revision==g_surface_revision){APP_NODE_SCOPE(prefix_scope,prefix_us);pet_app_frame_copy(g_framebuffer,g_surface_background,sizeof(g_surface_background));first=prefix;}
  else {APP_NODE_SCOPE(clear_scope,clear_us);g_app_clip.active=false;
#if PET_P4_APP_COLD_FAST
    g_cold_fast_jpeg_pending=false;
    if(cold_fast_opaque_first_image(surface,app->surface_page))pet_render_counter_increment(&g_cold_fast_clear_skips);else
#endif
    app_clear_uncovered(surface,app->surface_page,cache_frame);}
#else
  if(!dynamic_media&&prefix&&g_surface_background_valid&&g_surface_background_page==app->surface_page){APP_NODE_SCOPE(prefix_scope,prefix_us);pet_app_frame_copy(g_framebuffer,g_surface_background,sizeof(g_surface_background));first=prefix;}
  else {APP_NODE_SCOPE(clear_scope,clear_us);fill_rect(0,0,640,480,surface->background);}
#endif
  for(int i=first;i<surface->count;i++){
    g_app_clip.active=false;
    if(!dynamic_media&&i==prefix&&prefix&&first==0
#if PET_P4_APP_DIRTY_CACHE
        &&!cache_frame
#endif
      ){APP_NODE_SCOPE(prefix_scope,prefix_us);pet_app_frame_copy(g_surface_background,g_framebuffer,sizeof(g_surface_background));g_surface_background_valid=true;g_surface_background_page=app->surface_page;
#if PET_P4_APP_DIRTY_CACHE
        g_app_background_revision=g_surface_revision;
#endif
      }
    if(i==prefix){draw_app_game(surface,&app->game);draw_app_scene(surface,&app->game,now_ms);draw_app_data(surface,app);draw_app_side();draw_app_canvas(surface,app->surface_page);}
    pet_surface_node_t projected;
    const pet_surface_viewport_t *viewport=pet_surface_node_viewport(surface,&surface->nodes[i]);
    if(!pet_surface_project(viewport,&surface->nodes[i],viewport?viewport->offset:0,&projected))continue;
    const pet_surface_node_t *n=&projected;
    if(n->hidden||!pet_surface_node_on_page(n,app->surface_page))continue;
    if(viewport){g_app_clip.active=true;g_app_clip.x=viewport->x;g_app_clip.y=viewport->y;g_app_clip.right=viewport->x+viewport->w;g_app_clip.bottom=viewport->y+viewport->h;}
#if PET_P4_APP_COLD_FAST
    if(g_cold_fast_jpeg_pending&&n->type!=6){g_cold_fast_jpeg_pending=false;g_app_clip.active=false;fill_rect(0,0,640,480,surface->background);if(viewport){g_app_clip.active=true;}}
#endif
#if PET_P4_APP_DIRTY_CACHE
    if(partial){
      if(pet_app_dirty_empty(&dirty)||n->x>=dirty.right||n->y>=dirty.bottom||n->x+n->w<=dirty.x||n->y+n->h<=dirty.y)continue;
      app_dirty_clip(&dirty);
      if(g_app_clip.x>=g_app_clip.right||g_app_clip.y>=g_app_clip.bottom)continue;
    }
#endif
    APP_NODE_MARK(i);APP_NODE_SCOPE(node_scope,nodes_us[i]);
    if(n->type==6){
        pet_surface_node_t image=*n;
        if(n->floating)image.y=n->y+(int)lroundf(sinf((now_ms%6000)*6.2831853f/6000)*3);
#if PET_SERVICE_BRIDGE_ENABLED
        if(!draw_service_image(&image,app->widget_id,now_ms))
#endif
        draw_surface_image(&image);
    }
#if PET_P4_CLIP
    else if(n->type==PET_SURFACE_NODE_CLIP)renderer_ext_draw_clip(n,now_ms);
#endif
    else if(n->type==0)draw_app_panel(n);
    else if(n->type==7)draw_app_icon(n->icon,n->x,n->y,n->w<n->h?n->w:n->h,n->color);
#if PET_P4_CELLS_V1
    else if(n->type==PET_SURFACE_NODE_CELLS)draw_app_cells(n,&g_miniapp_render_snapshot.cells);
#endif
    else if(n->type==1||n->type==8){
      char progress[16];snprintf(progress,sizeof(progress),"%d%%",app->progress_percent<0?0:app->progress_percent);
      const char *text=n->bind<9?values[n->bind]:n->bind==9?progress:n->text;
      if(surface->graphics){
        if(n->type==8)draw_app_panel(n);
        draw_app_label(n,text,n->type==8?n->foreground:n->color);
        if(n->disabled)blend_round_rect(n->x,n->y,n->w,n->h,n->radius,128,128,128,110);
        else if(n->pressed)blend_round_rect(n->x,n->y,n->w,n->h,n->radius,0,0,0,31);
        continue;
      }
      // Fixed line height and clipping area; no wrap outside the node.
      int y=n->y;
      while(text&&*text&&y+32*n->scale<=n->y+n->h){
        bool last=y+64*n->scale>n->y+n->h;
        const char *next=draw_text_line(text,n->x,y,n->w,n->color,n->scale,last);
        if(last||!next||next==text)break;text=next;y+=32*n->scale;
      }
    }else if(n->type==2){int value=n->var_bind[0]?n->bound_value:app->progress_percent;if(value<0)value=0;if(value>100)value=100;fill_rect(n->x,n->y,n->w*value/100,n->h,n->color);}
  }
  g_app_clip.active=false;
  if(prefix==surface->count){draw_app_game(surface,&app->game);draw_app_scene(surface,&app->game,now_ms);draw_app_data(surface,app);draw_app_side();draw_app_canvas(surface,app->surface_page);}
  bool populate_cache=cache_frame;
#if PET_P4_APP_CACHE_POPULATION_BUDGET && !PET_P4_APP_CACHE_PROGRESSIVE
  populate_cache=populate_cache&&pet_app_cache_population_admitted(cache_population_started,esp_timer_get_time());
#endif
#if PET_P4_APP_NODE_TRACE
  g_render_trace.app_nodes.cache_deferred=cache_frame&&!populate_cache;
#endif
  if(populate_cache){
    APP_NODE_SCOPE(remember_scope,remember_us);
#if PET_P4_APP_CACHE_PROGRESSIVE
    if(partial&&dirty.bottom-dirty.y<=PET_APP_CACHE_ROW_QUANTUM){
      pet_app_dirty_copy(g_surface_background,g_framebuffer,&dirty);
      pet_app_frame_remember(&g_app_frame_key,surface,values,app->surface_page,app->progress_percent);
      pet_app_cache_progressive_cancel(&g_app_cache_progressive);
    }else{
      pet_app_cache_progressive_prepare(&g_app_cache_progressive,&g_app_frame_key,surface,values,
        app->surface_page,app->progress_percent,g_surface_revision,partial);
      g_surface_background_valid=false;
      pet_app_cache_progressive_copy(&g_app_cache_progressive,&g_app_frame_key,g_surface_background,g_framebuffer);
    }
    g_surface_background_valid=g_app_frame_key.valid;
    g_surface_background_page=app->surface_page;g_app_background_revision=g_surface_revision;
#if PET_P4_APP_NODE_TRACE
    g_render_trace.app_nodes.cache_deferred=g_app_cache_progressive.active;
#endif
#else
#if PET_P4_APP_DIRTY_CACHE
    if(partial)pet_app_dirty_copy(g_surface_background,g_framebuffer,&dirty);
    else
#endif
    pet_app_frame_copy(g_surface_background,g_framebuffer,sizeof(g_surface_background));g_surface_background_valid=true;g_surface_background_page=app->surface_page;
#if PET_P4_APP_DIRTY_CACHE
    g_app_background_revision=g_surface_revision;
#endif
    pet_app_frame_remember(&g_app_frame_key,surface,values,app->surface_page,app->progress_percent);
#endif
  }
app_overlays:;
#if PET_P4_PARTICLES
  /* Decorative dots live above the cached package layer and never enter it. */
  draw_app_particles(surface,app->widget_id,g_surface_revision,app->surface_page,now_ms);
#endif
#if PET_P4_APP_RETAIN_LOGICAL
  g_app_final_eligible=cache_frame&&!g_miniapp_render_snapshot.confirmation_active&&!PET_PARTICLES_ACTIVE(surface);
#endif
  const char *prompt=g_miniapp_render_snapshot.confirmation_prompt;
  if(g_miniapp_render_snapshot.confirmation_active){
    blend_rect(0,0,640,480,0,0,0,115);
    fill_round_rect(108,156,424,202,24,rgb565(255,250,242));
    pet_surface_node_t label={.x=136,.y=180,.w=368,.h=76,.font_size=20,.line_height=28,.align=1,.icon=-1};
    draw_app_label(&label,prompt,rgb565(77,64,53));
    fill_round_rect(136,276,176,56,16,rgb565(238,228,216));
    fill_round_rect(328,276,176,56,16,rgb565(163,102,69));
    label.x=136;label.y=276;label.w=176;label.h=56;label.valign=1;
    draw_app_label(&label,"取消",rgb565(77,64,53));
    label.x=328;draw_app_label(&label,"确认",rgb565(255,255,255));
  }
  return true;
}
static bool render_package_media(unsigned long long now_ms, pet_p4_media_view_t *m) {
#if PET_P4_APP_DIRTY_CACHE
  /* This path may reuse the same buffer as a media prefix. Never leave an
   * exact full-app key authorizing that different owner/content. */
  g_app_frame_key.valid=false;
#if PET_P4_APP_CACHE_PROGRESSIVE
  if(g_app_cache_progressive.active){pet_app_cache_progressive_cancel(&g_app_cache_progressive);g_surface_background_valid=false;}
#endif
#endif
  if(!g_miniapp_render_snapshot.surface_active)return false;
  const pet_surface_t *surface=&g_miniapp_render_snapshot.surface;
  if(strcmp(g_miniapp_render_snapshot.media_source,m->source))memset(m,0,sizeof(*m));
  load_surface_images(now_ms);
  bool playing=!strcmp(m->status,"playing");
  if(playing&&g_surface_was_playing&&now_ms>=g_surface_last_ms&&now_ms-g_surface_last_ms<250)g_surface_play_ms+=now_ms-g_surface_last_ms;
  g_surface_last_ms=now_ms;g_surface_was_playing=playing;
  const pet_surface_node_t *first=&surface->nodes[0];
  bool cached_background=surface->count&&first->type==6&&!first->rotate_ms&&first->page==2
    &&first->x==0&&first->y==0&&first->w==640&&first->h==480&&g_surface_background_valid;
  if(!cached_background)fill_rect(0,0,640,480,surface->background);
  int index=0;for(int i=0;i<m->count;i++)if(!strcmp(m->queue[i],m->title))index=i;
  for(int i=0;i<surface->count;i++) {
    const pet_surface_node_t *n=&surface->nodes[i];
    if(n->page!=2&&n->page!=m->page)continue;
    int x=n->x,y=n->y,w=n->w,h=n->h;uint16_t c=n->color;
    if(n->type==6){
      bool static_background=i==0&&!n->rotate_ms&&n->page==2&&x==0&&y==0&&w==640&&h==480;
      if(static_background&&g_surface_background_valid)memcpy(g_framebuffer,g_surface_background,sizeof(g_surface_background));
      else{
        draw_surface_image(n);
        if(static_background){memcpy(g_surface_background,g_framebuffer,sizeof(g_surface_background));g_surface_background_valid=true;}
      }
    }
    else if(n->type==0){fill_round_rect_outline(x,y,w,h,n->radius,c,c);}
    else if(n->type==1){
      char number[24];const char *text=n->text;
      switch(n->bind){
        case 0:text=m->title[0]?m->title:"选一台，听一会";break;
        case 1:text=m->artist[0]?m->artist:"打开列表，发现好声音";break;
        case 2:text=!strcmp(m->status,"playing")?"正在直播":!strcmp(m->status,"buffering")?"正在缓冲":!strcmp(m->status,"error")?"播放失败":m->count?"等待收听":"等待选台";break;
        case 3:text=m->message;break;
        case 4:snprintf(number,sizeof(number),"%u%%",m->volume);text=number;break;
        case 5:snprintf(number,sizeof(number),"%02d",m->count?index+1:0);text=number;break;
        case 6:snprintf(number,sizeof(number),"%02u",m->count);text=number;break;
        case 7:snprintf(number,sizeof(number),"%u",m->level);text=number;break;
      }
      if(n->action[0]){
        // Media action labels share their hit box's center. Font visual bounds,
        // rather than a browser/native font-size offset, choose the baseline.
        int label_width=utf8_text_width(text,-1,n->scale);if(label_width>w)label_width=w;
        int inset=(w-label_width)/2;
        draw_text_line_vcenter(text,x+inset,y,h,w-inset,c,n->scale,true);
      }else draw_text_line(text,x,y,w,c,n->scale,true);
    }else if(n->type==2){int value=n->bind==4?m->volume:m->level;if(value>100)value=100;fill_rect(x,y,w*value/100,h,c);}
    else if(n->type==3){
      for(int j=0;j<=40;j++){int height=j%5?h/3:h/2;fill_rect(x+j*(w-2)/40,y+h-height,1,height,c);}
      if(m->count)fill_rect(x+(m->count>1?index*(w-2)/(m->count-1):(w-2)/2),y,2,h,c);
    }else if(n->type==4){
      int rows=h/48,first=m->selected/rows*rows;
      for(int row=0;row<rows&&first+row<m->count;row++){
        int at=first+row,ry=y+row*48;
        if(at==m->selected)fill_round_rect_outline(x,ry+2,w,44,10,c,c);
        draw_text_line(m->queue[at],x+16,ry+8,w-32,at==m->selected?surface->background:c,1,true);
      }
    }else if(n->type==5){
      int size=w<h?w:h;if(size<12)continue;int r=size/2;
      int cx=x+w/2,cy=y+h/2,left=cx-r,top=cy-r;
      fill_ellipse(cx,cy,r,r,c);
      if(!strcmp(m->status,"playing")||!strcmp(m->status,"buffering"))fill_rect(left+size/3,top+size/3,size/3,size/3,surface->background);
      else{
        int base_x=left+size/3,tip_x=left+size*2/3,span=tip_x-base_x;
        int upper=cy-(top+size/4),lower=top+size*3/4-cy;
        // Include the final column: a real one-pixel tip, not a clipped trapezoid.
        for(int j=0;j<=span;j++){
          int up=upper*(span-j)/span,down=lower*(span-j)/span;
          fill_rect(base_x+j,cy-up,1,up+down+1,surface->background);
        }
      }
    }
  }
  return true;
}
static void render_media_page(unsigned long long now_ms) {
  pet_p4_media_view_t *m = &g_media_view_snapshot;
  pet_p4_media_view(m);
  if(render_package_media(now_ms,m))return;
  const uint16_t bg=rgb565(22,21,24),surface=rgb565(45,36,35),ink=rgb565(255,246,237),muted=rgb565(177,160,149),accent=rgb565(255,136,62);
  fill_rect(0,0,640,480,bg);
  if(m->page==2) {
    if(!strcmp(m->status,"error")) {draw_text_center(m->message,320,222,580,muted,1);return;}
    bool has_lines=false;for(int i=0;i<5;i++)if(m->lyric_lines[i][0])has_lines=true;
    if(!has_lines) {
      const char *empty=!m->lyrics_ready?"正在获取歌词":m->lyric_index<0?"暂无同步歌词":"音乐间奏";
      draw_text_center(empty,320,222,580,muted,2);
    } else {
      unsigned long long age=now_ms>=m->lyric_changed_ms?now_ms-m->lyric_changed_ms:280;
      float t=age>=280?1.0f:(float)age/280.0f;
      int shift=(int)(82*(1-t)*(1-t)*(1-t));
      for(int row=0;row<5;row++) {
        int y=55+row*82+shift;
        uint16_t color=row==2?accent:(row==1||row==3)?muted:rgb565(85,76,74);
        if(row==2) {
          // Two bounded lines keep long Chinese lyrics readable, rather than
          // shrinking everything or drawing over the adjacent lyric rows.
          if(utf8_text_width(m->lyric_lines[row],-1,2)<=568)draw_text_center(m->lyric_lines[row],320,y,568,color,2);
          else {
            const char *rest=draw_text_line(m->lyric_lines[row],36,y,568,color,2,false);
            if(rest&&*rest)draw_text_line(rest,36,y+35,568,color,2,true);
          }
        } else draw_text_center(m->lyric_lines[row],320,y+6,568,color,1);
      }
    }
    return; // Immersive lyrics: no cover, transport bar, header or footer.
  }
  draw_media_note(25,27,accent);
  draw_text_line("随身听",65,23,210,ink,2,true);
  const char *state=!strcmp(m->status,"playing")?"正在播放":!strcmp(m->status,"paused")?"已暂停":!strcmp(m->status,"buffering")?"正在缓冲":!strcmp(m->status,"ended")?"播放结束":!strcmp(m->status,"error")?"播放失败":!strcmp(m->status,"interrupted")?"语音优先":"准备播放";
  fill_ellipse(471,37,4,4,!strcmp(m->status,"playing")?accent:muted);
  draw_text_line(state,486,29,130,muted,1,true);
  if(m->page==1) {
    draw_text_line("播放列表",28,81,350,muted,1,true);
    int first=m->selected/5*5;
    for(int row=0;row<5 && first+row<m->count;row++) {
      int index=first+row,y=114+row*58;
      if(index==m->selected)fill_round_rect_outline(24,y-6,592,53,12,surface,rgb565(85,58,43));
      char number[8];snprintf(number,sizeof(number),"%02d",index+1);
      draw_text_line(number,40,y+8,45,index==m->selected?accent:muted,1,true);
      draw_text_line(m->queue[index],94,y+5,500,index==m->selected?ink:muted,1,true);
    }
    if(!m->count)draw_text_line("在 PC 搜索歌曲并添加到播放列表",30,179,580,ink,1,true);
    draw_text_line("摇杆上下选择 · 中键播放 · 副操作返回",28,444,590,muted,1,true);return;
  }
  fill_round_rect_outline(30,104,212,212,22,rgb565(68,48,43),rgb565(88,61,49));
  if(m->cover_ready)pet_p4_media_blit(g_framebuffer,PET_P4_UI_WIDTH,40,114);
  else {
    fill_ellipse(136,210,86,86,rgb565(18,18,22));
    for(int r=80;r>34;r--)fill_ellipse(136,210,r,r,r%7==0?rgb565(46,40,42):rgb565(18,18,22));
    fill_ellipse(136,210,30,30,accent);fill_ellipse(136,210,5,5,bg);
  }
  draw_text_line("HACHIMO / MUSIC",276,105,330,rgb565(191,139,110),1,true);
  const char *title=m->title[0]?m->title:"音乐，陪你一会儿";
  const char *rest=draw_text_line(title,276,145,332,ink,2,false);
  if(rest&&*rest)draw_text_line(rest,276,186,332,ink,2,true);
  draw_text_line(m->artist[0]?m->artist:"搜索歌曲或语音点歌",276,242,332,muted,1,true);
  fill_round_rect_outline(276,278,174,30,15,surface,rgb565(88,61,49));
  draw_text_center("哈基米扬声器",363,285,158,rgb565(226,165,121),1);
  fill_round_rect_outline(30,341,580,6,3,rgb565(67,52,47),rgb565(67,52,47));
  int progress=m->duration_ms? (int)((uint64_t)m->position_ms*580/m->duration_ms):0;if(progress>580)progress=580;
  if(progress>0)fill_round_rect_outline(30,341,progress,6,3,accent,accent);
  char elapsed[16],duration[16],volume[20];
  snprintf(elapsed,sizeof(elapsed),"%02lu:%02lu",(unsigned long)m->position_ms/60000,(unsigned long)m->position_ms/1000%60);
  if(m->duration_ms)snprintf(duration,sizeof(duration),"%02lu:%02lu",(unsigned long)m->duration_ms/60000,(unsigned long)m->duration_ms/1000%60);else strcpy(duration,"--:--");
  draw_text_line(elapsed,30,357,90,muted,1,true);draw_text_line(duration,537,357,80,muted,1,true);
  if(m->seeking)draw_text_center("左右调整进度 · 中键确认",320,357,370,accent,1);
  fill_ellipse(320,405,29,29,accent);
  if(!strcmp(m->status,"playing")) {fill_rect(310,392,7,26,bg);fill_rect(324,392,7,26,bg);}
  else for(int x=0;x<22;x++)fill_rect(312+x,392+x/2,1,26-x,bg);
  for(int i=0;i<18;i++) {fill_rect(230-i,395+i/2,1,21-i,ink);fill_rect(410+i,395+i/2,1,21-i,ink);}
  fill_rect(210,396,3,20,ink);fill_rect(430,396,3,20,ink);
  draw_media_volume(30,392,muted);
  snprintf(volume,sizeof(volume),"%u%%",m->volume);draw_text_line(volume,69,398,105,muted,1,true);
  draw_media_queue(535,395,muted);
  char count[20];snprintf(count,sizeof(count),"%u",m->count);draw_text_line(count,575,398,45,muted,1,true);
  draw_text_center(m->message[0]?m->message:"摇杆切歌与音量 · 全局键退出",320,449,590,muted,1);
}

/* Lossless instrument layers: decompress once per visit, never in a strike. */
extern const uint8_t wooden_body_z[] asm("_binary_wooden_fish_body_deflate_start");
extern const uint8_t wooden_body_z_end[] asm("_binary_wooden_fish_body_deflate_end");
extern const uint8_t wooden_mallet_z[] asm("_binary_wooden_fish_mallet_deflate_start");
extern const uint8_t wooden_mallet_z_end[] asm("_binary_wooden_fish_mallet_deflate_end");
static uint8_t *wooden_body,*wooden_mallet;
static bool wooden_failed;
static void instrument_release(void){
  if(wooden_body)heap_caps_free(wooden_body);
  if(wooden_mallet)heap_caps_free(wooden_mallet);
  wooden_body=wooden_mallet=NULL;wooden_failed=false;
}
static uint8_t *instrument_decode(const uint8_t *input,size_t size,unsigned width,unsigned height){
  size_t expected=4+width*height*3;
  uint8_t *output=heap_caps_malloc(expected,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  tinfl_decompressor *inflater=calloc(1,sizeof(*inflater));
  if(!output||!inflater){if(output)heap_caps_free(output);free(inflater);return NULL;}
  tinfl_init(inflater);size_t consumed=size,decoded=expected;
  tinfl_status status=tinfl_decompress(inflater,input,&consumed,output,output,&decoded,
    TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(inflater);
  if(status!=TINFL_STATUS_DONE||consumed!=size||decoded!=expected||
      (unsigned)(output[0]|output[1]<<8)!=width||(unsigned)(output[2]|output[3]<<8)!=height){
    heap_caps_free(output);return NULL;
  }
  return output;
}
static bool instrument_prepare(void){
  if(wooden_failed)return false;
  if(wooden_body&&wooden_mallet)return true;
  wooden_body=instrument_decode(wooden_body_z,wooden_body_z_end-wooden_body_z,320,244);
  wooden_mallet=instrument_decode(wooden_mallet_z,wooden_mallet_z_end-wooden_mallet_z,160,163);
  if(!wooden_body||!wooden_mallet){instrument_release();wooden_failed=true;ESP_LOGW(TAG,"Instrument artwork unavailable");return false;}
  return true;
}
static void instrument_layer(const uint8_t *data,int x,int y,int height) {
  int w=data[0]|data[1]<<8,h=data[2]|data[3]<<8;
  int width=w*height/h;
  for(int dy=0;dy<height;dy++)for(int dx=0;dx<width;dx++) {
    int px=x+dx,py=y+dy;if(px<0||px>=640||py<0||py>=480)continue;
    const uint8_t *p=data+4+((dy*h/height)*w+dx*w/width)*3;
    unsigned a=p[2];if(!a)continue;
    uint16_t fg=p[0]|p[1]<<8,bg=g_framebuffer[py*640+px];
    unsigned r=(((fg>>11)&31)*a+((bg>>11)&31)*(255-a))/255;
    unsigned g=(((fg>>5)&63)*a+((bg>>5)&63)*(255-a))/255;
    unsigned b=((fg&31)*a+(bg&31)*(255-a))/255;
    g_framebuffer[py*640+px]=(r<<11)|(g<<5)|b;
  }
}
static pet_fruit_t g_fruit_render;
static void fruit_sprite(unsigned kind,float x,float y,float angle,int half,unsigned age,float radius) {
  if(kind>=g_miniapp_sprites.count)return;
  const pet_p4_miniapp_sprite_t *sp=&g_miniapp_sprites.items[kind];
  float c=cosf(angle*.017453293f),s=sinf(angle*.017453293f);
  const float size=radius*2.4f;
  int extent=(int)(size*.71f)+1;
  for(int dy=-extent;dy<=extent;dy++)for(int dx=-extent;dx<=extent;dx++) {
    float sx=(c*dx+s*dy)/size+.5f,sy=(-s*dx+c*dy)/size+.5f;
    if(sx<0||sx>=1||sy<0||sy>=1||(half<0&&sx>=.5f)||(half>0&&sx<.5f))continue;
    unsigned at=sp->data_offset+((unsigned)(sy*sp->frame_height)*sp->frame_width+(unsigned)(sx*sp->frame_width))*3;
    if(at+2>=g_miniapp_sprites.data_length)continue;
    uint16_t color=g_miniapp_sprites.pixels[at]|g_miniapp_sprites.pixels[at+1]<<8;
    unsigned alpha=g_miniapp_sprites.pixels[at+2];
    if(age>=650)alpha=0;else if(age>350)alpha=alpha*(650-age)/300;
    if(alpha)blend_px((int)x+dx,(int)y+dy,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,alpha);
  }
}
static void fruit_spark(int x,int y,int r,uint16_t color) {
  draw_line(x-r,y,x+r,y,color);draw_line(x,y-r,x,y+r,color);
}
static void render_fruit_page(void) {
  if(!pet_p4_miniapp_fruit_snapshot(&g_fruit_render))return;
  const pet_fruit_t *s=&g_fruit_render;
  (void)pet_p4_miniapp_get_sprites(&g_miniapp_sprites);
  uint16_t cream=rgb565(246,245,224),gold=rgb565(255,198,92),muted=rgb565(141,177,159);
  uint16_t deep=rgb565(16,40,35),panel=rgb565(29,63,53),mint=rgb565(158,220,149),coral=rgb565(255,119,87);
  const char *modes[]={"经典挑战","限时冲分","轻松练习"};
  const char *rules[]={"漏三果或碰炸弹结束","漏果不扣分，炸弹 -30","没有炸弹，尽情划切"};
  bool menu=s->phase==PET_FRUIT_IDLE||s->phase==PET_FRUIT_RESULT;
  for(int y=0;y<480;y+=4)fill_rect(0,y,640,4,rgb565(16+y/65,43+y/30,38+y/70));
  fill_ellipse(630,135,112,220,rgb565(27,61,48));
  fill_ellipse(-40,470,240,100,rgb565(29,65,46));
  for(int i=0;i<11;i++)fill_ellipse(23+i*61,115+(i*73)%287,1,1,rgb565(64,91,64));
  char text[96];
  if(menu) {
    fill_round_rect(32,26,159,26,13,panel);draw_text_line("FRESH FRUIT CLUB",45,32,145,mint,1,true);
    if(s->phase==PET_FRUIT_IDLE) {
      draw_text_line("鲜果一刀",32,77,325,cream,3,true);
      draw_text_line("一指划开，快乐爆汁。",36,148,306,muted,1,true);
      fill_ellipse(456,249,109,15,deep);
      fruit_sprite(3,449,156,-18,0,0,47);
      fruit_sprite(0,537,107,24,0,0,32);
      fruit_sprite(1,533,220,18,0,0,27);
      fruit_sprite(2,354,203,-27,0,0,22);
      fruit_spark(367,91,7,gold);fruit_spark(570,176,5,mint);fruit_spark(415,245,4,coral);
      snprintf(text,sizeof(text),"当前模式最佳  %u",s->best);draw_text_line(text,36,222,295,gold,1,true);
    } else {
      draw_text_line(s->bomb_hit?"哎呀，切到炸弹了":"漂亮！本局收刀",36,73,490,cream,2,true);
      snprintf(text,sizeof(text),"%u",s->score);draw_text_center(text,320,126,500,gold,4);
      snprintf(text,sizeof(text),"切中 %u 果     最高连击 %u     最佳 %u",s->cut_count,s->max_combo,s->best);
      draw_text_center(text,320,222,580,muted,1);
    }
    draw_text_line("选择你的节奏",34,272,260,cream,1,true);
    for(unsigned i=0;i<3;i++) {
      int x=32+i*192;bool selected=s->mode==i;
      fill_round_rect(x,306,184,72,16,deep);
      fill_round_rect(x,302,184,72,16,selected?cream:panel);
      draw_text_line(modes[i],x+16,317,164,selected?deep:cream,1,true);
      draw_text_line(rules[i],x+12,346,169,selected?rgb565(62,104,77):muted,1,true);
      if(selected)fill_ellipse(x+164,319,4,4,coral);
    }
    fill_round_rect(185,403,270,56,27,deep);
    fill_round_rect(185,399,270,56,27,gold);
    draw_text_center(s->phase==PET_FRUIT_RESULT?"再来一局":"开始切果",320,415,248,deep,2);
    draw_text_center("点击模式卡选择 · 开始键开局 · 全局键退出",320,462,610,muted,1);
    return;
  }
  fill_round_rect(24,20,128,72,18,deep);
  draw_text_line("SCORE",40,28,108,muted,1,true);
  snprintf(text,sizeof(text),"%u",s->score);draw_text_line(text,40,50,113,cream,2,true);
  fill_round_rect(236,24,168,34,17,panel);draw_text_center(modes[s->mode],320,33,150,mint,1);
  snprintf(text,sizeof(text),"第 %u 阶",s->level);draw_text_center(text,320,73,130,muted,1);
  fill_round_rect(493,20,123,72,18,deep);
  unsigned left=s->duration_sec>s->elapsed_ms/1000?s->duration_sec-s->elapsed_ms/1000:0;
  snprintf(text,sizeof(text),"%02u:%02u",left/60,left%60);draw_text_center(text,554,36,108,left<=10?coral:cream,2);
  if(s->mode==PET_FRUIT_CLASSIC)for(unsigned i=0;i<s->lives;i++)fill_ellipse(47+i*20,112,5,5,i<s->misses?panel:coral);
  else draw_text_line("尽情切，别停手",28,105,200,muted,1,true);
  for(unsigned i=0;i<PET_FRUIT_MAX;i++) {
    const pet_fruit_item_t *f=&s->fruit[i];if(!f->active)continue;
    int radius=(int)f->radius;
    if(f->bomb) {
      fill_ellipse(f->x+3,f->y+5,radius+2,radius+2,deep);
      fill_ellipse(f->x,f->y,radius,radius,rgb565(43,48,55));
      fill_ellipse(f->x-8,f->y-9,8,10,rgb565(85,99,107));
      draw_line(f->x+10,f->y-radius+3,f->x+20,f->y-radius-13,gold);
      fruit_spark(f->x+20,f->y-radius-13,5,coral);
      draw_line(f->x-7,f->y-5,f->x+7,f->y+9,cream);draw_line(f->x+7,f->y-5,f->x-7,f->y+9,cream);
    } else if(f->cut) {
      float gap=f->cut_age*.055f;
      fruit_sprite(f->kind,f->x-gap,f->y,f->angle-gap,-1,f->cut_age,f->radius);
      fruit_sprite(f->kind,f->x+gap,f->y+gap*.4f,f->angle+gap,1,f->cut_age,f->radius);
    } else {
      fill_ellipse(f->x+3,f->y+6,radius,radius,deep);
      if(f->golden) {
        int ring=radius+7;
        for(int a=0;a<360;a+=20) {
          float rad=a*.017453293f;
          fill_ellipse(f->x+cosf(rad)*ring,f->y+sinf(rad)*ring,2,2,gold);
        }
        fruit_spark(f->x+radius+6,f->y-radius-6,4,gold);
      }
      fruit_sprite(f->kind,f->x,f->y,f->angle,0,0,f->radius);
    }
  }
  uint16_t colors[]={coral,gold,mint,rgb565(255,134,161)};
  for(unsigned i=0;i<PET_FRUIT_PARTICLES;i++) {
    const pet_fruit_particle_t *p=&s->particles[i];if(!p->life)continue;
    fill_ellipse(p->x,p->y,p->life>250?3:2,p->life>250?5:2,colors[p->kind%4]);
  }
  for(unsigned i=1;i<s->trail_count;i++)if(!s->trail[i].start) {
    const pet_fruit_trail_t *a=&s->trail[i-1],*b=&s->trail[i];
    unsigned age=s->now_ms>=b->at?s->now_ms-b->at:0;int width=age<60?3:age<110?2:1;
    for(int w=-width;w<=width;w++)draw_line(a->x,a->y+w,b->x,b->y+w,gold);
    draw_line(a->x,a->y,b->x,b->y,cream);
  }
  if(s->feedback_ms) {
    int x=(int)s->feedback_x,y=(int)s->feedback_y-40-(650-s->feedback_ms)/22;
    if(x<80)x=80;if(x>560)x=560;if(y<135)y=135;
    snprintf(text,sizeof(text),s->feedback_kind==3?"-%u":s->feedback_kind==2?"金果 +%u":"+%u",s->feedback_points);
    draw_text_center(text,x,y,160,s->feedback_kind==3?coral:gold,2);
  }
  if(s->combo_ms&&s->feedback_combo>=3){snprintf(text,sizeof(text),"连击 ×%u",s->feedback_combo);draw_text_center(text,320,119,230,gold,2);}
  fill_round_rect(24,414,592,45,16,deep);
  draw_text_line(s->fever_ms?"爆汁时刻 · 双倍得分":"果汁能量",40,425,250,s->fever_ms?gold:muted,1,true);
  fill_round_rect(287,431,222,7,3,panel);
  unsigned energy=s->fever_ms?s->fever_ms/50:s->energy;
  if(energy)fill_round_rect(287,431,222*energy/100,7,3,s->fever_ms?gold:mint);
  snprintf(text,sizeof(text),s->fever_ms?"%us ×2":"%u%%",s->fever_ms?(s->fever_ms+999)/1000:s->energy);
  draw_text_line(text,530,425,79,s->fever_ms?gold:muted,1,true);
  if(s->phase==PET_FRUIT_PLAYING){
    fill_round_rect(528,100,88,48,16,panel);
    draw_text_center("暂停",572,116,78,cream,1);
  }
  if(s->phase==PET_FRUIT_PAUSED) {
    fill_round_rect(143,170,354,191,25,deep);
    draw_text_center("休息一下，果汁不急",320,203,329,cream,2);
    draw_text_center("当前进度已暂停",320,250,280,muted,1);
    fill_round_rect(190,280,260,58,24,gold);
    draw_text_center("继续切果",320,298,245,deep,2);
  }
}

static void render_instrument_page(unsigned long long now_ms, const char *title) {
  uint16_t bg=rgb565(241,227,207),ink=rgb565(84,66,47),muted=rgb565(139,115,84);
  fill_rect(0,0,640,480,bg);
  draw_text_line(title && title[0] ? title : "静心木鱼",30,27,250,ink,2,true);
  draw_text_line("一声木响，一刻安静",31,68,520,muted,1,true);
  if(!instrument_prepare()){draw_text_center("画面资源暂不可用，请退出后重试",320,240,580,ink,1);return;}
  pet_instrument_motion_t m=pet_p4_audio_instrument_motion(now_ms);
  if(m.wave<1) {
    int rx=160+(int)(m.wave*110),ry=49+(int)(m.wave*37);
    uint16_t wave=rgb565(205+(int)(36*m.wave),173+(int)(54*m.wave),127+(int)(80*m.wave));
    fill_ellipse(316,356,rx,ry,wave);fill_ellipse(316,356,rx-2,ry-2,bg);
  }
  fill_ellipse(308,410,175,15,rgb565(218,195,161));
  int height=(int)(297*m.fish_scale);
  instrument_layer(wooden_body,110,425-height,height);
  instrument_layer(wooden_mallet,340,85+(int)m.mallet_y,214);
  draw_text_center("按主操作敲一下 · 全局键退出",320,449,595,muted,1);
}
static void render_miniapp_page(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  bool available = pet_p4_miniapp_render_snapshot(&g_miniapp_render_snapshot,&g_surface_images,&g_surface_revision);
  if(!renderer_component_media_frame(available?&g_miniapp_render_snapshot:NULL))return;
#if PET_P4_APP_PHASE_TRACE
  g_render_trace.app_snapshot_us=available?g_miniapp_render_snapshot.capture_total_us:0;
  g_render_trace.app_snapshot_wait_us=available?g_miniapp_render_snapshot.capture_wait_us:0;
  g_render_trace.app_draw_started_us=(uint64_t)esp_timer_get_time();
#endif
  const pet_p4_miniapp_view_t *app = available ? &g_miniapp_render_snapshot.view : NULL;
  if(!app||!app->active||!app->instrument)instrument_release();
  if(app && app->active && app->slicing) {render_fruit_page();return;}
  if(app && app->active && app->instrument) {render_instrument_page(now_ms,app->title);return;}
  if(app && app->active && app->media) {render_media_page(now_ms);return;}
  if(app && app->active && app->app_surface && render_package_app(now_ms,app))return;
  if (app && app->active && app->data.enabled) { render_data_miniapp_page(app); return; }
  uint16_t background = rgb565(5, 7, 6);
  uint16_t panel = rgb565(11, 14, 13);
  uint16_t panel_outline = rgb565(52, 58, 53);
  uint16_t ivory = rgb565(250, 241, 204);
  uint16_t soft_ivory = rgb565(225, 241, 231);
  uint16_t muted = rgb565(126, 133, 126);
  uint16_t divider = rgb565(63, 69, 64);
  uint16_t orange = rgb565(255, 163, 31);
  uint16_t green = rgb565(85, 193, 133);
  uint16_t progress_bg = rgb565(38, 43, 39);
  uint16_t badge_ink = background;
  uint16_t footer_color = green;
  uint16_t page_muted = muted;
  bool pixel_tool = app && app->active
    && strcmp(app->visual_style, "pixel") == 0
    && strcmp(app->visual_layout, "tool") == 0;
  bool clean_game = app && app->active
    && strcmp(app->visual_style, "clean") == 0
    && app->game.kind != PET_P4_GAME_NONE;
  if (app && app->active && app->visual_palette[0]
      && strcmp(app->visual_style, "pixel") != 0) {
    miniapp_pixel_palette_t palette = miniapp_pixel_palette(app->visual_palette);
    bool arcade_page = strcmp(app->visual_palette, "arcade") == 0;
    background = palette.background;
    panel = palette.surface;
    panel_outline = palette.ink;
    ivory = palette.ink;
    soft_ivory = arcade_page ? palette.light : palette.ink;
    muted = palette.shadow;
    page_muted = arcade_page ? palette.secondary : palette.shadow;
    divider = page_muted;
    orange = palette.primary;
    green = palette.secondary;
    progress_bg = palette.background_alt;
    badge_ink = palette.ink;
    footer_color = soft_ivory;
  }

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  if (!app || !app->active) {
    draw_text_line("MINI APP", 28, 24, 240, green, 1, false);
    draw_text_line("NO APP INSTALLED", 28, 90, 584, ivory, 3, true);
    draw_text_line("INSTALL FROM PET MANAGER", 28, 156, 584, muted, 1, true);
    fill_rect(28, 210, 584, 1, divider);
    return;
  }
  if ((strcmp(app->visual_style, "pixel") == 0 || clean_game) && !pixel_tool) {
    render_pixel_miniapp_page(app);
    return;
  }
  if (pixel_tool) {
    render_pixel_tool_miniapp_page(app);
    return;
  }

  bool scoreboard = strcmp(app->visual_layout, "scoreboard") == 0;
  const char *title = app->title[0] ? app->title : app->widget_id;
  uint16_t state_color = miniapp_state_color(app->state);
  int state_width = utf8_text_width(app->state, -1, 1) + 34;
  if (state_width < 76) state_width = 76;
  if (state_width > 154) state_width = 154;
  if (state_width & 1) state_width += 1;
  int state_x = 612 - state_width;
  int title_x = 28;
  int title_max_px = PET_P4_PAGE_INDICATOR_X - title_x - PET_P4_PAGE_INDICATOR_TITLE_GAP;

  draw_text_line(title, title_x, 16, title_max_px, soft_ivory, 2, true);
  bool show_app_state = state
    && pet_p4_state_host_connected(state, now_ms)
    && state->asset_family_count > 0;
  if (app->state[0] && show_app_state) {
    fill_round_rect_outline(state_x, 18, state_width, 32, 16, panel, panel_outline);
    fill_ellipse(state_x + 14, 34, 4, 4, state_color);
    draw_text_center(
      app->state,
      state_x + state_width / 2 + 6,
      text_y_in_box(app->state, 1, 18, 32),
      state_width - 30,
      muted,
      1
    );
  }
  if (app->eyebrow[0]) {
    draw_text_line(app->eyebrow, 28, 60, 584, page_muted, 1, true);
  }
  if (app->headline[0]) {
    draw_text_line(app->headline, 28, 86, 584, orange, 2, true);
  }

  fill_round_rect_outline(28, 138, 584, 236, 8, panel, panel_outline);
  if (app->metric_label[0]) {
    if (scoreboard) {
      draw_text_center(app->metric_label, 320,
                       text_y_in_box(app->metric_label, 1, 154, 30),
                       520, muted, 1);
    } else {
      draw_text_line(app->metric_label, 50, 160, 370, muted, 1, true);
    }
  }
  const char *metric_value = app->metric_value[0] ? app->metric_value : "-";
  int value_max_width = scoreboard ? 520 : 390;
  int value_scale = fitted_text_scale(metric_value, value_max_width, scoreboard ? 5 : 4, 2);
  int value_width = utf8_text_width(metric_value, -1, value_scale);
  if (value_width > value_max_width) value_width = value_max_width;
  if (scoreboard) {
    draw_text_center(metric_value, 320,
                     text_y_in_box(metric_value, value_scale, 180, 86),
                     value_max_width, ivory, value_scale);
    if (app->metric_unit[0]) {
      draw_text_center(app->metric_unit, 320,
                       text_y_in_box(app->metric_unit, 1, 266, 26),
                       520, muted, 1);
    }
  } else {
    draw_text_line(metric_value, 50, 176, 390, ivory, value_scale, true);
    if (app->metric_unit[0]) {
      int unit_x = 50 + value_width + 14;
      if (unit_x > 470) unit_x = 470;
      draw_text_line(app->metric_unit, unit_x, 176 + (value_scale - 1) * 12, 104, muted, 1, true);
    }
  }
  if (app->badge[0]) {
    fill_ellipse(562, 190, 24, 24, green);
    draw_text_center(
      app->badge,
      562,
      text_y_in_box(app->badge, 1, 166, 48),
      36,
      badge_ink,
      1
    );
  }

  if (app->note[0]) {
    if (scoreboard) {
      draw_text_center(app->note, 320,
                       text_y_in_box(app->note, 1, 296, 28),
                       520, muted, 1);
    } else {
      draw_text_line(app->note, 50, 280, 540, muted, 1, true);
    }
  }
  if (app->progress_percent >= 0) {
    int progress_width = (int) ((uint32_t) 540U * (uint32_t) app->progress_percent / 100U);
    char percent[12];
    snprintf(percent, sizeof(percent), "%d%%", app->progress_percent);
    if (app->progress_label[0]) draw_text_line(app->progress_label, 50, 320, 380, muted, 1, true);
    draw_text_right(percent, 590, 320, 92, ivory, 1);
    fill_round_rect(50, 348, 540, 8, 4, progress_bg);
    if (progress_width > 0 && progress_width < 8) progress_width = 8;
    if (progress_width > 0) fill_round_rect(50, 348, progress_width, 8, 4, orange);
  }

  for (int x = 28; x < 612; x += 12) fill_rect(x, 420, 6, 1, divider);
  if (app->footer[0]) {
    draw_text_center(
      app->footer,
      320,
      text_y_in_box(app->footer, 2, 424, 44),
      560,
      footer_color,
      2
    );
  }
}

static void draw_boot_diagnostic(unsigned long long now_ms) {
  if (now_ms > P4_BOOT_DIAGNOSTIC_MS) return;

  uint16_t white = rgb565(255, 255, 255);
  uint16_t orange = rgb565(255, 120, 0);
  int border = 6;
  int marker_w = 96;
  int marker_h = 18;
  fill_rect(0, 0, PET_P4_UI_WIDTH, border, white);
  fill_rect(0, PET_P4_UI_HEIGHT - border, PET_P4_UI_WIDTH, border, white);
  fill_rect(0, 0, border, PET_P4_UI_HEIGHT, white);
  fill_rect(PET_P4_UI_WIDTH - border, 0, border, PET_P4_UI_HEIGHT, white);
  fill_rect(16, 16, marker_w, marker_h, orange);
  fill_rect(PET_P4_UI_WIDTH - marker_w - 16, 16, marker_w, marker_h, orange);
  fill_rect(16, PET_P4_UI_HEIGHT - marker_h - 16, marker_w, marker_h, orange);
  fill_rect(PET_P4_UI_WIDTH - marker_w - 16, PET_P4_UI_HEIGHT - marker_h - 16, marker_w, marker_h, orange);
}

static esp_err_t rotate_landscape_to_panel(void) {
  if (!g_framebuffer || !g_native_framebuffer) return ESP_ERR_INVALID_STATE;
#if PET_P4_RENDER_ROTATE_TILE
  /* Same destination and original RGB565 element mapping. The original LCD
   * submit/cache-sync/refresh fence still owns publication and buffer reuse. */
  if(BSP_LCD_H_RES==PET_P4_UI_HEIGHT&&BSP_LCD_V_RES==PET_P4_UI_WIDTH&&
     pet_rgb565_rotate_panel(g_native_framebuffer,(size_t)BSP_LCD_H_RES*BSP_LCD_V_RES,
       g_framebuffer,PET_P4_UI_WIDTH,PET_P4_UI_HEIGHT))return ESP_OK;
#endif
  /* Every producer and the ST7701S panel use the same RGB565 element order. */
  if (g_ppa_srm_client
      && g_framebuffer_ppa_compatible
      && g_native_framebuffer_ppa_compatible) {
    ppa_srm_oper_config_t rotation = {
      .in = {
        .buffer = g_framebuffer,
        .pic_w = PET_P4_UI_WIDTH,
        .pic_h = PET_P4_UI_HEIGHT,
        .block_w = PET_P4_UI_WIDTH,
        .block_h = PET_P4_UI_HEIGHT,
        .block_offset_x = 0,
        .block_offset_y = 0,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
      },
      .out = {
        .buffer = g_native_framebuffer,
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
        .pic_w = BSP_LCD_H_RES,
        .pic_h = BSP_LCD_V_RES,
        .block_offset_x = 0,
        .block_offset_y = 0,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
      },
      .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
      .scale_x = 1.0f,
      .scale_y = 1.0f,
      .rgb_swap = false,
      .byte_swap = false,
      .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_trace_srm(PET_PPA_TRACE_ROTATE, &rotation);
    if (err == ESP_OK) {
      if (!g_logged_ppa_rotation) {
        ESP_LOGD(TAG, "P4 rotation=PPA angle=270 output=RGB565 panel-order=RGB size=%dx%d",
          BSP_LCD_H_RES, BSP_LCD_V_RES);
        g_logged_ppa_rotation = true;
      }
      return ESP_OK;
    }
    if (!g_logged_ppa_fallback) {
      ESP_LOGW(TAG, "P4 PPA rotation failed (%s); using CPU fallback", esp_err_to_name(err));
      g_logged_ppa_fallback = true;
    }
  }
  for (int y = 0; y < PET_P4_UI_HEIGHT; y += 1) {
    for (int x = 0; x < PET_P4_UI_WIDTH; x += 1) {
      int panel_x = PET_P4_UI_HEIGHT - 1 - y;
      int panel_y = x;
      g_native_framebuffer[panel_y * BSP_LCD_H_RES + panel_x] =
        g_framebuffer[y * PET_P4_UI_WIDTH + x];
    }
  }
  return ESP_OK;
}

static void renderer_init_native_buffers(void) {
  if (!g_native_framebuffers[0] && !g_native_framebuffers[1])
    g_native_framebuffer_ppa_compatible = true;
  for (unsigned int index = 0; index < PET_P4_NATIVE_OUTPUT_BUFFER_COUNT; index += 1) {
    /* Keep successful allocations when the other slot failed. A retry fills
     * only missing slots, avoiding both a leaked buffer and a permanent hole. */
    if (g_native_framebuffers[index]) continue;
    g_native_framebuffers[index] = (uint16_t *) heap_caps_aligned_calloc(
      PET_P4_PPA_BUFFER_ALIGNMENT,
      BSP_LCD_H_RES * BSP_LCD_V_RES,
      sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
    );
    if (!g_native_framebuffers[index]) {
      g_native_framebuffer_ppa_compatible = false;
      g_native_framebuffers[index] = (uint16_t *) heap_caps_malloc(
        BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
    }
    if (!g_native_framebuffers[index]) {
      g_native_framebuffers[index] = (uint16_t *) heap_caps_malloc(
        BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
        MALLOC_CAP_8BIT
      );
    }
  }
  if (!g_native_framebuffer) g_native_framebuffer = g_native_framebuffers[0];
}

esp_err_t pet_p4_renderer_init(void) {
  if (!g_framebuffer) {
    g_framebuffer = (uint16_t *) heap_caps_aligned_calloc(
      PET_P4_PPA_BUFFER_ALIGNMENT,
      PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT,
      sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA
    );
    g_framebuffer_ppa_compatible = g_framebuffer != NULL;
    if (!g_framebuffer) {
      g_framebuffer = (uint16_t *) heap_caps_malloc(
        PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
      );
    }
    if (!g_framebuffer) g_framebuffer = (uint16_t *) heap_caps_malloc(PET_P4_UI_WIDTH * PET_P4_UI_HEIGHT * sizeof(uint16_t), MALLOC_CAP_8BIT);
  }
  renderer_init_native_buffers();
  if (!g_jpeg_input) {
    jpeg_decode_memory_alloc_cfg_t input_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    g_jpeg_input = (uint8_t *) jpeg_alloc_decoder_mem(
      PET_P4_MAX_JPEG_BYTES,
      &input_mem_cfg,
      &g_jpeg_input_capacity
    );
    g_jpeg_input_dma_compatible = g_jpeg_input != NULL;
    if (!g_jpeg_input) {
      g_jpeg_input = (uint8_t *) heap_caps_malloc(PET_P4_MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!g_jpeg_input) g_jpeg_input = (uint8_t *) heap_caps_malloc(PET_P4_MAX_JPEG_BYTES, MALLOC_CAP_8BIT);
      if (g_jpeg_input) g_jpeg_input_capacity = PET_P4_MAX_JPEG_BYTES;
    }
  }
  if (!g_jpeg_output) {
    size_t requested_output_capacity = PET_P4_ASSET_WIDTH * PET_P4_ASSET_HEIGHT * sizeof(uint16_t);
    jpeg_decode_memory_alloc_cfg_t output_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    g_jpeg_output = (uint16_t *) jpeg_alloc_decoder_mem(
      requested_output_capacity,
      &output_mem_cfg,
      &g_jpeg_output_capacity
    );
    g_jpeg_output_dma_compatible = g_jpeg_output != NULL;
    if (!g_jpeg_output) {
      g_jpeg_output_capacity = requested_output_capacity;
      g_jpeg_output = (uint16_t *) heap_caps_malloc(g_jpeg_output_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!g_jpeg_output) g_jpeg_output = (uint16_t *) heap_caps_malloc(g_jpeg_output_capacity, MALLOC_CAP_8BIT);
    }
  }
  if (!g_tjpgd_workbuf) {
    g_tjpgd_workbuf = (uint8_t *) heap_caps_malloc(PET_P4_TJPGD_WORK_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_tjpgd_workbuf) g_tjpgd_workbuf = (uint8_t *) heap_caps_malloc(PET_P4_TJPGD_WORK_BUFFER_BYTES, MALLOC_CAP_8BIT);
  }
#if PET_P4_KEYFRAME_BG
  keyframe_init();
#endif
  if (!g_h264_pack_task_init_attempted) {
    g_h264_pack_task_init_attempted = true;
    g_h264_pack_done = xSemaphoreCreateBinary();
    BaseType_t task_result = g_h264_pack_done
      ? xTaskCreatePinnedToCore(
          h264_pack_worker,
          "h264-pack",
          PET_P4_H264_PACK_TASK_STACK_BYTES,
          NULL,
          PET_P4_H264_PACK_TASK_PRIORITY,
          &g_h264_pack_task,
          CONFIG_ESP_H264_DUAL_TASK_CORE
        )
      : pdFAIL;
    if (task_result != pdPASS) {
      if (g_h264_pack_done) vSemaphoreDelete(g_h264_pack_done);
      g_h264_pack_done = NULL;
      g_h264_pack_task = NULL;
      ESP_LOGW(TAG, "P4 H264 parallel pack task unavailable; using one core");
    }
  }
  if (!g_framebuffer || !g_native_framebuffers[0] || !g_native_framebuffers[1]
      || !g_jpeg_input || !g_jpeg_output || !g_tjpgd_workbuf) {
    return ESP_ERR_NO_MEM;
  }
  if (!g_ppa_init_attempted) {
    g_ppa_init_attempted = true;
    if (g_native_framebuffer_ppa_compatible) {
      ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
      };
      esp_err_t ppa_err = ppa_register_client(&ppa_config, &g_ppa_srm_client);
      if (ppa_err != ESP_OK) {
        g_ppa_srm_client = NULL;
        ESP_LOGW(
          TAG,
          "P4 PPA rotation init failed (%s); using CPU fallback",
          esp_err_to_name(ppa_err)
        );
      } else {
        const ppa_event_callbacks_t callbacks={.on_trans_done=ppa_trace_done};
        bool available=ppa_client_register_event_callbacks(g_ppa_srm_client,&callbacks)==ESP_OK;
        /* Optional registration failure only disables diagnostic splits. */
        portENTER_CRITICAL(&g_ppa_trace_mux);
        pet_ppa_trace_set_available(&g_ppa_trace,available);
        portEXIT_CRITICAL(&g_ppa_trace_mux);
      }
    } else {
      ESP_LOGW(TAG, "P4 PPA output buffer unavailable; using CPU rotation");
    }
  }
  if (!g_jpeg_decoder_init_attempted) {
    g_jpeg_decoder_init_attempted = true;
    if (g_jpeg_input_dma_compatible && g_jpeg_output_dma_compatible) {
      jpeg_decode_engine_cfg_t decoder_cfg = {
        .intr_priority = 0,
        .timeout_ms = PET_P4_HARDWARE_JPEG_TIMEOUT_MS,
      };
      esp_err_t decoder_err = jpeg_new_decoder_engine(&decoder_cfg, &g_jpeg_decoder);
      if (decoder_err != ESP_OK) {
        g_jpeg_decoder = NULL;
        ESP_LOGW(
          TAG,
          "P4 hardware JPEG decoder init failed (%s); using TJPGD fallback",
          esp_err_to_name(decoder_err)
        );
      }
    } else {
      ESP_LOGW(TAG, "P4 JPEG DMA buffers unavailable; using TJPGD fallback");
    }
  }
  return ESP_OK;
}

/* The APP clip owns the SAME decoder/worker as main-page appearances. Once
 * pinned, its draw/hidden/generation paths own retirement. Cancelling the shared
 * job at every APP frame would discard every ready clip frame before take. */
static void renderer_cancel_inactive_prepares(const char *page){
  if(!strcmp(page,"main"))return;
#if PET_P4_CLIP
  if(!strcmp(page,"app")&&g_clip_lease.token){release_inactive_asset_prepares();return;}
#endif
  cancel_pending_asset_prepares();
}

static void draw_approval_button(
  int x,
  int y,
  int w,
  int h,
  const char *label,
  uint16_t fill,
  uint16_t outline,
  uint16_t ink
) {
  fill_round_rect_outline(x, y, w, h, 12, fill, outline);
  draw_text_center(label, x + w / 2, text_y_in_box(label, 2, y, h), w - 24, ink, 2);
}

/* R5 DSH approval: a modal dark card over the current page. It never mutates
 * normal page rendering; it is the last paint before rotation so it covers the
 * home page, component center and mini-app frames identically. */
static void draw_approval_overlay(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  const uint16_t card = rgb565(18, 22, 26);
  const uint16_t shadow = rgb565(2, 3, 4);
  const uint16_t outline = rgb565(255, 176, 32);
  const uint16_t divider = rgb565(58, 64, 70);
  const uint16_t ivory = rgb565(250, 241, 204);
  const uint16_t ink = rgb565(235, 238, 240);
  const uint16_t muted = rgb565(132, 140, 146);
  const uint16_t reason_panel = rgb565(11, 14, 17);
  const uint16_t reason_ink = rgb565(200, 208, 214);
  const int card_x = PET_P4_APPROVAL_CARD_X;
  const int card_y = PET_P4_APPROVAL_CARD_Y;
  const int content_x = card_x + 28;
  const int content_w = PET_P4_APPROVAL_CARD_WIDTH - 56;
  const int button_w = (content_w - 16) / 2;
  const int button_h = 46;
  char countdown[24];
  unsigned long long remaining_ms;
  unsigned int seconds;

  if (!state || !pet_p4_state_approval_pending(state, now_ms)) return;
  blend_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, 0, 0, 0, 168);
  fill_round_rect(card_x + 4, card_y + 6,
                  PET_P4_APPROVAL_CARD_WIDTH, PET_P4_APPROVAL_CARD_HEIGHT,
                  PET_P4_APPROVAL_CARD_RADIUS, shadow);
  fill_round_rect_outline(card_x, card_y,
                          PET_P4_APPROVAL_CARD_WIDTH, PET_P4_APPROVAL_CARD_HEIGHT,
                          PET_P4_APPROVAL_CARD_RADIUS, card, outline);
  draw_text_line("需要授权", content_x, card_y + 28, 220, ivory, 2, false);
  if (state->approval_expires_ms == 0) {
    /* A zero deadline is the explicit never-expire mode; do not render 0 s. */
    draw_text_right("不超时", card_x + PET_P4_APPROVAL_CARD_WIDTH - 26,
                    card_y + 30, 210, outline, 2);
  } else {
    remaining_ms = state->approval_expires_ms > now_ms
      ? state->approval_expires_ms - now_ms
      : 0;
    if (remaining_ms > 999000ULL) remaining_ms = 999000ULL;
    seconds = (unsigned int) ((remaining_ms + 999ULL) / 1000ULL);
    snprintf(countdown, sizeof(countdown), "剩余 %u 秒", seconds);
    draw_text_right(countdown, card_x + PET_P4_APPROVAL_CARD_WIDTH - 26,
                    card_y + 30, 210, outline, 2);
  }
  fill_rect(content_x, card_y + 82, content_w, 1, divider);
  draw_text_line_vcenter("来源", content_x, card_y + 92, 40, 64, muted, 1, false);
  draw_text_line_vcenter(
    state->approval_agent[0] ? state->approval_agent : "未知 Agent",
    content_x + 72, card_y + 92, 40, content_w - 72, ink, 2, true
  );
  draw_text_line_vcenter("工具", content_x, card_y + 132, 40, 64, muted, 1, false);
  draw_text_line_vcenter(
    state->approval_tool[0] ? state->approval_tool : "未知工具",
    content_x + 72, card_y + 132, 40, content_w - 72, ink, 2, true
  );
  fill_round_rect(content_x, card_y + 180, content_w, 94, 12, reason_panel);
  draw_text_line("理由", content_x + 16, card_y + 188, 56, muted, 1, false);
  draw_card_body_lines(
    state->approval_reason[0] ? state->approval_reason : "无说明",
    content_x + 16,
    card_y + 206,
    content_w - 32,
    reason_ink,
    NULL /* Approval text is local-only, outside the host bubble asset scope. */
  );
  draw_approval_button(content_x, card_y + 290, button_w, button_h, "允许",
                       rgb565(18, 54, 38), rgb565(64, 196, 128),
                       rgb565(122, 236, 172));
  draw_approval_button(content_x + button_w + 16, card_y + 290, button_w, button_h,
                       "拒绝", rgb565(54, 22, 22), rgb565(236, 96, 88),
                       rgb565(255, 150, 140));
}

esp_err_t pet_p4_renderer_render(
  const pet_p4_runtime_state_t *state,
  const pet_p4_view_model_t *view,
  unsigned long long now_ms
) {
  /* Entry reset also covers initialization failures; no lifetime/window reset. */
  pet_p4_render_trace_begin(&g_render_trace, g_render_trace_frame, view && view->page ? view->page : "main");
  const esp_err_t renderer_init_result=pet_p4_renderer_init();
  if(renderer_init_result!=ESP_OK)(void)renderer_component_media_frame(NULL);
  ESP_RETURN_ON_ERROR(renderer_init_result, TAG, "allocate render buffers");
#if PET_P4_H264_WORKER_PACK
  h264_frame_offer_begin();
#endif
  const uint16_t *previous_native = g_native_framebuffer;
#if PET_P4_LOGICAL_COMPARE
  const unsigned previous_native_index=g_native_framebuffer_index;
  g_app_logical_same=false;
#endif
  const bool previous_valid = g_last_native_valid;
  g_last_native_valid = false; /* rotation/draw failures must force a refresh */
  g_native_framebuffer_index =
    (g_native_framebuffer_index + 1U) % PET_P4_NATIVE_OUTPUT_BUFFER_COUNT;
  g_native_framebuffer = g_native_framebuffers[g_native_framebuffer_index];
  int64_t render_started_us = esp_timer_get_time();
  int64_t overlay_started_us = render_started_us;
  uint32_t asset_us = 0;
  g_last_decode_us = 0;
  g_last_h264_pack_us = 0;
  g_last_h264_convert_us = 0;
  const char *page = view && view->page ? view->page : "main";
  if(strcmp(page,"main")) {
    g_main_appearance_ready=false;
    pet_compact_underlay_forget(&g_main_compact_underlay);
  }
  pet_p4_render_trace_begin(&g_render_trace, ++g_render_trace_frame, page);
#if PET_P4_APP_RETAIN_LOGICAL
  /* Keep all new planning work INSIDE the unchanged whole-frame clock. */
  g_app_common_plan=pet_logical_common_plan(state!=NULL,
    pet_p4_state_touch_feedback_active(state,now_ms),state?state->local_touch_x:0,state?state->local_touch_y:0,
    state&&pet_p4_state_host_connected(state,now_ms),state&&state->asset_family_count>0,
    state&&state->desktop_device_id[0],state&&state->native_usb_mounted,state&&state->host_last_seen_ms>0,
    now_ms<=P4_BOOT_DIAGNOSTIC_MS);
  g_app_may_retain=pet_logical_retention_begin(&g_app_final_retention,g_app_common_plan,previous_valid,
    !strcmp(g_last_render_page,"app"),!strcmp(page,"app"));
  g_app_final_reused=g_app_final_eligible=false;
#endif
  ppa_trace_frame_begin();
  g_render_trace.start_us = (uint64_t)render_started_us;
  if (g_previous_render_ended_us > 0) {
    g_render_trace.previous_end_us=(uint64_t)g_previous_render_ended_us;
    g_render_trace.gap_us=elapsed_us_clamped(g_previous_render_ended_us,render_started_us);
  }
  pet_widget_trace_snapshot_t operation_snapshot;
  pet_widget_trace_snapshot(&operation_snapshot);
  pet_p4_render_trace_observe(&g_render_trace, &operation_snapshot, false);
  renderer_cancel_inactive_prepares(page);
  g_asset_cache_pending = false;
  if (!g_framebuffer_initialized) {
#if PET_P4_APP_RETAIN_LOGICAL
    g_app_may_retain=false;
#endif
    fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, rgb565(0, 0, 0));
    g_framebuffer_initialized = true;
  }
  if(strcmp(page,"components"))center_art_release();
  if(strcmp(page,"app"))instrument_release();
  bool realtime_mode = view && view->realtime_conversation;
  bool component_media_idle=true;
  if(strcmp(page,"app"))component_media_idle=renderer_component_media_frame(NULL);
  if (realtime_mode != g_last_realtime_conversation) {
#if PET_P4_APP_RETAIN_LOGICAL
    g_app_may_retain=false;
#endif
    // Even a missing/failed appearance must not leave old Agent cards beneath the chat.
    fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, rgb565(0, 0, 0));
    g_session_overlay_cache_valid = false;
    g_main_appearance_ready = false;
    pet_compact_underlay_forget(&g_main_compact_underlay);
    g_last_realtime_conversation = realtime_mode;
  }
  const bool approval_visible = state && pet_p4_state_approval_pending(state, now_ms);
  const bool approval_changed = approval_visible != g_last_approval_visible;
#if PET_P4_APP_RETAIN_LOGICAL
  /* A final retained APP frame includes common overlays. Never reuse or publish
   * that final frame across an approval modal/countdown; keep the clean texture. */
  if (approval_visible || approval_changed) g_app_may_retain = false;
#endif
  if (approval_changed) {
    // The modal dims arbitrary pixels; repaint the page beneath from black so
    // entering/leaving it cannot leave a ghost card on pages (for example a
    // missing appearance frame) that only repaint their own controls.
    fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, rgb565(0, 0, 0));
    g_session_overlay_cache_valid = false;
    g_main_appearance_ready = false;
    pet_compact_underlay_forget(&g_main_compact_underlay);
    g_last_approval_visible = approval_visible;
  }
  if (strcmp(page, "components") == 0) {
    render_component_center_page(view && view->component_loading);
  } else if (strcmp(page, "touch_test") == 0 && state) {
    render_touch_test_page(state);
  } else if (strcmp(page, "app") == 0) {
    render_miniapp_page(state, now_ms);
  } else {
    bool realtime = view && view->realtime_conversation;
    bool show_session_queue = !realtime && state && state->session_queue_count > 0;
    int64_t asset_started_us = esp_timer_get_time();
    bool drew_asset = component_media_idle&&render_asset_pet_frame(state, view, now_ms);
    if(drew_asset)g_main_appearance_ready=true;
    if(!component_media_idle)g_asset_cache_pending=true;
    /* A queued prepare is not a missing/bad asset. Keep the last logical pixels
     * while continuing all live overlays and the normal unchanged-frame gate. */
    if (!drew_asset && !g_asset_cache_pending && g_last_render_page[0]
        && strcmp(g_last_render_page, "main") != 0)
      fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, rgb565(0, 0, 0));
    overlay_started_us = esp_timer_get_time();
    asset_us = elapsed_us_clamped(asset_started_us, overlay_started_us);
    if (!drew_asset && !g_asset_cache_pending) {
      if (!g_logged_missing_asset) {
        ESP_LOGD(TAG, "no usable P4 appearance frames yet; keeping bubble/status visible");
        g_logged_missing_asset = true;
      }
    }
    bool compact_requested = !show_session_queue
      && (!view || (view->show_bubble && view->compact_bubble && !view->show_voice_overlay));
    main_compact_underlay_prepare(drew_asset, compact_requested);
    /* Appearance and its overlays enter the same logical/LCD frame. While the
     * first decode is queued, retain the previous page without an early bubble. */
    if(g_main_appearance_ready){
      if (view && view->show_voice_overlay) draw_standalone_voice_overlay();
      else if (!show_session_queue) draw_bubble(state, view, now_ms);
      if (!realtime) draw_session_queue(state, now_ms);
      else g_session_overlay_cache_valid = false;
    }else g_session_overlay_cache_valid=false;
  }
  const int64_t common_overlay_started_us = esp_timer_get_time();
#if PET_P4_APP_PHASE_TRACE
  if(g_render_trace.app_draw_started_us)g_render_trace.app_draw_us=elapsed_us_clamped((int64_t)g_render_trace.app_draw_started_us,common_overlay_started_us);
#endif
  g_render_trace.content_us = elapsed_us_clamped(overlay_started_us, common_overlay_started_us);
  const bool media_page = strcmp(page, "app") == 0 && pet_p4_miniapp_media_active();
  const bool instrument_page = strcmp(page, "app") == 0
    && pet_p4_miniapp_instrument_config(NULL, NULL);
  const bool dedicated_page = media_page || instrument_page || (strcmp(page,"app")==0 && pet_p4_miniapp_slicing_active()) || strcmp(page, "components") == 0
    || strcmp(page, "touch_test") == 0;
#if PET_P4_APP_RETAIN_LOGICAL
  if(!g_app_final_reused){
    if(!dedicated_page&&g_app_common_plan.touch)draw_touch_feedback_pixels(g_app_common_plan.touch_x,g_app_common_plan.touch_y);
  }
#else
  if (!dedicated_page) draw_touch_feedback(state, now_ms);
#endif
  // Native media owns its status and error UI. Generic Agent/connection banners
  // must not cover playback status or break the immersive lyrics-only page.
#if PET_P4_APP_RETAIN_LOGICAL
  if(!dedicated_page&&!g_app_final_reused)draw_common_connection(g_app_common_plan.banner);
#else
  if (!dedicated_page) draw_connection_banner(state, now_ms);
#endif
  snprintf(g_last_render_page, sizeof(g_last_render_page), "%s", page);
#if PET_P4_APP_RETAIN_LOGICAL
  if(!dedicated_page&&!g_app_final_reused&&g_app_common_plan.boot)draw_boot_diagnostic(now_ms);
#else
  if (!dedicated_page) draw_boot_diagnostic(now_ms);
#endif
  draw_approval_overlay(state, now_ms);
  int64_t rotate_started_us = esp_timer_get_time();
  g_render_trace.common_overlay_us = elapsed_us_clamped(common_overlay_started_us, rotate_started_us);
  uint32_t overlay_us = elapsed_us_clamped(overlay_started_us, rotate_started_us);
#if PET_P4_LOGICAL_COMPARE
  int64_t compare_started_us=rotate_started_us;
  bool logical_unchanged=false;
  if(g_app_logical_same&&previous_valid&&BSP_LCD_H_RES==PET_P4_UI_HEIGHT&&BSP_LCD_V_RES==PET_P4_UI_WIDTH){
    logical_unchanged=pet_p4_logical_frame_unchanged(g_framebuffer,previous_native,PET_P4_UI_WIDTH,PET_P4_UI_HEIGHT);
    g_render_trace.logical_compare_us=elapsed_us_clamped(compare_started_us,esp_timer_get_time());
  }
  rotate_started_us=esp_timer_get_time();
  if(logical_unchanged){
    /* Keep the proven LCD reference's pointer AND index. The unused next
     * buffer has not been written and must never become the next reference. */
    g_native_framebuffer_index=previous_native_index;
    g_native_framebuffer=g_native_framebuffers[previous_native_index];
  }else
#endif
  {
    esp_err_t rotate_err=rotate_landscape_to_panel();
    if(rotate_err!=ESP_OK){
#if PET_P4_H264_WORKER_PACK
      h264_frame_offer_end(false);
#endif
      return rotate_err;
    }
  }
  int64_t lcd_started_us = esp_timer_get_time();
  uint32_t rotate_us = elapsed_us_clamped(rotate_started_us, lcd_started_us);
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  g_last_compare_us=0;memset(&g_last_lcd_observation,0,sizeof(g_last_lcd_observation));
#endif
  bool unchanged =
#if PET_P4_LOGICAL_COMPARE
    logical_unchanged||
#endif
    pet_p4_frame_unchanged(g_native_framebuffer, previous_native,
    (size_t) BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t), previous_valid);
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  g_last_compare_valid=true;g_last_compare_us=pet_lcd_elapsed(lcd_started_us,esp_timer_get_time(),&g_last_compare_valid);
  esp_err_t lcd_err=unchanged?ESP_OK:pet_p4_lcd_draw_rgb565_observed(0,0,BSP_LCD_H_RES,BSP_LCD_V_RES,g_native_framebuffer,&g_last_lcd_observation);
#else
  esp_err_t lcd_err = unchanged ? ESP_OK : pet_p4_lcd_draw_rgb565(
    0,
    0,
    BSP_LCD_H_RES,
    BSP_LCD_V_RES,
    g_native_framebuffer
  );
#endif
  g_last_native_valid = lcd_err == ESP_OK;
#if PET_P4_APP_RETAIN_LOGICAL
  pet_logical_retention_finish(&g_app_final_retention,g_app_common_plan,
    lcd_err==ESP_OK&&g_app_final_eligible&&!approval_visible&&!strcmp(page,"app"));
#endif
  if (unchanged) g_lcd_skipped++;
  else if (lcd_err == ESP_OK) g_lcd_submitted++;
  int64_t render_ended_us = esp_timer_get_time();
  g_render_trace.end_us = (uint64_t)render_ended_us;
  g_previous_render_ended_us=render_ended_us;
  ppa_trace_frame_snapshot(&g_render_trace.ppa);
  pet_widget_trace_snapshot(&operation_snapshot);
  pet_p4_render_trace_observe(&g_render_trace, &operation_snapshot, true);
  record_render_performance(
    render_started_us,
    render_ended_us,
    asset_us,
    overlay_us,
    rotate_us,
    elapsed_us_clamped(lcd_started_us, render_ended_us)
  );
#if PET_P4_H264_WORKER_PACK
  /* A new QUEUED decode may start only after the full frame was submitted and
   * recorded. Its scheduling cost remains in the unmodified next-frame gap. */
  h264_frame_offer_end(lcd_err==ESP_OK);
#endif
  return lcd_err;
}

esp_err_t pet_p4_renderer_render_transfer_status(
  bool firmware_update,
  unsigned long long now_ms
) {
  /* Entry reset also covers initialization failures; no lifetime/window reset. */
  pet_p4_render_trace_begin(&g_render_trace, g_render_trace_frame, "transfer");
  /* Transfer-only drawing intentionally replaces the normal renderer cadence. */
  g_previous_render_ended_us=0;
  (void)renderer_component_media_frame(NULL); /* poll old clip cleanup on transfer frames too */
  cancel_pending_asset_prepares();
  ESP_RETURN_ON_ERROR(pet_p4_renderer_init(), TAG, "allocate transfer render buffers");
  g_last_native_valid = false; /* transfer renders bypass normal frame gating */
  g_native_framebuffer_index =
    (g_native_framebuffer_index + 1U) % PET_P4_NATIVE_OUTPUT_BUFFER_COUNT;
  g_native_framebuffer = g_native_framebuffers[g_native_framebuffer_index];

  const uint16_t background = rgb565(5, 7, 6);
  const uint16_t panel = rgb565(14, 18, 16);
  const uint16_t outline = rgb565(52, 58, 53);
  const uint16_t orange = rgb565(255, 163, 31);
  const uint16_t ivory = rgb565(250, 241, 204);
  const uint16_t muted = rgb565(126, 133, 126);
  const char *title = firmware_update ? "固件更新中" : "形象同步中";
  const char *detail = firmware_update
    ? "正在写入并校验新固件"
    : "正在传输并校验形象素材";

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, background);
  fill_round_rect_outline(64, 102, 512, 276, 20, panel, outline);
  fill_ellipse(320, 170, 30, 30, orange);
  fill_ellipse(320, 170, 15, 15, panel);
  unsigned int active_dot = (unsigned int) ((now_ms / 300ULL) % 3ULL);
  for (unsigned int index = 0; index < 3; index += 1) {
    int dot_radius = index == active_dot ? 6 : 3;
    fill_ellipse(
      292 + (int) index * 28,
      170,
      dot_radius,
      dot_radius,
      index == active_dot ? ivory : muted
    );
  }
  draw_text_center(title, 320, 222, 440, ivory, 3);
  draw_text_center(detail, 320, 278, 440, muted, 1);
  draw_text_center("请保持设备连接，不要拔出数据线", 320, 322, 440, orange, 1);
  snprintf(g_last_render_page, sizeof(g_last_render_page), "%s", "transfer");
  ESP_RETURN_ON_ERROR(rotate_landscape_to_panel(), TAG, "rotate transfer framebuffer");
  return pet_p4_lcd_draw_rgb565(
    0,
    0,
    BSP_LCD_H_RES,
    BSP_LCD_V_RES,
    g_native_framebuffer
  );
}

const uint16_t *pet_p4_renderer_logical_framebuffer(int *width, int *height) {
  if (width) *width = PET_P4_UI_WIDTH;
  if (height) *height = PET_P4_UI_HEIGHT;
  return g_framebuffer;
}
