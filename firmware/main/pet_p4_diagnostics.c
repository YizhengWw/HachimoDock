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
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
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
#include "pet_p4_renderer.h"
#include "pet_p4_worker_locked.h"
#include "pet_p4_storage_policy.h"
#include "pet_p4_spiffs_hint.h"
#include "pet_p4_reset_phase.h"
#include "pet_p4_native_panic.h"
#include "pet_p4_reset_cost.h"
#include "pet_p4_boot_trace.h"
#include "pet_p4_boot_trace_json.h"
#include "pet_p4_boot_dma_json.h"
#include "pet_p4_flash_phase.h"
#include "pet_p4_save_stack_json.h"
#include "pet_p4_storage_snapshot.h"
#include "esp_attr.h"

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
/* SDK's ESP_RST_WDT combines several ROM causes. Read-only numeric evidence,
 * not a register dump or proof of which task/critical section caused a reset. */
static uint32_t g_raw_reset_reason[2];
static unsigned long long g_reboot_at_ms;
static atomic_bool g_reboot_pending;
static SemaphoreHandle_t g_diagnostics_mutex;
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
static DRAM_ATTR pet_storage_snapshot_t g_storage_snapshot;
static DRAM_ATTR portMUX_TYPE g_storage_snapshot_mux=portMUX_INITIALIZER_UNLOCKED;
#endif

void pet_p4_diagnostics_storage_refresh(void){
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
  /* Boot runs before the save worker exists; afterwards ONLY that worker
   * calls this function. FS metadata is read outside every platform lock. */
  if(pet_p4_audio_active()||pet_p4_audio_playback_active()
      ||pet_p4_audio_conversation_active()||pet_p4_audio_music_active())return;
  size_t total=0,used=0;
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(storage_info_phase,PET_PHASE_DIAGNOSTIC_FS_INFO);
#endif
  esp_err_t result=esp_spiffs_info("storage",&total,&used);
  int64_t sampled=esp_timer_get_time();
  uint64_t sampled_ms=sampled>=0?(uint64_t)sampled/1000:0;
  portENTER_CRITICAL(&g_storage_snapshot_mux);
  (void)pet_storage_snapshot_publish(&g_storage_snapshot,total,used,
    sampled_ms,sampled>=0?result:ESP_ERR_INVALID_STATE);
  portEXIT_CRITICAL(&g_storage_snapshot_mux);
#endif
}

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

static char *prepare_topic(
  const char *topic,
  cJSON *payload
) {
  cJSON *root = cJSON_CreateObject();
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return NULL;
  }
  if (!cJSON_AddStringToObject(root, "topic", topic)
      || !cJSON_AddItemToObject(root, "payload", payload)) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return NULL;
  }
  char *line = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return line;
}

static void send_topic(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *topic,
  cJSON *payload
) {
  char *line = prepare_topic(topic, payload);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
}

static void add_request_id(cJSON *response, const cJSON *request) {
  const cJSON *request_id = request
    ? cJSON_GetObjectItemCaseSensitive(request, "requestId")
    : NULL;
  if (!response || !request_id) return;
  cJSON *copy = cJSON_Duplicate(request_id, true);
  if (copy) cJSON_AddItemToObject(response, "requestId", copy);
}

static char *prepare_status(
  const cJSON *request,
  const pet_p4_runtime_state_t *state
) {
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(diagnostic_alloc_phase,PET_PHASE_DIAGNOSTIC_ALLOC);
#endif
  size_t storage_total = 0;
  size_t storage_used = 0;
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_app_desc_t *app = esp_app_get_description();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_DIAGNOSTIC_IMAGE_STATE);
#endif
#if PET_P4_OTA_METADATA_CACHED
  esp_err_t image_state_err=pet_p4_ota_observed_image_state(&image_state);
#else
  esp_err_t image_state_err = running
    ? esp_ota_get_state_partition(running, &image_state)
    : ESP_ERR_NOT_FOUND;
#endif
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_DIAGNOSTIC_ALLOC);
#endif
  cJSON *payload = cJSON_CreateObject();
  cJSON *memory = cJSON_CreateObject();
  cJSON *storage = cJSON_CreateObject();
  cJSON *runtime = cJSON_CreateObject();
  if (!payload || !memory || !storage || !runtime) {
    cJSON_Delete(payload);
    cJSON_Delete(memory);
    cJSON_Delete(storage);
    cJSON_Delete(runtime);
    return NULL;
  }

#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
  pet_storage_snapshot_t copied;
  portENTER_CRITICAL(&g_storage_snapshot_mux);copied=g_storage_snapshot;portEXIT_CRITICAL(&g_storage_snapshot_mux);
  int64_t observed_us=esp_timer_get_time();
  pet_storage_snapshot_view_t storage_view=pet_storage_snapshot_observe(copied,observed_us>=0?(uint64_t)observed_us/1000:0);
  storage_total=copied.total;storage_used=copied.used;
