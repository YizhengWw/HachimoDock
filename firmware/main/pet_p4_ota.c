/*
 * [Input] firmware update JSON topics, inactive ESP-IDF OTA partition, and runtime
 *         readiness from the LCD/render initialization path.
 * [Output] 4KB ACK-gated A/B image writes, request-correlated resumable status,
 *          SHA-256 verification, boot-slot switching, delayed validity
 *          confirmation, and automatic rollback.
 * [Pos] ESP32-P4 firmware update node in firmware/main
 * [Sync] If this file changes, update firmware/protocol.md and .folder.md.
 */

#include "pet_p4_ota.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "pet_p4_diagnostics.h"

#define PET_P4_OTA_TRANSFER_ID_MAX 64
#define PET_P4_OTA_SHA256_HEX_LEN 64
#define PET_P4_OTA_DECODED_CHUNK_MAX (4 * 1024)
#define PET_P4_OTA_VALIDATE_STABLE_MS 8000ULL
#define PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS 1000ULL
#define PET_P4_OTA_VALIDATE_MIN_SAMPLES 20U
#define PET_P4_OTA_FAILED_BOOT_RESTART_MS 20000ULL
#define PET_P4_OTA_RESTART_DELAY_MS 5000ULL
#define PET_P4_OTA_TRANSFER_IDLE_TIMEOUT_MS 30000ULL

typedef struct {
  bool valid;
  char transfer_id[PET_P4_OTA_TRANSFER_ID_MAX];
  char target_partition[17];
  char version[sizeof(((esp_app_desc_t *) 0)->version)];
  char project_name[sizeof(((esp_app_desc_t *) 0)->project_name)];
  char sha256[PET_P4_OTA_SHA256_HEX_LEN + 1];
  unsigned long long bytes;
  unsigned int next_sequence;
} pet_p4_ota_commit_result_t;

static const char *TAG = "pet-p4-ota";
static pet_p4_runtime_state_t *g_state;
static esp_ota_handle_t g_ota_handle;
static const esp_partition_t *g_update_partition;
static char g_transfer_id[PET_P4_OTA_TRANSFER_ID_MAX];
static char g_expected_sha256[PET_P4_OTA_SHA256_HEX_LEN + 1];
static unsigned long long g_expected_size;
static unsigned long long g_received_size;
static unsigned int g_next_sequence;
static unsigned int g_last_chunk_sequence;
static size_t g_last_chunk_size;
static bool g_last_chunk_valid;
static mbedtls_sha256_context g_sha256;
static bool g_sha256_initialized;
static bool g_active;
static bool g_runtime_ready;
static bool g_pending_verify;
static unsigned int g_runtime_healthy_samples;
static unsigned long long g_runtime_first_healthy_ms;
static unsigned long long g_runtime_last_healthy_ms;
static unsigned long long g_initialized_ms;
static unsigned long long g_restart_at_ms;
static unsigned long long g_last_activity_ms;
static SemaphoreHandle_t g_ota_mutex;
static bool g_initialized;
static pet_p4_ota_commit_result_t g_commit_result;

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
  return cJSON_IsString(item) ? item->valuestring : "";
}

static unsigned long long json_u64(
  const cJSON *object,
  const char *key,
  unsigned long long fallback
) {
  const cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
  double integer = 0;
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)
      || item->valuedouble < 0
      || item->valuedouble >= 18446744073709551616.0
      || modf(item->valuedouble, &integer) != 0.0) {
    return fallback;
  }
  return (unsigned long long) integer;
}

