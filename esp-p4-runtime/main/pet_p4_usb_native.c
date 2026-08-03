/*
 * [Input] TinyUSB vendor frames for JSON control and raw asset payloads.
 * [Output] framed native-USB responses, callback-safe queued protocol work,
 *          serialized runtime mutations, transactional asset writes,
 *          restart-safe slot cleanup, and legacy-pack migration.
 * [Pos] ESP32-P4 high-speed USB device transport.
 * [Sync] If this file changes, update esp-p4-runtime/protocol.md and .folder.md.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "pet_p4_build_info.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_ota.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_usb_native.h"

static const char *TAG = "pet-p4-native-usb";

#define PET_P4_NATIVE_MAGIC "P4BU"
#define PET_P4_NATIVE_VERSION 1
#define PET_P4_NATIVE_HEADER_LEN 16
#define PET_P4_NATIVE_MAX_PAYLOAD (64 * 1024)
#define PET_P4_NATIVE_PATH_MAX 96
#define PET_P4_NATIVE_TRANSFER_MAX 64
#define PET_P4_NATIVE_RX_QUEUE_DEPTH 4

enum {
  PET_P4_NATIVE_KIND_JSON = 1,
  PET_P4_NATIVE_KIND_FILE_BEGIN = 2,
  PET_P4_NATIVE_KIND_FILE_DATA = 3,
  PET_P4_NATIVE_KIND_FILE_END = 4,
  PET_P4_NATIVE_KIND_COMMIT = 5,
  PET_P4_NATIVE_KIND_PING = 6,
};

static pet_p4_runtime_state_t *g_state;
static usb_phy_handle_t g_phy;
static bool g_started;
static uint32_t g_tx_seq;
static SemaphoreHandle_t g_tx_mutex;
static SemaphoreHandle_t g_state_mutex;
static QueueHandle_t g_rx_queue;
static pet_p4_native_protocol_enqueue_fn g_enqueue_protocol;
static void *g_enqueue_protocol_ctx;

typedef struct {
  uint8_t kind;
  uint32_t seq;
  uint32_t payload_len;
  uint8_t *payload;
} pet_p4_native_rx_frame_t;

static uint8_t g_header[PET_P4_NATIVE_HEADER_LEN];
static size_t g_header_used;
static uint8_t *g_payload;
static uint32_t g_payload_len;
static uint32_t g_payload_used;
static uint8_t g_frame_kind;
static uint32_t g_frame_seq;

static char g_transfer_id[PET_P4_NATIVE_TRANSFER_MAX];
static char g_cleaned_transfer_id[PET_P4_NATIVE_TRANSFER_MAX];
static char g_current_path[PET_P4_NATIVE_PATH_MAX];
static char g_expected_checksum[17];
static unsigned long long g_expected_size;
static unsigned long long g_received_size;
static int g_target_slot = -1;

static uint32_t le32(const uint8_t *bytes) {
  return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8) | ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static void put_le32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t) (value & 0xff);
  bytes[1] = (uint8_t) ((value >> 8) & 0xff);
  bytes[2] = (uint8_t) ((value >> 16) & 0xff);
  bytes[3] = (uint8_t) ((value >> 24) & 0xff);
}

static unsigned long long fnv1a64_update(unsigned long long hash, const unsigned char *bytes, size_t len) {
  for (size_t i = 0; i < len; i += 1) {
    hash ^= (unsigned long long) bytes[i];
    hash *= 0x00000100000001b3ULL;
  }
  return hash;
}

static unsigned long long fnv1a64_file(FILE *file, unsigned long long *size_out) {
  unsigned char buf[512];
  unsigned long long hash = 0xcbf29ce484222325ULL;
  unsigned long long size = 0;
  size_t n;
  if (!file) return hash;
  while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
    hash = fnv1a64_update(hash, buf, n);
    size += (unsigned long long) n;
  }
  if (size_out) *size_out = size;
  return hash;
}

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static unsigned long long json_u64(const cJSON *object, const char *key, unsigned long long fallback) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item)) return fallback;
  return (unsigned long long) item->valuedouble;
}

static cJSON *parse_payload_json(const uint8_t *payload, uint32_t len) {
  char *text = (char *) malloc((size_t) len + 1);
  cJSON *root;
  if (!text) return NULL;
  memcpy(text, payload, len);
  text[len] = '\0';
  root = cJSON_Parse(text);
  free(text);
  return root;
}

static bool native_write_all(const uint8_t *bytes, size_t len) {
  size_t offset = 0;
  int idle_ticks = 0;
  while (offset < len && idle_ticks < 200) {
    uint32_t written = tud_vendor_write(bytes + offset, len - offset);
    if (written == 0) {
      tud_vendor_write_flush();
      vTaskDelay(pdMS_TO_TICKS(1));
      idle_ticks += 1;
    } else {
      offset += written;
      idle_ticks = 0;
    }
  }
  return offset == len;
}

static bool native_send_frame(uint8_t kind, const uint8_t *payload, uint32_t len) {
  uint8_t header[PET_P4_NATIVE_HEADER_LEN];
  bool ok = false;
  if (!g_started || !tud_mounted() || !g_tx_mutex) return false;
  if (xSemaphoreTake(g_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  if (!tud_mounted()) goto done;
  memcpy(header, PET_P4_NATIVE_MAGIC, 4);
  header[4] = PET_P4_NATIVE_VERSION;
  header[5] = kind;
  header[6] = 0;
  header[7] = 0;
  put_le32(&header[8], g_tx_seq++);
  put_le32(&header[12], len);
  ok = native_write_all(header, sizeof(header)) && native_write_all(payload, len);
  tud_vendor_write_flush();
done:
  xSemaphoreGive(g_tx_mutex);
  return ok;
}

void pet_p4_native_usb_send_json_line(const char *line, void *ctx) {
  (void) ctx;
  if (!line || !line[0]) return;
  (void) native_send_frame(PET_P4_NATIVE_KIND_JSON, (const uint8_t *) line, (uint32_t) strlen(line));
}

static void send_topic_payload(const char *topic, cJSON *payload) {
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
  if (line) pet_p4_native_usb_send_json_line(line, NULL);
  cJSON_free(line);
  cJSON_Delete(root);
}

static void send_asset_ack(const char *transfer_id, const char *phase, const char *path, bool ok, const char *error) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  if (path && path[0]) cJSON_AddStringToObject(payload, "path", path);
  cJSON_AddBoolToObject(payload, "ok", ok);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  send_topic_payload("asset/ack", payload);
}

static bool clear_slot_files(int slot) {
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "s%d_", slot);
  while (true) {
    DIR *dir = opendir("/spiffs");
    struct dirent *entry;
    char fs_path[96];
    bool found = false;
    if (!dir) return false;
    while ((entry = readdir(dir)) != NULL) {
      if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
      snprintf(fs_path, sizeof(fs_path), "/spiffs/%s", entry->d_name);
      found = true;
      break;
    }
    closedir(dir);
    if (!found) {
      if (slot == PET_P4_RAW_APPEARANCE_SLOT && pet_p4_raw_assets_available()) {
        char error[96];
        if (!pet_p4_raw_assets_invalidate(error, sizeof(error))) {
          ESP_LOGE(TAG, "failed to invalidate native raw slot: %s", error);
          return false;
        }
      }
      return true;
    }
    errno = 0;
    if (remove(fs_path) != 0 && errno != ENOENT) {
      ESP_LOGE(TAG, "failed to clear appearance slot=%d path=%s error=%s",
               slot, fs_path, errno ? strerror(errno) : "unknown");
      return false;
    }
  }
}

static bool ensure_transfer_slot(const char *transfer_id) {
  if (g_target_slot < 0 || strcmp(g_cleaned_transfer_id, transfer_id ? transfer_id : "") != 0) {
    g_target_slot = PET_P4_RAW_APPEARANCE_SLOT;
    if (!clear_slot_files(g_target_slot)) return false;
    snprintf(g_cleaned_transfer_id, sizeof(g_cleaned_transfer_id), "%s", transfer_id ? transfer_id : "");
  }
  return g_target_slot >= 0;
}

static void handle_file_begin(const uint8_t *payload, uint32_t len) {
  cJSON *root = parse_payload_json(payload, len);
  const char *transfer_id;
  const char *path;
  char tmp_path[96];
  FILE *file;
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    send_asset_ack("", "file-begin", "", false, "invalid file metadata");
    return;
  }
  transfer_id = json_string(root, "transferId");
  path = json_string(root, "path");
  if (pet_p4_diagnostics_reboot_pending() || pet_p4_ota_transfer_active()) {
    send_asset_ack(transfer_id, "file-begin", path, false,
                   "device firmware update or reboot is active");
    cJSON_Delete(root);
    return;
  }
  if (!ensure_transfer_slot(transfer_id) ||
      !pet_p4_asset_fs_path_for_slot(g_target_slot, path, true, tmp_path, sizeof(tmp_path))) {
    send_asset_ack(transfer_id, "file-begin", path, false, "unsupported p4 asset path");
    cJSON_Delete(root);
    return;
  }
  file = fopen(tmp_path, "wb");
  if (!file) {
    send_asset_ack(transfer_id, "file-begin", path, false, strerror(errno));
    cJSON_Delete(root);
    return;
  }
  fclose(file);
  snprintf(g_transfer_id, sizeof(g_transfer_id), "%s", transfer_id);
  snprintf(g_current_path, sizeof(g_current_path), "%s", path);
  snprintf(g_expected_checksum, sizeof(g_expected_checksum), "%s", json_string(root, "checksum"));
  g_expected_size = json_u64(root, "size", 0);
  g_received_size = 0;
  if (g_state) g_state->asset_transfer_active = true;
  send_asset_ack(transfer_id, "file-begin", path, true, NULL);
  cJSON_Delete(root);
}

static void handle_file_data(const uint8_t *payload, uint32_t len) {
  char tmp_path[96];
  FILE *file;
  if (!g_current_path[0] ||
      !pet_p4_asset_fs_path_for_slot(g_target_slot, g_current_path, true, tmp_path, sizeof(tmp_path))) {
    send_asset_ack(g_transfer_id, "chunk", g_current_path, false, "file not open");
    return;
  }
  file = fopen(tmp_path, "ab");
  if (!file) {
    send_asset_ack(g_transfer_id, "chunk", g_current_path, false, strerror(errno));
    return;
  }
  if (len > 0 && fwrite(payload, 1, len, file) != len) {
    fclose(file);
    send_asset_ack(g_transfer_id, "chunk", g_current_path, false, "chunk write failed");
    return;
  }
  fclose(file);
  g_received_size += (unsigned long long) len;
}

static void handle_file_end(const uint8_t *payload, uint32_t len) {
  cJSON *root = len > 0 ? parse_payload_json(payload, len) : NULL;
  const char *path = cJSON_IsObject(root) ? json_string(root, "path") : g_current_path;
  char tmp_path[96];
  char fs_path[96];
  char checksum_hex[17];
  unsigned long long size = 0;
  unsigned long long checksum = 0;
  FILE *file;
  if (!path || !path[0]) path = g_current_path;
  if (!pet_p4_asset_fs_path_for_slot(g_target_slot, path, true, tmp_path, sizeof(tmp_path)) ||
      !pet_p4_asset_fs_path_for_slot(g_target_slot, path, false, fs_path, sizeof(fs_path))) {
    send_asset_ack(g_transfer_id, "file", path, false, "unsupported p4 asset path");
    cJSON_Delete(root);
    return;
  }
  file = fopen(tmp_path, "rb");
  if (!file) {
    send_asset_ack(g_transfer_id, "file", path, false, "missing staged file");
    cJSON_Delete(root);
    return;
  }
  checksum = fnv1a64_file(file, &size);
  fclose(file);
  snprintf(checksum_hex, sizeof(checksum_hex), "%016llx", checksum);
  if (size != g_expected_size || (g_expected_checksum[0] && strcmp(checksum_hex, g_expected_checksum) != 0)) {
    char error[160];
    snprintf(
      error,
      sizeof(error),
      "checksum mismatch expected_size=%llu actual_size=%llu expected=%s actual=%s",
      g_expected_size,
      size,
      g_expected_checksum,
      checksum_hex
    );
    remove(tmp_path);
    send_asset_ack(g_transfer_id, "file", path, false, error);
    cJSON_Delete(root);
    return;
  }
  remove(fs_path);
  if (rename(tmp_path, fs_path) != 0) {
    send_asset_ack(g_transfer_id, "file", path, false, strerror(errno));
    cJSON_Delete(root);
    return;
  }
  send_asset_ack(g_transfer_id, "file", path, true, NULL);
  g_current_path[0] = '\0';
  g_expected_checksum[0] = '\0';
  g_expected_size = 0;
  g_received_size = 0;
  cJSON_Delete(root);
}

static void handle_commit(const uint8_t *payload, uint32_t len) {
  cJSON *root = parse_payload_json(payload, len);
  const char *transfer_id = cJSON_IsObject(root) ? json_string(root, "transferId") : g_transfer_id;
  if (g_target_slot < 0 ||
      !pet_p4_asset_mark_slot_ready(g_target_slot) ||
      !pet_p4_asset_set_active_slot(g_target_slot)) {
    send_asset_ack(transfer_id, "commit", NULL, false, "slot commit failed");
    cJSON_Delete(root);
    return;
  }
  pet_p4_asset_clean_legacy_files();
  if (g_state) {
    pet_p4_load_asset_manifest(g_state);
    g_state->asset_transfer_active = false;
  }
  send_asset_ack(transfer_id, "commit", NULL, true, NULL);
  ESP_LOGI(TAG, "native USB committed transfer=%s slot=%d", transfer_id, g_target_slot);
  cJSON_Delete(root);
}

static bool handle_asset_abort_json(const uint8_t *payload, uint32_t len) {
  cJSON *root = parse_payload_json(payload, len);
  cJSON *body;
  const char *transfer_id;
  if (!cJSON_IsObject(root) || strcmp(json_string(root, "topic"), "asset/abort") != 0) {
    cJSON_Delete(root);
    return false;
  }
  body = cJSON_GetObjectItemCaseSensitive(root, "payload");
  transfer_id = cJSON_IsObject(body) ? json_string(body, "transferId") : g_transfer_id;
  if (g_target_slot >= 0) (void) clear_slot_files(g_target_slot);
  g_transfer_id[0] = '\0';
  g_cleaned_transfer_id[0] = '\0';
  g_current_path[0] = '\0';
  g_expected_checksum[0] = '\0';
  g_expected_size = 0;
  g_received_size = 0;
  g_target_slot = -1;
  if (g_state) g_state->asset_transfer_active = false;
  send_asset_ack(transfer_id, "abort", NULL, true, NULL);
  cJSON_Delete(root);
  return true;
}

static void handle_json_frame(const uint8_t *payload, uint32_t len) {
  char *line;
  if (handle_asset_abort_json(payload, len)) return;
  line = (char *) malloc((size_t) len + 1);
  if (!line) return;
  memcpy(line, payload, len);
  line[len] = '\0';
  if (g_state) {
    if (g_enqueue_protocol) {
      (void) g_enqueue_protocol(
        line,
        pet_p4_native_usb_send_json_line,
        NULL,
        g_enqueue_protocol_ctx
      );
    } else {
      pet_p4_handle_line(g_state, line, pet_p4_native_usb_send_json_line, NULL);
    }
  }
  free(line);
}

static void handle_identity_ping(const uint8_t *payload, uint32_t len) {
  cJSON *request = parse_payload_json(payload, len);
  const char *nonce;
  cJSON *response;
  if (!cJSON_IsObject(request)) {
    ESP_LOGW(TAG, "ignoring native identity ping with invalid JSON");
    cJSON_Delete(request);
    return;
  }
  nonce = json_string(request, "nonce");
  if (!nonce[0] || !g_state || !g_state->board_device_id[0] ||
      strcmp(g_state->board_device_id, "p4-unknown") == 0) {
    ESP_LOGW(TAG, "ignoring native identity ping without nonce or board identity");
    cJSON_Delete(request);
    return;
  }
  response = cJSON_CreateObject();
  if (!response) {
    cJSON_Delete(request);
    return;
  }
  cJSON_AddStringToObject(response, "protocol", "pet-usb-native-v1");
  cJSON_AddStringToObject(response, "boardDeviceId", g_state->board_device_id);
  cJSON_AddStringToObject(response, "nonce", nonce);
  cJSON_AddNumberToObject(response, "protocolSchema", PET_P4_PROTOCOL_SCHEMA);
  cJSON_AddStringToObject(response, "buildId", PET_P4_BUILD_ID);
  send_topic_payload("native/pong", response);
  cJSON_Delete(request);
}

static void handle_complete_frame(
  uint8_t kind,
  const uint8_t *payload,
  uint32_t payload_len
) {
  if (kind == PET_P4_NATIVE_KIND_JSON) {
    handle_json_frame(payload, payload_len);
  } else if (kind == PET_P4_NATIVE_KIND_FILE_BEGIN) {
    handle_file_begin(payload, payload_len);
  } else if (kind == PET_P4_NATIVE_KIND_FILE_DATA) {
    handle_file_data(payload, payload_len);
  } else if (kind == PET_P4_NATIVE_KIND_FILE_END) {
    handle_file_end(payload, payload_len);
  } else if (kind == PET_P4_NATIVE_KIND_COMMIT) {
    handle_commit(payload, payload_len);
  } else if (kind == PET_P4_NATIVE_KIND_PING) {
    handle_identity_ping(payload, payload_len);
  }
}

static bool enqueue_complete_frame(void) {
  pet_p4_native_rx_frame_t frame = {
    .kind = g_frame_kind,
    .seq = g_frame_seq,
    .payload_len = g_payload_len,
    .payload = g_payload,
  };
  if (!g_rx_queue || xQueueSend(g_rx_queue, &frame, 0) != pdTRUE) {
    ESP_LOGW(TAG, "dropping native frame because protocol queue is full seq=%u",
             (unsigned int) g_frame_seq);
    return false;
  }
  g_payload = NULL;
  return true;
}

static void reset_rx_frame(void) {
  free(g_payload);
  g_payload = NULL;
  g_payload_len = 0;
  g_payload_used = 0;
  g_header_used = 0;
  g_frame_kind = 0;
  g_frame_seq = 0;
}

static bool start_payload_from_header(void) {
  if (memcmp(g_header, PET_P4_NATIVE_MAGIC, 4) != 0 || g_header[4] != PET_P4_NATIVE_VERSION) {
    reset_rx_frame();
    return false;
  }
  g_frame_kind = g_header[5];
  g_frame_seq = le32(&g_header[8]);
  g_payload_len = le32(&g_header[12]);
  if (g_payload_len > PET_P4_NATIVE_MAX_PAYLOAD) {
    ESP_LOGW(TAG, "dropping oversized native frame seq=%u len=%u", (unsigned int) g_frame_seq, (unsigned int) g_payload_len);
    reset_rx_frame();
    return false;
  }
  if (g_payload_len == 0) {
    (void) enqueue_complete_frame();
    reset_rx_frame();
    return false;
  }
  g_payload = (uint8_t *) malloc(g_payload_len);
  if (!g_payload) {
    reset_rx_frame();
    return false;
  }
  g_payload_used = 0;
  return true;
}

static void feed_rx_bytes(const uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    if (g_header_used < PET_P4_NATIVE_HEADER_LEN) {
      size_t n = PET_P4_NATIVE_HEADER_LEN - g_header_used;
      if (n > len - offset) n = len - offset;
      memcpy(g_header + g_header_used, data + offset, n);
      g_header_used += n;
      offset += n;
      if (g_header_used == PET_P4_NATIVE_HEADER_LEN) {
        (void) start_payload_from_header();
      }
      continue;
    }
    if (!g_payload) {
      reset_rx_frame();
      continue;
    }
    size_t n = g_payload_len - g_payload_used;
    if (n > len - offset) n = len - offset;
    memcpy(g_payload + g_payload_used, data + offset, n);
    g_payload_used += (uint32_t) n;
    offset += n;
    if (g_payload_used == g_payload_len) {
      (void) enqueue_complete_frame();
      reset_rx_frame();
    }
  }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize) {
  (void) buffer;
  (void) bufsize;
  static uint8_t rx_buf[512];
  while (tud_vendor_n_available(itf)) {
    int n = tud_vendor_n_read(itf, rx_buf, sizeof(rx_buf));
    if (n > 0) feed_rx_bytes(rx_buf, (size_t) n);
  }
}

static void tusb_device_task(void *arg) {
  (void) arg;
  while (true) {
    tud_task();
  }
}

static void native_protocol_worker_task(void *arg) {
  (void) arg;
  pet_p4_native_rx_frame_t frame;
  while (true) {
    if (xQueueReceive(g_rx_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
    if (g_state_mutex) xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    handle_complete_frame(frame.kind, frame.payload, frame.payload_len);
    if (g_state_mutex) xSemaphoreGive(g_state_mutex);
    free(frame.payload);
  }
}

esp_err_t pet_p4_native_usb_init(
  pet_p4_runtime_state_t *state,
  SemaphoreHandle_t state_mutex,
  pet_p4_native_protocol_enqueue_fn enqueue_protocol,
  void *enqueue_ctx
) {
  if (g_started) return ESP_OK;
  g_state = state;
  g_state_mutex = state_mutex;
  g_enqueue_protocol = enqueue_protocol;
  g_enqueue_protocol_ctx = enqueue_ctx;
  if (!g_tx_mutex) g_tx_mutex = xSemaphoreCreateMutex();
  if (!g_tx_mutex) return ESP_ERR_NO_MEM;
  usb_phy_config_t phy_conf = {
    .controller = USB_PHY_CTRL_OTG,
    .target = USB_PHY_TARGET_INT,
    .otg_mode = USB_OTG_MODE_DEVICE,
  };
  esp_err_t err = usb_new_phy(&phy_conf, &g_phy);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "USB PHY init failed: %s", esp_err_to_name(err));
    return err;
  }
  if (!tusb_init()) {
    ESP_LOGW(TAG, "TinyUSB init failed");
    return ESP_FAIL;
  }
  g_rx_queue = xQueueCreate(PET_P4_NATIVE_RX_QUEUE_DEPTH, sizeof(pet_p4_native_rx_frame_t));
  if (!g_rx_queue) return ESP_ERR_NO_MEM;
  if (xTaskCreate(
        native_protocol_worker_task,
        "pet_p4_usb_exec",
        8192,
        NULL,
        9,
        NULL
      ) != pdPASS) {
    vQueueDelete(g_rx_queue);
    g_rx_queue = NULL;
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(tusb_device_task, "pet_p4_tusb", 4096, NULL, 8, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  g_started = true;
  ESP_LOGI(TAG, "P4 native USB vendor bulk ready VID=0x303A PID=0x4040 protocol=pet-usb-native-v1");
  return ESP_OK;
}

void tud_mount_cb(void) {
  ESP_LOGI(TAG, "native USB mounted");
  if (g_state) {
    if (g_state_mutex) xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    pet_p4_state_set_native_usb_mounted(
      g_state,
      true,
      (unsigned long long) (esp_timer_get_time() / 1000ULL)
    );
    pet_p4_send_hello(g_state, pet_p4_native_usb_send_json_line, NULL);
    if (g_state_mutex) xSemaphoreGive(g_state_mutex);
  }
}

void tud_umount_cb(void) {
  ESP_LOGI(TAG, "native USB unmounted");
  if (g_state) {
    if (g_state_mutex) xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    pet_p4_state_set_native_usb_mounted(
      g_state,
      false,
      (unsigned long long) (esp_timer_get_time() / 1000ULL)
    );
    if (g_state_mutex) xSemaphoreGive(g_state_mutex);
  }
}

void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
}

void tud_resume_cb(void) {
}
