#include "pet_p4_audio_patch.h"
#include "pet_p4_audio.h"
#include "pet_p4_audio_volume.h"
#include "pet_p4_fs_activity.h"
#include "pet_p4_instrument_core.h"
#include "pet_p4_audio_frontend.h"
#include "pet_p4_system_cues.h"
#include "pet_p4_completion_cue_guard.h"
#include "pet_p4_component_features.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_p4_resources_device.h"
#include "mbedtls/base64.h"

#include "pet_p4_behavior.h"

#define PET_P4_AUDIO_FRAME_BYTES \
  (PET_P4_AUDIO_SAMPLE_RATE * PET_P4_AUDIO_CHANNELS * (PET_P4_AUDIO_BITS_PER_SAMPLE / 8) \
   * PET_P4_AUDIO_FRAME_MS / 1000)
#define PET_P4_AUDIO_MAX_CAPTURE_MS 30000ULL
#define PET_P4_ES7210_I2C_ADDRESS 0x40
#define PET_P4_ES8311_I2C_ADDRESS 0x18
/* Largest streaming write is 1920 bytes (20 ms at 48 kHz mono).
 * A 2 KiB scratch buffer keeps that cadence unchanged and leaves 2 KiB more
 * stack for codec/status calls; standalone WAV cancellation is checked sooner. */
#define PET_P4_AUDIO_PLAYBACK_BUFFER_BYTES 2048
_Static_assert(PET_P4_AUDIO_PLAYBACK_BUFFER_BYTES >= 1920, "music write scratch capacity");
#define PET_P4_AUDIO_PLAYBACK_MAX_PCM_BYTES (1024 * 1024)
/* Codec's documented full-scale output: 100 = 0 dB (60 was -20 dB). */
#define PET_P4_AUDIO_PLAYBACK_VOLUME 100

typedef struct {
  char family[PET_P4_ASSET_FAMILY_MAX];
  char logical_path[PET_P4_ASSET_PATH_MAX];
  bool audio_custom;
  bool completion_guarded;
  unsigned int generation;
} pet_p4_audio_playback_request_t;

static const char *TAG = "pet-p4-audio";
static esp_codec_dev_handle_t g_microphone;
static esp_codec_dev_handle_t g_speaker;
static TaskHandle_t g_audio_task;
static TaskHandle_t g_playback_task;
static QueueHandle_t g_playback_queue;
static SemaphoreHandle_t g_stream_lock;
static pet_p4_send_line_fn g_send_line;
static void *g_send_ctx;
static portMUX_TYPE g_send_lock = portMUX_INITIALIZER_UNLOCKED;

void pet_p4_audio_set_transport(pet_p4_send_line_fn send_line, void *ctx) {
  if (!send_line) return;
  portENTER_CRITICAL(&g_send_lock);
  g_send_line = send_line; g_send_ctx = ctx;
  portEXIT_CRITICAL(&g_send_lock);
}
static char g_board_device_id[PET_P4_DEVICE_ID_MAX];
static const char *g_microphone_codec = "unavailable";
static atomic_bool g_ready;
static atomic_bool g_enabled;
static atomic_bool g_capture_requested;
static atomic_bool g_capture_active;
static atomic_bool g_capture_session_queue_empty;
static atomic_bool g_playback_ready;
static atomic_bool g_playback_active;
static atomic_uint g_session_sequence;
static atomic_uint g_playback_generation;
static pet_p4_behavior_t g_playback_behavior;
static char g_playback_family[PET_P4_ASSET_FAMILY_MAX];
static unsigned int g_playback_asset_revision;
static char g_playback_session_key[PET_P4_SESSION_QUEUE_ID_MAX];
static unsigned long long g_playback_session_revision;

/* Status cues only. The protocol owner publishes defer BEFORE claim/IO
 * admission, and clears it only after known owner/operation retirement.
 * Lock order: state owner -> cue admission mux -> resource broker mux.
 * No filesystem, codec, queue wait or DSP runs under this mux. */
static portMUX_TYPE g_status_cue_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool g_status_cue_deferred;
static atomic_bool g_status_cue_recheck;
static pet_completion_cue_guard_t g_completion_cues;
static atomic_uint g_completion_cue_queued, g_completion_cue_started, g_completion_cue_suppressed;
void pet_p4_audio_status_cues_defer(bool busy) {
  portENTER_CRITICAL(&g_status_cue_mux);
  bool previous=atomic_exchange_explicit(&g_status_cue_deferred,busy,memory_order_acq_rel);
  if(busy||previous)atomic_store_explicit(&g_status_cue_recheck,true,memory_order_release);
  portEXIT_CRITICAL(&g_status_cue_mux);
}
static bool status_cue_deferred(void) {
  return atomic_load_explicit(&g_status_cue_deferred,memory_order_acquire);
}
static pet_resource_result_t status_cue_realtime_begin(pet_resource_realtime_t *ticket, const pet_p4_audio_playback_request_t *request) {
  portENTER_CRITICAL(&g_status_cue_mux);
  pet_resource_result_t result;
  bool guarded=request&&request->completion_guarded;
  if(guarded&&!pet_completion_pending(&g_completion_cues,request->generation)) {
    result=PET_RESOURCE_PROTECTED;
  } else if(atomic_load_explicit(&g_status_cue_deferred,memory_order_acquire)) {
    if(guarded)pet_completion_cancel(&g_completion_cues,request->generation);
    atomic_store_explicit(&g_status_cue_recheck,true,memory_order_release);
    result=PET_RESOURCE_PROTECTED;
  } else {
    result=pet_resources_device_realtime_begin(ticket);
    if(guarded) {
      if(result==PET_RESOURCE_OK) {
        if(pet_completion_started(&g_completion_cues,request->generation))atomic_fetch_add(&g_completion_cue_started,1U);
        else result=PET_RESOURCE_PROTECTED;
      } else {
        pet_completion_cancel(&g_completion_cues,request->generation);
        atomic_store_explicit(&g_status_cue_recheck,true,memory_order_release);
      }
    }
  }
  portEXIT_CRITICAL(&g_status_cue_mux);
  return result;
}
static void completion_cue_cancel_pending(const pet_p4_audio_playback_request_t *request, bool retry) {
  if(!request->completion_guarded)return;
  portENTER_CRITICAL(&g_status_cue_mux);
  if(pet_completion_cancel(&g_completion_cues,request->generation)&&retry)
    atomic_store_explicit(&g_status_cue_recheck,true,memory_order_release);
  portEXIT_CRITICAL(&g_status_cue_mux);
}
static bool status_cue_enqueue(const pet_p4_audio_playback_request_t *request) {
  if(status_cue_deferred()) {
    atomic_store_explicit(&g_status_cue_recheck,true,memory_order_release);
    return false;
  }
  /* A concurrent defer can leave private queue metadata, never a new realtime
   * grant: the consumer's admission is serialized with defer publication. */
  return xQueueOverwrite(g_playback_queue,request)==pdPASS;
}

/* ---- 实时对话：持续采集 + 流式播放 ---- */
#define PET_P4_AUDIO_STREAM_RING_BYTES (96 * 1024)  /* 3 s @ 16 kHz s16 mono */
#define PET_P4_AUDIO_STREAM_START_BYTES 6400          /* 200 ms 起播水位 */
#define PET_P4_AUDIO_STREAM_WRITE_BYTES 640           /* 20 ms; AEC reference follows codec cadence */
#define PET_P4_AUDIO_HALF_DUPLEX_TAIL_MS 250ULL
static atomic_bool g_conversation;
static atomic_bool g_conversation_half_duplex;
static atomic_bool g_conversation_starting;
static portMUX_TYPE g_conversation_lock = portMUX_INITIALIZER_UNLOCKED;
static char g_conversation_session[64];
static char g_capture_owner[64];
/* Pending requests and physical workers own different exact reservations.
 * Stop may release a pending request, never the worker's in-flight ticket.
 * Lock order: stream mutex -> capture resource mux -> resource broker mux. */
static portMUX_TYPE g_capture_resource_lock = portMUX_INITIALIZER_UNLOCKED;
static pet_resource_realtime_t g_capture_pending;
static uint64_t g_capture_request_serial;
static pet_resource_realtime_t g_stream_realtime, g_music_realtime, g_conversation_realtime;
#define PET_AUDIO_REALTIME_SCOPE(name) \
  pet_resource_realtime_t name __attribute__((cleanup(pet_resources_device_realtime_end))) = {0}
static void capture_request_cancel(void) {
  portENTER_CRITICAL(&g_capture_resource_lock);
  atomic_store_explicit(&g_capture_requested, false, memory_order_release);
  g_capture_request_serial = 0;
  pet_resources_device_realtime_end(&g_capture_pending);
  portEXIT_CRITICAL(&g_capture_resource_lock);
}
static bool capture_request_publish(pet_resource_realtime_t *reservation,
    bool queue_empty, const char *owner) {
  portENTER_CRITICAL(&g_capture_resource_lock);
  bool expected = false;
  bool admitted = reservation->serial && !g_capture_pending.serial
    && atomic_compare_exchange_strong_explicit(&g_capture_requested, &expected, true,
      memory_order_acq_rel, memory_order_acquire);
  if (admitted) {
    g_capture_pending = *reservation;
    g_capture_request_serial = reservation->serial;
    memset(reservation, 0, sizeof(*reservation));
    atomic_store_explicit(&g_capture_session_queue_empty, queue_empty, memory_order_release);
    memcpy(g_capture_owner, owner, strlen(owner) + 1);
  }
  portEXIT_CRITICAL(&g_capture_resource_lock);
  return admitted;
}
static bool capture_worker_take(pet_resource_realtime_t *reservation, bool *queue_empty,
    char *owner) {
  portENTER_CRITICAL(&g_capture_resource_lock);
  *reservation = g_capture_pending;
  memset(&g_capture_pending, 0, sizeof(g_capture_pending));
  bool run = reservation->serial && reservation->serial == g_capture_request_serial
    && atomic_load_explicit(&g_capture_requested, memory_order_acquire)
    && atomic_load_explicit(&g_enabled, memory_order_acquire);
  if (run) {
    atomic_store_explicit(&g_capture_active, true, memory_order_release);
    if (queue_empty) *queue_empty = atomic_load_explicit(&g_capture_session_queue_empty, memory_order_acquire);
    /* The admitted request owns this label even if stop/start or conversation
     * replaces the global owner before this worker publishes its begin. */
    if (owner) memcpy(owner, g_capture_owner, sizeof(g_capture_owner));
  }
  portEXIT_CRITICAL(&g_capture_resource_lock);
  return run;
}
/* A stop/start can restore the bool before an old read returns. The exact
 * request serial, not that bool, owns every worker boundary and terminal error. */