static void send_topic(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *topic,
  cJSON *payload
) {
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
  if (line && send_line) send_line(line, ctx);
  cJSON_free(line);
  cJSON_Delete(root);
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

static void add_partition(cJSON *payload, const char *key, const esp_partition_t *partition) {
  cJSON *item = cJSON_CreateObject();
  if (!item) return;
  if (partition) {
    cJSON_AddStringToObject(item, "label", partition->label);
    cJSON_AddNumberToObject(item, "address", partition->address);
    cJSON_AddNumberToObject(item, "size", partition->size);
    cJSON_AddNumberToObject(item, "subtype", partition->subtype);
  }
  cJSON_AddItemToObject(payload, key, item);
}

static void send_status_locked(
  const cJSON *request,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  const esp_app_desc_t *app = esp_app_get_description();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t state_err = running
    ? esp_ota_get_state_partition(running, &image_state)
    : ESP_ERR_NOT_FOUND;
  cJSON *payload = cJSON_CreateObject();
  const char *request_id = json_string(request, "requestId");
  if (request_id[0]) cJSON_AddStringToObject(payload, "requestId", request_id);
  cJSON_AddBoolToObject(payload, "supported", running && next && running != next);
  cJSON_AddBoolToObject(payload, "active", g_active);
  cJSON_AddBoolToObject(payload, "pendingVerify", g_pending_verify);
  cJSON_AddBoolToObject(payload, "runtimeReady", g_runtime_ready);
  cJSON_AddStringToObject(payload, "version", app ? app->version : "unknown");
  cJSON_AddStringToObject(payload, "projectName", app ? app->project_name : "unknown");
  cJSON_AddStringToObject(
    payload,
    "imageState",
    state_err == ESP_OK ? ota_state_name(image_state) : "unavailable"
  );
  add_partition(payload, "runningPartition", running);
  add_partition(payload, "bootPartition", boot);
  add_partition(payload, "nextPartition", next);
  if (g_active) {
    cJSON_AddStringToObject(payload, "transferId", g_transfer_id);
    cJSON_AddNumberToObject(payload, "receivedBytes", (double) g_received_size);
    cJSON_AddNumberToObject(payload, "expectedBytes", (double) g_expected_size);
    cJSON_AddNumberToObject(payload, "nextSequence", g_next_sequence);
  }
  send_topic(send_line, ctx, "firmware/status", payload);
}

static void send_ack_details(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  bool ok,
  const char *error,
  bool include_chunk,
  unsigned long long sequence,
  size_t chunk_bytes,
  bool duplicate
) {
  cJSON *payload = cJSON_CreateObject();
  if (!payload) return;
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  cJSON_AddBoolToObject(payload, "ok", ok);
  cJSON_AddNumberToObject(payload, "receivedBytes", (double) g_received_size);
  cJSON_AddNumberToObject(payload, "expectedBytes", (double) g_expected_size);
  cJSON_AddNumberToObject(payload, "nextSequence", g_next_sequence);
  if (include_chunk) {
    cJSON_AddNumberToObject(payload, "seq", (double) sequence);
    cJSON_AddNumberToObject(payload, "chunkBytes", (double) chunk_bytes);
    cJSON_AddBoolToObject(payload, "duplicate", duplicate);
  }
  if (g_update_partition) cJSON_AddStringToObject(payload, "targetPartition", g_update_partition->label);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  send_topic(send_line, ctx, "firmware/ack", payload);
}

static void send_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  bool ok,
  const char *error
) {
  send_ack_details(send_line, ctx, transfer_id, phase, ok, error, false, 0, 0, false);
}

static void send_chunk_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  bool ok,
  const char *error,
  unsigned long long sequence,
  size_t chunk_bytes,
  bool duplicate
) {
  send_ack_details(
    send_line,
    ctx,
    transfer_id,
    "chunk",
    ok,
    error,
    true,
    sequence,
    chunk_bytes,
    duplicate
  );
}

static void send_commit_ack(pet_p4_send_line_fn send_line, void *ctx) {
  cJSON *ack = cJSON_CreateObject();
  if (!ack || !g_commit_result.valid) {
    cJSON_Delete(ack);
    return;
  }
  cJSON_AddStringToObject(ack, "transferId", g_commit_result.transfer_id);
  cJSON_AddStringToObject(ack, "phase", "commit");
  cJSON_AddBoolToObject(ack, "ok", true);
  cJSON_AddBoolToObject(ack, "pendingReboot", true);
  cJSON_AddStringToObject(ack, "targetPartition", g_commit_result.target_partition);
  cJSON_AddStringToObject(ack, "version", g_commit_result.version);
  cJSON_AddStringToObject(ack, "projectName", g_commit_result.project_name);
  cJSON_AddStringToObject(ack, "sha256", g_commit_result.sha256);
  cJSON_AddNumberToObject(ack, "bytes", (double) g_commit_result.bytes);
  cJSON_AddNumberToObject(ack, "receivedBytes", (double) g_commit_result.bytes);
  cJSON_AddNumberToObject(ack, "expectedBytes", (double) g_commit_result.bytes);
  cJSON_AddNumberToObject(ack, "nextSequence", g_commit_result.next_sequence);
  send_topic(send_line, ctx, "firmware/ack", ack);
}

