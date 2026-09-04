#include "pet_p4_audio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#define PET_P4_AUDIO_PLAYBACK_VOLUME 60

typedef struct {
  char family[PET_P4_ASSET_FAMILY_MAX];
  char logical_path[PET_P4_ASSET_PATH_MAX];
  unsigned int generation;
} pet_p4_audio_playback_request_t;

static const char *TAG = "pet-p4-audio";
static esp_codec_dev_handle_t g_microphone;
static esp_codec_dev_handle_t g_speaker;
static TaskHandle_t g_audio_task;
static TaskHandle_t g_playback_task;
static QueueHandle_t g_playback_queue;
static pet_p4_send_line_fn g_send_line;
static void *g_send_ctx;
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
  if (line && g_send_line) g_send_line(line, g_send_ctx);
  cJSON_free(line);
  cJSON_Delete(root);
}

void pet_p4_audio_send_status(void) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddBoolToObject(payload, "ready", pet_p4_audio_ready());
  cJSON_AddBoolToObject(payload, "enabled", pet_p4_audio_enabled());
  cJSON_AddBoolToObject(payload, "active", pet_p4_audio_active());
  cJSON_AddBoolToObject(payload, "playbackReady", pet_p4_audio_playback_ready());
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
  send_topic("audio/status", payload);
}

static void send_audio_error(const char *session_id, const char *code, const char *message) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "sessionId", session_id ? session_id : "");
  cJSON_AddStringToObject(payload, "code", code ? code : "audio_error");
  cJSON_AddStringToObject(payload, "error", message ? message : "audio capture failed");
  send_topic("audio/error", payload);
}

static void send_audio_begin(const char *session_id, bool session_queue_empty) {
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
  send_topic("audio/begin", payload);
}

static bool send_audio_chunk(
  const char *session_id,
  uint32_t sequence,
  const uint8_t *pcm,
  size_t pcm_len
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

static void playback_task(void *arg) {
  (void) arg;
  uint8_t pcm[PET_P4_AUDIO_PLAYBACK_BUFFER_BYTES];
  pet_p4_audio_playback_request_t request;
  while (true) {
    if (xQueueReceive(g_playback_queue, &request, portMAX_DELAY) != pdTRUE) continue;
    if (atomic_load_explicit(&g_playback_generation, memory_order_acquire)
        != request.generation) {
      continue;
    }
    char fs_path[80];
    if (!pet_p4_asset_fs_path(request.logical_path, fs_path, sizeof(fs_path))) {
      ESP_LOGW(TAG, "audio path rejected family=%s path=%s", request.family, request.logical_path);
      continue;
    }
    FILE *file = fopen(fs_path, "rb");
    uint32_t remaining = 0;
    if (!file || !wav_seek_pcm(file, &remaining)) {
      ESP_LOGW(TAG, "invalid P4 WAV family=%s path=%s", request.family, request.logical_path);
      if (file) fclose(file);
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
      size_t count = fread(pcm, 1, wanted, file);
      if (count != wanted || esp_codec_dev_write(g_speaker, pcm, (int) count) != ESP_OK) {
        ok = false;
        break;
      }
      remaining -= (uint32_t) count;
    }
    fclose(file);
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
      if (now_ms - started_ms >= PET_P4_AUDIO_MAX_CAPTURE_MS) {
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
      if (!send_audio_chunk(session_id, chunks, pcm, sizeof(pcm))) {
        reason = "encode_error";
        send_audio_error(session_id, "base64_failed", "PCM frame encoding failed");
        atomic_store_explicit(&g_capture_requested, false, memory_order_release);
        break;
      }
      stream_checksum = fnv1a64_update(stream_checksum, pcm, sizeof(pcm));
      total_bytes += sizeof(pcm);
      chunks += 1;
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
  g_send_line = send_line;
  g_send_ctx = ctx;
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
        && xTaskCreate(audio_task, "pet_p4_audio", 8192, NULL, 8, &g_audio_task) == pdPASS) {
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
    int volume_result = esp_codec_dev_set_out_vol(g_speaker, PET_P4_AUDIO_PLAYBACK_VOLUME);
    int open_result = esp_codec_dev_open(g_speaker, &format);
    g_playback_queue = open_result == ESP_OK
      ? xQueueCreate(1, sizeof(pet_p4_audio_playback_request_t))
      : NULL;
    if (volume_result == ESP_OK && open_result == ESP_OK && g_playback_queue
        && xTaskCreate(playback_task, "pet_p4_playback", 8192, NULL, 8, &g_playback_task)
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
  int index = pet_p4_behavior_select(
    &g_playback_behavior,
    &state->asset_catalog,
    pet_p4_state_effective_lifecycle(state, now_ms),
    state->asset_revision,
    now_ms
  );
  if (index < 0 || index >= state->asset_catalog.count) return;
  const pet_p4_asset_entry_t *entry = &state->asset_catalog.entries[index];
  if (g_playback_asset_revision == state->asset_revision
      && strcmp(g_playback_family, entry->family) == 0) {
    return;
  }

  snprintf(g_playback_family, sizeof(g_playback_family), "%s", entry->family);
  g_playback_asset_revision = state->asset_revision;
  unsigned int generation = atomic_fetch_add_explicit(
    &g_playback_generation, 1U, memory_order_acq_rel
  ) + 1U;
  if (!entry->audio_path[0]) return;

  pet_p4_audio_playback_request_t request;
  memset(&request, 0, sizeof(request));
  snprintf(request.family, sizeof(request.family), "%s", entry->family);
  snprintf(request.logical_path, sizeof(request.logical_path), "%s", entry->audio_path);
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