static bool capture_worker_current(pet_resource_realtime_t reservation) {
  portENTER_CRITICAL(&g_capture_resource_lock);
  bool current = reservation.serial && reservation.serial == g_capture_request_serial
    && atomic_load_explicit(&g_capture_requested, memory_order_acquire)
    && atomic_load_explicit(&g_enabled, memory_order_acquire);
  portEXIT_CRITICAL(&g_capture_resource_lock);
  return current;
}
static void capture_worker_cancel(pet_resource_realtime_t reservation) {
  portENTER_CRITICAL(&g_capture_resource_lock);
  if (reservation.serial && reservation.serial == g_capture_request_serial) {
    atomic_store_explicit(&g_capture_requested, false, memory_order_release);
    g_capture_request_serial = 0;
  }
  portEXIT_CRITICAL(&g_capture_resource_lock);
}
static void capture_worker_done(void) {
  portENTER_CRITICAL(&g_capture_resource_lock);
  atomic_store_explicit(&g_capture_active, false, memory_order_release);
  if (!atomic_load_explicit(&g_capture_requested, memory_order_acquire))
    pet_resources_device_realtime_end(&g_capture_pending);
  portEXIT_CRITICAL(&g_capture_resource_lock);
}
static void conversation_session_copy(char session[64]) {
  portENTER_CRITICAL(&g_conversation_lock);
  memcpy(session, g_conversation_session, sizeof(g_conversation_session));
  portEXIT_CRITICAL(&g_conversation_lock);
}
static atomic_uint g_capture_tx_last_us, g_capture_tx_max_us;
static void add_conversation_metadata(cJSON *payload) {
  char session[64];
  conversation_session_copy(session);
  cJSON_AddStringToObject(payload, "conversationSessionId", session);
  cJSON_AddBoolToObject(payload, "audioAecPreparing", atomic_load(&g_conversation_starting)
    || (pet_p4_audio_conversation_active() && !atomic_load(&g_conversation_half_duplex) && pet_p4_afe_preparing()));
  cJSON_AddStringToObject(payload, "aecReason", pet_p4_afe_failure());
  pet_p4_afe_metrics_t dsp = pet_p4_afe_metrics();
  cJSON_AddNumberToObject(payload, "dspFrames", dsp.frames);
  cJSON_AddNumberToObject(payload, "dspLastUs", dsp.last_us);
  cJSON_AddNumberToObject(payload, "dspMaxUs", dsp.max_us);
  cJSON_AddNumberToObject(payload, "dspAecUs", dsp.aec_us);
  cJSON_AddNumberToObject(payload, "dspNsUs", dsp.ns_us);
  cJSON_AddNumberToObject(payload, "dspVadUs", dsp.vad_us);
  cJSON_AddNumberToObject(payload, "aecReferenceSamples", dsp.reference_samples);
  cJSON_AddNumberToObject(payload, "internalFreeBytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(payload, "internalLargestBytes", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  cJSON_AddNumberToObject(payload, "captureTxLastUs", atomic_load(&g_capture_tx_last_us));
  cJSON_AddNumberToObject(payload, "captureTxMaxUs", atomic_load(&g_capture_tx_max_us));
  if (g_audio_task) cJSON_AddNumberToObject(payload, "captureStackMinBytes", uxTaskGetStackHighWaterMark(g_audio_task));
}
static atomic_bool g_stream_active;
static atomic_bool g_stream_ended;
static atomic_bool g_stream_playing;
static atomic_uint g_stream_played_bytes;
static atomic_bool g_music;
static atomic_bool g_music_paused;
static atomic_bool g_music_pause_requested;
static atomic_uint g_music_volume = 65;
static int32_t g_music_gain = 0;

unsigned int pet_p4_audio_stream_played_bytes(void) { return atomic_load(&g_stream_played_bytes); }
bool pet_p4_audio_stream_playing(void) { return atomic_load(&g_stream_playing); }
static atomic_uint_least64_t g_stream_last_play_ms;
static atomic_size_t g_stream_head;   /* producer writes */
static atomic_size_t g_stream_tail;   /* consumer reads */
/* 环形缓冲放 PSRAM，首次 play_begin 时分配；静态放片内 RAM 会挤掉任务栈导致启动自检失败回滚（0.7.54 教训）。 */
static uint8_t *g_stream_ring;
static char g_stream_session[64];
static atomic_uint g_diagnostic_sequence;
static atomic_uint g_stream_underruns;
static atomic_size_t g_stream_received_bytes;

static bool conversation_mic_gain(bool duplex) {
  if (!g_microphone || strcmp(g_microphone_codec, "ES8311") != 0) return true;
  /* Leave speaker volume unchanged. Avoid ADC clipping at maximum playback;
   * restore the established 30 dB sensitivity for PTT/half-duplex. */
  int result = esp_codec_dev_set_in_gain(g_microphone, duplex ? 24.0f : 30.0f);
  if (result != ESP_OK) ESP_LOGW(TAG, "microphone mode gain failed: %d", result);
  return result == ESP_OK;
}

static bool stream_ring_ensure(void) {
  if (g_stream_ring) return true;
  g_stream_ring = (uint8_t *) heap_caps_calloc(
    1, PET_P4_AUDIO_STREAM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );
  if (!g_stream_ring) {
    ESP_LOGW(TAG, "stream ring PSRAM allocation failed; trying internal RAM");
    g_stream_ring = (uint8_t *) heap_caps_calloc(1, PET_P4_AUDIO_STREAM_RING_BYTES, MALLOC_CAP_8BIT);
  }
  if (!g_stream_ring) {
    ESP_LOGE(TAG, "stream ring allocation failed (%u bytes)", (unsigned int) PET_P4_AUDIO_STREAM_RING_BYTES);
  }
  return g_stream_ring != NULL;
}

static size_t stream_buffered(void) {
  size_t head = atomic_load_explicit(&g_stream_head, memory_order_acquire);
  size_t tail = atomic_load_explicit(&g_stream_tail, memory_order_acquire);
  return head >= tail ? head - tail : PET_P4_AUDIO_STREAM_RING_BYTES - tail + head;
}

static bool playback_gates_capture(uint64_t now_ms) {
  if (!atomic_load_explicit(&g_conversation, memory_order_acquire)
      || !atomic_load_explicit(&g_conversation_half_duplex, memory_order_acquire)) {
    return false;
  }
  if (atomic_load_explicit(&g_stream_playing, memory_order_acquire)
      || atomic_load_explicit(&g_playback_active, memory_order_acquire)) {
    return true;
  }
  uint64_t last = atomic_load_explicit(&g_stream_last_play_ms, memory_order_acquire);
  return last != 0 && now_ms - last < PET_P4_AUDIO_HALF_DUPLEX_TAIL_MS;
}

static uint64_t fnv1a64_update(uint64_t checksum, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i += 1) {
    checksum ^= data[i];
    checksum *= 0x00000100000001b3ULL;
  }
  return checksum;
}

static void send_topic(const char *topic, cJSON *payload) {
  cJSON *root = cJSON_CreateObject();
  char *line;
  if (!root || !payload) {
    cJSON_Delete(root);
    cJSON_Delete(payload);
    return;
  }
  if (!cJSON_GetObjectItemCaseSensitive(payload, "boardDeviceId"))
    cJSON_AddStringToObject(payload, "boardDeviceId", g_board_device_id);
  cJSON_AddStringToObject(root, "topic", topic);
  cJSON_AddItemToObject(root, "payload", payload);
  line = cJSON_PrintUnformatted(root);
  portENTER_CRITICAL(&g_send_lock);
  pet_p4_send_line_fn send_line = g_send_line;
  void *ctx = g_send_ctx;
  portEXIT_CRITICAL(&g_send_lock);
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

void pet_p4_audio_diagnostic(const char *event, const char *session_id, size_t bytes, bool ok) {
  uint32_t seq = atomic_fetch_add_explicit(&g_diagnostic_sequence, 1, memory_order_relaxed);
  ESP_LOGI(TAG, "rtc event=%s session=%s bytes=%u buffered=%u ok=%d seq=%lu", event,
           session_id ? session_id : "", (unsigned int) bytes, (unsigned int) stream_buffered(), ok, (unsigned long) seq);
  cJSON *payload = cJSON_CreateObject();
  if (!payload) return;
  cJSON_AddStringToObject(payload, "event", event);
  cJSON_AddStringToObject(payload, "firmware", esp_app_get_description()->version);
  cJSON_AddStringToObject(payload, "sessionId", session_id ? session_id : "");
  cJSON_AddNumberToObject(payload, "seq", seq);
  cJSON_AddNumberToObject(payload, "bytes", bytes);
  cJSON_AddNumberToObject(payload, "bufferedMs", stream_buffered() * 1000ULL / 32000ULL);
  cJSON_AddNumberToObject(payload, "uptimeMs", esp_timer_get_time() / 1000ULL);
  cJSON_AddBoolToObject(payload, "ok", ok);
  add_conversation_metadata(payload);
  send_topic("audio/diagnostic", payload);
}

void pet_p4_audio_send_status(void) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddBoolToObject(payload, "ready", pet_p4_audio_ready());
  cJSON_AddBoolToObject(payload, "enabled", pet_p4_audio_enabled());
  cJSON_AddBoolToObject(payload, "active", pet_p4_audio_active());
  cJSON_AddBoolToObject(payload, "playbackReady", pet_p4_audio_playback_ready());
  int output_volume = -1;
  if (g_speaker) (void) esp_codec_dev_get_out_vol(g_speaker, &output_volume);
  cJSON_AddNumberToObject(payload, "playbackVolume", output_volume);
  cJSON_AddBoolToObject(payload, "playbackActive", pet_p4_audio_playback_active());
  cJSON_AddStringToObject(payload, "codec", pet_p4_audio_codec_name());
  cJSON_AddStringToObject(
    payload,
    "playbackCodec",
    pet_p4_audio_playback_ready() ? "ES8311" : "unavailable"
  );
  cJSON_AddNumberToObject(payload, "sampleRate", PET_P4_AUDIO_SAMPLE_RATE);
  cJSON_AddNumberToObject(payload, "channels", PET_P4_AUDIO_CHANNELS);
  cJSON_AddNumberToObject(payload, "bitsPerSample", PET_P4_AUDIO_BITS_PER_SAMPLE);
  cJSON_AddNumberToObject(payload, "frameMs", PET_P4_AUDIO_FRAME_MS);
  cJSON_AddBoolToObject(payload, "conversation", pet_p4_audio_conversation_active());
  cJSON_AddBoolToObject(payload, "audioAec", pet_p4_afe_ready());
  cJSON_AddBoolToObject(payload, "audioVad", pet_p4_afe_ready());
  cJSON_AddBoolToObject(payload, "audioFullDuplex", pet_p4_afe_ready());
  add_conversation_metadata(payload);
  cJSON_AddBoolToObject(payload, "streamActive", atomic_load_explicit(&g_stream_active, memory_order_acquire));
  cJSON_AddNumberToObject(payload, "streamBufferedMs", (double) (stream_buffered() * 1000ULL / 32000ULL));
  cJSON_AddNumberToObject(payload, "streamUnderruns", atomic_load(&g_stream_underruns));
  cJSON_AddNumberToObject(payload, "streamReceivedBytes", atomic_load(&g_stream_received_bytes));
  cJSON_AddNumberToObject(payload, "completionCueQueued", atomic_load(&g_completion_cue_queued));
  cJSON_AddNumberToObject(payload, "completionCueAdmitted", atomic_load(&g_completion_cue_started));
  cJSON_AddNumberToObject(payload, "completionCueSuppressed", atomic_load(&g_completion_cue_suppressed));
  send_topic("audio/status", payload);
}

static void send_audio_error(const char *session_id, const char *code, const char *message) {
  pet_p4_audio_diagnostic("capture_error", session_id, 0, false);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id ? session_id : "");
  cJSON_AddStringToObject(payload, "code", code ? code : "audio_error");
  cJSON_AddStringToObject(payload, "error", message ? message : "audio capture failed");
  add_conversation_metadata(payload);
  send_topic("audio/error", payload);
}