static void clear_transfer(bool abort_write) {
  if (g_active && abort_write && g_ota_handle) {
    (void) esp_ota_abort(g_ota_handle);
  }
  if (g_sha256_initialized) {
    mbedtls_sha256_free(&g_sha256);
    g_sha256_initialized = false;
  }
  g_ota_handle = 0;
  g_update_partition = NULL;
  g_transfer_id[0] = '\0';
  g_expected_sha256[0] = '\0';
  g_expected_size = 0;
  g_received_size = 0;
  g_next_sequence = 0;
  g_last_chunk_sequence = 0;
  g_last_chunk_size = 0;
  g_last_chunk_valid = false;
  g_last_activity_ms = 0;
  g_active = false;
}

static bool transfer_matches(const cJSON *payload) {
  const char *transfer_id = json_string(payload, "transferId");
  return g_active && transfer_id[0] && strcmp(transfer_id, g_transfer_id) == 0;
}

static bool sha256_text_valid(const char *value) {
  if (!value || strlen(value) != PET_P4_OTA_SHA256_HEX_LEN) return false;
  for (size_t i = 0; value[i]; i += 1) {
    if (!((value[i] >= '0' && value[i] <= '9')
          || (value[i] >= 'a' && value[i] <= 'f')
          || (value[i] >= 'A' && value[i] <= 'F'))) {
      return false;
    }
  }
  return true;
}

static void digest_hex(const unsigned char digest[32], char output[65]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i += 1) {
    output[i * 2] = digits[digest[i] >> 4];
    output[i * 2 + 1] = digits[digest[i] & 0x0f];
  }
  output[64] = '\0';
}

static void handle_begin(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *sha256 = json_string(payload, "sha256");
  unsigned long long size = json_u64(payload, "size", 0);
  if (g_restart_at_ms) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "device is rebooting into committed firmware");
    return;
  }
  if (pet_p4_diagnostics_reboot_pending()) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "device reboot is already scheduled");
    return;
  }
  if (g_pending_verify) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "running firmware is still pending validation");
    return;
  }
  if (g_state && g_state->asset_transfer_active) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "appearance asset transfer is active");
    return;
  }
  if (g_active) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "another firmware transfer is active");
    return;
  }
  g_update_partition = esp_ota_get_next_update_partition(NULL);
  if (!transfer_id[0] || strlen(transfer_id) >= sizeof(g_transfer_id)
      || !sha256_text_valid(sha256) || size == 0 || !g_update_partition) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "invalid metadata or no inactive OTA partition");
    clear_transfer(false);
    return;
  }
  if (size > g_update_partition->size) {
    send_ack(send_line, ctx, transfer_id, "begin", false, "firmware image exceeds OTA partition");
    clear_transfer(false);
    return;
  }
  esp_err_t err = esp_ota_begin(g_update_partition, (size_t) size, &g_ota_handle);
  if (err != ESP_OK) {
    send_ack(send_line, ctx, transfer_id, "begin", false, esp_err_to_name(err));
    clear_transfer(false);
    return;
  }
  mbedtls_sha256_init(&g_sha256);
  if (mbedtls_sha256_starts(&g_sha256, false) != 0) {
    snprintf(g_transfer_id, sizeof(g_transfer_id), "%s", transfer_id);
    g_active = true;
    send_ack(send_line, ctx, g_transfer_id, "begin", false, "SHA-256 initialization failed");
    clear_transfer(true);
    return;
  }
  g_sha256_initialized = true;
  snprintf(g_transfer_id, sizeof(g_transfer_id), "%s", transfer_id);
  snprintf(g_expected_sha256, sizeof(g_expected_sha256), "%s", sha256);
  for (size_t i = 0; g_expected_sha256[i]; i += 1) {
    if (g_expected_sha256[i] >= 'A' && g_expected_sha256[i] <= 'F') {
      g_expected_sha256[i] = (char) (g_expected_sha256[i] - 'A' + 'a');
    }
  }
  g_expected_size = size;
  g_received_size = 0;
  g_next_sequence = 0;
  g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  g_active = true;
  ESP_LOGI(TAG, "OTA begin transfer=%s target=%s bytes=%llu", g_transfer_id,
           g_update_partition->label, g_expected_size);
  send_ack(send_line, ctx, g_transfer_id, "begin", true, NULL);
}