#else
  {
#if PET_P4_RESET_PHASE_TRACE
    PET_RESET_PHASE_SCOPE(storage_info_phase,PET_PHASE_DIAGNOSTIC_FS_INFO);
#endif
    (void) esp_spiffs_info("storage", &storage_total, &storage_used);
  }
#endif
  add_request_id(payload, request);
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "");
  cJSON_AddNumberToObject(payload, "bootCount", g_boot_record.boot_count);
  cJSON_AddNumberToObject(payload, "faultResetCount", g_boot_record.fault_reset_count);
  cJSON_AddNumberToObject(payload, "lastResetReasonCode", g_boot_record.last_reset_reason);
  cJSON_AddNumberToObject(payload, "lastResetRawReasonCpu0", g_raw_reset_reason[0]);
  cJSON_AddNumberToObject(payload, "lastResetRawReasonCpu1", g_raw_reset_reason[1]);
/* Opt-in immutable early-boot metadata. No runtime MMIO, reset or NVS IO. */
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"includeBootDma"))){
    pet_boot_dma_snapshot_t boot_dma={0};bool observed=pet_boot_dma_observe(&boot_dma);
    (void)pet_boot_dma_add_json(payload,&boot_dma,observed);
  }
#if PET_P4_BOOT_TRACE_ENABLE
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"includeBootTrace"))){
    pet_boot_trace_t boot={0};bool observed=pet_boot_trace_observe(&boot);
    (void)pet_boot_trace_add_json(payload,&boot,observed);
  }
#endif
#if PET_P4_RESET_PHASE_BENCHMARK
  /* Explicit private request only: ordinary polling performs no allocation
   * or sampling here. The immutable result was measured once at native boot. */
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"includeRecorderCost"))){
    pet_reset_cost_t cost={0};bool available=pet_reset_cost_observe(&cost);
    cJSON *object=cJSON_AddObjectToObject(payload,"resetRecorderCost");
    if(object){
      cJSON_AddBoolToObject(object,"available",available);
      cJSON_AddBoolToObject(object,"valid",cost.valid);
      cJSON_AddNumberToObject(object,"samples",cost.samples);
      cJSON_AddNumberToObject(object,"clockHz",cost.clock_hz);
      cJSON_AddNumberToObject(object,"totalCycles",(double)cost.total_cycles);
      cJSON_AddNumberToObject(object,"minCycles",cost.min_cycles);
      cJSON_AddNumberToObject(object,"maxCycles",cost.max_cycles);
      cJSON_AddNumberToObject(object,"totalWallUs",(double)cost.total_wall_us);
      cJSON_AddNumberToObject(object,"minWallUs",cost.min_wall_us);
      cJSON_AddNumberToObject(object,"maxWallUs",cost.max_wall_us);
      cJSON_AddBoolToObject(object,"includesPreemption",true);
    }
  }
#endif
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_record_snapshot_t retained_snapshot={0};uint32_t retained_errors=0;
  bool retained_available=pet_reset_phase_previous(&retained_snapshot,&retained_errors);
#if PET_P4_FLASH_PHASE_TRACE
  cJSON_AddNumberToObject(payload,"nativeErasePhaseCoverage",pet_flash_phase_coverage());
#endif
  cJSON *retained_trace=cJSON_AddObjectToObject(payload,"previousResetTrace");
  if(retained_trace){
    cJSON_AddBoolToObject(retained_trace,"available",retained_available);
    cJSON_AddBoolToObject(retained_trace,"lastValidOnly",true);
    cJSON_AddBoolToObject(retained_trace,"authentication",false);
    cJSON_AddNumberToObject(retained_trace,"storageBytes",sizeof(pet_reset_record_storage_t));
    cJSON_AddBoolToObject(retained_trace,"headerValid",retained_snapshot.header_valid);
    cJSON_AddBoolToObject(retained_trace,"sameBuild",retained_snapshot.same_build);
    cJSON_AddNumberToObject(retained_trace,"validMask",retained_snapshot.valid_mask);
    cJSON_AddNumberToObject(retained_trace,"saturatedMask",retained_snapshot.saturated_mask);
    cJSON_AddNumberToObject(retained_trace,"currentErrors",retained_errors);
    cJSON *records=cJSON_AddArrayToObject(retained_trace,"actors");
    if(records)for(unsigned actor=0;actor<PET_RESET_ACTORS;actor++){
      if(!(retained_snapshot.valid_mask&(1U<<actor)))continue;
      const pet_reset_phase_record_t *r=&retained_snapshot.records[actor];
      cJSON *item=cJSON_CreateObject();if(!item)break;
      cJSON_AddNumberToObject(item,"actor",actor);
      cJSON_AddNumberToObject(item,"phase",r->phase_core&255U);
      cJSON_AddNumberToObject(item,"core",(r->phase_core>>8)&255U);
      cJSON_AddNumberToObject(item,"cycles",r->cycles);
      cJSON_AddNumberToObject(item,"sequence",r->sequence);
      if(!cJSON_AddItemToArray(records,item)){cJSON_Delete(item);break;}
    }
  }
