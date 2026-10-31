#include "pet_p4_audio_frontend.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include "esp_aec.h"
#include "esp_ns.h"
#include "esp_vad.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* DeskBot V2's microphone/reference topology and cancel boundaries, adapted to
 * ESP-IDF P4 codecs. Standalone ESP-SR DSP avoids unused neural/offline models
 * that do not fit the existing 3.5 MiB OTA application slot. */
#define FRAME 320
#define NS_FRAME 160 /* The linked ESP-SR NS implementation requires 10 ms. */
#define MAX_CHUNK 1024
#define REF_SIZE 8192
#define REF_DELAY 64 /* 4 ms: keep reference ahead of the measured codec echo. */
typedef struct { int16_t pcm[FRAME]; bool speech; unsigned generation; } output_t;
static const char *TAG = "pet-p4-afe";
static aec_handle_t *aec;
static ns_handle_t ns;
static vad_handle_t vad;
static QueueHandle_t output;
static StaticQueue_t output_control;
static uint8_t *output_storage;
static atomic_bool ready;
static atomic_uint generation;
static portMUX_TYPE ref_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t *reference;
static size_t ref_head, ref_tail, ref_count;
static int16_t *mic_chunk, *ref_chunk, *clean_chunk;
static size_t used, filled;
static int chunk_samples;
static atomic_uint feed_generation;
static atomic_int failure;
static aec_config_t saved_config;
/* USB is available before boot initialization finishes. Serialize that one
 * boot caller with capture-owner recovery; never spin at audio-task priority. */
static atomic_flag lifecycle_lock = ATOMIC_FLAG_INIT;
static void lifecycle_enter(void) {
  while (atomic_flag_test_and_set(&lifecycle_lock)) vTaskDelay(1);
}
static void lifecycle_leave(void) { atomic_flag_clear(&lifecycle_lock); }
static bool initialize(void);
static uint64_t ref_last_us;
static output_t assembled;
static int16_t denoised[FRAME];
static unsigned slow_frames;
static unsigned measured_frames;
static uint64_t processing_us, maximum_us;
static uint64_t aec_us, ns_us, vad_us;
static pet_p4_afe_metrics_t metrics;
pet_p4_afe_metrics_t pet_p4_afe_metrics(void) {
  portENTER_CRITICAL(&ref_lock);
  pet_p4_afe_metrics_t copy = metrics;
  copy.reference_samples = ref_count;
  portEXIT_CRITICAL(&ref_lock);
  return copy;
}
/* Opt-in bench calibration: no recorded audio leaves the device. */
#ifdef PET_P4_AUDIO_ALIGNMENT_DIAGNOSTICS
#define CALIBRATION_SAMPLES 6144
static int16_t *cal_mic, *cal_ref;
static size_t cal_used;
static float cal_mic_low[CALIBRATION_SAMPLES / 8], cal_ref_low[CALIBRATION_SAMPLES / 8];
static void calibrate(const int16_t *mic, const int16_t *ref) {
  if (!cal_mic || !cal_ref) return;
  for (size_t n = 0; n < FRAME; ++n) {
    cal_mic[cal_used] = mic[n]; cal_ref[cal_used++] = ref[n];
    if (cal_used != CALIBRATION_SAMPLES) continue;
    float best = 0; int best_lag = 0; unsigned clipped = 0;
    for (size_t i = 0; i < CALIBRATION_SAMPLES; ++i)
      if (cal_mic[i] > 32000 || cal_mic[i] < -32000) ++clipped;
    for (size_t i = 0; i < CALIBRATION_SAMPLES / 8; ++i) {
      float x = 0, y = 0;
      for (size_t j = 0; j < 8; ++j) {x += cal_mic[i*8+j]; y += cal_ref[i*8+j];}
      cal_mic_low[i] = x / 8; cal_ref_low[i] = y / 8;
    }
    for (int lag = -256; lag <= 256; ++lag) {
      float dot = 0, x2 = 0, y2 = 0;
      for (int i = 256; i < 512; ++i) {
        float x = cal_mic_low[i], y = cal_ref_low[i - lag];
        dot += x*y; x2 += x*x; y2 += y*y;
      }
      float score = dot*dot / fmaxf(1, x2*y2);
      if (score > best) {best = score; best_lag = lag * 8;}
    }
    ESP_LOGI(TAG, "alignment lag=%d samples corr=%.3f clipped=%u", best_lag, (double)sqrtf(best), clipped);
    cal_used = 0;
  }
}
#endif