static void send_audio_begin(const char *session_id, bool session_queue_empty, const char *owner) {
  pet_p4_audio_diagnostic("capture_started", session_id, 0, true);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id);
  cJSON_AddStringToObject(payload,"captureOwner",owner);
  cJSON_AddStringToObject(payload, "boardDeviceId", g_board_device_id);
  cJSON_AddBoolToObject(payload, "sessionQueueEmpty", session_queue_empty);
  cJSON_AddStringToObject(payload, "format", "pcm_s16le");
  cJSON_AddNumberToObject(payload, "sampleRate", PET_P4_AUDIO_SAMPLE_RATE);
  cJSON_AddNumberToObject(payload, "channels", PET_P4_AUDIO_CHANNELS);
  cJSON_AddNumberToObject(payload, "bitsPerSample", PET_P4_AUDIO_BITS_PER_SAMPLE);
  cJSON_AddNumberToObject(payload, "frameMs", PET_P4_AUDIO_FRAME_MS);
  cJSON_AddStringToObject(payload, "transport", "usb-jsonl-pcm-v1");
  cJSON_AddBoolToObject(payload, "conversation", pet_p4_audio_conversation_active());
  add_conversation_metadata(payload);
  send_topic("audio/begin", payload);
}

static bool send_audio_chunk(
  const char *session_id,
  uint32_t sequence,
  const uint8_t *pcm,
  size_t pcm_len,
  bool processed,
  bool speech,
  const char *conversation_session
) {
  char encoded[((PET_P4_AUDIO_FRAME_BYTES + 2) / 3) * 4 + 1];
  char checksum_hex[17];
  size_t encoded_len = 0;
  uint64_t checksum = fnv1a64_update(0xcbf29ce484222325ULL, pcm, pcm_len);
  if (mbedtls_base64_encode((unsigned char *) encoded, sizeof(encoded), &encoded_len,
                            pcm, pcm_len) != 0) {
    return false;
  }
  encoded[encoded_len] = '\0';
  snprintf(checksum_hex, sizeof(checksum_hex), "%016llx", (unsigned long long) checksum);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id);
  cJSON_AddNumberToObject(payload, "seq", sequence);
  cJSON_AddNumberToObject(payload, "bytes", pcm_len);
  cJSON_AddStringToObject(payload, "checksum", checksum_hex);
  cJSON_AddStringToObject(payload, "data", encoded);
  cJSON_AddBoolToObject(payload, "aec", processed);
  /* Bind to the timeline captured BEFORE the codec read, never relabel a
   * queued PTT/previous conversation frame with the latest control session. */
  cJSON_AddStringToObject(payload, "conversationSessionId", conversation_session);
  if (processed) cJSON_AddBoolToObject(payload, "vadSpeech", speech);
  send_topic("audio/chunk", payload);
  return true;
}

static void send_audio_end(
  const char *session_id,
  const char *reason,
  uint32_t chunks,
  uint64_t total_bytes,
  uint64_t checksum
) {
  char checksum_hex[17];
  uint64_t bytes_per_second = PET_P4_AUDIO_SAMPLE_RATE * PET_P4_AUDIO_CHANNELS
    * (PET_P4_AUDIO_BITS_PER_SAMPLE / 8);
  snprintf(checksum_hex, sizeof(checksum_hex), "%016llx", (unsigned long long) checksum);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id);
  cJSON_AddStringToObject(payload, "reason", reason ? reason : "released");
  cJSON_AddNumberToObject(payload, "chunks", chunks);
  cJSON_AddNumberToObject(payload, "bytes", (double) total_bytes);
  cJSON_AddNumberToObject(payload, "durationMs",
                          bytes_per_second ? (double) (total_bytes * 1000ULL / bytes_per_second) : 0);
  cJSON_AddStringToObject(payload, "checksum", checksum_hex);
  send_topic("audio/end", payload);
}

static uint16_t read_le16(const uint8_t *bytes) {
  return (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes) {
  return (uint32_t) bytes[0]
    | ((uint32_t) bytes[1] << 8)
    | ((uint32_t) bytes[2] << 16)
    | ((uint32_t) bytes[3] << 24);
}

static bool wav_seek_pcm(FILE *file, uint32_t *pcm_bytes) {
  uint8_t riff[12];
  long file_size;
  bool format_ok = false;
  if (!file || !pcm_bytes) return false;
  if (fseek(file, 0, SEEK_END) != 0) return false;
  file_size = ftell(file);
  if (file_size < 44 || file_size > PET_P4_AUDIO_PLAYBACK_MAX_PCM_BYTES + 4096) return false;
  if (fseek(file, 0, SEEK_SET) != 0 || fread(riff, 1, sizeof(riff), file) != sizeof(riff)) {
    return false;
  }
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;

  while (ftell(file) >= 0 && ftell(file) + 8 <= file_size) {
    uint8_t header[8];
    uint32_t chunk_size;
    long data_offset;
    long next_offset;
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) return false;
    chunk_size = read_le32(header + 4);
    data_offset = ftell(file);
    next_offset = data_offset + (long) chunk_size + (long) (chunk_size & 1U);
    if (data_offset < 0 || next_offset < data_offset || next_offset > file_size) return false;

    if (memcmp(header, "fmt ", 4) == 0) {
      uint8_t format[16];
      if (chunk_size < sizeof(format)
          || fread(format, 1, sizeof(format), file) != sizeof(format)) {
        return false;
      }
      format_ok = read_le16(format) == 1
        && read_le16(format + 2) == PET_P4_AUDIO_CHANNELS
        && read_le32(format + 4) == PET_P4_AUDIO_SAMPLE_RATE
        && read_le32(format + 8) == PET_P4_AUDIO_SAMPLE_RATE * 2
        && read_le16(format + 12) == 2
        && read_le16(format + 14) == PET_P4_AUDIO_BITS_PER_SAMPLE;
    } else if (memcmp(header, "data", 4) == 0) {
      if (!format_ok || chunk_size == 0 || (chunk_size & 1U) != 0
          || chunk_size > PET_P4_AUDIO_PLAYBACK_MAX_PCM_BYTES) {
        return false;
      }
      *pcm_bytes = chunk_size;
      return true;
    }
    if (fseek(file, next_offset, SEEK_SET) != 0) return false;
  }
  return false;
}

static void stream_silence_step(uint8_t *pcm, size_t capacity) {
  if (!pet_p4_audio_conversation_active() || atomic_load(&g_conversation_half_duplex)) return;
  size_t bytes = capacity < PET_P4_AUDIO_STREAM_WRITE_BYTES ? capacity : PET_P4_AUDIO_STREAM_WRITE_BYTES;
  memset(pcm, 0, bytes);
  pet_p4_afe_reference((const int16_t *)pcm, bytes / sizeof(int16_t));
  if (esp_codec_dev_write(g_speaker, pcm, (int)bytes) != ESP_OK) pet_p4_afe_reference_abort();
}

static void stream_drain_step_locked(uint8_t *pcm, size_t capacity) {
  if (atomic_load(&g_music_paused) && atomic_load(&g_music)) return;
  if (!atomic_load_explicit(&g_stream_active, memory_order_acquire) || !g_stream_ring) {
    stream_silence_step(pcm, capacity);
    return;
  }
  size_t buffered = stream_buffered();
  bool ended = atomic_load_explicit(&g_stream_ended, memory_order_acquire);
  bool playing = atomic_load_explicit(&g_stream_playing, memory_order_acquire);
  if (!playing) {
    size_t start_bytes = atomic_load(&g_music) ? 38400 : PET_P4_AUDIO_STREAM_START_BYTES;
    if (buffered < start_bytes && !(ended && buffered > 0)) {
      if (ended && buffered == 0) {
        atomic_store_explicit(&g_stream_active, false, memory_order_release);
        pet_resources_device_realtime_end(&g_stream_realtime);
        pet_p4_audio_send_status();
      }
      stream_silence_step(pcm, capacity);
      return;
    }
    atomic_store_explicit(&g_stream_playing, true, memory_order_release);
    atomic_store_explicit(&g_playback_active, true, memory_order_release);
    pet_p4_audio_diagnostic("playback_started", g_stream_session, buffered, true);
    pet_p4_audio_send_status();
    playing = true;
  }
  if (buffered == 0) {
    if (ended) {
      atomic_store_explicit(&g_stream_playing, false, memory_order_release);
      atomic_store_explicit(&g_stream_active, false, memory_order_release);
      atomic_store_explicit(&g_playback_active, false, memory_order_release);
      pet_resources_device_realtime_end(&g_stream_realtime);
      atomic_store_explicit(&g_stream_last_play_ms, (uint64_t) (esp_timer_get_time() / 1000ULL), memory_order_release);
      pet_p4_audio_diagnostic("playback_completed", g_stream_session, atomic_load(&g_stream_received_bytes), true);
      pet_p4_audio_send_status();
    } else {
      unsigned int n = atomic_fetch_add(&g_stream_underruns, 1);
      if (n == 0 || n % 100 == 0) pet_p4_audio_diagnostic("playback_underrun", g_stream_session, 0, false);
      if (atomic_load(&g_music)) {
        atomic_store(&g_stream_playing, false);
        atomic_store(&g_playback_active, false);
      }
    }
    /* Keep the codec/reference clock continuous, including network gaps. */
    stream_silence_step(pcm, capacity);
    return;
  }
  size_t want = buffered < capacity ? buffered : capacity;
  size_t write_bytes = atomic_load(&g_music) ? 1920 : PET_P4_AUDIO_STREAM_WRITE_BYTES;
  if (want > write_bytes) want = write_bytes;
  size_t tail = atomic_load_explicit(&g_stream_tail, memory_order_acquire);
  for (size_t i = 0; i < want; i += 1) {
    pcm[i] = g_stream_ring[(tail + i) % PET_P4_AUDIO_STREAM_RING_BYTES];
  }
  atomic_store_explicit(&g_stream_tail, (tail + want) % PET_P4_AUDIO_STREAM_RING_BYTES, memory_order_release);
  if (atomic_load(&g_music)) {
    int target = atomic_load(&g_music_pause_requested)?0:(int)atomic_load(&g_music_volume)*256;
    for (size_t i=0;i+1<want;i+=2) {
      int delta=target-g_music_gain;
      g_music_gain+=delta>32?32:delta<-32?-32:delta;
      int16_t value=(int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i+1]<<8));
      value=(int16_t)((int32_t)value*g_music_gain/(100*256));
      pcm[i]=(uint8_t)value;pcm[i+1]=(uint8_t)((uint16_t)value>>8);
    }
    if(target==0 && g_music_gain==0 && atomic_load(&g_music_pause_requested))atomic_store(&g_music_paused,true);
  }
  if (pet_p4_audio_conversation_active() && !atomic_load(&g_conversation_half_duplex))
    pet_p4_afe_reference((const int16_t *)pcm, want / sizeof(int16_t));
  if (esp_codec_dev_write(g_speaker, pcm, (int) want) != ESP_OK) {
    pet_p4_afe_reference_abort();
    ESP_LOGW(TAG, "stream playback write failed");
    pet_p4_audio_diagnostic("playback_write_failed", g_stream_session, want, false);
    send_audio_error(g_stream_session, "speaker_write_failed", "speaker playback failed");
    pet_p4_audio_stream_flush();
    return;
  }
  atomic_fetch_add(&g_stream_played_bytes, want);
  atomic_store_explicit(&g_stream_last_play_ms, (uint64_t) (esp_timer_get_time() / 1000ULL), memory_order_release);
}