#endif
#if PET_P4_NATIVE_PANIC_TRACE
  pet_native_panic_snapshot_t native_snapshot={0};
  bool native_available=pet_native_panic_previous(&native_snapshot);
  cJSON *native_trace=cJSON_AddObjectToObject(payload,"previousNativePanic");
  if(native_trace){
    cJSON_AddBoolToObject(native_trace,"available",native_available);
    cJSON_AddBoolToObject(native_trace,"sameBuild",native_snapshot.same_build);
    cJSON_AddBoolToObject(native_trace,"authentication",false);
    cJSON_AddNumberToObject(native_trace,"validMask",native_snapshot.valid_mask);
    cJSON_AddNumberToObject(native_trace,"storageBytes",144);
    cJSON *sites=cJSON_AddArrayToObject(native_trace,"sites");
    if(sites)for(unsigned core=0;core<2;core++){
      if(!(native_snapshot.valid_mask&(1U<<core)))continue;
      const pet_native_panic_site_t *site=&native_snapshot.sites[core];
      cJSON *item=cJSON_CreateObject();if(!item)break;
      cJSON_AddNumberToObject(item,"core",site->core);
      cJSON_AddNumberToObject(item,"pc",site->pc);
      cJSON_AddNumberToObject(item,"returnPc",site->return_pc);
      cJSON_AddNumberToObject(item,"cause",site->cause);
      cJSON_AddNumberToObject(item,"exception",site->exception);
      cJSON_AddNumberToObject(item,"flags",site->flags);
      if(!cJSON_AddItemToArray(sites,item)){cJSON_Delete(item);break;}
    }
  }
#endif
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
  cJSON_AddNumberToObject(memory, "freeInternalBytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(memory, "largestInternalBlockBytes", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(memory, "minimumFreeInternalBytes", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(memory, "freeDmaBytes", heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(memory, "largestDmaBlockBytes", heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(memory, "largestPsramBlockBytes", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  cJSON_AddNumberToObject(
    memory,
    "minimumFreePsramBytes",
    heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)
  );
  cJSON_AddItemToObject(payload, "memory", memory);

  cJSON_AddNumberToObject(storage, "totalBytes", (double) storage_total);
  cJSON_AddNumberToObject(storage, "usedBytes", (double) storage_used);
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
  cJSON_AddBoolToObject(storage,"statisticsCached",true);
  cJSON_AddBoolToObject(storage,"statisticsAvailable",copied.available);
  cJSON_AddBoolToObject(storage,"statisticsStale",storage_view.stale);
  cJSON_AddBoolToObject(storage,"statisticsClockInvalid",storage_view.clock_invalid||observed_us<0);
  cJSON_AddNumberToObject(storage,"statisticsAgeMs",(double)storage_view.age_ms);
  cJSON_AddNumberToObject(storage,"statisticsRefreshError",copied.last_error);
#endif
  cJSON_AddNumberToObject(storage, "spiffsMaxFiles", PET_P4_SPIFFS_MAX_FILES);
#if PET_P4_SPIFFS_HINT_ENABLE
  pet_spiffs_hint_stats_t hint;pet_spiffs_hint_stats(&hint);
  cJSON *lookup=cJSON_CreateObject();
  if(lookup){
    cJSON_AddBoolToObject(lookup,"applying",PET_P4_SPIFFS_HINT_APPLY);
    cJSON_AddNumberToObject(lookup,"calls",hint.calls);
    cJSON_AddNumberToObject(lookup,"candidates",hint.candidates);
    cJSON_AddNumberToObject(lookup,"applied",hint.applied);
    cJSON_AddNumberToObject(lookup,"positives",hint.positives);
    cJSON_AddNumberToObject(lookup,"negatives",hint.negatives);
    cJSON_AddNumberToObject(lookup,"errors",hint.errors);
    cJSON_AddNumberToObject(lookup,"positiveWallUs",(double)hint.positive_us);
    cJSON_AddNumberToObject(lookup,"negativeWallUs",(double)hint.negative_us);
    cJSON_AddNumberToObject(lookup,"errorWallUs",(double)hint.error_us);
    cJSON_AddNumberToObject(lookup,"maximumWallUs",hint.maximum_us);
    cJSON_AddItemToObject(storage,"lookupHint",lookup);
  }
#endif
  cJSON_AddNumberToObject(storage, "activeAppearanceSlot", pet_p4_asset_active_slot());
  cJSON_AddItemToObject(payload, "storage", storage);

  cJSON_AddNumberToObject(
    runtime,
    "uptimeMs",
    (double) ((unsigned long long) esp_timer_get_time() / 1000ULL)
  );
  cJSON_AddNumberToObject(runtime, "taskCount", uxTaskGetNumberOfTasks());
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
  cJSON *save_stack=pet_app_save_stack_json();
  if(save_stack&&!cJSON_AddItemToObject(runtime,"saveWorkerStack",save_stack))cJSON_Delete(save_stack);
