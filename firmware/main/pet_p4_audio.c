#include "pet_p4_audio.h"
#include "pet_p4_audio_frontend.h"
#include "pet_p4_system_cues.h"

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
#include "mbedtls/base64.h"

#include "pet_p4_behavior.h"

#define PET_P4_AUDIO_FRAME_BYTES \
  (PET_P4_AUDIO_SAMPLE_RATE * PET_P4_AUDIO_CHANNELS * (PET_P4_AUDIO_BITS_PER_SAMPLE / 8) \
   * PET_P4_AUDIO_FRAME_MS / 1000)
#define PET_P4_AUDIO_MAX_CAPTURE_MS 30000ULL
#define PET_P4_ES7210_I2C_ADDRESS 0x40
#define PET_P4_ES8311_I2C_ADDRESS 0x18
#define PET_P4_AUDIO_PLAYBACK_BUFFER_BYTES 4096
#define PET_P4_AUDIO_PLAYBACK_MAX_PCM_BYTES (1024 * 1024)
/* Codec's documented full-scale output: 100 = 0 dB (60 was -20 dB). */
#define PET_P4_AUDIO_PLAYBACK_VOLUME 100

typedef struct {
  char family[PET_P4_ASSET_FAMILY_MAX];
  char logical_path[PET_P4_ASSET_PATH_MAX];
  bool audio_custom;
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

/* ---- 实时对话：持续采集 + 流式播放 ---- */
#define PET_P4_AUDIO_STREAM_RING_BYTES (96 * 1024)  /* 3 s @ 16 kHz s16 mono */
#define PET_P4_AUDIO_STREAM_START_BYTES 6400          /* 200 ms 起播水位 */
#define PET_P4_AUDIO_STREAM_WRITE_BYTES 640           /* 20 ms; AEC reference follows codec cadence */
#define PET_P4_AUDIO_HALF_DUPLEX_TAIL_MS 250ULL
static atomic_bool g_conversation;
static atomic_bool g_conversation_half_duplex;
static atomic_bool g_stream_active;
static atomic_bool g_stream_ended;
static atomic_bool g_stream_playing;
static atomic_uint g_stream_played_bytes;

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
  cJSON_AddBoolToObject(payload, "streamActive", atomic_load_explicit(&g_stream_active, memory_order_acquire));
  cJSON_AddNumberToObject(payload, "streamBufferedMs", (double) (stream_buffered() * 1000ULL / 32000ULL));
  cJSON_AddNumberToObject(payload, "streamUnderruns", atomic_load(&g_stream_underruns));
  cJSON_AddNumberToObject(payload, "streamReceivedBytes", atomic_load(&g_stream_received_bytes));
  send_topic("audio/status", payload);
}

static void send_audio_error(const char *session_id, const char *code, const char *message) {
  pet_p4_audio_diagnostic("capture_error", session_id, 0, false);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id ? session_id : "");
  cJSON_AddStringToObject(payload, "code", code ? code : "audio_error");
  cJSON_AddStringToObject(payload, "error", message ? message : "audio capture failed");
  send_topic("audio/error", payload);
}

static void send_audio_begin(const char *session_id, bool session_queue_empty) {
  pet_p4_audio_diagnostic("capture_started", session_id, 0, true);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id);
  cJSON_AddStringToObject(payload, "boardDeviceId", g_board_device_id);
  cJSON_AddBoolToObject(payload, "sessionQueueEmpty", session_queue_empty);
  cJSON_AddStringToObject(payload, "format", "pcm_s16le");
  cJSON_AddNumberToObject(payload, "sampleRate", PET_P4_AUDIO_SAMPLE_RATE);
  cJSON_AddNumberToObject(payload, "channels", PET_P4_AUDIO_CHANNELS);
  cJSON_AddNumberToObject(payload, "bitsPerSample", PET_P4_AUDIO_BITS_PER_SAMPLE);
  cJSON_AddNumberToObject(payload, "frameMs", PET_P4_AUDIO_FRAME_MS);
  cJSON_AddStringToObject(payload, "transport", "usb-jsonl-pcm-v1");
  cJSON_AddBoolToObject(payload, "conversation", pet_p4_audio_conversation_active());
  send_topic("audio/begin", payload);
}