static void stream_drain_step(uint8_t *pcm, size_t capacity) {
  if (!g_stream_lock) return;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  stream_drain_step_locked(pcm, capacity);
  xSemaphoreGiveRecursive(g_stream_lock);
}

static atomic_bool g_instrument_enabled;
static atomic_uint g_instrument_pending, g_instrument_effect, g_instrument_ambience;
static atomic_uint g_instrument_hit_ms;
static atomic_bool g_instrument_hit;
static atomic_int g_instrument_start_y, g_instrument_start_scale;
static pet_instrument_core_t g_instrument_core;
static atomic_bool g_fruit_enabled;
static atomic_uint g_fruit_pending,g_fruit_volume;
void pet_p4_audio_fruit_sound(unsigned events,unsigned volume,bool active) {
  atomic_store(&g_fruit_volume,volume>100?100:volume);
  bool old=atomic_exchange(&g_fruit_enabled,active);
  if(old!=active) {
    atomic_store(&g_fruit_pending,0);
    atomic_fetch_add(&g_playback_generation,1);
    if(g_playback_queue)xQueueReset(g_playback_queue);
  }
  if(active)atomic_fetch_or(&g_fruit_pending,events&7);
}
static bool fruit_drain_step(uint8_t *pcm) {
  static unsigned position,length,kind,phase;
  static uint32_t noise=0x739a42;
  if(!g_stream_lock)return false;
  xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  bool blocked=!atomic_load(&g_fruit_enabled)||atomic_load(&g_capture_requested)||atomic_load(&g_capture_active)
    ||atomic_load(&g_conversation)||atomic_load(&g_stream_active)||atomic_load(&g_music);
  unsigned events=atomic_exchange(&g_fruit_pending,0);
  if(blocked){position=length=0;xSemaphoreGiveRecursive(g_stream_lock);return false;}
  if(events){kind=(events&2)?2:(events&4)?4:1;position=phase=0;length=kind==1?1920:3840;}
  if(position>=length){xSemaphoreGiveRecursive(g_stream_lock);return false;}
  PET_AUDIO_REALTIME_SCOPE(fruit_work);
  if (pet_resources_device_realtime_begin(&fruit_work) != PET_RESOURCE_OK) {
    xSemaphoreGiveRecursive(g_stream_lock); return false;
  }
  unsigned volume=atomic_load(&g_fruit_volume);
  int16_t *samples=(int16_t*)pcm;
  for(unsigned i=0;i<320;i++) {
    if(position>=length){samples[i]=0;continue;}
    noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;
    unsigned hz=kind==2?95:kind==4?(position<1920?660:880):1100-position/3;
    phase=(phase+hz*65536/16000)&65535;
    int triangle=phase<32768?(int)phase-16384:49151-(int)phase;
    int n=(int)(noise&32767)-16384;
    int wave=kind==1?(triangle+2*n)/3:kind==2?(triangle+n)/2:triangle;
    unsigned attack=position<80?position:80;
    samples[i]=(int16_t)((int64_t)wave*(length-position)*attack*volume/((int64_t)length*80*180));
    position++;
  }
  if(esp_codec_dev_write(g_speaker,pcm,640)!=ESP_OK)position=length;
  xSemaphoreGiveRecursive(g_stream_lock);return true;
}

static atomic_bool g_sfx_enabled;
static atomic_uint g_sfx_pending,g_sfx_volume;
static pet_sfx_core_t g_sfx_core;
void pet_p4_audio_sfx_set(bool active,unsigned volume) {
  atomic_store(&g_sfx_volume,volume>100?100:volume);
  bool old=atomic_exchange(&g_sfx_enabled,active);
  if(old!=active) {
    atomic_store(&g_sfx_pending,0);
    atomic_fetch_add(&g_playback_generation,1);
    if(g_playback_queue)xQueueReset(g_playback_queue);
  }
}
bool pet_p4_audio_sfx_trigger(unsigned preset) {
  if(preset>=PET_SFX_PRESETS||!atomic_load(&g_sfx_enabled)||atomic_load(&g_capture_requested)
     ||atomic_load(&g_conversation)||!pet_p4_audio_playback_ready())return false;
  atomic_fetch_or(&g_sfx_pending,1u<<preset);
  return true;
}
#if PET_P4_SEQUENCE || PET_P4_SAMPLES
#include "pet_p4_component_media_clients.h"
/* Track C: sequencer + one-shot samples ride the sfx drain step. Program/PCM copies
 * and the sampler pointers are only touched under g_stream_lock. */
static atomic_bool g_cext_enabled;
static atomic_uint g_cext_sequence_volume,g_cext_sample_volume;
static atomic_bool g_cext_sequence_requested,g_cext_sequence_started;
static pet_seq_program_t g_cext_program;
static pet_seq_state_t g_cext_sequencer;
static pet_sampler_t g_cext_sampler;
static pet_cmedia_sample_lease_t g_cext_sample_queue[PET_SAMPLE_VOICES];
static uint64_t g_cext_voice_tokens[PET_SAMPLE_VOICES];
static unsigned g_cext_sample_queued;
static void cext_samples_reset(void) {
  pet_sampler_reset(&g_cext_sampler);
  for(unsigned i=0;i<PET_SAMPLE_VOICES;i++) {
    (void)pet_cmedia_unpin_deferred(g_cext_voice_tokens[i]); g_cext_voice_tokens[i]=0;
    (void)pet_cmedia_unpin_deferred(g_cext_sample_queue[i].token);
    memset(&g_cext_sample_queue[i],0,sizeof(g_cext_sample_queue[i]));
  }
  g_cext_sample_queued=0;
}
void pet_p4_audio_cext_set(bool active,const pet_seq_program_t *program,bool sequence_playing,unsigned sequence_volume,unsigned sample_volume) {
  atomic_store(&g_cext_sequence_volume,sequence_volume>100?100:sequence_volume);
  atomic_store(&g_cext_sample_volume,sample_volume>100?100:sample_volume);
  if(!g_stream_lock)return;
  xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  if(program&&sequence_playing&&!atomic_load(&g_cext_sequence_requested))g_cext_program=*program;
  atomic_store(&g_cext_sequence_requested,active&&sequence_playing);
  bool old=atomic_exchange(&g_cext_enabled,active);
  if(!active){pet_seq_stop(&g_cext_sequencer);atomic_store(&g_cext_sequence_started,false);cext_samples_reset();}
  if(old!=active){atomic_fetch_add(&g_playback_generation,1);if(g_playback_queue)xQueueReset(g_playback_queue);}
  xSemaphoreGiveRecursive(g_stream_lock);
}
/* Legacy untracked samples fail closed; the caller transfers a tracked lease. */
bool pet_p4_audio_cext_sample(const int16_t *pcm,uint32_t frames) {(void)pcm;(void)frames;return false;}
bool pet_p4_audio_cext_sample_lease(pet_cmedia_sample_lease_t *lease) {
  if(!lease||!lease->token||!lease->pcm||!lease->frames||lease->frames>PET_SAMPLE_MAX_BYTES/2||!atomic_load(&g_cext_enabled)||atomic_load(&g_capture_requested)||atomic_load(&g_conversation)||!pet_p4_audio_playback_ready()||!g_stream_lock)return false;
  /* Caller holds its generation snapshot lock; no media mutex is taken here. */
  xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  if(!atomic_load(&g_cext_enabled)||atomic_load(&g_capture_requested)||atomic_load(&g_conversation)){xSemaphoreGiveRecursive(g_stream_lock);return false;}
  if(g_cext_sample_queued>=PET_SAMPLE_VOICES){(void)pet_cmedia_unpin_deferred(g_cext_sample_queue[0].token);g_cext_sample_queue[0]=g_cext_sample_queue[1];g_cext_sample_queued=1;}
  g_cext_sample_queue[g_cext_sample_queued++]=*lease;memset(lease,0,sizeof(*lease));
  xSemaphoreGiveRecursive(g_stream_lock);return true;
}
/* Under g_stream_lock, after sfx voices were rendered or zeroed into pcm. */
static bool cext_mix_step(int16_t *pcm,bool blocked) {
  if(blocked||!atomic_load(&g_cext_enabled)){pet_seq_stop(&g_cext_sequencer);atomic_store(&g_cext_sequence_started,false);cext_samples_reset();return false;}
  bool want=atomic_load(&g_cext_sequence_requested);
  if(want&&!atomic_load(&g_cext_sequence_started)){pet_seq_start(&g_cext_sequencer,&g_cext_program);atomic_store(&g_cext_sequence_started,true);}
  if(!want){pet_seq_stop(&g_cext_sequencer);atomic_store(&g_cext_sequence_started,false);}
  for(unsigned i=0;i<g_cext_sample_queued;i++) {
    unsigned voice=PET_SAMPLE_VOICES;
    for(unsigned j=0;j<PET_SAMPLE_VOICES;j++) if(!g_cext_sampler.voice[j].active){voice=j;break;}
    if(voice==PET_SAMPLE_VOICES)voice=g_cext_sampler.next%PET_SAMPLE_VOICES;
    (void)pet_cmedia_unpin_deferred(g_cext_voice_tokens[voice]); g_cext_voice_tokens[voice]=0;
    if(pet_sampler_trigger(&g_cext_sampler,g_cext_sample_queue[i].pcm,g_cext_sample_queue[i].frames))g_cext_voice_tokens[voice]=g_cext_sample_queue[i].token;
    else (void)pet_cmedia_unpin_deferred(g_cext_sample_queue[i].token);
    memset(&g_cext_sample_queue[i],0,sizeof(g_cext_sample_queue[i]));
  }
  g_cext_sample_queued=0;
  bool active=pet_seq_active(&g_cext_sequencer)||pet_sampler_active(&g_cext_sampler);
  if(active){
    pet_seq_mix(&g_cext_sequencer,&g_cext_program,pcm,320,atomic_load(&g_cext_sequence_volume));
    pet_sampler_mix(&g_cext_sampler,pcm,320,atomic_load(&g_cext_sample_volume));
    for(unsigned i=0;i<PET_SAMPLE_VOICES;i++) if(!g_cext_sampler.voice[i].active) {
      (void)pet_cmedia_unpin_deferred(g_cext_voice_tokens[i]);g_cext_voice_tokens[i]=0;
    }
  }
  return active;
}
#define CEXT_ENABLED() atomic_load(&g_cext_enabled)
#define CEXT_PENDING() ((atomic_load(&g_cext_sequence_requested)&&!atomic_load(&g_cext_sequence_started))||g_cext_sample_queued||pet_seq_active(&g_cext_sequencer)||pet_sampler_active(&g_cext_sampler))
#else
#define CEXT_ENABLED() false
#define CEXT_PENDING() false
#define cext_mix_step(pcm,blocked) false
#endif
static bool sfx_drain_step(uint8_t *pcm) {
  if(!g_stream_lock)return false;
  xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  bool blocked=atomic_load(&g_capture_requested)||atomic_load(&g_capture_active)
    ||atomic_load(&g_conversation)||atomic_load(&g_stream_active)||atomic_load(&g_music);
  bool enabled=atomic_load(&g_sfx_enabled)||CEXT_ENABLED();
  unsigned pending=atomic_exchange(&g_sfx_pending,0);
  if(blocked||!enabled){pet_sfx_reset(&g_sfx_core);(void)cext_mix_step((int16_t*)pcm,true);xSemaphoreGiveRecursive(g_stream_lock);return false;}
  for(unsigned p=0;p<PET_SFX_PRESETS;p++)if(pending&(1u<<p)&&atomic_load(&g_sfx_enabled))pet_sfx_trigger(&g_sfx_core,p);
  if(!pet_sfx_active(&g_sfx_core)&&!CEXT_PENDING()){xSemaphoreGiveRecursive(g_stream_lock);return false;}
  PET_AUDIO_REALTIME_SCOPE(sfx_work);
  if (pet_resources_device_realtime_begin(&sfx_work) != PET_RESOURCE_OK) {
    xSemaphoreGiveRecursive(g_stream_lock); return false;
  }
  pet_sfx_render(&g_sfx_core,(int16_t*)pcm,320,atomic_load(&g_sfx_volume));
  (void)cext_mix_step((int16_t*)pcm,false);
  if(esp_codec_dev_write(g_speaker,pcm,640)!=ESP_OK)pet_sfx_reset(&g_sfx_core);
  xSemaphoreGiveRecursive(g_stream_lock);
  return true;
}