#endif
  pet_p4_render_metrics_t render = pet_p4_renderer_metrics();
  cJSON_AddNumberToObject(runtime, "renderLastUs", render.last_us);
  cJSON_AddNumberToObject(runtime, "renderP95Us", render.p95_us);
  cJSON_AddNumberToObject(runtime, "renderMaxUs", render.max_us);
  cJSON_AddNumberToObject(runtime, "renderDecodeUs", render.last_decode_us);
  cJSON_AddNumberToObject(runtime, "renderWindowSamples", render.window_samples);
  cJSON_AddNumberToObject(runtime, "renderSubmittedFrames", render.submitted_frames);
  cJSON_AddNumberToObject(runtime, "renderUnchangedFrames", render.unchanged_frames);
#if PET_P4_H264_PREFETCH
  cJSON *prefetch=cJSON_AddObjectToObject(runtime,"nativePrefetch");
  if(prefetch){
    cJSON_AddNumberToObject(prefetch,"jobs",render.prefetch.jobs);
    cJSON_AddNumberToObject(prefetch,"completed",render.prefetch.completed);
    cJSON_AddNumberToObject(prefetch,"cancelled",render.prefetch.cancelled);
    cJSON_AddNumberToObject(prefetch,"decodedFrames",render.prefetch.decoded_frames);
    cJSON_AddNumberToObject(prefetch,"presentedFrames",render.prefetch.presented_frames);
    cJSON_AddNumberToObject(prefetch,"lastWorkerUs",render.prefetch.last_worker_us);
    cJSON_AddNumberToObject(prefetch,"maxWorkerUs",render.prefetch.max_worker_us);
    cJSON_AddNumberToObject(prefetch,"lastDecodeUs",render.prefetch.last_decode_us);
    cJSON_AddNumberToObject(prefetch,"maxDecodeUs",render.prefetch.max_decode_us);
#if PET_P4_H264_WORKER_PACK
    cJSON_AddNumberToObject(prefetch,"lastOfferUs",render.prefetch.last_offer_us);
    cJSON_AddNumberToObject(prefetch,"maxOfferUs",render.prefetch.max_offer_us);
#endif
    cJSON_AddNumberToObject(prefetch,"maxReadyLatencyUs",render.prefetch.max_ready_latency_us);
    cJSON_AddNumberToObject(prefetch,"minWorkerFreeBytes",render.prefetch.min_worker_free_bytes);
  }