static void handle_chunk(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  unsigned long long requested_sequence = json_u64(payload, "seq", ULLONG_MAX);
  unsigned int sequence = requested_sequence <= UINT_MAX
    ? (unsigned int) requested_sequence
    : UINT_MAX;
  unsigned long long decoded_size = json_u64(payload, "decodedSize", 0);
  const char *encoded = json_string(payload, "data");
  const char *request_transfer_id = json_string(payload, "transferId");
  size_t encoded_len = strlen(encoded);
  size_t actual_size = 0;
  unsigned char *decoded;
  if (!transfer_matches(payload)) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware transferId mismatch",
      requested_sequence,
      0,
      false
    );
    return;
  }
  g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  if (requested_sequence > UINT_MAX) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware chunk sequence is invalid",
      requested_sequence,
      0,
      false
    );
    return;
  }
  if (g_last_chunk_valid && sequence == g_last_chunk_sequence
      && sequence + 1 == g_next_sequence) {
    if (decoded_size != g_last_chunk_size) {
      send_chunk_ack(
        send_line,
        ctx,
        request_transfer_id,
        false,
        "duplicate firmware chunk size mismatch",
        sequence,
        0,
        true
      );
      return;
    }
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      true,
      NULL,
      sequence,
      g_last_chunk_size,
      true
    );
    return;
  }
  if (sequence != g_next_sequence) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware chunk sequence mismatch",
      sequence,
      0,
      false
    );
    return;
  }
  if (!encoded_len || !decoded_size || decoded_size > PET_P4_OTA_DECODED_CHUNK_MAX
      || g_received_size + decoded_size > g_expected_size) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "invalid firmware chunk size",
      sequence,
      0,
      false
    );
    return;
  }
  decoded = (unsigned char *) malloc((size_t) decoded_size);
  if (!decoded) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "out of memory decoding firmware chunk",
      sequence,
      0,
      false
    );
    return;
  }
  int decode_result = mbedtls_base64_decode(
    decoded,
    (size_t) decoded_size,
    &actual_size,
    (const unsigned char *) encoded,
    encoded_len
  );
  if (decode_result != 0 || actual_size != decoded_size) {
    free(decoded);
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      "firmware chunk base64 mismatch",
      sequence,
      0,
      false
    );
    return;
  }
  esp_err_t write_err = esp_ota_write(g_ota_handle, decoded, actual_size);
  int sha_err = write_err == ESP_OK ? mbedtls_sha256_update(&g_sha256, decoded, actual_size) : -1;
  free(decoded);
  if (write_err != ESP_OK || sha_err != 0) {
    send_chunk_ack(
      send_line,
      ctx,
      request_transfer_id,
      false,
      write_err != ESP_OK ? esp_err_to_name(write_err) : "SHA-256 update failed",
      sequence,
      0,
      false
    );
    clear_transfer(true);
    return;
  }
  g_received_size += actual_size;
  g_last_chunk_sequence = sequence;
  g_last_chunk_size = actual_size;
  g_last_chunk_valid = true;
  g_next_sequence += 1;
  send_chunk_ack(
    send_line,
    ctx,
    request_transfer_id,
    true,
    NULL,
    sequence,
    actual_size,
    false
  );
}