void pet_p4_audio_instrument_set(bool active,unsigned effect,unsigned ambience) {
  atomic_store(&g_instrument_effect,effect>100?100:effect);
  atomic_store(&g_instrument_ambience,ambience>100?100:ambience);
  bool old=atomic_exchange(&g_instrument_enabled,active);
  if(old!=active) {
    atomic_store(&g_instrument_pending,0);atomic_store(&g_instrument_hit,false);
    atomic_fetch_add(&g_playback_generation,1);
    if(g_playback_queue)xQueueReset(g_playback_queue);
  }
}
bool pet_p4_audio_instrument_strike(uint64_t now_ms) {
  if(!atomic_load(&g_instrument_enabled) || atomic_load(&g_capture_requested)
     || atomic_load(&g_conversation) || !pet_p4_audio_playback_ready())return false;
  unsigned n=atomic_load(&g_instrument_pending);
  do {if(n>=4)return false;} while(!atomic_compare_exchange_weak(&g_instrument_pending,&n,n+1));
  pet_instrument_motion_t current=pet_p4_audio_instrument_motion(now_ms);
  atomic_store(&g_instrument_start_y,(int)(current.mallet_y*1024));
  atomic_store(&g_instrument_start_scale,(int)(current.fish_scale*1024));
  atomic_store(&g_instrument_hit_ms,(uint32_t)now_ms);atomic_store(&g_instrument_hit,true);
  return true;
}
uint32_t pet_p4_audio_instrument_age(uint64_t now_ms) {
  return atomic_load(&g_instrument_hit)?(uint32_t)now_ms-atomic_load(&g_instrument_hit_ms):1000;
}
pet_instrument_motion_t pet_p4_audio_instrument_motion(uint64_t now_ms) {
  return pet_instrument_motion_from(pet_p4_audio_instrument_age(now_ms),
    atomic_load(&g_instrument_start_y)/1024.0f,
    atomic_load(&g_instrument_start_scale)/1024.0f);
}
static bool instrument_drain_step(uint8_t *pcm) {
  if(!g_stream_lock)return false;
  xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  bool blocked=atomic_load(&g_capture_requested)||atomic_load(&g_capture_active)
    ||atomic_load(&g_conversation)||atomic_load(&g_stream_active)||atomic_load(&g_music);
  bool enabled=atomic_load(&g_instrument_enabled);
  bool render=!blocked && (enabled||g_instrument_core.gain>0);
  unsigned hits=atomic_exchange(&g_instrument_pending,0);
  if(blocked)pet_instrument_reset(&g_instrument_core);
  if(render) {
    PET_AUDIO_REALTIME_SCOPE(instrument_work);
    if (pet_resources_device_realtime_begin(&instrument_work) != PET_RESOURCE_OK) {
      xSemaphoreGiveRecursive(g_stream_lock); return false;
    }
    if(enabled)for(unsigned i=0;i<hits;i++)pet_instrument_strike(&g_instrument_core);
    pet_instrument_render(&g_instrument_core,(int16_t*)pcm,320,enabled,
      atomic_load(&g_instrument_effect),atomic_load(&g_instrument_ambience));
    if(esp_codec_dev_write(g_speaker,pcm,640)!=ESP_OK)pet_instrument_reset(&g_instrument_core);
  }
  xSemaphoreGiveRecursive(g_stream_lock);
  return render;
}
static void playback_task(void *arg) {
  (void) arg;
  uint8_t pcm[PET_P4_AUDIO_PLAYBACK_BUFFER_BYTES];
  pet_p4_audio_playback_request_t request;
  pet_instrument_reset(&g_instrument_core);
  while (true) {
    if(fruit_drain_step(pcm)) {vTaskDelay(1);continue;}
    if(sfx_drain_step(pcm)) {vTaskDelay(1);continue;}
    if(instrument_drain_step(pcm)) {vTaskDelay(1);continue;}
    if (xQueueReceive(g_playback_queue, &request,
          atomic_load(&g_stream_active) ? 0 : pdMS_TO_TICKS(10)) != pdTRUE) {
      stream_drain_step(pcm, sizeof(pcm));
      /* Release the stream mutex to lower-priority USB RX between DMA writes.
       * Immediate reacquisition can starve play_chunk even with inheritance. */
      vTaskDelay(1);
      continue;
    }
    if (atomic_load(&g_fruit_enabled) || atomic_load(&g_sfx_enabled) || CEXT_ENABLED() || atomic_load(&g_instrument_enabled) || atomic_load(&g_music) || atomic_load_explicit(&g_stream_active, memory_order_acquire)
        || pet_p4_audio_conversation_active()) {
      /* 实时对话播放中：状态音效让路，不打断形象说话 */
      completion_cue_cancel_pending(&request,true);
      continue;
    }
    if (atomic_load_explicit(&g_playback_generation, memory_order_acquire)
        != request.generation) {
      completion_cue_cancel_pending(&request,false);
      continue;
    }
    PET_AUDIO_REALTIME_SCOPE(file_playback_work);
    if (status_cue_realtime_begin(&file_playback_work,&request) != PET_RESOURCE_OK) {
      TickType_t wait=pdMS_TO_TICKS(10);vTaskDelay(wait?wait:1);
      continue;
    }
    char fs_path[80];
    FILE *file = NULL;
    pet_fs_activity_t file_activity __attribute__((cleanup(pet_fs_activity_end)))={0};
    const uint8_t *shared_pcm = NULL;
    size_t shared_size = 0;
    /* The existing realtime ticket excludes appearance storage IO. Resolve an
     * independently persisted global override, shared by every appearance. */
    file_activity=pet_fs_activity_begin();
    if (pet_p4_asset_audio_override_path(request.family,fs_path,sizeof(fs_path))) {
      file=fopen(fs_path,"rb");
      if(file)request.audio_custom=true;
    }
    /* Status cues use the global bank or immutable system defaults. Video
     * manifests and appearance-local WAV files do not select these sounds. */
    /* Existing packs may still contain shipped defaults. Migrate by content,
     * never by filename: an uploaded custom WAV can use that same filename. */
    if (file && !request.audio_custom && pet_p4_system_cue_family(request.family)) {
      uint64_t checksum = UINT64_C(0xcbf29ce484222325);
      size_t count, total = 0;
      while ((count = fread(pcm, 1, sizeof(pcm), file)) > 0) {
        checksum = fnv1a64_update(checksum, pcm, count);
        total += count;
        if (total > PET_P4_AUDIO_PLAYBACK_MAX_PCM_BYTES + 4096) break;
      }
      if (!ferror(file) && pet_p4_system_cue_legacy(request.family, checksum)) {
        fclose(file); file = NULL;
      } else rewind(file);
    }
    uint32_t remaining = 0;
    if (file && !wav_seek_pcm(file, &remaining)) { fclose(file); file = NULL; }
    if (!file) pet_fs_activity_end(&file_activity);
    if (!file && pet_p4_system_cue_pcm(request.family, &shared_pcm, &shared_size)) {
      remaining = (uint32_t)shared_size;
    }
    if (!file && !shared_pcm) {
      continue;
    }

    atomic_store_explicit(&g_playback_active, true, memory_order_release);
    pet_p4_audio_send_status();
    ESP_LOGI(TAG, "playing P4 audio family=%s bytes=%u", request.family, (unsigned int) remaining);
    bool ok = true;
    while (remaining > 0
           && atomic_load_explicit(&g_playback_generation, memory_order_acquire)
                == request.generation) {
      size_t wanted = remaining < sizeof(pcm) ? remaining : sizeof(pcm);
      size_t count = wanted;
      if (file) count = fread(pcm, 1, wanted, file);
      else { memcpy(pcm, shared_pcm, wanted); shared_pcm += wanted; }
      if (count != wanted || esp_codec_dev_write(g_speaker, pcm, (int) count) != ESP_OK) {
        ok = false;
        break;
      }
      remaining -= (uint32_t) count;
    }
    if (file) fclose(file);
    pet_fs_activity_end(&file_activity);
    atomic_store_explicit(&g_playback_active, false, memory_order_release);
    pet_p4_audio_send_status();
    if (!ok) ESP_LOGW(TAG, "P4 audio playback failed family=%s", request.family);
  }
}