#endif
  cJSON *render_lifetime=cJSON_AddObjectToObject(runtime,"renderLifetime");
  if(render_lifetime){
    const pet_render_lifetime_t *m=&render.lifetime;
    cJSON_AddNumberToObject(render_lifetime,"samples",m->samples);
    cJSON_AddNumberToObject(render_lifetime,"maxUs",m->max_us);
    cJSON_AddNumberToObject(render_lifetime,"maxGapUs",m->max_gap_us);
    cJSON_AddNumberToObject(render_lifetime,"overBudget",m->over_budget);
    cJSON_AddNumberToObject(render_lifetime,"gapOverBudget",m->gap_over_budget);
    cJSON_AddNumberToObject(render_lifetime,"invalidClock",m->invalid_clock);
    cJSON_AddNumberToObject(render_lifetime,"peakFrame",m->peak_frame);
    cJSON_AddNumberToObject(render_lifetime,"peakPage",m->peak_page);
    cJSON_AddNumberToObject(render_lifetime,"peakStartUs",(double)m->peak_start_us);
    cJSON_AddNumberToObject(render_lifetime,"peakEndUs",(double)m->peak_end_us);
    cJSON_AddNumberToObject(render_lifetime,"gapPeakFrame",m->gap_peak_frame);
    cJSON_AddNumberToObject(render_lifetime,"windowResets",m->window_resets);
    cJSON_AddNumberToObject(render_lifetime,"completedWindows",m->completed_windows);
    cJSON_AddNumberToObject(render_lifetime,"windowEndFrame",m->window_end_frame);
#if PET_P4_APP_CLEAR_OPAQUE_CENTER
    cJSON_AddNumberToObject(render_lifetime,"clearCenterFrames",m->clear_center_frames);
    cJSON_AddNumberToObject(render_lifetime,"clearFullFrames",m->clear_full_frames);
#endif
#if PET_P4_APP_COLD_FAST
    cJSON *cold=cJSON_AddObjectToObject(render_lifetime,"coldFast");
    if(cold){cJSON_AddNumberToObject(cold,"imageNodes",m->cold_fast_image_nodes);cJSON_AddNumberToObject(cold,"panelNodes",m->cold_fast_panel_nodes);cJSON_AddNumberToObject(cold,"clearSkips",m->cold_fast_clear_skips);}
#endif
#if PET_P4_APP_PPA_COPY
    cJSON *ppa=cJSON_AddObjectToObject(render_lifetime,"ppaCopy");
    if(ppa){cJSON_AddNumberToObject(ppa,"copies",m->ppa_copies);cJSON_AddNumberToObject(ppa,"fallbacks",m->ppa_copy_fallbacks);cJSON_AddNumberToObject(ppa,"mismatches",m->ppa_copy_mismatches);}
#endif
#if PET_P4_H264_WORKER_PACK
    cJSON *wp=cJSON_AddObjectToObject(render_lifetime,"workerPack");
    if(wp){cJSON_AddNumberToObject(wp,"frames",m->worker_packs);cJSON_AddNumberToObject(wp,"fallbacks",m->worker_pack_fallbacks);cJSON_AddNumberToObject(wp,"maxUs",m->worker_pack_max_us);cJSON_AddNumberToObject(wp,"lastUs",m->worker_pack_last_us);}
#endif
#if PET_P4_GAP_PEAK_CONTEXT
    cJSON *gap=cJSON_AddObjectToObject(render_lifetime,"gapPeak");
    if(gap){
      const pet_render_gap_context_t *g=&m->gap_peak;
      cJSON_AddNumberToObject(gap,"gapUs",g->gap_us);cJSON_AddNumberToObject(gap,"frame",g->frame);
      cJSON_AddNumberToObject(gap,"waitMaxUs",g->wait_max_us);cJSON_AddNumberToObject(gap,"heldMaxUs",g->held_max_us);
      cJSON_AddNumberToObject(gap,"miniappMaxUs",g->miniapp_max_us);cJSON_AddNumberToObject(gap,"heldTotalUs",g->held_total_us);
      cJSON_AddNumberToObject(gap,"iterations",g->iterations);
      cJSON_AddNumberToObject(gap,"lane0Phase",g->lane0_phase);cJSON_AddNumberToObject(gap,"lane1Phase",g->lane1_phase);
#if PET_P4_GAP_PEAK_HOLDER
      cJSON_AddNumberToObject(gap,"atUs",(double)g->at_us);cJSON_AddStringToObject(gap,"waitHolder",g->wait_holder);
      cJSON_AddNumberToObject(gap,"waitTopicHash",g->wait_topic_hash);
#endif
    }
#endif
#if PET_P4_FRAME_IO_OVERLAP
    cJSON *overlap=cJSON_AddObjectToObject(render_lifetime,"ioOverlap");
    if(overlap){
      cJSON_AddNumberToObject(overlap,"overlapFrames",m->overlap_frames);
      cJSON_AddBoolToObject(overlap,"peakOverlapped",m->peak_overlapped);
      const pet_render_overlap_peak_t *peaks[3]={&m->quiet_peak,&m->overlap_peak,&m->quiet_app_peak};
      const char *names[3]={"quietPeak","overlapPeak","quietAppPeak"};
      for(unsigned i=0;i<3;i++){
        cJSON *peak=cJSON_AddObjectToObject(overlap,names[i]);
        if(!peak)continue;
        cJSON_AddNumberToObject(peak,"maxUs",peaks[i]->max_us);cJSON_AddNumberToObject(peak,"frame",peaks[i]->frame);
        cJSON_AddNumberToObject(peak,"appDrawUs",peaks[i]->app_draw_us);cJSON_AddNumberToObject(peak,"rotateUs",peaks[i]->rotate_us);
        cJSON_AddNumberToObject(peak,"lcdUs",peaks[i]->lcd_us);cJSON_AddNumberToObject(peak,"clearUs",peaks[i]->clear_us);
        cJSON_AddNumberToObject(peak,"nodesUs",peaks[i]->nodes_us);cJSON_AddNumberToObject(peak,"overlayUs",peaks[i]->overlay_us);
      }
    }
#endif
#if PET_P4_RETAIN_PEAK_PHASES
    cJSON *p=cJSON_AddObjectToObject(render_lifetime,"sameFramePhases");
    if(p){
      cJSON_AddNumberToObject(p,"assetUs",m->asset_us);cJSON_AddNumberToObject(p,"decodeUs",m->decode_us);
      cJSON_AddNumberToObject(p,"packUs",m->pack_us);cJSON_AddNumberToObject(p,"convertUs",m->convert_us);
      cJSON_AddNumberToObject(p,"overlayUs",m->overlay_us);cJSON_AddNumberToObject(p,"rotateUs",m->rotate_us);
      cJSON_AddNumberToObject(p,"lcdUs",m->lcd_us);cJSON_AddNumberToObject(p,"contentUs",m->content_us);
      /* Same winning frame; inclusive nested durations are not additive. */
      cJSON *content=cJSON_AddObjectToObject(p,"contentStagesInclusive");
      if(content){
        cJSON_AddNumberToObject(content,"bubbleSelectUs",m->bubble_select_us);
        cJSON_AddNumberToObject(content,"pinnedBlitUs",m->pinned_blit_us);
        cJSON_AddNumberToObject(content,"bubbleDrawUs",m->bubble_draw_us);
        cJSON_AddNumberToObject(content,"sessionCacheFillUs",m->session_cache_fill_us);
        cJSON_AddNumberToObject(content,"sessionCacheDrawUs",m->session_cache_draw_us);
        cJSON_AddNumberToObject(content,"sessionCacheScanUs",m->session_cache_scan_us);
        cJSON_AddNumberToObject(content,"sessionCacheCopyUs",m->session_cache_copy_us);
        cJSON_AddNumberToObject(content,"voiceUs",m->voice_us);
      }
      cJSON_AddNumberToObject(p,"commonUs",m->common_us);cJSON_AddNumberToObject(p,"resolveUs",m->resolve_us);
      cJSON_AddNumberToObject(p,"cacheUs",m->cache_us);cJSON_AddNumberToObject(p,"scaleUs",m->scale_us);
#if PET_P4_LOGICAL_COMPARE
      cJSON_AddNumberToObject(p,"logicalCompareUs",m->logical_compare_us);
#if PET_P4_APP_RETAIN_LOGICAL
      cJSON_AddBoolToObject(p,"logicalRetained",m->logical_retained);
#endif
#endif
#if PET_P4_APP_PHASE_TRACE
      cJSON_AddNumberToObject(p,"appSnapshotUs",m->app_snapshot_us);cJSON_AddNumberToObject(p,"appSnapshotWaitUs",m->app_snapshot_wait_us);
      cJSON_AddNumberToObject(p,"appDrawUs",m->app_draw_us);
#endif
#if PET_P4_APP_NODE_TRACE
      cJSON *nodes=cJSON_AddObjectToObject(p,"appNodes");
      if(nodes){
        const pet_app_draw_trace_t *n=&m->app_nodes;
        cJSON_AddNumberToObject(nodes,"imagesUs",n->images_us);cJSON_AddNumberToObject(nodes,"keyUs",n->key_us);
        cJSON_AddNumberToObject(nodes,"cacheCopyUs",n->cache_copy_us);cJSON_AddNumberToObject(nodes,"prefixUs",n->prefix_us);
        cJSON_AddNumberToObject(nodes,"clearUs",n->clear_us);cJSON_AddNumberToObject(nodes,"rememberUs",n->remember_us);
        cJSON_AddNumberToObject(nodes,"mask",n->nodes_mask);cJSON_AddNumberToObject(nodes,"cacheHit",n->cache_hit);
        /* Keep the legacy number; split words preserve all bits through JSON doubles. */
        cJSON_AddNumberToObject(nodes,"maskLow",(uint32_t)n->nodes_mask);
        cJSON_AddNumberToObject(nodes,"maskHigh",(uint32_t)(n->nodes_mask>>32));
        cJSON_AddNumberToObject(nodes,"cacheDeferred",n->cache_deferred);cJSON_AddNumberToObject(nodes,"invalidClock",n->invalid_clock);
        cJSON *costs=cJSON_AddArrayToObject(nodes,"nodeUs");
        if(costs)for(unsigned i=0;i<PET_APP_TRACE_NODES;i++)cJSON_AddItemToArray(costs,cJSON_CreateNumber(n->nodes_us[i]));
      }
#endif
#if PET_P4_RENDER_LCD_SPLIT_PROBE
      cJSON *lcd=cJSON_AddObjectToObject(p,"lcdObservation");
      if(lcd){
        cJSON_AddNumberToObject(lcd,"compareUs",m->compare_us);cJSON_AddBoolToObject(lcd,"compareValid",m->compare_valid);
        cJSON_AddNumberToObject(lcd,"submitUs",m->lcd_submit_us);cJSON_AddNumberToObject(lcd,"waitUs",m->lcd_wait_us);
        cJSON_AddBoolToObject(lcd,"valid",m->lcd_valid);cJSON_AddBoolToObject(lcd,"submitted",m->lcd_submitted);
        cJSON_AddBoolToObject(lcd,"waited",m->lcd_waited);
      }
#endif
    }
#endif
  }
  cJSON_AddNumberToObject(runtime, "inputDroppedEvents", pet_p4_input_dropped_events());