static bool send_audio_chunk(
  const char *session_id,
  uint32_t sequence,
  const uint8_t *pcm,
  size_t pcm_len,
  bool processed,
  bool speech
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
  if (!atomic_load_explicit(&g_stream_active, memory_order_acquire) || !g_stream_ring) {
    stream_silence_step(pcm, capacity);
    return;
  }
  size_t buffered = stream_buffered();
  bool ended = atomic_load_explicit(&g_stream_ended, memory_order_acquire);
  bool playing = atomic_load_explicit(&g_stream_playing, memory_order_acquire);
  if (!playing) {
    if (buffered < PET_P4_AUDIO_STREAM_START_BYTES && !(ended && buffered > 0)) {
      if (ended && buffered == 0) {
        atomic_store_explicit(&g_stream_active, false, memory_order_release);
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
      atomic_store_explicit(&g_stream_last_play_ms, (uint64_t) (esp_timer_get_time() / 1000ULL), memory_order_release);
      pet_p4_audio_diagnostic("playback_completed", g_stream_session, atomic_load(&g_stream_received_bytes), true);
      pet_p4_audio_send_status();
    } else {
      unsigned int n = atomic_fetch_add(&g_stream_underruns, 1);
      if (n == 0 || n % 100 == 0) pet_p4_audio_diagnostic("playback_underrun", g_stream_session, 0, false);
    }
    /* Keep the codec/reference clock continuous, including network gaps. */
    stream_silence_step(pcm, capacity);
    return;
  }
  size_t want = buffered < capacity ? buffered : capacity;
  if (want > PET_P4_AUDIO_STREAM_WRITE_BYTES) want = PET_P4_AUDIO_STREAM_WRITE_BYTES;
  size_t tail = atomic_load_explicit(&g_stream_tail, memory_order_acquire);
  for (size_t i = 0; i < want; i += 1) {
    pcm[i] = g_stream_ring[(tail + i) % PET_P4_AUDIO_STREAM_RING_BYTES];
  }
  atomic_store_explicit(&g_stream_tail, (tail + want) % PET_P4_AUDIO_STREAM_RING_BYTES, memory_order_release);
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

static void playback_task(void *arg) {
  (void) arg;
  uint8_t pcm[PET_P4_AUDIO_PLAYBACK_BUFFER_BYTES];
  pet_p4_audio_playback_request_t request;
  while (true) {
    if (xQueueReceive(g_playback_queue, &request,
          atomic_load(&g_stream_active) ? 0 : pdMS_TO_TICKS(10)) != pdTRUE) {
      stream_drain_step(pcm, sizeof(pcm));
      /* Release the stream mutex to lower-priority USB RX between DMA writes.
       * Immediate reacquisition can starve play_chunk even with inheritance. */
      vTaskDelay(1);
      continue;
    }
    if (atomic_load_explicit(&g_stream_active, memory_order_acquire)
        || pet_p4_audio_conversation_active()) {
      /* 实时对话播放中：状态音效让路，不打断形象说话 */
      continue;
    }
    if (atomic_load_explicit(&g_playback_generation, memory_order_acquire)
        != request.generation) {
      continue;
    }
    char fs_path[80];
    FILE *file = NULL;
    const uint8_t *shared_pcm = NULL;
    size_t shared_size = 0;
    if (request.logical_path[0] && pet_p4_asset_fs_path(request.logical_path, fs_path, sizeof(fs_path))) {
      file = fopen(fs_path, "rb");
    }
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
    if (!atomic_load_explicit(&g_capture_requested, memory_order_acquire)
        || !atomic_load_explicit(&g_enabled, memory_order_acquire)) {
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
    atomic_store_explicit(&g_capture_active, true, memory_order_release);
    bool session_queue_empty = atomic_load_explicit(
      &g_capture_session_queue_empty, memory_order_acquire
    );
    send_audio_begin(session_id, session_queue_empty);

    while (atomic_load_explicit(&g_capture_requested, memory_order_acquire)
           && atomic_load_explicit(&g_enabled, memory_order_acquire)) {
      uint64_t now_ms = (uint64_t) (esp_timer_get_time() / 1000ULL);
      bool conversation = atomic_load_explicit(&g_conversation, memory_order_acquire);
      if (!conversation && now_ms - started_ms >= PET_P4_AUDIO_MAX_CAPTURE_MS) {
        reason = "timeout";
        atomic_store_explicit(&g_capture_requested, false, memory_order_release);
        break;
      }
      int read_result = esp_codec_dev_read(g_microphone, pcm, sizeof(pcm));
      if (read_result != ESP_OK) {
        reason = "read_error";
        send_audio_error(session_id, "codec_read_failed", "device PCM read failed");
        atomic_store_explicit(&g_capture_requested, false, memory_order_release);
        break;
      }
      bool processed = conversation && !atomic_load(&g_conversation_half_duplex) && pet_p4_afe_ready();
      if (processed && !pet_p4_afe_feed((const int16_t *)pcm)) processed = false;
      if (conversation && !processed && !atomic_load(&g_conversation_half_duplex)) {
        atomic_store(&g_conversation_half_duplex, true);
        (void) conversation_mic_gain(false);
        pet_p4_audio_diagnostic("aec_fallback", session_id, 0, false);
        pet_p4_audio_send_status();
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
      if (!send_audio_chunk(session_id, chunks, pcm, sizeof(pcm), processed, speech)) {
        reason = "encode_error";
        send_audio_error(session_id, "base64_failed", "PCM frame encoding failed");
        atomic_store_explicit(&g_capture_requested, false, memory_order_release);
        break;
      }
      stream_checksum = fnv1a64_update(stream_checksum, pcm, sizeof(pcm));
      total_bytes += sizeof(pcm);
      chunks += 1;
      } while (processed && pet_p4_afe_take((int16_t *)pcm, &speech));
    }
    if (!atomic_load_explicit(&g_enabled, memory_order_acquire)) reason = "disabled";
    send_audio_end(session_id, reason, chunks, total_bytes, stream_checksum);
    atomic_store_explicit(&g_capture_active, false, memory_order_release);
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
    g_stream_lock = xSemaphoreCreateRecursiveMutex();
    int volume_result = esp_codec_dev_set_out_vol(g_speaker, PET_P4_AUDIO_PLAYBACK_VOLUME);
    int open_result = esp_codec_dev_open(g_speaker, &format);
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
               PET_P4_AUDIO_PLAYBACK_VOLUME, es8311_duplex ? " duplex" : "");
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

void pet_p4_audio_process(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state || !pet_p4_audio_playback_ready() || !g_playback_queue) return;
  /* Listening/working animation changes must not play status WAVs into the microphone.
   * Those cues also gate half-duplex capture and can suppress an entire user utterance. */
  if (pet_p4_audio_conversation_active()
      || atomic_load_explicit(&g_stream_active, memory_order_acquire)) return;
  const char *lifecycle = pet_p4_canonical_lifecycle(pet_p4_state_effective_lifecycle(state, now_ms));
  int index = pet_p4_behavior_select(
    &g_playback_behavior,
    &state->asset_catalog,
    lifecycle,
    state->asset_revision,
    now_ms
  );
  const pet_p4_asset_entry_t *entry = index >= 0 && index < state->asset_catalog.count
    ? &state->asset_catalog.entries[index] : NULL;
  const char *family = pet_p4_system_cue_family(lifecycle) ? lifecycle : entry ? entry->family : "";
  /* Status cues do not depend on a matching animation being installed. */
  if (entry && strcmp(entry->family, family)) entry = NULL;
  if (g_playback_asset_revision == state->asset_revision
      && strcmp(g_playback_family, family) == 0) {
    return;
  }

  snprintf(g_playback_family, sizeof(g_playback_family), "%s", family);
  g_playback_asset_revision = state->asset_revision;
  unsigned int generation = atomic_fetch_add_explicit(
    &g_playback_generation, 1U, memory_order_acq_rel
  ) + 1U;
  if ((!entry || !entry->audio_path[0]) && !pet_p4_system_cue_family(family)) return;

  pet_p4_audio_playback_request_t request;
  memset(&request, 0, sizeof(request));
  snprintf(request.family, sizeof(request.family), "%s", family);
  if (entry) {
    snprintf(request.logical_path, sizeof(request.logical_path), "%s", entry->audio_path);
    request.audio_custom = entry->audio_custom;
  }
  request.generation = generation;
  xQueueOverwrite(g_playback_queue, &request);
}

esp_err_t pet_p4_audio_set_enabled(bool enabled) {
  if (!pet_p4_audio_ready()) return ESP_ERR_INVALID_STATE;
  atomic_store_explicit(&g_enabled, enabled, memory_order_release);
  if (!enabled) atomic_store_explicit(&g_capture_requested, false, memory_order_release);
  pet_p4_audio_send_status();
  return ESP_OK;
}

esp_err_t pet_p4_audio_capture_start(bool session_queue_empty) {
  if (!pet_p4_audio_ready() || !pet_p4_audio_enabled()) return ESP_ERR_INVALID_STATE;
  bool expected = false;
  if (!atomic_compare_exchange_strong_explicit(
        &g_capture_requested, &expected, true, memory_order_acq_rel, memory_order_acquire)) {
    return ESP_ERR_INVALID_STATE;
  }
  atomic_store_explicit(
    &g_capture_session_queue_empty, session_queue_empty, memory_order_release
  );
  xTaskNotifyGive(g_audio_task);
  return ESP_OK;
}

esp_err_t pet_p4_audio_capture_stop(void) {
  if (!pet_p4_audio_ready()) return ESP_ERR_INVALID_STATE;
  atomic_store_explicit(&g_capture_requested, false, memory_order_release);
  return ESP_OK;
}

bool pet_p4_audio_conversation_active(void) {
  return atomic_load_explicit(&g_conversation, memory_order_acquire);
}

esp_err_t pet_p4_audio_conversation_set(bool enabled, bool half_duplex) {
  if (!enabled) {
    pet_p4_audio_diagnostic("conversation_stopped", "", 0, true);
    atomic_store_explicit(&g_conversation, false, memory_order_release);
    pet_p4_audio_stream_flush();
    (void) pet_p4_audio_capture_stop();
    (void) conversation_mic_gain(false);
    pet_p4_audio_send_status();
    return ESP_OK;
  }
  if (!pet_p4_audio_ready()) return ESP_ERR_INVALID_STATE;
  half_duplex = half_duplex || !pet_p4_afe_ready();
  if (!conversation_mic_gain(!half_duplex)) return ESP_FAIL;
  pet_p4_afe_reset_stream();
  atomic_store_explicit(&g_conversation_half_duplex, half_duplex || !pet_p4_afe_ready(), memory_order_release);
  atomic_store_explicit(&g_conversation, true, memory_order_release);
  atomic_fetch_add_explicit(&g_playback_generation, 1U, memory_order_acq_rel);
  esp_err_t err = pet_p4_audio_set_enabled(true);
  if (err == ESP_OK) {
    err = pet_p4_audio_capture_start(true);
    if (err == ESP_ERR_INVALID_STATE && atomic_load_explicit(&g_capture_active, memory_order_acquire)) {
      err = ESP_OK; /* PTT 采集已经在跑：直接转入对话模式 */
    }
  }
  if (err != ESP_OK) atomic_store_explicit(&g_conversation, false, memory_order_release);
  pet_p4_audio_diagnostic("conversation_capture", "", 0, err == ESP_OK);
  pet_p4_audio_send_status();
  return err;
}

esp_err_t pet_p4_audio_stream_begin(const char *session_id) {
  if (!pet_p4_audio_playback_ready()) return ESP_ERR_INVALID_STATE;
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
  atomic_store_explicit(&g_stream_active, true, memory_order_release);
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
  if (g_stream_lock) xSemaphoreGiveRecursive(g_stream_lock);
}