enum { AFE_OK, AFE_INIT_FAILED, AFE_RESET_FAILED, AFE_REFERENCE_OVERFLOW,
       AFE_OUTPUT_OVERFLOW, AFE_TOO_SLOW };
const char *pet_p4_afe_failure(void) {
  switch (atomic_load(&failure)) {
    case AFE_INIT_FAILED: return "init_failed";
    case AFE_RESET_FAILED: return "stream_reset_failed";
    case AFE_REFERENCE_OVERFLOW: return "reference_overflow";
    case AFE_OUTPUT_OVERFLOW: return "processed_queue_overflow";
    case AFE_TOO_SLOW: return "dsp_too_slow";
    default: return "none";
  }
}
static void fail(int code, unsigned timeline) {
  bool revoked = false;
  portENTER_CRITICAL(&ref_lock);
  /* A late overflow/timeout from a retired timeline must not revoke a newer
   * capture that the control task has already requested. */
  if (timeline == atomic_load(&generation)) {
    atomic_store(&failure, code);
    revoked = atomic_exchange(&ready, false);
  }
  portEXIT_CRITICAL(&ref_lock);
  if (revoked) ESP_LOGE(TAG, "AEC unavailable: %s", pet_p4_afe_failure());
}
bool pet_p4_afe_ready(void) { return atomic_load(&ready); }
bool pet_p4_afe_preparing(void) { return atomic_load(&feed_generation) != atomic_load(&generation); }
void pet_p4_afe_reference_abort(void) {
  portENTER_CRITICAL(&ref_lock);
  ref_head = ref_tail = ref_count = 0; ref_last_us = 0;
  portEXIT_CRITICAL(&ref_lock);
}
void pet_p4_afe_reference(const int16_t *pcm, size_t samples) {
  if (!pet_p4_afe_ready() || !pcm || samples > FRAME) return;
  unsigned current = atomic_load(&generation);
  uint64_t now = esp_timer_get_time(); bool overflow = false;
  portENTER_CRITICAL(&ref_lock);
  if (!pet_p4_afe_ready() || current != atomic_load(&generation)
      || current != atomic_load(&feed_generation)) {
    portEXIT_CRITICAL(&ref_lock); return;
  }
  if (!ref_last_us || now - ref_last_us > 200000) {
    ref_head = ref_tail = ref_count = 0;
    for (size_t i = 0; i < REF_DELAY; ++i) reference[ref_head++] = 0;
    ref_count = REF_DELAY;
  }
  ref_last_us = now;
  if (ref_count + samples > REF_SIZE) overflow = true;
  else for (size_t i = 0; i < samples; ++i) {
    reference[ref_head] = pcm[i]; ref_head = (ref_head + 1) % REF_SIZE; ++ref_count;
  }
  portEXIT_CRITICAL(&ref_lock);
  if (overflow) fail(AFE_REFERENCE_OVERFLOW, current);
}
void pet_p4_afe_reset_stream(void) {
  portENTER_CRITICAL(&ref_lock);
  atomic_fetch_add(&generation, 1);
  ref_head = ref_tail = ref_count = 0; ref_last_us = 0;
  portEXIT_CRITICAL(&ref_lock);
}
bool pet_p4_afe_prepare(void) {
  lifecycle_enter();
  unsigned current = atomic_load(&generation);
  if (atomic_load(&feed_generation) != current) {
    atomic_store(&ready, false);
    used = filled = 0;
    slow_frames = measured_frames = 0;
    processing_us = maximum_us = aec_us = ns_us = vad_us = 0;
    portENTER_CRITICAL(&ref_lock);
    memset(&metrics, 0, sizeof(metrics));
    portEXIT_CRITICAL(&ref_lock);
    pet_p4_afe_reference_abort();
#ifdef PET_P4_AUDIO_ALIGNMENT_DIAGNOSTICS
    cal_used = 0;
#endif
    /* A new capture/reference timeline cannot reuse an old adaptive filter or
     * noise/VAD history. Recreate only on this DSP-owning task, never from the
     * USB control task while aec_process is running. Keep the bulk buffers and
     * free ALL old DSP handles before allocating replacements. A failed reset
     * leaves nullable handles; a later generation can still recover safely. */
    if (!output) {
      /* Boot allocation failure already cleaned up all resources. */
      (void) initialize();
    } else {
      xQueueReset(output);
      if (aec) aec_destroy(aec);
      if (ns) ns_destroy(ns);
      if (vad) vad_destroy(vad);
      aec = NULL; ns = NULL; vad = NULL;
      /* Re-entry must have the SAME internal-RAM protection as boot. ESP-SR
       * uses small ordinary mallocs as well as config.caps; without the
       * reserve a reset after loading UI/catalogs can starve USB/DMA. */
      void *dma_reserve[2] = {NULL, NULL};
      dma_reserve[0] = heap_caps_malloc(8 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (dma_reserve[0])
        dma_reserve[1] = heap_caps_malloc(8 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (dma_reserve[1]) {
        aec = aec_create_from_config(&saved_config);
        if (aec) ns = ns_create(10);
        if (ns) vad = vad_create_with_param(VAD_MODE_2, 16000, 20, 120, 100);
      }
      heap_caps_free(dma_reserve[0]);
      heap_caps_free(dma_reserve[1]);
      if (!aec || !ns || !vad || aec_get_chunksize(aec) != chunk_samples) {
        if (aec) aec_destroy(aec);
        if (ns) ns_destroy(ns);
        if (vad) vad_destroy(vad);
        aec = NULL; ns = NULL; vad = NULL;
        fail(AFE_RESET_FAILED, current);
      } else {
        memset(mic_chunk, 0, chunk_samples * sizeof(int16_t));
        memset(ref_chunk, 0, chunk_samples * sizeof(int16_t));
        memset(clean_chunk, 0, chunk_samples * sizeof(int16_t));
        atomic_store(&failure, AFE_OK);
        atomic_store(&ready, true);
      }
    }
    atomic_store(&feed_generation, current);
    ESP_LOGI(TAG, "new capture timeline: AEC/NS/VAD ready=%d", pet_p4_afe_ready());
  }
  bool available = pet_p4_afe_ready();
  lifecycle_leave();
  return available;
}
bool pet_p4_afe_feed(const int16_t *mic) {
  if (!mic || !pet_p4_afe_prepare()) return false;
  unsigned current = atomic_load(&feed_generation);
  int16_t ref[FRAME];
  portENTER_CRITICAL(&ref_lock);
  for (size_t i = 0; i < FRAME; ++i) {
    ref[i] = ref_count ? reference[ref_tail] : 0;
    if (ref_count) { ref_tail = (ref_tail + 1) % REF_SIZE; --ref_count; }
  }
  portEXIT_CRITICAL(&ref_lock);
  uint64_t started = esp_timer_get_time();
  uint64_t aec_before = aec_us, ns_before = ns_us, vad_before = vad_us;
#ifdef PET_P4_AUDIO_ALIGNMENT_DIAGNOSTICS
  calibrate(mic, ref);
#endif
  for (size_t i = 0; i < FRAME; ++i) {
    mic_chunk[used] = mic[i]; ref_chunk[used] = ref[i];
    if (++used != (size_t)chunk_samples) continue;
    uint64_t step_started = esp_timer_get_time();
    aec_process(aec, mic_chunk, ref_chunk, clean_chunk); used = 0;
    aec_us += (uint64_t)esp_timer_get_time() - step_started;
    for (int n = 0; n < chunk_samples; ++n) {
      assembled.pcm[filled++] = clean_chunk[n];
      if (filled != FRAME) continue;
      step_started = esp_timer_get_time();
      for (size_t offset = 0; offset < FRAME; offset += NS_FRAME)
        ns_process(ns, assembled.pcm + offset, denoised + offset);
      ns_us += (uint64_t)esp_timer_get_time() - step_started;
      memcpy(assembled.pcm, denoised, sizeof(denoised));
      step_started = esp_timer_get_time();
      assembled.speech = vad_process_with_trigger(vad, assembled.pcm) == VAD_SPEECH;
      vad_us += (uint64_t)esp_timer_get_time() - step_started;
      assembled.generation = current;
      if (xQueueSend(output, &assembled, 0) != pdTRUE) {fail(AFE_OUTPUT_OVERFLOW, current); return false;}
      filled = 0;
    }
  }
  uint64_t elapsed = (uint64_t)esp_timer_get_time() - started;
  portENTER_CRITICAL(&ref_lock);
  ++metrics.frames;
  metrics.last_us = elapsed;
  if (elapsed > metrics.max_us) metrics.max_us = elapsed;
  metrics.aec_us = aec_us - aec_before;
  metrics.ns_us = ns_us - ns_before;
  metrics.vad_us = vad_us - vad_before;
  portEXIT_CRITICAL(&ref_lock);
  processing_us += elapsed;
  if (elapsed > maximum_us) maximum_us = elapsed;
  if (++measured_frames == 50) {
    ESP_LOGI(TAG, "DSP 20ms frames: mean=%u us max=%u us aec=%u ns=%u vad=%u",
             (unsigned)(processing_us / measured_frames), (unsigned)maximum_us,
             (unsigned)(aec_us / measured_frames), (unsigned)(ns_us / measured_frames), (unsigned)(vad_us / measured_frames));
    measured_frames = 0; processing_us = maximum_us = 0;
    aec_us = ns_us = vad_us = 0;
  }
  if (elapsed > 60000) ++slow_frames; else slow_frames = 0;
  if (slow_frames >= 3) {
    ESP_LOGW(TAG, "DSP over budget: last=%u max=%u aec=%u ns=%u vad=%u us",
             (unsigned)elapsed, (unsigned)maximum_us, (unsigned)aec_us, (unsigned)ns_us, (unsigned)vad_us);
    fail(AFE_TOO_SLOW, current);
  }
  return pet_p4_afe_ready();
}
bool pet_p4_afe_take(int16_t *pcm, bool *speech) {
  if (!pet_p4_afe_ready()) return false;
  output_t frame;
  while (xQueueReceive(output, &frame, 0) == pdTRUE) {
    if (frame.generation != atomic_load(&generation)) continue;
    memcpy(pcm, frame.pcm, sizeof(frame.pcm)); *speech = frame.speech; return true;
  }
  return false;
}
bool pet_p4_afe_init(void) {
  lifecycle_enter();
  bool available = initialize();
  lifecycle_leave();
  return available;
}
static bool initialize(void) {
  if (pet_p4_afe_ready()) return true;
  /* Runtime resources are retained for owner-task preparation, not replaced by
   * another init call (which used to leak live buffers/handles). */
  if (output) return false;
  void *dma_reserve[2] = {NULL, NULL};
  ESP_LOGI(TAG, "initializing after core peripherals; internal free=%u largest=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  // ESP-SR also uses ordinary malloc internally. Temporarily reserve space so
  // those allocations spill into PSRAM, leaving room for USB endpoint buffers
  // allocated on enumeration and for runtime protocol traffic.
  for (size_t i = 0; i < 2; ++i) {
    dma_reserve[i] = heap_caps_malloc(8 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dma_reserve[i]) goto failed;
  }
  reference = heap_caps_calloc(REF_SIZE, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#ifdef PET_P4_AUDIO_ALIGNMENT_DIAGNOSTICS
  cal_mic = heap_caps_calloc(CALIBRATION_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  cal_ref = heap_caps_calloc(CALIBRATION_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (!reference) goto failed;
  /* Keep internal/DMA RAM available for the display and USB RX task stacks. */
  aec_config_t config = {
    .mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = 4,
    .sample_rate = 16000, .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
#ifdef PET_P4_AEC_BENCH_FD
    .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_AGGR,
#elif defined(CONFIG_ESP32P4_REV_MIN_FULL) && CONFIG_ESP32P4_REV_MIN_FULL >= 300
    /* P4 v3 uses a different ESP-SR binary. Validate full-duplex performance
     * on that silicon independently from the legacy VOIP path on v1. */
    .mode = AEC_MODE_FD_HIGH_PERF, .nlp_level = AEC_NLP_LEVEL_VERYAGGR,
#else
    .mode = AEC_MODE_VOIP_LOW_COST, .nlp_level = AEC_NLP_LEVEL_VERYAGGR,
#endif
  };
  saved_config = config;
  aec = aec_create_from_config(&config);
  ESP_LOGI(TAG, "AEC allocation ok=%d internal=%u", aec != NULL,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ns = ns_create(10);
  vad = vad_create_with_param(VAD_MODE_2, 16000, 20, 120, 100);
  ESP_LOGI(TAG, "NS/VAD allocation ok=%d internal=%u", ns && vad,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  if (!aec || !ns || !vad) goto failed;
  chunk_samples = aec_get_chunksize(aec);
  if (chunk_samples <= 0 || chunk_samples > MAX_CHUNK) goto failed;
  /* Hot SIMD blocks remain internal; bulk JSON/reference/output live in PSRAM. */
  mic_chunk = heap_caps_aligned_alloc(16, chunk_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  ref_chunk = heap_caps_aligned_alloc(16, chunk_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  clean_chunk = heap_caps_aligned_alloc(16, chunk_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  output_storage = heap_caps_malloc(4 * sizeof(output_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (output_storage) output = xQueueCreateStatic(4, sizeof(output_t), output_storage, &output_control);
  if (!mic_chunk || !ref_chunk || !clean_chunk || !output) goto failed;
  for (size_t i = 0; i < 2; ++i) {heap_caps_free(dma_reserve[i]); dma_reserve[i] = NULL;}
  atomic_store(&failure, AFE_OK);
  atomic_store(&ready, true);
  ESP_LOGI(TAG, "AEC/NS/VAD initialized; DSP=%d samples; USB=20 ms; internal free=%u largest=%u",
           chunk_samples, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
failed:
  for (size_t i = 0; i < 2; ++i) heap_caps_free(dma_reserve[i]);
  heap_caps_free(reference); reference = NULL;
#ifdef PET_P4_AUDIO_ALIGNMENT_DIAGNOSTICS
  heap_caps_free(cal_mic); cal_mic = NULL;
  heap_caps_free(cal_ref); cal_ref = NULL;
#endif
  if (aec) aec_destroy(aec);
  aec = NULL;
  if (ns) ns_destroy(ns);
  ns = NULL;
  if (vad) vad_destroy(vad);
  vad = NULL;
  if (output) vQueueDelete(output);
  output = NULL;
  heap_caps_free(output_storage); output_storage = NULL;
  heap_caps_free(mic_chunk); mic_chunk = NULL;
  heap_caps_free(ref_chunk); ref_chunk = NULL;
  heap_caps_free(clean_chunk); clean_chunk = NULL;
  atomic_store(&failure, AFE_INIT_FAILED);
  ESP_LOGW(TAG, "initialization failed; retaining half-duplex");
  return false;
}