static void audio_task(void *arg) {
  (void) arg;
  uint8_t pcm[PET_P4_AUDIO_FRAME_BYTES];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    PET_AUDIO_REALTIME_SCOPE(capture_work);
    bool session_queue_empty = false;
    char capture_owner[sizeof(g_capture_owner)];
    if (!capture_worker_take(&capture_work, &session_queue_empty, capture_owner)) {
      continue;
    }

    char session_id[64];
    uint32_t session_sequence = atomic_fetch_add_explicit(
      &g_session_sequence, 1U, memory_order_relaxed
    ) + 1U;
    uint64_t started_ms = (uint64_t) (esp_timer_get_time() / 1000ULL);
    uint64_t stream_checksum = 0xcbf29ce484222325ULL;
    uint64_t total_bytes = 0;
    uint32_t chunks = 0;
    const char *reason = "released";
    snprintf(session_id, sizeof(session_id), "p4-audio-%llu-%u",
             (unsigned long long) started_ms, (unsigned int) session_sequence);
    atomic_store(&g_capture_tx_last_us, 0);
    atomic_store(&g_capture_tx_max_us, 0);
    send_audio_begin(session_id, session_queue_empty, capture_owner);

    while (capture_worker_current(capture_work)) {
      uint64_t now_ms = (uint64_t) (esp_timer_get_time() / 1000ULL);
      bool conversation = atomic_load_explicit(&g_conversation, memory_order_acquire);
      if (atomic_load(&g_conversation_starting)) { vTaskDelay(1); continue; }
      char conversation_session[64];
      conversation_session_copy(conversation_session);
      if (conversation && !atomic_load(&g_conversation_half_duplex) && pet_p4_afe_preparing()) {
        (void) pet_p4_afe_prepare();
        pet_p4_audio_send_status();
      }
#if PET_P4_CONVERSATION_VIDEO
      /* The pending/capture actor blocks video through microphone/AEC setup.
       * Only this physical worker may relax its own exact reservation. */
      (void)pet_resources_device_realtime_video_ready(capture_work,
        conversation&&!atomic_load(&g_conversation_starting)
          &&(atomic_load(&g_conversation_half_duplex)||pet_p4_afe_ready()));
#endif
      if (!conversation && now_ms - started_ms >= PET_P4_AUDIO_MAX_CAPTURE_MS) {
        reason = "timeout";
        capture_worker_cancel(capture_work);
        break;
      }
      int read_result = esp_codec_dev_read(g_microphone, pcm, sizeof(pcm));
      if (!capture_worker_current(capture_work)) break;
      if (read_result != ESP_OK) {
        reason = "read_error";
        send_audio_error(session_id, "codec_read_failed", "device PCM read failed");
        capture_worker_cancel(capture_work);
        break;
      }
      char current_session[64];
      conversation_session_copy(current_session);
      if (atomic_load(&g_conversation_starting)
          || conversation != pet_p4_audio_conversation_active()
          || strcmp(current_session, conversation_session) != 0) continue;
      bool processed = conversation && !atomic_load(&g_conversation_half_duplex) && pet_p4_afe_ready();
      if (processed && !pet_p4_afe_feed((const int16_t *)pcm)) processed = false;
      if (!capture_worker_current(capture_work)) break;
      conversation_session_copy(current_session);
      if (atomic_load(&g_conversation_starting)
          || conversation != pet_p4_audio_conversation_active()
          || strcmp(current_session, conversation_session) != 0) continue;
      if (conversation && !processed && !atomic_load(&g_conversation_half_duplex)) {
        atomic_store(&g_conversation_half_duplex, true);
        (void) conversation_mic_gain(false);
        pet_p4_audio_diagnostic("aec_fallback", session_id, 0, false);
        pet_p4_audio_send_status();
        /* Fail closed: do not transmit raw echo even while the PC's stop
         * command is still in flight. A new entry can rebuild DSP once. */
        reason = "aec_unavailable";
        capture_worker_cancel(capture_work);
        break;
      }
      if (playback_gates_capture(now_ms)) {
        /* 半双工：形象说话时麦克风帧只读不发，避免自己听见自己 */
        bool ignored_speech;
        while (processed && pet_p4_afe_take((int16_t *)pcm, &ignored_speech)) {}
        continue;
      }
      bool speech = false;
      if (processed && !pet_p4_afe_take((int16_t *)pcm, &speech)) continue;
      do {
      if (!capture_worker_current(capture_work)) break;
      uint64_t tx_started_us = esp_timer_get_time();
      bool sent = send_audio_chunk(session_id, chunks, pcm, sizeof(pcm), processed, speech, conversation_session);
      uint64_t tx_elapsed = (uint64_t)esp_timer_get_time() - tx_started_us;
      unsigned tx_us = tx_elapsed > UINT32_MAX ? UINT32_MAX : (unsigned)tx_elapsed;
      atomic_store(&g_capture_tx_last_us, tx_us);
      if (tx_us > atomic_load(&g_capture_tx_max_us)) atomic_store(&g_capture_tx_max_us, tx_us);
      if (!sent) {
        reason = "encode_error";
        send_audio_error(session_id, "base64_failed", "PCM frame encoding failed");
        capture_worker_cancel(capture_work);
        break;
      }
      stream_checksum = fnv1a64_update(stream_checksum, pcm, sizeof(pcm));
      total_bytes += sizeof(pcm);
      chunks += 1;
      } while (processed && pet_p4_afe_take((int16_t *)pcm, &speech));
    }
    if (!atomic_load_explicit(&g_enabled, memory_order_acquire)) reason = "disabled";
    send_audio_end(session_id, reason, chunks, total_bytes, stream_checksum);
    capture_worker_done();
    pet_p4_audio_send_status();
  }
}

esp_err_t pet_p4_audio_init(
  const char *board_device_id,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (pet_p4_audio_ready() || pet_p4_audio_playback_ready()) return ESP_OK;
  pet_p4_audio_set_transport(send_line, ctx);
  snprintf(g_board_device_id, sizeof(g_board_device_id), "%s",
           board_device_id && board_device_id[0] ? board_device_id : "p4-unknown");
  esp_err_t i2c_err = bsp_i2c_init();
  i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
  if (i2c_err != ESP_OK || !i2c_bus) {
    ESP_LOGW(TAG, "audio I2C bus unavailable: %s", esp_err_to_name(i2c_err));
    return i2c_err == ESP_OK ? ESP_ERR_INVALID_STATE : i2c_err;
  }

  bool initialized = false;
  bool es8311_duplex = false;
  esp_err_t es7210_probe = i2c_master_probe(
    i2c_bus, PET_P4_ES7210_I2C_ADDRESS, 100
  );
  esp_err_t es8311_probe = i2c_master_probe(
    i2c_bus, PET_P4_ES8311_I2C_ADDRESS, 100
  );
  bool es8311_available = es8311_probe == ESP_OK;
  if (es7210_probe == ESP_OK) {
    g_microphone = bsp_audio_codec_microphone_init();
    if (g_microphone) g_microphone_codec = "ES7210";
  } else if (es8311_available) {
    g_microphone = bsp_audio_codec_es8311_duplex_init();
    if (g_microphone) {
      g_speaker = g_microphone;
      g_microphone_codec = "ES8311";
      es8311_duplex = true;
      ESP_LOGI(TAG, "using ES8311 ADC/DAC fallback");
    }
  } else {
    ESP_LOGW(TAG, "ES7210 not detected at I2C address 0x%02X: %s",
             PET_P4_ES7210_I2C_ADDRESS, esp_err_to_name(es7210_probe));
    ESP_LOGW(TAG, "ES8311 not detected at I2C address 0x%02X: %s",
             PET_P4_ES8311_I2C_ADDRESS, esp_err_to_name(es8311_probe));
    ESP_LOGI(TAG, "no supported audio codec detected; audio features disabled");
  }

  esp_codec_dev_sample_info_t format = {
    .sample_rate = PET_P4_AUDIO_SAMPLE_RATE,
    .channel = PET_P4_AUDIO_CHANNELS,
    .bits_per_sample = PET_P4_AUDIO_BITS_PER_SAMPLE,
  };
  if (g_microphone) {
    int gain_result = esp_codec_dev_set_in_gain(g_microphone, 30.0f);
    if (gain_result != ESP_OK) ESP_LOGW(TAG, "microphone gain setup failed: %d", gain_result);
    int open_result = esp_codec_dev_open(g_microphone, &format);
    if (open_result == ESP_OK
        // PCM deadlines must precede H.264 decode/pack (priorities 17/16).
        // Pin floating-point DSP away from the decoder's core-1 worker.
        && xTaskCreatePinnedToCore(audio_task, "pet_p4_audio", 8192, NULL, 18, &g_audio_task, 0) == pdPASS) {
      atomic_store_explicit(&g_enabled, true, memory_order_release);
      atomic_store_explicit(&g_ready, true, memory_order_release);
      initialized = true;
      ESP_LOGI(TAG, "%s microphone ready: %d Hz, %d-bit mono", g_microphone_codec,
               PET_P4_AUDIO_SAMPLE_RATE, PET_P4_AUDIO_BITS_PER_SAMPLE);
    } else {
      ESP_LOGW(TAG, "%s microphone open/task setup failed: %d", g_microphone_codec,
               open_result);
      if (open_result == ESP_OK) esp_codec_dev_close(g_microphone);
      g_microphone = NULL;
      g_microphone_codec = "unavailable";
    }
  }

  if (!g_speaker && es8311_available) g_speaker = bsp_audio_codec_speaker_init();
  if (g_speaker) {
    unsigned master_boot_volume = pet_audio_volume_boot_load();
    g_stream_lock = xSemaphoreCreateRecursiveMutex();
    int open_result = esp_codec_dev_open(g_speaker, &format);
    int volume_result = open_result == ESP_OK && g_stream_lock
      ? pet_audio_volume_attach(g_speaker, g_stream_lock, i2c_bus) : ESP_ERR_INVALID_STATE;
    g_playback_queue = open_result == ESP_OK
      ? xQueueCreate(1, sizeof(pet_p4_audio_playback_request_t))
      : NULL;
    if (volume_result == ESP_OK && open_result == ESP_OK && g_playback_queue && g_stream_lock
        && xTaskCreatePinnedToCore(playback_task, "pet_p4_playback", 8192, NULL, 18, &g_playback_task, 0)
          == pdPASS) {
      pet_p4_behavior_init(&g_playback_behavior);
      atomic_store_explicit(&g_playback_ready, true, memory_order_release);
      initialized = true;
      ESP_LOGI(TAG, "ES8311 speaker ready: %d Hz, %d-bit mono volume=%d%s",
               PET_P4_AUDIO_SAMPLE_RATE, PET_P4_AUDIO_BITS_PER_SAMPLE,
               (int)master_boot_volume, es8311_duplex ? " duplex" : "");
    } else {
      ESP_LOGW(TAG, "ES8311 speaker open/task setup failed volume=%d open=%d queue=%d",
               volume_result, open_result, g_playback_queue ? 1 : 0);
      if (g_playback_queue) {
        vQueueDelete(g_playback_queue);
        g_playback_queue = NULL;
      }
      if (open_result == ESP_OK) esp_codec_dev_close(g_speaker);
      g_speaker = NULL;
    }
  }
  return initialized ? ESP_OK : ESP_FAIL;
}

bool pet_p4_audio_ready(void) {
  return atomic_load_explicit(&g_ready, memory_order_acquire);
}

bool pet_p4_audio_enabled(void) {
  return atomic_load_explicit(&g_enabled, memory_order_acquire);
}

bool pet_p4_audio_active(void) {
  return atomic_load_explicit(&g_capture_active, memory_order_acquire);
}

const char *pet_p4_audio_codec_name(void) {
  return pet_p4_audio_ready() ? g_microphone_codec : "unavailable";
}

bool pet_p4_audio_playback_ready(void) {
  return atomic_load_explicit(&g_playback_ready, memory_order_acquire);
}

bool pet_p4_audio_playback_active(void) {
  return atomic_load_explicit(&g_playback_active, memory_order_acquire);
}

bool pet_p4_audio_master_idle(void) {
  if (atomic_load(&g_capture_requested) || pet_p4_audio_active()
      || atomic_load(&g_conversation_starting) || pet_p4_audio_conversation_active()
      || atomic_load(&g_stream_active) || pet_p4_audio_stream_playing()
      || pet_p4_audio_music_active() || pet_p4_audio_playback_active()
      || atomic_load(&g_fruit_enabled) || atomic_load(&g_instrument_enabled)
      || atomic_load(&g_fruit_pending) || atomic_load(&g_instrument_pending)
      || atomic_load(&g_sfx_pending)
      || (g_playback_queue && uxQueueMessagesWaiting(g_playback_queue))) return false;
  /* Fruit keeps local voice position; armed fruit and continuously rendered
   * instrument are conservatively busy. Other voice state uses the same mutex
   * as its speaker worker, with zero wait and no I/O while inspecting it. */
  if (!g_stream_lock || xSemaphoreTakeRecursive(g_stream_lock,0)!=pdTRUE) return false;
  bool idle=!pet_sfx_active(&g_sfx_core) && !CEXT_PENDING() && g_instrument_core.gain<=0;
  xSemaphoreGiveRecursive(g_stream_lock);
  return idle;
}