#if PET_PROGRAM_PHASE_TRACE
  pet_program_phase_t program_peak={0};
  if(pet_p4_miniapp_program_peak(&program_peak)){
    cJSON *p=cJSON_AddObjectToObject(runtime,"programParentPeak");
    if(p){
      cJSON_AddNumberToObject(p,"startUs",(double)program_peak.start_us);cJSON_AddNumberToObject(p,"endUs",(double)program_peak.end_us);
      cJSON_AddNumberToObject(p,"parentUs",program_peak.parent_us);cJSON_AddNumberToObject(p,"schedulerUs",program_peak.scheduler_us);
      cJSON_AddNumberToObject(p,"registryUs",program_peak.registry_us);
      cJSON_AddNumberToObject(p,"collectUs",program_peak.collect_us);cJSON_AddNumberToObject(p,"captureUs",program_peak.capture_us);
      cJSON_AddNumberToObject(p,"collected",program_peak.collected);cJSON_AddNumberToObject(p,"invalidClock",program_peak.invalid_clock);
#if PET_P4_QUERY_OBSERVATION
      pet_p4_query_observation_t query={0};pet_widget_trace_lane_snapshot_t save={0};
      if(pet_p4_miniapp_program_peak_context(&query,&save)){
        cJSON *q=cJSON_AddObjectToObject(p,"queryAtPeak"),*s=cJSON_AddObjectToObject(p,"saveAtPeak");
        if(q){
          cJSON_AddNumberToObject(q,"sequence",(double)query.sequence);cJSON_AddNumberToObject(q,"beginUs",(double)query.begin_us);
          cJSON_AddNumberToObject(q,"unlockedUs",(double)query.unlocked_us);cJSON_AddNumberToObject(q,"endUs",(double)query.end_us);
          cJSON_AddBoolToObject(q,"active",query.active);cJSON_AddBoolToObject(q,"invalidClock",query.invalid_clock);
          cJSON_AddNumberToObject(q,"nativeKind",query.native_kind);
        }
        if(s){
          cJSON_AddNumberToObject(s,"sequence",(double)save.seq);cJSON_AddNumberToObject(s,"startUs",(double)save.start_us);
          cJSON_AddNumberToObject(s,"completedSequence",(double)save.last_completed_seq);
          cJSON_AddNumberToObject(s,"completedStartUs",(double)save.last_start_us);cJSON_AddNumberToObject(s,"completedEndUs",(double)save.last_end_us);
          cJSON_AddNumberToObject(s,"stage",save.current_stage);cJSON_AddBoolToObject(s,"active",save.active);
        }
      }
#endif
    }
  }