static void handle_commit(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  const char *request_transfer_id = json_string(payload, "transferId");
  unsigned char digest[32];
  char actual_sha256[65];
  esp_app_desc_t image_desc;
  const esp_app_desc_t *running_desc = esp_app_get_description();
  if (!g_active && g_commit_result.valid
      && request_transfer_id[0]
      && strcmp(request_transfer_id, g_commit_result.transfer_id) == 0) {
    send_commit_ack(send_line, ctx);
    return;
  }
  if (!transfer_matches(payload)) {
    send_ack(send_line, ctx, request_transfer_id, "commit", false, "firmware transferId mismatch");
    return;
  }
  g_last_activity_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  if (g_received_size != g_expected_size) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, "firmware byte total mismatch");
    return;
  }
  if (mbedtls_sha256_finish(&g_sha256, digest) != 0) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, "SHA-256 finalize failed");
    clear_transfer(true);
    return;
  }
  mbedtls_sha256_free(&g_sha256);
  g_sha256_initialized = false;
  digest_hex(digest, actual_sha256);
  if (strcmp(actual_sha256, g_expected_sha256) != 0) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, "firmware SHA-256 mismatch");
    clear_transfer(true);
    return;
  }
  esp_err_t end_err = esp_ota_end(g_ota_handle);
  g_ota_handle = 0;
  if (end_err != ESP_OK) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, esp_err_to_name(end_err));
    clear_transfer(false);
    return;
  }
  esp_err_t desc_err = esp_ota_get_partition_description(g_update_partition, &image_desc);
  if (desc_err != ESP_OK) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, "firmware image description is invalid");
    clear_transfer(false);
    return;
  }
  if (!running_desc || !running_desc->project_name[0]
      || strcmp(image_desc.project_name, running_desc->project_name) != 0) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, "firmware project identity mismatch");
    clear_transfer(false);
    return;
  }
  esp_err_t boot_err = esp_ota_set_boot_partition(g_update_partition);
  if (boot_err != ESP_OK) {
    send_ack(send_line, ctx, g_transfer_id, "commit", false, esp_err_to_name(boot_err));
    clear_transfer(false);
    return;
  }
  memset(&g_commit_result, 0, sizeof(g_commit_result));
  g_commit_result.valid = true;
  snprintf(g_commit_result.transfer_id, sizeof(g_commit_result.transfer_id), "%s", g_transfer_id);
  snprintf(g_commit_result.target_partition, sizeof(g_commit_result.target_partition), "%s",
           g_update_partition->label);
  snprintf(g_commit_result.version, sizeof(g_commit_result.version), "%s", image_desc.version);
  snprintf(g_commit_result.project_name, sizeof(g_commit_result.project_name), "%s",
           image_desc.project_name);
  snprintf(g_commit_result.sha256, sizeof(g_commit_result.sha256), "%s", actual_sha256);
  g_commit_result.bytes = g_received_size;
  g_commit_result.next_sequence = g_next_sequence;
  send_commit_ack(send_line, ctx);
  ESP_LOGI(TAG, "OTA verified transfer=%s target=%s version=%s; scheduling reboot",
           g_transfer_id, g_update_partition->label, image_desc.version);
  clear_transfer(false);
  g_restart_at_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL)
    + PET_P4_OTA_RESTART_DELAY_MS;
}

static void handle_abort(const cJSON *payload, pet_p4_send_line_fn send_line, void *ctx) {
  const char *request_transfer_id = json_string(payload, "transferId");
  if (!g_active) {
    send_ack(send_line, ctx, request_transfer_id, "abort", true, NULL);
    return;
  }
  if (!transfer_matches(payload)) {
    send_ack(send_line, ctx, request_transfer_id, "abort", false, "firmware transferId mismatch");
    return;
  }
  send_ack(send_line, ctx, g_transfer_id, "abort", true, NULL);
  clear_transfer(true);
}