static bool completion_active(const char *state) {
  const char *s=pet_p4_canonical_lifecycle(state);
  return !strcmp(s,"working")||!strcmp(s,"speaking")||!strcmp(s,"waiting_user");
}
static const char *completion_agent(const pet_p4_runtime_state_t *state) {
  return state->current_session_agent[0]?state->current_session_agent:state->active_agent;
}
static void completion_cue_observe(const pet_p4_runtime_state_t *state,unsigned long long now_ms) {
  const char *agent=completion_agent(state);
  unsigned count=state->session_queue_count<PET_P4_SESSION_QUEUE_MAX?state->session_queue_count:PET_P4_SESSION_QUEUE_MAX;
  portENTER_CRITICAL(&g_status_cue_mux);
  pet_completion_snapshot_begin(&g_completion_cues);
  for(unsigned i=0;i<count;i++)pet_completion_snapshot_present(&g_completion_cues,agent,state->session_queue[i].id);
  if(!count)pet_completion_snapshot_present(&g_completion_cues,agent,state->current_session_id);
  for(unsigned i=0;i<count;i++)pet_completion_observe(&g_completion_cues,agent,state->session_queue[i].id,state->session_queue[i].transition_revision,completion_active(state->session_queue[i].state));
  if(!count)pet_completion_observe(&g_completion_cues,agent,state->current_session_id,0,completion_active(pet_p4_state_effective_lifecycle(state,now_ms)));
  pet_completion_snapshot_end(&g_completion_cues);
  portEXIT_CRITICAL(&g_status_cue_mux);
}
void pet_p4_audio_process(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state) return;
  completion_cue_observe(state,now_ms);
  if(state->asset_transfer_active||pet_p4_appearance_active()||status_cue_deferred()) {
    atomic_store_explicit(&g_status_cue_recheck,true,memory_order_release);
    return;
  }
  if (!pet_p4_audio_playback_ready() || !g_playback_queue) return;
  /* Listening/working animation changes must not play status WAVs into the microphone.
   * Those cues also gate half-duplex capture and can suppress an entire user utterance. */
  if (atomic_load(&g_instrument_enabled) || pet_p4_audio_conversation_active()
      || atomic_load_explicit(&g_stream_active, memory_order_acquire)) return;
  /* Never replace an event cue (for example session_terminated) with an
   * inferred lifecycle cue while the event cue is still playing. */
  if (pet_p4_audio_playback_active()) return;
  const char *lifecycle = pet_p4_canonical_lifecycle(pet_p4_state_effective_lifecycle(state, now_ms));
  const pet_p4_session_queue_item_t *current_session = NULL;
  for (unsigned i = 0; i < state->session_queue_count && i < PET_P4_SESSION_QUEUE_MAX; i += 1) {
    if (state->current_session_id[0]
        && strcmp(state->session_queue[i].id, state->current_session_id) == 0) {
      current_session = &state->session_queue[i];
      break;
    }
  }
  if (!current_session && state->current_session_index < state->session_queue_count) {
    current_session = &state->session_queue[state->current_session_index];
  }
  const char *session_key = current_session
    ? current_session->id
    : state->current_session_id;
  unsigned long long session_revision = current_session
    ? current_session->transition_revision
    : 0;
  int index = pet_p4_behavior_select(
    &g_playback_behavior,
    &state->asset_catalog,
    lifecycle,
    state->asset_revision,
    now_ms
  );
  const pet_p4_asset_entry_t *entry = index >= 0 && index < state->asset_catalog.count
    ? &state->asset_catalog.entries[index] : NULL;
  const char *family = (pet_p4_system_cue_family(lifecycle) || pet_audio_patch_global_enabled(lifecycle)) ? lifecycle : entry ? entry->family : "";
  /* Status cues do not depend on a matching animation being installed. */
  if (entry && strcmp(entry->family, family)) entry = NULL;
  bool completion_guarded=!strcmp(family,"done");
  if(completion_guarded) {
    portENTER_CRITICAL(&g_status_cue_mux);
    bool due=pet_completion_due(&g_completion_cues,completion_agent(state),session_key,session_revision,
      atomic_load_explicit(&g_playback_generation,memory_order_acquire));
    portEXIT_CRITICAL(&g_status_cue_mux);
    if(!due) {
      atomic_fetch_add(&g_completion_cue_suppressed,1U);
      /* Keep presentation bookkeeping without allocating a new playback generation. */
      snprintf(g_playback_family,sizeof(g_playback_family),"%s",family);
      snprintf(g_playback_session_key,sizeof(g_playback_session_key),"%s",session_key);
      g_playback_session_revision=session_revision;g_playback_asset_revision=state->asset_revision;
      /* A worker can cancel an unstarted pending cue and publish recheck after
       * due was read. Never erase that wake-up on this no-admission path. */
      return;
    }
  }
  if (g_playback_asset_revision == state->asset_revision
      && strcmp(g_playback_family, family) == 0
      && strcmp(g_playback_session_key, session_key) == 0
      && g_playback_session_revision == session_revision
      && !atomic_load_explicit(&g_status_cue_recheck,memory_order_acquire)) {
    return;
  }

  atomic_store_explicit(&g_status_cue_recheck,false,memory_order_release);
  snprintf(g_playback_family, sizeof(g_playback_family), "%s", family);
  snprintf(g_playback_session_key, sizeof(g_playback_session_key), "%s", session_key);
  g_playback_session_revision = session_revision;
  g_playback_asset_revision = state->asset_revision;
  unsigned int generation = atomic_fetch_add_explicit(
    &g_playback_generation, 1U, memory_order_acq_rel
  ) + 1U;
  if (!family[0]) return;
  if (!pet_p4_system_cue_family(family) && !pet_audio_patch_global_enabled(family)) return;

  pet_p4_audio_playback_request_t request;
  memset(&request, 0, sizeof(request));
  snprintf(request.family, sizeof(request.family), "%s", family);
  if (entry) {
    snprintf(request.logical_path, sizeof(request.logical_path), "%s", entry->audio_path);
    request.audio_custom = entry->audio_custom;
  }
  request.generation = generation;
  request.completion_guarded=completion_guarded;
  if(completion_guarded) {
    portENTER_CRITICAL(&g_status_cue_mux);
    bool reserved=pet_completion_reserve(&g_completion_cues,completion_agent(state),session_key,session_revision,generation);
    portEXIT_CRITICAL(&g_status_cue_mux);
    if(!reserved)return;
  }
  if(status_cue_enqueue(&request)) {
    if(completion_guarded)atomic_fetch_add(&g_completion_cue_queued,1U);
  } else completion_cue_cancel_pending(&request,true);
}

esp_err_t pet_p4_audio_play_event_cue(const char *family) {
  if (!family || !pet_p4_system_cue_family(family)) return ESP_ERR_INVALID_ARG;
  if (!pet_p4_audio_playback_ready() || !g_playback_queue || status_cue_deferred()) return ESP_ERR_INVALID_STATE;
  pet_p4_audio_playback_request_t request;
  memset(&request, 0, sizeof(request));
  snprintf(request.family, sizeof(request.family), "%s", family);
  request.audio_custom = false;
  request.generation = atomic_fetch_add_explicit(
    &g_playback_generation, 1U, memory_order_acq_rel
  ) + 1U;
  return status_cue_enqueue(&request)?ESP_OK:ESP_ERR_INVALID_STATE;
}

esp_err_t pet_p4_audio_set_enabled(bool enabled) {
  if (!pet_p4_audio_ready()) return ESP_ERR_INVALID_STATE;
  atomic_store_explicit(&g_enabled, enabled, memory_order_release);
  if (!enabled) capture_request_cancel();
  pet_p4_audio_send_status();
  return ESP_OK;
}

esp_err_t pet_p4_audio_capture_start_owned(bool session_queue_empty,const char *owner) {
  if(!owner||strlen(owner)>=sizeof(g_capture_owner))return ESP_ERR_INVALID_ARG;
  if(owner[0]&&(atomic_load(&g_capture_active)||pet_p4_audio_conversation_active()))return ESP_ERR_INVALID_STATE;
  if (!pet_p4_audio_ready() || !pet_p4_audio_enabled()) return ESP_ERR_INVALID_STATE;
  PET_AUDIO_REALTIME_SCOPE(request);
  if (pet_resources_device_realtime_begin(&request) != PET_RESOURCE_OK) return ESP_ERR_INVALID_STATE;
  if(g_stream_lock)xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  /* Restore the voice clock before waking the microphone task. */
  if (atomic_load(&g_music)) pet_p4_audio_stream_flush();
  if (!pet_p4_audio_ready()) {
    if(g_stream_lock)xSemaphoreGiveRecursive(g_stream_lock);
    return ESP_ERR_INVALID_STATE;
  }
  if (!capture_request_publish(&request, session_queue_empty, owner)) {
    if(g_stream_lock)xSemaphoreGiveRecursive(g_stream_lock);
    return ESP_ERR_INVALID_STATE;
  }
  if(g_stream_lock)xSemaphoreGiveRecursive(g_stream_lock);
  xTaskNotifyGive(g_audio_task);
  return ESP_OK;
}
esp_err_t pet_p4_audio_capture_start(bool session_queue_empty){return pet_p4_audio_capture_start_owned(session_queue_empty,"");}
esp_err_t pet_p4_audio_capture_stop_owned(const char *owner){
  if(!owner||!owner[0])return ESP_ERR_INVALID_ARG;
  portENTER_CRITICAL(&g_capture_resource_lock);
  bool matches=!strcmp(owner,g_capture_owner)&&!atomic_load(&g_conversation);
  if(matches) {
    atomic_store_explicit(&g_capture_requested,false,memory_order_release);
    g_capture_request_serial = 0;
    pet_resources_device_realtime_end(&g_capture_pending);
  }
  portEXIT_CRITICAL(&g_capture_resource_lock);return matches?ESP_OK:ESP_ERR_INVALID_STATE;
}

esp_err_t pet_p4_audio_capture_stop(void) {
  if (!pet_p4_audio_ready()) return ESP_ERR_INVALID_STATE;
  capture_request_cancel();
  return ESP_OK;
}

bool pet_p4_audio_conversation_active(void) {
  return atomic_load_explicit(&g_conversation, memory_order_acquire);
}
bool pet_p4_audio_conversation_video_ready(void) {
  return pet_p4_audio_conversation_active()&&!atomic_load(&g_conversation_starting)
    &&atomic_load_explicit(&g_capture_active,memory_order_acquire)
    &&(atomic_load(&g_conversation_half_duplex)||pet_p4_afe_ready());
}