#endif
#if PET_P4_QUERY_REPLY_TRACE
  pet_p4_query_timing_t query_timing = {0};
  if (pet_p4_protocol_query_timing(&query_timing)) {
    cJSON *q = cJSON_AddObjectToObject(runtime, "previousDiagnosticQuery");
    if (q) {
      cJSON_AddNumberToObject(q,"sequence",(double)query_timing.sequence);
      cJSON_AddNumberToObject(q,"beginUs",(double)query_timing.begin_us);
      cJSON_AddNumberToObject(q,"endUs",(double)query_timing.end_us);
      cJSON_AddNumberToObject(q,"lockWaitUs",query_timing.lock_wait_us);
      cJSON_AddNumberToObject(q,"stateHeldUs",query_timing.state_held_us);
      cJSON_AddNumberToObject(q,"postUnlockTxUs",query_timing.post_unlock_tx_us);
      cJSON_AddNumberToObject(q,"maxLockWaitUs",query_timing.max_lock_wait_us);
      cJSON_AddNumberToObject(q,"maxStateHeldUs",query_timing.max_state_held_us);
      cJSON_AddNumberToObject(q,"maxPostUnlockTxUs",query_timing.max_post_unlock_tx_us);
      cJSON_AddBoolToObject(q,"detached",query_timing.detached);
      cJSON_AddBoolToObject(q,"invalidClock",query_timing.invalid_clock);
    }
  }
#endif
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
  pet_p4_touch_health_t touch_health = {0};
  pet_p4_touch_health(&touch_health);
  cJSON *touch = cJSON_AddObjectToObject(runtime, "touchHealth");
  if (touch) {
    cJSON_AddStringToObject(touch, "stage", touch_health.stage);
    cJSON_AddStringToObject(touch, "controller", pet_p4_touch_controller());
    cJSON_AddNumberToObject(touch, "chipId", touch_health.chip_id);
    cJSON_AddNumberToObject(touch, "vendorId", touch_health.vendor_id);
    cJSON_AddStringToObject(touch, "error", esp_err_to_name(touch_health.error));
    cJSON_AddNumberToObject(touch, "address", touch_health.address);
    cJSON_AddStringToObject(touch, "primaryProbe", esp_err_to_name(touch_health.primary_error));
    cJSON_AddStringToObject(touch, "backupProbe", esp_err_to_name(touch_health.backup_error));
    cJSON_AddStringToObject(touch, "probe0x15", esp_err_to_name(touch_health.probe_15));
    cJSON_AddStringToObject(touch, "probe0x38", esp_err_to_name(touch_health.probe_38));
    cJSON_AddStringToObject(touch, "probe0x10", esp_err_to_name(touch_health.probe_10));
    cJSON_AddStringToObject(touch, "probe0x18", esp_err_to_name(touch_health.probe_18));
    cJSON_AddNumberToObject(touch, "samples", touch_health.samples);
    cJSON_AddNumberToObject(touch, "contacts", touch_health.contacts);
    cJSON_AddNumberToObject(touch, "panelX", touch_health.panel_x);
    cJSON_AddNumberToObject(touch, "panelY", touch_health.panel_y);
    cJSON_AddNumberToObject(touch, "uiX", touch_health.ui_x);
    cJSON_AddNumberToObject(touch, "uiY", touch_health.ui_y);
    cJSON_AddNumberToObject(touch, "pointerAgeMs", touch_health.pointer_age_ms);
    cJSON_AddNumberToObject(touch, "staleEvents", touch_health.stale_events);
    cJSON_AddNumberToObject(touch, "pollErrors", touch_health.poll_errors);
    cJSON_AddNumberToObject(touch, "consecutiveErrors", touch_health.consecutive_errors);
    cJSON_AddNumberToObject(touch, "testScore", state ? state->touch_test.score : 0);
  }
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
#if PET_P4_OTA_METADATA_CACHED
  cJSON_AddBoolToObject(runtime,"imageStateCached",true);