esp_err_t pet_p4_ota_init(
  pet_p4_runtime_state_t *state,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (!g_ota_mutex) g_ota_mutex = xSemaphoreCreateMutex();
  if (!g_ota_mutex) return ESP_ERR_NO_MEM;
  if (xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;
  g_state = state;
  memset(&g_commit_result, 0, sizeof(g_commit_result));
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t err = running
    ? esp_ota_get_state_partition(running, &image_state)
    : ESP_ERR_NOT_FOUND;
  g_pending_verify = err == ESP_OK && image_state == ESP_OTA_IMG_PENDING_VERIFY;
  g_initialized_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  ESP_LOGI(TAG, "running partition=%s state=%s rollback=%s",
           running ? running->label : "none",
           err == ESP_OK ? ota_state_name(image_state) : "unavailable",
           g_pending_verify ? "pending" : "settled");
  bool supported = running && esp_ota_get_next_update_partition(NULL);
  g_initialized = true;
  xSemaphoreGive(g_ota_mutex);
  pet_p4_ota_send_status(send_line, ctx);
  return supported ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

bool pet_p4_ota_handle_topic(
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  if (!topic) return false;
  bool handled = true;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return false;
  if (!g_initialized) {
    const char *transfer_id = json_string(payload, "transferId");
    if (strcmp(topic, "firmware/query") == 0) {
      send_status_locked(payload, send_line, ctx);
    } else if (strcmp(topic, "firmware/chunk") == 0) {
      send_chunk_ack(
        send_line,
        ctx,
        transfer_id,
        false,
        "firmware OTA is initializing",
        json_u64(payload, "seq", ULLONG_MAX),
        0,
        false
      );
    } else if (strcmp(topic, "firmware/begin") == 0
               || strcmp(topic, "firmware/commit") == 0
               || strcmp(topic, "firmware/abort") == 0) {
      send_ack(
        send_line,
        ctx,
        transfer_id,
        topic + strlen("firmware/"),
        false,
        "firmware OTA is initializing"
      );
    } else {
      handled = false;
    }
    xSemaphoreGive(g_ota_mutex);
    return handled;
  }
  if (strcmp(topic, "firmware/begin") == 0) {
    handle_begin(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/chunk") == 0) {
    handle_chunk(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/commit") == 0) {
    handle_commit(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/abort") == 0) {
    handle_abort(payload, send_line, ctx);
  } else if (strcmp(topic, "firmware/query") == 0) {
    send_status_locked(payload, send_line, ctx);
  } else {
    handled = false;
  }
  xSemaphoreGive(g_ota_mutex);
  return handled;
}

void pet_p4_ota_send_status(pet_p4_send_line_fn send_line, void *ctx) {
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  send_status_locked(NULL, send_line, ctx);
  xSemaphoreGive(g_ota_mutex);
}

void pet_p4_ota_runtime_ready(unsigned long long now_ms) {
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  if (!g_runtime_last_healthy_ms
      || now_ms - g_runtime_last_healthy_ms > PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS) {
    g_runtime_first_healthy_ms = now_ms;
    g_runtime_healthy_samples = 0;
  }
  g_runtime_last_healthy_ms = now_ms;
  g_runtime_healthy_samples += 1;
  xSemaphoreGive(g_ota_mutex);
}

void pet_p4_ota_runtime_failed(unsigned long long now_ms) {
  (void) now_ms;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  g_runtime_ready = false;
  g_runtime_healthy_samples = 0;
  g_runtime_first_healthy_ms = 0;
  g_runtime_last_healthy_ms = 0;
  xSemaphoreGive(g_ota_mutex);
}

void pet_p4_ota_process(unsigned long long now_ms) {
  bool rollback = false;
  bool restart = false;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return;
  if (g_active && g_last_activity_ms
      && now_ms >= g_last_activity_ms
      && now_ms - g_last_activity_ms >= PET_P4_OTA_TRANSFER_IDLE_TIMEOUT_MS) {
    ESP_LOGW(TAG, "aborting idle OTA transfer=%s after %llu ms", g_transfer_id,
             now_ms - g_last_activity_ms);
    clear_transfer(true);
  }
  g_runtime_ready = g_runtime_healthy_samples >= PET_P4_OTA_VALIDATE_MIN_SAMPLES
    && g_runtime_first_healthy_ms
    && now_ms - g_runtime_first_healthy_ms >= PET_P4_OTA_VALIDATE_STABLE_MS
    && now_ms - g_runtime_last_healthy_ms <= PET_P4_OTA_VALIDATE_SAMPLE_GAP_MS;
  if (g_pending_verify && g_runtime_ready) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
      g_pending_verify = false;
      ESP_LOGI(TAG, "OTA image marked valid after sustained runtime validation");
    } else {
      ESP_LOGE(TAG, "failed to mark OTA image valid: %s", esp_err_to_name(err));
    }
  }
  if (g_pending_verify
      && now_ms - g_initialized_ms >= PET_P4_OTA_FAILED_BOOT_RESTART_MS) {
    rollback = true;
  }
  if (g_restart_at_ms && now_ms >= g_restart_at_ms) {
    restart = true;
  }
  xSemaphoreGive(g_ota_mutex);
  if (rollback) {
    ESP_LOGE(TAG, "runtime self-test did not stay healthy; rolling back OTA image");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  if (restart) {
    ESP_LOGI(TAG, "restarting into verified OTA image");
    esp_restart();
  }
}

bool pet_p4_ota_active(void) {
  bool active = false;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return false;
  active = g_active || g_pending_verify || g_restart_at_ms != 0;
  xSemaphoreGive(g_ota_mutex);
  return active;
}

bool pet_p4_ota_transfer_active(void) {
  bool active = false;
  if (!g_ota_mutex || xSemaphoreTake(g_ota_mutex, portMAX_DELAY) != pdTRUE) return false;
  active = g_active;
  xSemaphoreGive(g_ota_mutex);
  return active;
}