esp_err_t pet_p4_audio_conversation_set(bool enabled, bool half_duplex, const char *session_id) {
  if (!enabled) {
    if (g_stream_lock) xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
    pet_p4_audio_diagnostic("conversation_stopped", "", 0, true);
    atomic_store_explicit(&g_conversation, false, memory_order_release);
    portENTER_CRITICAL(&g_conversation_lock);
    g_conversation_session[0] = '\0';
    portEXIT_CRITICAL(&g_conversation_lock);
    pet_p4_audio_stream_flush();
    (void) pet_p4_audio_capture_stop();
    (void) conversation_mic_gain(false);
    pet_resources_device_realtime_end(&g_conversation_realtime);
    if (g_stream_lock) xSemaphoreGiveRecursive(g_stream_lock);
    pet_p4_audio_send_status();
    return ESP_OK;
  }
  if (!pet_p4_audio_ready()) return ESP_ERR_INVALID_STATE;
  if (session_id && strlen(session_id) >= sizeof(g_conversation_session)) return ESP_ERR_INVALID_ARG;
  char current_session[64];
  conversation_session_copy(current_session);
  if (pet_p4_audio_conversation_active() && session_id && session_id[0]
      && strcmp(current_session, session_id) == 0) {
    pet_p4_audio_send_status();
    return ESP_OK; /* Duplicate control request must not reset a live filter. */
  }
  PET_AUDIO_REALTIME_SCOPE(conversation_request);
  if (pet_resources_device_realtime_begin(&conversation_request) != PET_RESOURCE_OK) return ESP_ERR_INVALID_STATE;
  if (g_stream_lock) xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  if (atomic_load(&g_music)) pet_p4_audio_stream_flush();
  if (!conversation_mic_gain(!half_duplex)) {
    if (g_stream_lock) xSemaphoreGiveRecursive(g_stream_lock);
    return ESP_FAIL;
  }
  atomic_store(&g_conversation_starting, true);
  portENTER_CRITICAL(&g_conversation_lock);
  snprintf(g_conversation_session, sizeof(g_conversation_session), "%s", session_id ? session_id : "");
  portEXIT_CRITICAL(&g_conversation_lock);
  portENTER_CRITICAL(&g_capture_resource_lock);
  g_capture_owner[0]=0; /* Conversation takes ownership; stale component stops cannot affect it. */
  portEXIT_CRITICAL(&g_capture_resource_lock);
  pet_p4_afe_reset_stream();
  atomic_store_explicit(&g_conversation_half_duplex, half_duplex, memory_order_release);
  pet_resources_device_realtime_end(&g_conversation_realtime);
  g_conversation_realtime = conversation_request;
  memset(&conversation_request, 0, sizeof(conversation_request));
  atomic_store_explicit(&g_conversation, true, memory_order_release);
  atomic_store(&g_conversation_starting, false);
  atomic_fetch_add_explicit(&g_playback_generation, 1U, memory_order_acq_rel);
  esp_err_t err = pet_p4_audio_set_enabled(true);
  if (err == ESP_OK) {
    err = pet_p4_audio_capture_start(true);
    if (err == ESP_ERR_INVALID_STATE && atomic_load_explicit(&g_capture_active, memory_order_acquire)) {
      err = ESP_OK; /* PTT 采集已经在跑：直接转入对话模式 */
    }
  }
  if (err != ESP_OK) {
    atomic_store_explicit(&g_conversation, false, memory_order_release);
    pet_resources_device_realtime_end(&g_conversation_realtime);
  }
#if PET_P4_CONVERSATION_VIDEO
  /* capture_start has already reserved a separate, initially protected ticket;
   * it continues to block video until the worker finishes AEC preparation. */
  if(err==ESP_OK)(void)pet_resources_device_realtime_video_ready(g_conversation_realtime,true);
#endif
  if (g_stream_lock) xSemaphoreGiveRecursive(g_stream_lock);
  pet_p4_audio_diagnostic("conversation_capture", "", 0, err == ESP_OK);
  pet_p4_audio_send_status();
  return err;
}

esp_err_t pet_p4_audio_stream_begin(const char *session_id) {
  if (!pet_p4_audio_playback_ready()) return ESP_ERR_INVALID_STATE;
  PET_AUDIO_REALTIME_SCOPE(stream_request);
  if (pet_resources_device_realtime_begin(&stream_request) != PET_RESOURCE_OK) return ESP_ERR_INVALID_STATE;
  if (atomic_load(&g_music) && (!session_id || strncmp(session_id,"music-",6))) pet_p4_audio_stream_flush();
  if (!stream_ring_ensure()) return ESP_ERR_NO_MEM;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  atomic_store_explicit(&g_stream_playing, false, memory_order_release);
  atomic_store_explicit(&g_stream_head, 0, memory_order_release);
  atomic_store_explicit(&g_stream_tail, 0, memory_order_release);
  atomic_store_explicit(&g_stream_ended, false, memory_order_release);
  snprintf(g_stream_session, sizeof(g_stream_session), "%s", session_id ? session_id : "");
  atomic_store(&g_stream_received_bytes, 0);
  atomic_store(&g_stream_played_bytes, 0);
  atomic_store(&g_stream_underruns, 0);
  pet_resources_device_realtime_end(&g_stream_realtime);
  g_stream_realtime = stream_request;
  memset(&stream_request, 0, sizeof(stream_request));
  atomic_store_explicit(&g_stream_active, true, memory_order_release);
#if PET_P4_CONVERSATION_VIDEO
  /* Ring/codec ownership is published. Other initialization tickets, if any,
   * still protect video independently. Music/file playback never opts in. */
  if(pet_p4_audio_conversation_active()&&!atomic_load(&g_conversation_starting))
    (void)pet_resources_device_realtime_video_ready(g_stream_realtime,true);
#endif
  pet_p4_audio_diagnostic("playback_begin", g_stream_session, 0, true);
  xSemaphoreGiveRecursive(g_stream_lock);
  return ESP_OK;
}

bool pet_p4_audio_stream_matches(const char *session_id) {
  if (!session_id || !session_id[0] || !g_stream_lock) return false;
  xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  bool matches = atomic_load(&g_stream_active) && strcmp(session_id, g_stream_session) == 0;
  xSemaphoreGiveRecursive(g_stream_lock);
  return matches;
}

esp_err_t pet_p4_audio_stream_push(const uint8_t *pcm, size_t length) {
  if (!pcm || length == 0) return ESP_ERR_INVALID_ARG;
  if (!atomic_load_explicit(&g_stream_active, memory_order_acquire) || !g_stream_ring) return ESP_ERR_INVALID_STATE;
  if (stream_buffered() + length >= PET_P4_AUDIO_STREAM_RING_BYTES - 1) {
    ESP_LOGW(TAG, "stream ring full, dropping %u bytes", (unsigned int) length);
    pet_p4_audio_diagnostic("playback_overflow", g_stream_session, length, false);
    return ESP_ERR_NO_MEM;
  }
  size_t head = atomic_load_explicit(&g_stream_head, memory_order_acquire);
  for (size_t i = 0; i < length; i += 1) {
    g_stream_ring[(head + i) % PET_P4_AUDIO_STREAM_RING_BYTES] = pcm[i];
  }
  atomic_store_explicit(&g_stream_head, (head + length) % PET_P4_AUDIO_STREAM_RING_BYTES, memory_order_release);
  atomic_fetch_add(&g_stream_received_bytes, length);
  return ESP_OK;
}

esp_err_t pet_p4_audio_stream_end(void) {
  if (!atomic_load_explicit(&g_stream_active, memory_order_acquire)) return ESP_ERR_INVALID_STATE;
  atomic_store_explicit(&g_stream_ended, true, memory_order_release);
  pet_p4_audio_diagnostic("playback_input_end", g_stream_session, atomic_load(&g_stream_received_bytes), true);
  return ESP_OK;
}

void pet_p4_audio_stream_flush(void) {
  if (g_stream_lock) xSemaphoreTakeRecursive(g_stream_lock, portMAX_DELAY);
  atomic_store_explicit(&g_stream_ended, true, memory_order_release);
  atomic_store_explicit(&g_stream_head, 0, memory_order_release);
  atomic_store_explicit(&g_stream_tail, 0, memory_order_release);
  if (atomic_load_explicit(&g_stream_active, memory_order_acquire)) {
    pet_p4_audio_diagnostic("playback_flushed", g_stream_session, atomic_load(&g_stream_received_bytes), true);
    atomic_store_explicit(&g_stream_playing, false, memory_order_release);
    atomic_store_explicit(&g_stream_active, false, memory_order_release);
    atomic_store_explicit(&g_playback_active, false, memory_order_release);
    atomic_store_explicit(&g_stream_last_play_ms, (uint64_t) (esp_timer_get_time() / 1000ULL), memory_order_release);
  }
  g_stream_session[0] = '\0';
  if (atomic_exchange(&g_music, false)) {
    /* Both handles share one I2S bus. Never change only one half's clock. */
    esp_codec_dev_close(g_speaker);
    if (g_microphone && g_microphone != g_speaker) esp_codec_dev_close(g_microphone);
    esp_codec_dev_sample_info_t format = {.sample_rate=16000,.channel=1,.bits_per_sample=16};
    int restored = g_microphone ? esp_codec_dev_open(g_microphone,&format) : ESP_OK;
    if (g_speaker != g_microphone) {
      int r=esp_codec_dev_open(g_speaker,&format); if (r!=ESP_OK) restored=r;
    }
    if (restored != ESP_OK) {
      atomic_store(&g_ready,false);atomic_store(&g_playback_ready,false);
      ESP_LOGE(TAG,"music to voice codec restore failed: %d",restored);
    }
    atomic_store(&g_music_paused,false);
  }
  /* g_stream_lock has waited for any active codec write/restore to return. */
  pet_resources_device_realtime_end(&g_stream_realtime);
  pet_resources_device_realtime_end(&g_music_realtime);
  if (g_stream_lock) xSemaphoreGiveRecursive(g_stream_lock);
}

esp_err_t pet_p4_audio_music_begin(const char *session_id) {
  if (!g_stream_lock || !pet_p4_audio_playback_ready()) return ESP_ERR_INVALID_STATE;
  PET_AUDIO_REALTIME_SCOPE(music_request);
  if (pet_resources_device_realtime_begin(&music_request) != PET_RESOURCE_OK) return ESP_ERR_INVALID_STATE;
  xSemaphoreTakeRecursive(g_stream_lock,portMAX_DELAY);
  if (atomic_load(&g_capture_requested) || atomic_load(&g_capture_active) || atomic_load(&g_conversation)) {
    xSemaphoreGiveRecursive(g_stream_lock);return ESP_ERR_INVALID_STATE;
  }
  pet_p4_audio_stream_flush();
  atomic_fetch_add(&g_playback_generation,1);
  xQueueReset(g_playback_queue);
  for (int i=0; i<100 && atomic_load(&g_playback_active); i++) vTaskDelay(pdMS_TO_TICKS(5));
  if (atomic_load(&g_playback_active)) {xSemaphoreGiveRecursive(g_stream_lock);return ESP_ERR_INVALID_STATE;}
  esp_codec_dev_close(g_speaker);
  if (g_microphone && g_microphone!=g_speaker) esp_codec_dev_close(g_microphone);
  esp_codec_dev_sample_info_t format={.sample_rate=48000,.channel=1,.bits_per_sample=16};
  int result=g_microphone ? esp_codec_dev_open(g_microphone,&format) : ESP_OK;
  if (result==ESP_OK && g_speaker!=g_microphone) result=esp_codec_dev_open(g_speaker,&format);
  atomic_store(&g_music,true);atomic_store(&g_music_paused,false);atomic_store(&g_music_pause_requested,false);g_music_gain=0;
  g_music_realtime = music_request;
  memset(&music_request, 0, sizeof(music_request));
  if (result==ESP_OK) result=pet_p4_audio_stream_begin(session_id);
  if (result!=ESP_OK) pet_p4_audio_stream_flush();
  xSemaphoreGiveRecursive(g_stream_lock);return result;
}
bool pet_p4_audio_music_active(void) {return atomic_load(&g_music);}
bool pet_p4_audio_music_paused(void) {return atomic_load(&g_music_paused);}
bool pet_p4_audio_music_ended(void) {return atomic_load(&g_music) && !atomic_load(&g_stream_active) && atomic_load(&g_stream_ended);}
void pet_p4_audio_music_pause(bool paused) {
  atomic_store(&g_music_pause_requested,paused);
  if(!paused || !atomic_load(&g_stream_playing))atomic_store(&g_music_paused,paused);
}
void pet_p4_audio_music_volume(unsigned int v) {atomic_store(&g_music_volume,v>100?100:v);}
unsigned int pet_p4_audio_music_buffered_ms(void) {return (unsigned int)(stream_buffered()*1000ULL/96000ULL);}