#endif
  cJSON_AddStringToObject(runtime, "buildId", PET_P4_BUILD_ID);
  cJSON_AddStringToObject(runtime, "gitSha", PET_P4_BUILD_GIT_SHA);
  cJSON_AddBoolToObject(runtime, "buildDirty", PET_P4_BUILD_DIRTY != 0);
  cJSON_AddNumberToObject(runtime, "protocolSchema", PET_P4_PROTOCOL_SCHEMA);
  cJSON_AddStringToObject(runtime, "projectName", app ? app->project_name : "");
  cJSON_AddStringToObject(runtime, "idfVersion", esp_get_idf_version());
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  cJSON_AddNumberToObject(runtime, "chipRevision", chip_info.revision);
  cJSON_AddStringToObject(runtime, "runningPartition", running ? running->label : "");
  cJSON_AddStringToObject(
    runtime,
    "imageState",
    image_state_err == ESP_OK ? ota_state_name(image_state) : "unavailable"
  );
  cJSON_AddBoolToObject(runtime, "rebootPending", g_reboot_at_ms != 0);
#if PET_P4_GAP_PEAK_HOLDER
  {
    const pet_worker_locked_peak_t *w=pet_p4_worker_locked_peak();
    cJSON *locked=cJSON_AddObjectToObject(runtime,"workerLocked");
    if(locked){
      cJSON_AddNumberToObject(locked,"maxUs",w->max_us);cJSON_AddNumberToObject(locked,"topicHash",w->max_topic_hash);
      cJSON_AddNumberToObject(locked,"atUs",(double)w->max_at_us);cJSON_AddNumberToObject(locked,"spans",w->spans);
      cJSON_AddNumberToObject(locked,"overBudgetSpans",w->over_budget_spans);cJSON_AddNumberToObject(locked,"budgetUs",PET_WORKER_LOCKED_BUDGET_US);
    }
  }
#endif
  cJSON_AddItemToObject(payload, "runtime", runtime);

  return prepare_topic("diagnostics/status", payload);
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
  g_raw_reset_reason[0] = (uint32_t)esp_rom_get_reset_reason(0);
  g_raw_reset_reason[1] = (uint32_t)esp_rom_get_reset_reason(1);
  nvs_handle_t handle;
  size_t size = sizeof(g_boot_record);
  if (!g_diagnostics_mutex) g_diagnostics_mutex = xSemaphoreCreateMutex();
  if (!g_diagnostics_mutex) return ESP_ERR_NO_MEM;
  pet_p4_diagnostics_storage_refresh();
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

bool pet_p4_diagnostics_handle_topic_with_reply(
  const char *topic,
  const cJSON *payload,
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx,
  pet_p4_query_reply_t *reply
) {
  if (!topic || !payload || (reply && reply->line)) return false;
  bool handled = true;
  if (!g_diagnostics_mutex
      || xSemaphoreTake(g_diagnostics_mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (strcmp(topic, "diagnostics/query") == 0) {
    if (reply) reply->diagnostic_query = true;
    char *line = prepare_status(payload, state);
    /* No mutable state or diagnostics lock escapes with this serialization.
     * Allocation failure preserves old no-response semantics, never retries. */
    xSemaphoreGive(g_diagnostics_mutex);
    if (reply && PET_P4_QUERY_REPLY_DEFERRED) reply->line = line;
    else {
      if (line && send_line) send_line(line, ctx);
      cJSON_free(line);
    }
    return true;
  } else if (strcmp(topic, "diagnostics/touch-test") == 0) {
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(payload, "action");
    bool start = cJSON_IsString(op) && !strcmp(op->valuestring, "start");
    bool stop = cJSON_IsString(op) && !strcmp(op->valuestring, "stop");
    bool available = state && !pet_p4_ota_active() && !state->asset_transfer_active
      && (!state->conversation_state[0] || !strcmp(state->conversation_state, "ended"))
      && pet_p4_touch_ready();
    if (stop && state) {
      if (!strcmp(state->screen_page, "touch_test")) snprintf(state->screen_page, sizeof(state->screen_page), "main");
      state->touch_test_until_ms = 0;
      memset(&state->touch_test, 0, sizeof(state->touch_test));
      send_action(payload, send_line, ctx, "touch-test", true, "touch test stopped");
    } else if (start && available) {
      uint64_t now = esp_timer_get_time() / 1000ULL;
      pet_p4_slice_start(&state->touch_test, now);
      state->touch_test_until_ms = now + 120000;
      snprintf(state->screen_page, sizeof(state->screen_page), "touch_test");
      send_action(payload, send_line, ctx, "touch-test", true, "native touch test started (120s)");
    } else send_action(payload, send_line, ctx, "touch-test", false,
      start ? "touch unavailable or device busy; no test started" : "expected start or stop");
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

bool pet_p4_diagnostics_handle_topic(
  const char *topic, const cJSON *payload, pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line, void *ctx
) {
  return pet_p4_diagnostics_handle_topic_with_reply(topic, payload, state,
    send_line, ctx, NULL);
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
