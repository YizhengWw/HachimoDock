/*
 * [Input] JSON-line control/state envelopes from USB-UART and native USB.
 * [Output] validated runtime updates, ACK/NACK responses, persisted input
 *          configuration snapshots, transfer routing, deterministic appearance
 *          pack discovery/reactivation across both flash slots, compatible
 *          v3/v4 component capabilities with bounded sprite limits, restart-safe
 *          slot cleanup, idle-transfer recovery and legacy-pack migration, 60-second per-session
 *          terminal conversation visibility across active-queue refreshes,
 *          stable first-seen card ordering, and exact retained-card selection
 *          preservation after host refreshes.
 * [Pos] shared ESP32-P4 protocol dispatcher.
 * [Sync] If this file changes, update esp-p4-runtime/protocol.md and .folder.md.
 */

#include "pet_p4_protocol.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "bsp/display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_p4_audio.h"
#include "pet_p4_build_info.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_input.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_ota.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_touch.h"

#define PET_P4_WIRE_PROTOCOL "pet-usb-jsonl-v3"
#define PET_P4_SPIFFS_PREFIX "/spiffs"
#define PET_P4_FS_PATH_MAX 64
#define PET_P4_LOGICAL_PATH_MAX 128
#define PET_P4_DECODED_CHUNK_LIMIT (64 * 1024)
#define PET_P4_ASSET_SLOT_COUNT 2
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_RAW_ASSET_CHUNK_LIMIT (64 * 1024)
#define PET_P4_ASSET_BEGIN_GC_BYTES (64 * 1024)
#define PET_P4_ASSET_BEGIN_GC_SLICE_BYTES (4 * 1024)
#define PET_P4_JSON_SAFE_INTEGER_MAX 9007199254740991ULL
#define PET_P4_ASSET_SLOT_MARKER PET_P4_SPIFFS_PREFIX "/p4slot.txt"
#define PET_P4_SESSION_NOTICE_MS 2200ULL

static const char *TAG = "pet-p4-protocol";
static int g_asset_active_slot = -1;
static int g_asset_transfer_slot = -1;
static char g_asset_transfer_id[PET_P4_DEVICE_ID_MAX];
static unsigned long long g_asset_transfer_last_activity_ms;
static FILE *g_asset_chunk_file;
static char g_asset_chunk_logical_path[PET_P4_LOGICAL_PATH_MAX];
static char g_asset_chunk_error[128];
static unsigned long long g_asset_chunk_size;
static unsigned long long g_asset_chunk_checksum = 0xcbf29ce484222325ULL;
static bool g_asset_chunk_stat_valid;
static struct {
  bool active;
  unsigned char *data;
  size_t expected_size;
  size_t received_size;
  unsigned long long expected_checksum;
  char transfer_id[PET_P4_DEVICE_ID_MAX];
  char logical_path[PET_P4_LOGICAL_PATH_MAX];
  char index[24];
} g_raw_asset_chunk;

static unsigned long long fnv1a64_update(unsigned long long hash, const unsigned char *bytes, size_t len) {
  for (size_t i = 0; i < len; i += 1) {
    hash ^= (unsigned long long) bytes[i];
    hash *= 0x00000100000001b3ULL;
  }
  return hash;
}

static unsigned long long fnv1a64_bytes(const unsigned char *bytes, size_t len) {
  return fnv1a64_update(0xcbf29ce484222325ULL, bytes, len);
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

static void copy_text(char *dest, size_t dest_size, const char *src) {
  if (!dest || dest_size == 0) return;
  if (!src) src = "";
  size_t limit = dest_size - 1;
  size_t copied = 0;
  while (src[copied] && copied < limit) {
    unsigned char lead = (unsigned char) src[copied];
    size_t width = lead < 0x80 ? 1 : ((lead & 0xE0) == 0xC0 ? 2 : ((lead & 0xF0) == 0xE0 ? 3 : 4));
    if (copied + width > limit) break;
    bool valid = true;
    for (size_t i = 1; i < width; i += 1) {
      if (!src[copied + i] || (((unsigned char) src[copied + i] & 0xC0) != 0x80)) {
        valid = false;
        break;
      }
    }
    copied += valid ? width : 1;
  }
  memcpy(dest, src, copied);
  dest[copied] = '\0';
}

static esp_err_t run_spiffs_gc_yielding(void) {
  for (
    size_t target = PET_P4_ASSET_BEGIN_GC_SLICE_BYTES;
    target <= PET_P4_ASSET_BEGIN_GC_BYTES;
    target += PET_P4_ASSET_BEGIN_GC_SLICE_BYTES
  ) {
    esp_err_t err = esp_spiffs_gc("storage", target);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return ESP_OK;
}

static const char *runtime_lifecycle(const pet_p4_runtime_state_t *state) {
  if (!state) return "idle";
  return state->current_status[0] ? state->current_status : state->current_state;
}

static bool lifecycle_is(const char *value, const char *canonical) {
  if (!value || !canonical) return false;
  if (strcmp(value, canonical) == 0) return true;
  if (strcmp(canonical, "done") == 0) {
    return strcmp(value, "complete") == 0 || strcmp(value, "completed") == 0;
  }
  if (strcmp(canonical, "error") == 0) return strcmp(value, "failed") == 0;
  if (strcmp(canonical, "waiting_user") == 0) return strcmp(value, "notification") == 0;
  return false;
}

static bool lifecycle_is_terminal(const char *value) {
  return lifecycle_is(value, "done") || lifecycle_is(value, "error");
}

static bool lifecycle_is_active(const char *value) {
  return lifecycle_is(value, "working")
    || lifecycle_is(value, "thinking")
    || lifecycle_is(value, "tool_running")
    || lifecycle_is(value, "speaking")
    || lifecycle_is(value, "waiting_user");
}

static bool session_ids_match(const char *left, const char *right) {
  if (!left || !right || !left[0] || !right[0]) return false;
  if (strcmp(left, right) == 0) return true;
  const char *left_short = strchr(left, ':');
  const char *right_short = strchr(right, ':');
  left_short = left_short && left_short[1] ? left_short + 1 : left;
  right_short = right_short && right_short[1] ? right_short + 1 : right;
  return strcmp(left_short, right_short) == 0;
}

static pet_p4_session_queue_item_t *find_session_queue_item(
  pet_p4_session_queue_item_t *items,
  unsigned int count,
  const char *session_id
) {
  if (!items || !session_id || !session_id[0]) return NULL;
  for (unsigned int i = 0; i < count; i += 1) {
    if (session_ids_match(items[i].id, session_id)) return &items[i];
  }
  return NULL;
}

static bool json_session_id_array_contains(const cJSON *items, const char *session_id) {
  if (!cJSON_IsArray(items) || !session_id || !session_id[0]) return false;
  const cJSON *item = NULL;
  cJSON_ArrayForEach(item, items) {
    if (cJSON_IsString(item) && session_ids_match(item->valuestring, session_id)) return true;
  }
  return false;
}

static void restore_idle_session_view(
  pet_p4_runtime_state_t *state,
  const char *event
) {
  if (!state || state->session_queue_count != 0) return;
  const char *next_event = event && event[0] ? event : "session_queue_empty";
  bool changed = !lifecycle_is(runtime_lifecycle(state), "idle")
    || strcmp(state->current_speech, "休息中") != 0
    || strcmp(state->current_event, next_event) != 0
    || state->current_status_text[0]
    || state->current_session_count != 0
    || state->current_session_index != 0;
  copy_text(state->current_state, sizeof(state->current_state), "idle");
  copy_text(state->current_status, sizeof(state->current_status), "idle");
  copy_text(state->current_event, sizeof(state->current_event), next_event);
  copy_text(state->current_speech, sizeof(state->current_speech), "休息中");
  state->current_status_text[0] = '\0';
  state->current_session_id[0] = '\0';
  state->current_session_title[0] = '\0';
  state->current_session_notice[0] = '\0';
  state->stats.session_title[0] = '\0';
  state->current_session_count = 0;
  state->current_session_index = 0;
  state->session_notice_until_ms = 0;
  state->session_voice_active = false;
  state->done_until_ms = 0;
  state->local_lifecycle[0] = '\0';
  state->local_lifecycle_until_ms = 0;
  state->touch_feedback_until_ms = 0;
  if (changed) state->last_update_ms += 1;
}

static void mark_current_session_terminal(
  pet_p4_runtime_state_t *state,
  const char *session_id,
  const char *lifecycle,
  unsigned long long now_ms
) {
  if (!state || !lifecycle_is_terminal(lifecycle)) return;
  pet_p4_session_queue_item_t *item = find_session_queue_item(
    state->session_queue,
    state->session_queue_count,
    session_id
  );
  if (!item && (!session_id || !session_id[0]) && state->session_queue_count > 0) {
    unsigned int selected = state->current_session_index > 0
      ? state->current_session_index - 1
      : 0;
    if (selected >= state->session_queue_count) selected = 0;
    item = &state->session_queue[selected];
  }
  if (!item) return;
  bool was_terminal = lifecycle_is_terminal(item->state);
  copy_text(item->state, sizeof(item->state), lifecycle);
  if (!was_terminal || item->terminal_until_ms == 0) {
    item->terminal_until_ms = now_ms + PET_P4_DONE_HOLD_MS;
  }
}

static void note_remote_lifecycle(pet_p4_runtime_state_t *state, bool was_done) {
  const char *lifecycle = runtime_lifecycle(state);
  bool is_done = lifecycle_is(lifecycle, "done");
  if (!is_done || !was_done) state->done_until_ms = 0;
  if (lifecycle_is(lifecycle, "waiting_user") || lifecycle_is(lifecycle, "error") || is_done) {
    state->local_lifecycle[0] = '\0';
    state->local_lifecycle_until_ms = 0;
    state->touch_feedback_until_ms = 0;
  }
}

static void close_asset_chunk_file(void) {
  if (g_asset_chunk_file) {
    fclose(g_asset_chunk_file);
    g_asset_chunk_file = NULL;
  }
}

static void reset_asset_chunk_tracking(void) {
  close_asset_chunk_file();
  g_asset_chunk_logical_path[0] = '\0';
  g_asset_chunk_size = 0;
  g_asset_chunk_checksum = 0xcbf29ce484222325ULL;
  g_asset_chunk_stat_valid = false;
}

static void reset_raw_asset_chunk(void) {
  free(g_raw_asset_chunk.data);
  memset(&g_raw_asset_chunk, 0, sizeof(g_raw_asset_chunk));
}

static const char *json_string(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

static const char *json_first_string(const cJSON *object, const char *const *keys, size_t count) {
  for (size_t i = 0; i < count; i += 1) {
    const char *value = json_string(object, keys[i]);
    if (value && value[0]) return value;
  }
  return "";
}

static unsigned long long json_u64(const cJSON *object, const char *key, unsigned long long fallback) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item)) return fallback;
  return (unsigned long long) item->valuedouble;
}

static unsigned long long json_bounded_u64(
  const cJSON *object,
  const char *key,
  unsigned long long maximum,
  unsigned long long fallback
) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item)
      || item->valuedouble < 0
      || item->valuedouble > (double) maximum) {
    return fallback;
  }
  return (unsigned long long) item->valuedouble;
}

static void send_topic_payload(pet_p4_send_line_fn send_line, void *ctx, const char *topic, cJSON *payload) {
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

static void send_protocol_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *request_topic,
  const cJSON *request_payload,
  bool ok,
  const char *code,
  const char *error
) {
  cJSON *payload = cJSON_CreateObject();
  const cJSON *request_id = request_payload
    ? cJSON_GetObjectItemCaseSensitive(request_payload, "requestId")
    : NULL;
  cJSON_AddStringToObject(payload, "requestTopic", request_topic ? request_topic : "");
  cJSON_AddBoolToObject(payload, "ok", ok);
  if (request_id) {
    cJSON *request_id_copy = cJSON_Duplicate(request_id, true);
    if (request_id_copy) cJSON_AddItemToObject(payload, "requestId", request_id_copy);
  }
  if (code && code[0]) cJSON_AddStringToObject(payload, "code", code);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  send_topic_payload(send_line, ctx, "protocol/ack", payload);
}

static void send_protocol_ack_if_requested(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *request_topic,
  const cJSON *request_payload
) {
  if (request_payload && cJSON_HasObjectItem(request_payload, "requestId")) {
    send_protocol_ack(send_line, ctx, request_topic, request_payload, true, "ok", NULL);
  }
}

static bool topic_is_known_but_unsupported(const char *topic) {
  if (!topic) return false;
  return strcmp(topic, "button_config") == 0
    || strcmp(topic, "control/apply-wifi") == 0
    || strncmp(topic, "input/", 6) == 0
    || strncmp(topic, "voice/", 6) == 0
    || strncmp(topic, "audio/", 6) == 0
    || strncmp(topic, "widget/", 7) == 0
    || strncmp(topic, "firmware/", 9) == 0;
}

static void send_asset_ack_indexed(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  const char *path,
  const char *index,
  bool ok,
  const char *error
) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  if (path && path[0]) cJSON_AddStringToObject(payload, "path", path);
  if (index && index[0]) cJSON_AddStringToObject(payload, "index", index);
  cJSON_AddBoolToObject(payload, "ok", ok);
  if (error && error[0]) cJSON_AddStringToObject(payload, "error", error);
  send_topic_payload(send_line, ctx, "asset/ack", payload);
}

static void send_asset_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  const char *path,
  bool ok,
  const char *error
) {
  send_asset_ack_indexed(
    send_line,
    ctx,
    transfer_id,
    phase,
    path,
    NULL,
    ok,
    error
  );
}

static void send_asset_stat_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *path,
  unsigned long long size,
  unsigned long long checksum
) {
  char checksum_hex[17];
  cJSON *payload = cJSON_CreateObject();
  snprintf(checksum_hex, sizeof(checksum_hex), "%016llx", checksum);
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", "stat");
  cJSON_AddStringToObject(payload, "path", path ? path : "");
  cJSON_AddBoolToObject(payload, "ok", true);
  cJSON_AddNumberToObject(payload, "size", (double) size);
  cJSON_AddStringToObject(payload, "checksum", checksum_hex);
  send_topic_payload(send_line, ctx, "asset/ack", payload);
}

static bool path_is_p4_asset(const char *path) {
  if (!path || strncmp(path, "p4/", 3) != 0) return false;
  if (path[0] == '/' || strchr(path, '\\')) return false;
  return strstr(path, "..") == NULL;
}

static bool slot_is_valid(int slot) {
  return slot >= 0 && slot < PET_P4_ASSET_SLOT_COUNT;
}

static void read_active_slot_marker(void) {
  FILE *file = fopen(PET_P4_ASSET_SLOT_MARKER, "rb");
  int ch;
  if (!file) return;
  ch = fgetc(file);
  fclose(file);
  if (ch == '0' || ch == '1') {
    g_asset_active_slot = ch - '0';
  }
}

bool pet_p4_asset_set_active_slot(int slot) {
  FILE *file;
  if (!slot_is_valid(slot)) return false;
  file = fopen(PET_P4_ASSET_SLOT_MARKER, "wb");
  if (!file) return false;
  fputc('0' + slot, file);
  fputc('\n', file);
  fclose(file);
  g_asset_active_slot = slot;
  return true;
}

int pet_p4_asset_active_slot(void) {
  if (g_asset_active_slot < 0) read_active_slot_marker();
  return g_asset_active_slot;
}

int pet_p4_asset_inactive_slot(void) {
  return PET_P4_RAW_APPEARANCE_SLOT;
}

static bool clear_slot_files(int slot) {
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "s%d_", slot);
  while (true) {
    DIR *dir = opendir(PET_P4_SPIFFS_PREFIX);
    struct dirent *entry;
    char fs_path[PET_P4_FS_PATH_MAX];
    bool found = false;
    if (!dir) return false;
    while ((entry = readdir(dir)) != NULL) {
      if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
      snprintf(fs_path, sizeof(fs_path), PET_P4_SPIFFS_PREFIX "/%s", entry->d_name);
      found = true;
      break;
    }
    closedir(dir);
    if (!found) {
      if (slot == PET_P4_RAW_APPEARANCE_SLOT && pet_p4_raw_assets_available()) {
        char error[96];
        if (!pet_p4_raw_assets_invalidate(error, sizeof(error))) {
          ESP_LOGE(TAG, "failed to invalidate raw appearance slot: %s", error);
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

static bool ensure_asset_transfer_slot(const char *transfer_id) {
  const char *id = transfer_id ? transfer_id : "";
  if (g_asset_transfer_slot < 0 || strcmp(g_asset_transfer_id, id) != 0) {
    reset_asset_chunk_tracking();
    pet_p4_asset_clean_legacy_files();
    g_asset_transfer_slot = PET_P4_RAW_APPEARANCE_SLOT;
    if (!slot_is_valid(g_asset_transfer_slot)) return false;
    if (!clear_slot_files(g_asset_transfer_slot)) return false;
    copy_text(g_asset_transfer_id, sizeof(g_asset_transfer_id), id);
  }
  return true;
}

static void reset_asset_transfer_slot(void) {
  pet_p4_raw_assets_reset_transfer();
  g_asset_transfer_slot = -1;
  g_asset_transfer_id[0] = '\0';
}

static void note_asset_transfer_activity(void) {
  g_asset_transfer_last_activity_ms =
    (unsigned long long) (esp_timer_get_time() / 1000ULL);
}

static void abort_serial_asset_transfer(pet_p4_runtime_state_t *state) {
  reset_raw_asset_chunk();
  reset_asset_chunk_tracking();
  if (slot_is_valid(g_asset_transfer_slot)) {
    (void) clear_slot_files(g_asset_transfer_slot);
  }
  reset_asset_transfer_slot();
  g_asset_transfer_last_activity_ms = 0;
  if (state) state->asset_transfer_active = false;
}

void pet_p4_asset_transfer_process(
  pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state || !state->asset_transfer_active || g_asset_transfer_slot < 0
      || g_asset_transfer_last_activity_ms == 0
      || now_ms < g_asset_transfer_last_activity_ms
      || now_ms - g_asset_transfer_last_activity_ms < PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS) {
    return;
  }
  ESP_LOGW(
    TAG,
    "aborting idle serial appearance transfer id=%s idle_ms=%llu",
    g_asset_transfer_id,
    now_ms - g_asset_transfer_last_activity_ms
  );
  abort_serial_asset_transfer(state);
}

static void p4_asset_fs_path_for_slot(int slot, const char *logical_path, bool tmp, char *out, size_t out_size) {
  unsigned long long hash = fnv1a64_bytes((const unsigned char *) logical_path, strlen(logical_path));
  if (slot_is_valid(slot)) {
    snprintf(out, out_size, PET_P4_SPIFFS_PREFIX "/s%d_%016llx%s", slot, hash, tmp ? ".tmp" : "");
  } else {
    snprintf(out, out_size, PET_P4_SPIFFS_PREFIX "/a%016llx%s", hash, tmp ? ".tmp" : "");
  }
}

static void p4_asset_fs_path(const char *logical_path, bool tmp, char *out, size_t out_size) {
  p4_asset_fs_path_for_slot(pet_p4_asset_active_slot(), logical_path, tmp, out, out_size);
}

bool pet_p4_asset_fs_path_for_slot(int slot, const char *logical_path, bool tmp, char *out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (!path_is_p4_asset(logical_path) || (!slot_is_valid(slot) && slot != -1)) return false;
  p4_asset_fs_path_for_slot(slot, logical_path, tmp, out, out_size);
  return true;
}

bool pet_p4_asset_fs_path(const char *logical_path, char *out, size_t out_size) {
  if (!out || out_size == 0) return false;
  out[0] = '\0';
  if (!path_is_p4_asset(logical_path)) return false;
  p4_asset_fs_path(logical_path, false, out, out_size);
  return true;
}

bool pet_p4_asset_read_all(
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
) {
  char fs_path[PET_P4_FS_PATH_MAX];
  FILE *file;
  size_t read;
  int active_slot;
  if (!path_is_p4_asset(logical_path) || !out || expected_size == 0) return false;
  active_slot = pet_p4_asset_active_slot();
  if (pet_p4_raw_assets_supports_path(logical_path)
      && pet_p4_raw_assets_slot_available(active_slot)
      && pet_p4_raw_assets_read_all_from_slot(
        active_slot,
        logical_path,
        out,
        expected_size
      )) {
    return true;
  }
  if (!pet_p4_asset_fs_path(logical_path, fs_path, sizeof(fs_path))) return false;
  file = fopen(fs_path, "rb");
  if (!file) return false;
  read = fread(out, 1, expected_size, file);
  fclose(file);
  return read == expected_size;
}

static bool p4_asset_manifest_path_for_candidate(int slot, char *out, size_t out_size) {
  return pet_p4_asset_fs_path_for_slot(slot, "p4/manifest.json", false, out, out_size);
}

static bool p4_asset_ready_marker_path(int slot, char *out, size_t out_size) {
  if (!slot_is_valid(slot) || !out || out_size == 0) return false;
  snprintf(out, out_size, PET_P4_SPIFFS_PREFIX "/s%d_ready", slot);
  return true;
}

static bool read_slot_pack_id(int slot, char *out, size_t out_size) {
  static const char needle[] = "\"packId\"";
  char manifest_path[PET_P4_FS_PATH_MAX];
  size_t matched = 0;
  size_t written = 0;
  int ch;
  FILE *file;
  if (!out || out_size < 2 ||
      !p4_asset_manifest_path_for_candidate(slot, manifest_path, sizeof(manifest_path))) {
    return false;
  }
  out[0] = '\0';
  file = fopen(manifest_path, "rb");
  if (!file) return false;
  while ((ch = fgetc(file)) != EOF) {
    if (ch == needle[matched]) {
      matched += 1;
      if (matched == sizeof(needle) - 1) break;
    } else {
      matched = ch == needle[0] ? 1 : 0;
    }
  }
  if (matched != sizeof(needle) - 1) {
    fclose(file);
    return false;
  }
  do {
    ch = fgetc(file);
  } while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
  if (ch != ':') {
    fclose(file);
    return false;
  }
  do {
    ch = fgetc(file);
  } while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
  if (ch != '"') {
    fclose(file);
    return false;
  }
  while ((ch = fgetc(file)) != EOF && ch != '"') {
    if (ch == '\\' || written + 1 >= out_size) {
      fclose(file);
      out[0] = '\0';
      return false;
    }
    out[written++] = (char) ch;
  }
  fclose(file);
  if (ch != '"' || written == 0) {
    out[0] = '\0';
    return false;
  }
  out[written] = '\0';
  return true;
}

static bool read_ready_slot_pack_id(int slot, char *out, size_t out_size) {
  char manifest_pack_id[PET_P4_PACK_ID_MAX];
  char marker_pack_id[PET_P4_PACK_ID_MAX];
  char marker_path[PET_P4_FS_PATH_MAX];
  FILE *file;
  size_t marker_len;
  if (!out || out_size < 2 ||
      !read_slot_pack_id(slot, manifest_pack_id, sizeof(manifest_pack_id)) ||
      !p4_asset_ready_marker_path(slot, marker_path, sizeof(marker_path))) {
    return false;
  }
  out[0] = '\0';
  file = fopen(marker_path, "rb");
  if (!file) return false;
  if (!fgets(marker_pack_id, sizeof(marker_pack_id), file)) {
    fclose(file);
    return false;
  }
  fclose(file);
  marker_len = strcspn(marker_pack_id, "\r\n");
  marker_pack_id[marker_len] = '\0';
  if (marker_len == 0 || strcmp(marker_pack_id, manifest_pack_id) != 0) return false;
  if (pet_p4_raw_assets_slot_available(slot)) {
    char raw_pack_id[PET_P4_PACK_ID_MAX];
    if (!pet_p4_raw_assets_committed_pack_id_for_slot(
          slot,
          raw_pack_id,
          sizeof(raw_pack_id)
        )
        || strcmp(raw_pack_id, manifest_pack_id) != 0) {
      return false;
    }
  }
  copy_text(out, out_size, manifest_pack_id);
  return true;
}

bool pet_p4_asset_mark_slot_ready(int slot) {
  char pack_id[PET_P4_PACK_ID_MAX];
  char marker_path[PET_P4_FS_PATH_MAX];
  FILE *file;
  bool ok;
  if (!read_slot_pack_id(slot, pack_id, sizeof(pack_id)) ||
      !p4_asset_ready_marker_path(slot, marker_path, sizeof(marker_path))) {
    return false;
  }
  file = fopen(marker_path, "wb");
  if (!file) return false;
  ok = fprintf(file, "%s\n", pack_id) > 0;
  if (fclose(file) != 0) ok = false;
  if (!ok) remove(marker_path);
  return ok;
}

bool pet_p4_asset_slot_has_pack_id(int slot) {
  char pack_id[PET_P4_PACK_ID_MAX];
  return read_slot_pack_id(slot, pack_id, sizeof(pack_id));
}

static void send_asset_slot_query_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id
) {
  cJSON *payload = cJSON_CreateObject();
  cJSON *slots = cJSON_CreateArray();
  int active_slot = pet_p4_asset_active_slot();
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", "slot-query");
  cJSON_AddBoolToObject(payload, "ok", true);
  if (slot_is_valid(active_slot)) cJSON_AddNumberToObject(payload, "activeSlot", active_slot);
  for (int slot = 0; slot < PET_P4_ASSET_SLOT_COUNT; slot += 1) {
    char pack_id[PET_P4_PACK_ID_MAX];
    bool valid = read_ready_slot_pack_id(slot, pack_id, sizeof(pack_id));
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "slot", slot);
    cJSON_AddBoolToObject(entry, "valid", valid);
    if (valid) cJSON_AddStringToObject(entry, "packId", pack_id);
    cJSON_AddItemToArray(slots, entry);
  }
  cJSON_AddItemToObject(payload, "slots", slots);
  send_topic_payload(send_line, ctx, "asset/ack", payload);
}

void pet_p4_asset_clean_legacy_files(void) {
  DIR *dir = opendir(PET_P4_SPIFFS_PREFIX);
  struct dirent *entry;
  if (!dir) return;
  while ((entry = readdir(dir)) != NULL) {
    char fs_path[PET_P4_FS_PATH_MAX];
    const char *name = entry->d_name;
    if (name[0] != 'a') continue;
    snprintf(fs_path, sizeof(fs_path), PET_P4_SPIFFS_PREFIX "/%s", name);
    remove(fs_path);
  }
  closedir(dir);
}

static bool read_file_stat(const char *fs_path, unsigned long long *size, unsigned long long *checksum) {
  FILE *file = fopen(fs_path, "rb");
  if (!file) return false;
  unsigned long long local_size = 0;
  unsigned long long local_checksum = fnv1a64_file(file, &local_size);
  fclose(file);
  if (size) *size = local_size;
  if (checksum) *checksum = local_checksum;
  return true;
}

void pet_p4_load_asset_manifest(pet_p4_runtime_state_t *state) {
  char fs_path[PET_P4_FS_PATH_MAX];
  FILE *file;
  size_t bytes_read;
  if (!state) return;
  read_active_slot_marker();
  file = NULL;
  if (slot_is_valid(g_asset_active_slot)) {
    char ready_pack_id[PET_P4_PACK_ID_MAX];
    if (!read_ready_slot_pack_id(g_asset_active_slot, ready_pack_id, sizeof(ready_pack_id)) &&
        pet_p4_asset_slot_has_pack_id(g_asset_active_slot)) {
      ESP_LOGW(TAG, "migrating active pre-marker appearance slot=%d", g_asset_active_slot);
      (void) pet_p4_asset_mark_slot_ready(g_asset_active_slot);
    }
    if (read_ready_slot_pack_id(g_asset_active_slot, ready_pack_id, sizeof(ready_pack_id)) &&
        p4_asset_manifest_path_for_candidate(g_asset_active_slot, fs_path, sizeof(fs_path))) {
      file = fopen(fs_path, "rb");
    }
  }
  if (!file && p4_asset_manifest_path_for_candidate(-1, fs_path, sizeof(fs_path))) {
    file = fopen(fs_path, "rb");
    if (file) g_asset_active_slot = -1;
  }
  for (int slot = 0; !file && slot < PET_P4_ASSET_SLOT_COUNT; slot += 1) {
    char ready_pack_id[PET_P4_PACK_ID_MAX];
    if (read_ready_slot_pack_id(slot, ready_pack_id, sizeof(ready_pack_id)) &&
        p4_asset_manifest_path_for_candidate(slot, fs_path, sizeof(fs_path))) {
      file = fopen(fs_path, "rb");
      if (file) g_asset_active_slot = slot;
    }
  }
  if (!file) {
    state->asset_manifest_json[0] = '\0';
    state->asset_family_count = 0;
    pet_p4_asset_catalog_init(&state->asset_catalog);
    return;
  }
  bytes_read = fread(state->asset_manifest_json, 1, sizeof(state->asset_manifest_json) - 1, file);
  fclose(file);
  state->asset_manifest_json[bytes_read] = '\0';
  if (pet_p4_asset_catalog_parse(&state->asset_catalog, state->asset_manifest_json)) {
    state->asset_family_count = state->asset_catalog.count;
    state->asset_revision += 1;
  } else {
    state->asset_family_count = 0;
  }
  ESP_LOGI(TAG, "loaded P4 asset manifest bytes=%u families=%u", (unsigned int) bytes_read, state->asset_family_count);
  if (state->asset_family_count == 0) {
    ESP_LOGW(TAG, "clearing stale or unusable P4 manifest: no real appearance families");
    remove(fs_path);
    state->asset_manifest_json[0] = '\0';
    pet_p4_asset_catalog_init(&state->asset_catalog);
  }
}

static bool write_asset_chunk_bytes(
  const char *transfer_id,
  const char *path,
  const char *index,
  const unsigned char *data,
  size_t data_len
) {
  char fs_path[PET_P4_FS_PATH_MAX];
  FILE *file;
  size_t written = 0;
  bool starts_new_file;
  bool ok = false;
  if (!path_is_p4_asset(path) || !data || data_len == 0) {
    copy_text(g_asset_chunk_error, sizeof(g_asset_chunk_error), "invalid asset chunk");
    return false;
  }
  if (!ensure_asset_transfer_slot(transfer_id)) {
    copy_text(g_asset_chunk_error, sizeof(g_asset_chunk_error), "slot prepare failed");
    return false;
  }
  if (g_asset_transfer_slot == PET_P4_RAW_APPEARANCE_SLOT
      && pet_p4_raw_assets_available()
      && pet_p4_raw_assets_supports_path(path)) {
    return pet_p4_raw_assets_write_chunk(
      path,
      index,
      data,
      data_len,
      g_asset_chunk_error,
      sizeof(g_asset_chunk_error)
    );
  }
  if (!pet_p4_asset_fs_path_for_slot(g_asset_transfer_slot, path, false, fs_path, sizeof(fs_path))) {
    copy_text(g_asset_chunk_error, sizeof(g_asset_chunk_error), "unsupported p4 asset path");
    return false;
  }
  starts_new_file = strcmp(index, "0") == 0;
  if (starts_new_file || strcmp(g_asset_chunk_logical_path, path) != 0) {
    reset_asset_chunk_tracking();
  }
  if (!g_asset_chunk_file) {
    file = fopen(fs_path, starts_new_file ? "wb" : "ab");
    if (file) {
      g_asset_chunk_file = file;
      copy_text(g_asset_chunk_logical_path, sizeof(g_asset_chunk_logical_path), path);
      if (starts_new_file) {
        g_asset_chunk_size = 0;
        g_asset_chunk_checksum = 0xcbf29ce484222325ULL;
        g_asset_chunk_stat_valid = true;
      }
    }
  }
  file = g_asset_chunk_file;
  if (file) {
    errno = 0;
    written = fwrite(data, 1, data_len, file);
    if (written > 0 && g_asset_chunk_stat_valid) {
      g_asset_chunk_checksum = fnv1a64_update(g_asset_chunk_checksum, data, written);
      g_asset_chunk_size += (unsigned long long) written;
    }
    ok = written == data_len;
    if (!ok) {
      snprintf(
        g_asset_chunk_error,
        sizeof(g_asset_chunk_error),
        "chunk write failed: %s",
        errno ? strerror(errno) : "short write"
      );
    }
  } else {
    snprintf(
      g_asset_chunk_error,
      sizeof(g_asset_chunk_error),
      "open transferred file failed: %s",
      errno ? strerror(errno) : "unknown"
    );
  }
  if (!ok) reset_asset_chunk_tracking();
  return ok;
}

static bool handle_asset_chunk(const cJSON *payload) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *path = json_string(payload, "path");
  const char *data = json_string(payload, "data");
  const char *index = json_string(payload, "index");
  unsigned long long expected_decoded_len = json_u64(payload, "size", 0);
  size_t encoded_len;
  size_t decoded_len = 0;
  unsigned char *decoded;
  bool ok;
  g_asset_chunk_error[0] = '\0';
  if (!path_is_p4_asset(path) || !data[0]) {
    copy_text(g_asset_chunk_error, sizeof(g_asset_chunk_error), "invalid asset chunk");
    return false;
  }
  encoded_len = strlen(data);
  decoded = (unsigned char *) malloc(PET_P4_DECODED_CHUNK_LIMIT);
  if (!decoded) {
    copy_text(g_asset_chunk_error, sizeof(g_asset_chunk_error), "no memory");
    return false;
  }
  if (mbedtls_base64_decode(decoded, PET_P4_DECODED_CHUNK_LIMIT, &decoded_len,
                            (const unsigned char *) data, encoded_len) != 0) {
    copy_text(g_asset_chunk_error, sizeof(g_asset_chunk_error), "base64 decode failed");
    free(decoded);
    return false;
  }
  if (expected_decoded_len > 0 && decoded_len != expected_decoded_len) {
    snprintf(
      g_asset_chunk_error,
      sizeof(g_asset_chunk_error),
      "chunk size mismatch expected=%llu actual=%u",
      expected_decoded_len,
      (unsigned int) decoded_len
    );
    free(decoded);
    return false;
  }
  ok = write_asset_chunk_bytes(transfer_id, path, index, decoded, decoded_len);
  free(decoded);
  return ok;
}

static bool begin_raw_asset_chunk(
  pet_p4_runtime_state_t *state,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *path = json_string(payload, "path");
  const char *index = json_string(payload, "index");
  const char *checksum_text = json_string(payload, "checksum");
  unsigned long long expected_size = json_u64(payload, "size", 0);
  char *checksum_end = NULL;
  unsigned long long expected_checksum;

  reset_raw_asset_chunk();
  g_asset_chunk_error[0] = '\0';
  errno = 0;
  expected_checksum = strtoull(checksum_text, &checksum_end, 16);
  if (!path_is_p4_asset(path)
      || !transfer_id[0]
      || !index[0]
      || strlen(checksum_text) != 16
      || !checksum_end
      || *checksum_end != '\0'
      || errno != 0
      || expected_size == 0
      || expected_size > PET_P4_RAW_ASSET_CHUNK_LIMIT) {
    send_asset_ack_indexed(
      send_line,
      ctx,
      transfer_id,
      "raw-ready",
      path,
      index,
      false,
      "invalid raw asset chunk header"
    );
    return false;
  }
  if (!ensure_asset_transfer_slot(transfer_id)) {
    send_asset_ack_indexed(
      send_line,
      ctx,
      transfer_id,
      "raw-ready",
      path,
      index,
      false,
      "slot prepare failed"
    );
    return false;
  }

  g_raw_asset_chunk.data = (unsigned char *) malloc((size_t) expected_size);
  if (!g_raw_asset_chunk.data) {
    send_asset_ack_indexed(
      send_line,
      ctx,
      transfer_id,
      "raw-ready",
      path,
      index,
      false,
      "no memory"
    );
    reset_raw_asset_chunk();
    return false;
  }
  g_raw_asset_chunk.active = true;
  g_raw_asset_chunk.expected_size = (size_t) expected_size;
  g_raw_asset_chunk.expected_checksum = expected_checksum;
  copy_text(g_raw_asset_chunk.transfer_id, sizeof(g_raw_asset_chunk.transfer_id), transfer_id);
  copy_text(g_raw_asset_chunk.logical_path, sizeof(g_raw_asset_chunk.logical_path), path);
  copy_text(g_raw_asset_chunk.index, sizeof(g_raw_asset_chunk.index), index);
  if (state) state->asset_transfer_active = true;
  send_asset_ack_indexed(
    send_line,
    ctx,
    transfer_id,
    "raw-ready",
    path,
    index,
    true,
    NULL
  );
  return true;
}

bool pet_p4_raw_asset_chunk_active(void) {
  return g_raw_asset_chunk.active;
}

size_t pet_p4_consume_raw_asset_bytes(
  const unsigned char *data,
  size_t len,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  size_t remaining;
  size_t consumed;
  bool ok;
  unsigned long long checksum;
  if (!g_raw_asset_chunk.active || !data || len == 0) return 0;
  note_asset_transfer_activity();

  remaining = g_raw_asset_chunk.expected_size - g_raw_asset_chunk.received_size;
  consumed = len < remaining ? len : remaining;
  memcpy(
    g_raw_asset_chunk.data + g_raw_asset_chunk.received_size,
    data,
    consumed
  );
  g_raw_asset_chunk.received_size += consumed;
  if (g_raw_asset_chunk.received_size < g_raw_asset_chunk.expected_size) {
    return consumed;
  }

  checksum = fnv1a64_bytes(g_raw_asset_chunk.data, g_raw_asset_chunk.expected_size);
  if (checksum != g_raw_asset_chunk.expected_checksum) {
    snprintf(
      g_asset_chunk_error,
      sizeof(g_asset_chunk_error),
      "raw chunk checksum mismatch expected=%016llx actual=%016llx",
      g_raw_asset_chunk.expected_checksum,
      checksum
    );
    ok = false;
  } else {
    ok = write_asset_chunk_bytes(
      g_raw_asset_chunk.transfer_id,
      g_raw_asset_chunk.logical_path,
      g_raw_asset_chunk.index,
      g_raw_asset_chunk.data,
      g_raw_asset_chunk.expected_size
    );
  }
  send_asset_ack_indexed(
    send_line,
    ctx,
    g_raw_asset_chunk.transfer_id,
    "raw-chunk",
    g_raw_asset_chunk.logical_path,
    g_raw_asset_chunk.index,
    ok,
    ok ? NULL : g_asset_chunk_error
  );
  reset_raw_asset_chunk();
  return consumed;
}

static void handle_asset_stat(
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *path = json_string(payload, "path");
  char fs_path[PET_P4_FS_PATH_MAX];
  unsigned long long size = 0;
  unsigned long long checksum = 0;
  if (!path_is_p4_asset(path)) {
    send_asset_ack(send_line, ctx, transfer_id, "stat", path, false,
                   "linux mp4/wav assets are not supported on esp-p4");
    return;
  }
  p4_asset_fs_path(path, false, fs_path, sizeof(fs_path));
  if (!read_file_stat(fs_path, &size, &checksum)) {
    send_asset_ack(send_line, ctx, transfer_id, "stat", path, true, NULL);
    return;
  }
  send_asset_stat_ack(send_line, ctx, transfer_id, path, size, checksum);
}

static void handle_asset_activate(
  pet_p4_runtime_state_t *state,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *expected_pack_id = json_string(payload, "packId");
  cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(payload, "slot");
  int slot = cJSON_IsNumber(slot_item) ? slot_item->valueint : -1;
  char actual_pack_id[PET_P4_PACK_ID_MAX];
  if (pet_p4_diagnostics_reboot_pending() || pet_p4_ota_transfer_active() ||
      state->asset_transfer_active) {
    send_asset_ack(send_line, ctx, transfer_id, "activate", NULL, false,
                   "device transfer, firmware update, or reboot is active");
    return;
  }
  if (!slot_is_valid(slot) || !expected_pack_id[0]) {
    send_asset_ack(send_line, ctx, transfer_id, "activate", NULL, false,
                   "invalid appearance slot or pack id");
    return;
  }
  if (!read_ready_slot_pack_id(slot, actual_pack_id, sizeof(actual_pack_id)) ||
      strcmp(actual_pack_id, expected_pack_id) != 0) {
    send_asset_ack(send_line, ctx, transfer_id, "activate", NULL, false,
                   "appearance pack is not present in requested slot");
    return;
  }
  if (!pet_p4_asset_set_active_slot(slot)) {
    send_asset_ack(send_line, ctx, transfer_id, "activate", NULL, false,
                   "appearance slot activation failed");
    return;
  }
  pet_p4_asset_clean_legacy_files();
  pet_p4_load_asset_manifest(state);
  send_asset_ack(send_line, ctx, transfer_id, "activate", NULL, true, NULL);
}

static void handle_asset_file(
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *path = json_string(payload, "path");
  const char *expected_checksum = json_string(payload, "checksum");
  unsigned long long expected_size = json_u64(payload, "size", 0);
  char fs_path[PET_P4_FS_PATH_MAX];
  char checksum_hex[17];
  unsigned long long size = 0;
  unsigned long long checksum = 0;
  bool tracked_stat;
  if (!path_is_p4_asset(path)) {
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false,
                   "linux mp4/wav assets are not supported on esp-p4");
    return;
  }
  if (!ensure_asset_transfer_slot(transfer_id)) {
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false, "slot prepare failed");
    return;
  }
  if (g_asset_transfer_slot == PET_P4_RAW_APPEARANCE_SLOT
      && pet_p4_raw_assets_available()
      && pet_p4_raw_assets_supports_path(path)) {
    bool ok = pet_p4_raw_assets_finish_file(
      path,
      expected_size,
      expected_checksum,
      g_asset_chunk_error,
      sizeof(g_asset_chunk_error)
    );
    send_asset_ack(
      send_line,
      ctx,
      transfer_id,
      "file",
      path,
      ok,
      ok ? NULL : g_asset_chunk_error
    );
    return;
  }
  tracked_stat = g_asset_chunk_stat_valid &&
                 strcmp(g_asset_chunk_logical_path, path) == 0;
  close_asset_chunk_file();
  if (!pet_p4_asset_fs_path_for_slot(g_asset_transfer_slot, path, false, fs_path, sizeof(fs_path))) {
    reset_asset_chunk_tracking();
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false, "unsupported p4 asset path");
    return;
  }
  if (tracked_stat) {
    size = g_asset_chunk_size;
    checksum = g_asset_chunk_checksum;
  } else if (!read_file_stat(fs_path, &size, &checksum)) {
    reset_asset_chunk_tracking();
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false, "missing transferred file");
    return;
  }
  snprintf(checksum_hex, sizeof(checksum_hex), "%016llx", checksum);
  if (expected_size != size || (expected_checksum[0] && strcmp(expected_checksum, checksum_hex) != 0)) {
    char error[128];
    snprintf(
      error,
      sizeof(error),
      "checksum mismatch expected_size=%llu actual_size=%llu expected=%s actual=%s",
      expected_size,
      size,
      expected_checksum,
      checksum_hex
    );
    remove(fs_path);
    reset_asset_chunk_tracking();
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false, error);
    return;
  }
  reset_asset_chunk_tracking();
  send_asset_ack(send_line, ctx, transfer_id, "file", path, true, NULL);
}

static void handle_asset_topic(
  pet_p4_runtime_state_t *state,
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *transfer_id = json_string(payload, "transferId");
  const char *path = json_string(payload, "path");
  note_asset_transfer_activity();
  if (strcmp(topic, "asset/slot-query") == 0) {
    send_asset_slot_query_ack(send_line, ctx, transfer_id);
  } else if (strcmp(topic, "asset/activate") == 0) {
    handle_asset_activate(state, payload, send_line, ctx);
  } else if (strcmp(topic, "asset/begin") == 0) {
    unsigned long long total_bytes = json_u64(payload, "totalBytes", 0);
    unsigned long long raw_bytes = json_u64(payload, "rawBytes", total_bytes);
    esp_err_t gc_err;
    if (pet_p4_diagnostics_reboot_pending() || pet_p4_ota_transfer_active()) {
      send_asset_ack(send_line, ctx, transfer_id, "begin", NULL, false,
                     "device firmware update or reboot is active");
      return;
    }
    state->asset_transfer_active = true;
    reset_raw_asset_chunk();
    reset_asset_chunk_tracking();
    reset_asset_transfer_slot();
    if (!ensure_asset_transfer_slot(transfer_id)) {
      state->asset_transfer_active = false;
      g_asset_transfer_last_activity_ms = 0;
      send_asset_ack(send_line, ctx, transfer_id, "begin", NULL, false, "slot prepare failed");
      return;
    }
    if (g_asset_transfer_slot == PET_P4_RAW_APPEARANCE_SLOT
        && pet_p4_raw_assets_available()) {
      char error[128];
      if (!pet_p4_raw_assets_prepare(raw_bytes, error, sizeof(error))) {
        (void) clear_slot_files(g_asset_transfer_slot);
        reset_asset_transfer_slot();
        state->asset_transfer_active = false;
        g_asset_transfer_last_activity_ms = 0;
        send_asset_ack(send_line, ctx, transfer_id, "begin", NULL, false, error);
        return;
      }
    }
    gc_err = run_spiffs_gc_yielding();
    if (gc_err != ESP_OK) {
      char error[128];
      snprintf(error, sizeof(error), "SPIFFS pre-GC failed: %s", esp_err_to_name(gc_err));
      (void) clear_slot_files(g_asset_transfer_slot);
      reset_asset_transfer_slot();
      state->asset_transfer_active = false;
      g_asset_transfer_last_activity_ms = 0;
      send_asset_ack(send_line, ctx, transfer_id, "begin", NULL, false, error);
      return;
    }
    ESP_LOGI(TAG, "prepared asset slot=%d total_bytes=%llu raw_bytes=%llu gc_bytes=%u",
             g_asset_transfer_slot, total_bytes, raw_bytes,
             (unsigned int) PET_P4_ASSET_BEGIN_GC_BYTES);
    send_asset_ack(send_line, ctx, transfer_id, "begin", NULL, true, NULL);
  } else if (strcmp(topic, "asset/abort") == 0) {
    abort_serial_asset_transfer(state);
    send_asset_ack(send_line, ctx, transfer_id, "abort", NULL, true, NULL);
  } else if (strcmp(topic, "asset/chunk") == 0) {
    state->asset_transfer_active = true;
    bool ok = handle_asset_chunk(payload);
    send_asset_ack_indexed(
      send_line,
      ctx,
      transfer_id,
      "chunk",
      path,
      json_string(payload, "index"),
      ok,
      ok ? NULL : g_asset_chunk_error
    );
  } else if (strcmp(topic, "asset/raw-chunk") == 0) {
    (void) begin_raw_asset_chunk(state, payload, send_line, ctx);
  } else if (strcmp(topic, "asset/file") == 0) {
    state->asset_transfer_active = true;
    handle_asset_file(payload, send_line, ctx);
  } else if (strcmp(topic, "asset/stat") == 0) {
    handle_asset_stat(payload, send_line, ctx);
  } else if (strcmp(topic, "asset/commit") == 0) {
    char commit_error[128];
    bool commit_ok = g_asset_transfer_slot >= 0;
    reset_raw_asset_chunk();
    reset_asset_chunk_tracking();
    copy_text(commit_error, sizeof(commit_error), "slot commit failed");
    if (commit_ok && g_asset_transfer_slot == PET_P4_RAW_APPEARANCE_SLOT
        && pet_p4_raw_assets_available()) {
      char pack_id[PET_P4_PACK_ID_MAX];
      commit_ok = read_slot_pack_id(g_asset_transfer_slot, pack_id, sizeof(pack_id))
               && pet_p4_raw_assets_commit(pack_id, commit_error, sizeof(commit_error));
    }
    if (commit_ok) {
      commit_ok = pet_p4_asset_mark_slot_ready(g_asset_transfer_slot)
               && pet_p4_asset_set_active_slot(g_asset_transfer_slot);
    }
    if (!commit_ok) {
      reset_asset_transfer_slot();
      state->asset_transfer_active = false;
      g_asset_transfer_last_activity_ms = 0;
      send_asset_ack(send_line, ctx, transfer_id, "commit", NULL, false, commit_error);
      return;
    }
    pet_p4_asset_clean_legacy_files();
    pet_p4_load_asset_manifest(state);
    reset_asset_transfer_slot();
    state->asset_transfer_active = false;
    g_asset_transfer_last_activity_ms = 0;
    send_asset_ack(send_line, ctx, transfer_id, "commit", NULL, true, NULL);
  } else if (strcmp(topic, "asset/patch-commit") == 0) {
    reset_raw_asset_chunk();
    reset_asset_chunk_tracking();
    reset_asset_transfer_slot();
    state->asset_transfer_active = false;
    g_asset_transfer_last_activity_ms = 0;
    send_asset_ack(send_line, ctx, transfer_id, "patch", NULL, true, NULL);
  } else if (path_is_p4_asset(path)) {
    send_asset_ack(send_line, ctx, transfer_id, "unknown", path, false, "unknown p4 asset phase");
  } else {
    send_asset_ack(send_line, ctx, transfer_id, "unknown", path, false,
                   "linux mp4/wav assets are not supported on esp-p4");
  }
}

void pet_p4_state_init(pet_p4_runtime_state_t *state, const char *board_device_id) {
  if (!state) return;
  memset(state, 0, sizeof(*state));
  copy_text(state->board_device_id, sizeof(state->board_device_id),
            board_device_id && board_device_id[0] ? board_device_id : "p4-unknown");
  copy_text(state->current_state, sizeof(state->current_state), "idle");
  copy_text(state->current_event, sizeof(state->current_event), "boot");
  copy_text(state->current_title, sizeof(state->current_title), "pet");
  copy_text(state->current_speech, sizeof(state->current_speech), "休息中");
  copy_text(state->current_status, sizeof(state->current_status), "idle");
  copy_text(state->screen_page, sizeof(state->screen_page), "main");
  pet_p4_stats_init(&state->stats);
  pet_p4_asset_catalog_init(&state->asset_catalog);
}

void pet_p4_state_process(pet_p4_runtime_state_t *state, unsigned long long now_ms) {
  if (!state) return;
  unsigned int retained = 0;
  bool queue_changed = false;
  bool snapshot_stale = state->session_snapshot_last_seen_ms > 0
    && now_ms >= state->session_snapshot_last_seen_ms
    && now_ms - state->session_snapshot_last_seen_ms > PET_P4_SESSION_SNAPSHOT_TIMEOUT_MS;
  for (unsigned int i = 0; i < state->session_queue_count; i += 1) {
    pet_p4_session_queue_item_t *item = &state->session_queue[i];
    if (item->terminal_until_ms > 0 && now_ms >= item->terminal_until_ms) {
      queue_changed = true;
      continue;
    }
    if (snapshot_stale && lifecycle_is_active(item->state)) {
      queue_changed = true;
      continue;
    }
    if (retained != i) state->session_queue[retained] = *item;
    retained += 1;
  }
  if (queue_changed) {
    memset(
      &state->session_queue[retained],
      0,
      sizeof(state->session_queue[0]) * (PET_P4_SESSION_QUEUE_MAX - retained)
    );
    state->session_queue_count = retained;
    if (state->current_session_index > retained) {
      state->current_session_index = retained;
    }
    state->last_update_ms += 1;
    if (retained == 0) {
      restore_idle_session_view(
        state,
        snapshot_stale ? "session_snapshot_timeout" : "session_terminal_timeout"
      );
    }
  }
  const char *lifecycle = runtime_lifecycle(state);
  if (lifecycle_is(lifecycle, "done")) {
    if (state->done_until_ms == 0) {
      state->done_until_ms = now_ms + PET_P4_DONE_HOLD_MS;
    } else if (now_ms >= state->done_until_ms) {
      copy_text(state->current_state, sizeof(state->current_state), "idle");
      copy_text(state->current_status, sizeof(state->current_status), "idle");
      copy_text(state->current_event, sizeof(state->current_event), "done_timeout");
      state->current_status_text[0] = '\0';
      state->done_until_ms = 0;
      state->last_update_ms += 1;
    }
  } else {
    state->done_until_ms = 0;
  }
  if (state->local_lifecycle_until_ms > 0 && now_ms >= state->local_lifecycle_until_ms) {
    state->local_lifecycle[0] = '\0';
    state->local_lifecycle_until_ms = 0;
    state->touch_feedback_until_ms = 0;
    state->last_update_ms += 1;
  }
}

bool pet_p4_state_request_touch(
  pet_p4_runtime_state_t *state,
  const char *family,
  int x,
  int y,
  unsigned long long duration_ms,
  unsigned long long now_ms
) {
  if (!state || !family || strncmp(family, "touch.", 6) != 0 || duration_ms == 0) return false;
  const char *lifecycle = runtime_lifecycle(state);
  if (!lifecycle_is(lifecycle, "idle")) return false;
  copy_text(state->local_lifecycle, sizeof(state->local_lifecycle), family);
  state->local_lifecycle_started_ms = now_ms;
  state->local_lifecycle_until_ms = now_ms + duration_ms;
  state->touch_feedback_until_ms = now_ms + 650ULL;
  state->local_touch_x = x;
  state->local_touch_y = y;
  state->last_update_ms += 1;
  return true;
}

const char *pet_p4_state_effective_lifecycle(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  const char *lifecycle = runtime_lifecycle(state);
  if (state && state->local_lifecycle[0]
      && state->local_lifecycle_until_ms > now_ms
      && lifecycle_is(lifecycle, "idle")) {
    return state->local_lifecycle;
  }
  return lifecycle;
}

bool pet_p4_state_touch_feedback_active(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  return state && state->touch_feedback_until_ms > now_ms;
}

void pet_p4_state_note_host_activity(
  pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state) return;
  state->host_last_seen_ms = now_ms;
}

void pet_p4_state_set_native_usb_mounted(
  pet_p4_runtime_state_t *state,
  bool mounted,
  unsigned long long now_ms
) {
  if (!state) return;
  state->native_usb_mounted = mounted;
  if (mounted) state->host_last_seen_ms = now_ms;
  state->last_update_ms += 1;
}

bool pet_p4_state_host_connected(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state || !state->desktop_device_id[0] || state->host_last_seen_ms == 0) return false;
  if (now_ms < state->host_last_seen_ms) return true;
  return now_ms - state->host_last_seen_ms <= PET_P4_HOST_HEARTBEAT_TIMEOUT_MS;
}

static void send_widget_ack(
  pet_p4_send_line_fn send_line,
  void *ctx,
  const char *transfer_id,
  const char *phase,
  const cJSON *request,
  bool ok,
  const char *message
) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  const char *path = json_string(request, "path");
  const cJSON *index = cJSON_GetObjectItemCaseSensitive(request, "index");
  if (path[0]) cJSON_AddStringToObject(payload, "path", path);
  if (index) cJSON_AddItemToObject(payload, "index", cJSON_Duplicate(index, true));
  cJSON_AddBoolToObject(payload, "ok", ok);
  if (message && message[0]) cJSON_AddStringToObject(payload, "msg", message);
  send_topic_payload(send_line, ctx, "widget-install-ack", payload);
}

static bool send_widget_inventory(
  const pet_p4_runtime_state_t *state,
  const cJSON *request,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *request_id = json_string(request, "requestId");
  char active_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
  bool active = pet_p4_miniapp_active_id(active_widget_id, sizeof(active_widget_id));
  size_t installed_count = pet_p4_miniapp_catalog_count();
  bool active_in_catalog = false;
  cJSON *payload = cJSON_CreateObject();
  cJSON *items = cJSON_CreateArray();
  cJSON *warnings = cJSON_CreateArray();
  if (!payload || !items || !warnings) {
    cJSON_Delete(payload);
    cJSON_Delete(items);
    cJSON_Delete(warnings);
    return false;
  }
  cJSON_AddNumberToObject(payload, "schemaVersion", 1);
  cJSON_AddStringToObject(payload, "requestId", request_id);
  cJSON_AddStringToObject(
    payload,
    "boardDeviceId",
    state ? state->board_device_id : ""
  );
  cJSON_AddStringToObject(payload, "runtime", "esp-p4");
  cJSON_AddNumberToObject(
    payload,
    "queriedAtMs",
    (double) ((unsigned long long) esp_timer_get_time() / 1000ULL)
  );
  cJSON_AddBoolToObject(payload, "supportsMultiple", true);
  cJSON_AddNumberToObject(payload, "maxInstalled", PET_P4_MINIAPP_CATALOG_MAX);
  cJSON_AddBoolToObject(payload, "complete", true);
  if (!request_id[0] || strlen(request_id) >= 96) {
    cJSON_AddBoolToObject(payload, "ok", false);
    cJSON_AddStringToObject(payload, "error", "missing/invalid requestId");
    cJSON_AddNullToObject(payload, "activeWidgetId");
  } else {
    cJSON_AddBoolToObject(payload, "ok", true);
    if (active) {
      cJSON_AddStringToObject(payload, "activeWidgetId", active_widget_id);
    } else {
      cJSON_AddNullToObject(payload, "activeWidgetId");
    }
    for (size_t index = 0; index < installed_count; index += 1) {
      pet_p4_miniapp_catalog_entry_t entry = {0};
      if (!pet_p4_miniapp_catalog_get(index, &entry)) continue;
      cJSON *item = cJSON_CreateObject();
      if (!item) {
        cJSON_Delete(payload);
        cJSON_Delete(items);
        cJSON_Delete(warnings);
        return false;
      }
      cJSON_AddStringToObject(item, "id", entry.widget_id);
      if (entry.title[0]) cJSON_AddStringToObject(item, "name", entry.title);
      else cJSON_AddNullToObject(item, "name");
      cJSON_AddNullToObject(item, "kind");
      cJSON_AddNullToObject(item, "version");
      cJSON_AddBoolToObject(item, "active", entry.active);
      cJSON_AddStringToObject(item, "manifestState", "valid");
      cJSON_AddBoolToObject(item, "removable", true);
      cJSON_AddItemToArray(items, item);
      active_in_catalog |= entry.active;
    }
    if (active && !active_in_catalog) {
      cJSON_AddItemToArray(
        warnings,
        cJSON_CreateString("active-widget-not-in-catalog")
      );
    }
  }
  cJSON_AddItemToObject(payload, "items", items);
  cJSON_AddItemToObject(payload, "warnings", warnings);
  send_topic_payload(send_line, ctx, "widget/inventory", payload);
  return request_id[0] && strlen(request_id) < 96;
}

static void send_miniapp_state(pet_p4_send_line_fn send_line, void *ctx) {
  pet_p4_miniapp_view_t snapshot = {0};
  bool available = pet_p4_miniapp_get_view(&snapshot);
  const pet_p4_miniapp_view_t *view = available ? &snapshot : NULL;
  cJSON *payload = cJSON_CreateObject();
  bool active = view && view->active;
  cJSON_AddBoolToObject(payload, "active", active);
  if (active) {
    cJSON_AddStringToObject(payload, "widgetId", view->widget_id);
    cJSON_AddStringToObject(payload, "state", view->state);
    cJSON_AddStringToObject(payload, "page", view->page);
    cJSON_AddStringToObject(payload, "title", view->title);
    cJSON_AddStringToObject(payload, "eyebrow", view->eyebrow);
    cJSON_AddStringToObject(payload, "headline", view->headline);
    cJSON_AddStringToObject(payload, "metricLabel", view->metric_label);
    cJSON_AddStringToObject(payload, "metricValue", view->metric_value);
    cJSON_AddStringToObject(payload, "metricUnit", view->metric_unit);
    cJSON_AddStringToObject(payload, "badge", view->badge);
    cJSON_AddStringToObject(payload, "note", view->note);
    cJSON_AddStringToObject(payload, "footer", view->footer);
    cJSON_AddNumberToObject(payload, "progressPercent", view->progress_percent);
    cJSON_AddStringToObject(payload, "progressLabel", view->progress_label);
    cJSON_AddStringToObject(payload, "visualStyle", view->visual_style);
    cJSON_AddStringToObject(payload, "visualPalette", view->visual_palette);
    cJSON_AddStringToObject(payload, "visualLayout", view->visual_layout);
    cJSON_AddStringToObject(payload, "visualSprite", view->visual_sprite);
    if (view->game.kind != PET_P4_GAME_NONE) {
      cJSON_AddStringToObject(payload, "gameType", pet_p4_game_kind_name(view->game.kind));
      cJSON_AddNumberToObject(payload, "gameGridWidth", view->game.width);
      cJSON_AddNumberToObject(payload, "gameGridHeight", view->game.height);
      cJSON_AddNumberToObject(payload, "gameScore", view->game.score);
      cJSON_AddBoolToObject(payload, "gameRunning", view->game.running);
      cJSON_AddBoolToObject(payload, "gameOver", view->game.game_over);
      cJSON_AddNumberToObject(payload, "gameRevision", view->game.revision);
    }
    cJSON_AddNumberToObject(payload, "revision", view->revision);
  }
  send_topic_payload(send_line, ctx, "miniapp/state", payload);
}

void pet_p4_send_hello(const pet_p4_runtime_state_t *state, pet_p4_send_line_fn send_line, void *ctx) {
  const esp_app_desc_t *app = esp_app_get_description();
  cJSON *payload = cJSON_CreateObject();
  cJSON *capabilities = cJSON_CreateObject();
  cJSON *display = cJSON_CreateObject();
  cJSON *asset_formats = cJSON_CreateArray();
  cJSON *hardware = cJSON_CreateObject();
  cJSON *transport = cJSON_CreateObject();
  cJSON *appearance = cJSON_CreateObject();
  cJSON *appearance_formats = cJSON_CreateArray();
  cJSON *features = cJSON_CreateObject();
  cJSON *widget_limits = cJSON_CreateObject();
  cJSON *widget_games = cJSON_CreateArray();
  cJSON *widget_game_presets = cJSON_CreateArray();
  cJSON *widget_runtimes = cJSON_CreateArray();
  cJSON *widget_scenes = cJSON_CreateArray();
  cJSON *audio_capture = cJSON_CreateObject();
  cJSON *firmware_update = cJSON_CreateObject();
  cJSON *screen_pages = cJSON_CreateArray();
  cJSON *controls = cJSON_CreateObject();
  cJSON *touch_input = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "boardDeviceId", state ? state->board_device_id : "p4-unknown");
  cJSON_AddStringToObject(payload, "runtime", "esp-p4");
  cJSON_AddStringToObject(payload, "deviceModel", "ESP32-P4 RISC-V Dual-Core + ESP32-C6");
  cJSON_AddStringToObject(payload, "fw", app ? app->version : "");
  cJSON_AddStringToObject(payload, "buildId", PET_P4_BUILD_ID);
  cJSON_AddStringToObject(payload, "gitSha", PET_P4_BUILD_GIT_SHA);
  cJSON_AddBoolToObject(payload, "buildDirty", PET_P4_BUILD_DIRTY != 0);
  cJSON_AddNumberToObject(payload, "protocolSchema", PET_P4_PROTOCOL_SCHEMA);
  cJSON_AddStringToObject(payload, "wireProtocol", PET_P4_WIRE_PROTOCOL);
  cJSON_AddStringToObject(hardware, "soc", "ESP32-P4 RISC-V Dual-Core");
  cJSON_AddStringToObject(hardware, "hpCpu", "HP@360MHz(Max 400MHz)");
  cJSON_AddStringToObject(hardware, "lpCpu", "LP@40MHz");
  cJSON_AddStringToObject(hardware, "onChipSram", "768KB L2MEM + 32KB LP SRAM + 8KB TCM");
  cJSON_AddStringToObject(hardware, "psram", "32MB In-package stacked");
  cJSON_AddStringToObject(hardware, "flash", "32MB QSPI NOR Flash");
  cJSON_AddItemToObject(payload, "hardware", hardware);

  cJSON_AddNumberToObject(capabilities, "version", 1);
  cJSON_AddBoolToObject(capabilities, "usbOnly", true);
  cJSON_AddBoolToObject(capabilities, "nativeUsb", true);
  cJSON_AddBoolToObject(capabilities, "bulk", true);
  cJSON_AddNumberToObject(capabilities, "maxAppearanceSlots", 2);
  cJSON_AddBoolToObject(capabilities, "appearanceSlotReuse", true);
  cJSON_AddBoolToObject(
    capabilities,
    "rawAppearanceSlot0",
    pet_p4_raw_assets_slot_available(PET_P4_BUILTIN_APPEARANCE_SLOT)
  );
  cJSON_AddBoolToObject(capabilities, "rawAppearanceSlot1", pet_p4_raw_assets_available());
  cJSON_AddNumberToObject(
    capabilities,
    "rawAppearanceCapacityBytes",
    (double) pet_p4_raw_assets_capacity_bytes()
  );
  cJSON_AddStringToObject(capabilities, "nativeProtocol", "pet-usb-native-v1");
  cJSON_AddBoolToObject(capabilities, "mp4", false);
  cJSON_AddBoolToObject(capabilities, "widgets", true);
  cJSON_AddBoolToObject(capabilities, "widgetDelete", true);
  cJSON_AddBoolToObject(capabilities, "widgetInventory", true);
  cJSON_AddBoolToObject(capabilities, "componentCatalogGeneration", true);
  cJSON_AddStringToObject(capabilities, "widgetRuntime", "p4-bounded-runtime-v4");
  cJSON_AddItemToArray(widget_runtimes, cJSON_CreateString("p4-bounded-runtime-v3"));
  cJSON_AddItemToArray(widget_runtimes, cJSON_CreateString("p4-bounded-runtime-v4"));
  cJSON_AddItemToObject(capabilities, "widgetRuntimes", widget_runtimes);
  cJSON_AddStringToObject(capabilities, "widgetScene", "p4-grid-scene-v2");
  cJSON_AddItemToArray(widget_scenes, cJSON_CreateString("p4-grid-scene-v1"));
  cJSON_AddItemToArray(widget_scenes, cJSON_CreateString("p4-grid-scene-v2"));
  cJSON_AddItemToObject(capabilities, "widgetScenes", widget_scenes);
  cJSON_AddBoolToObject(capabilities, "widgetSprites", true);
  cJSON_AddNumberToObject(capabilities, "componentCatalogMax", PET_P4_MINIAPP_CATALOG_MAX);
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("blocks"));
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("snake"));
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("flappy"));
  cJSON_AddItemToObject(capabilities, "widgetGames", widget_games);
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("blocks"));
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("snake"));
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("flappy"));
  cJSON_AddItemToObject(capabilities, "widgetGamePresets", widget_game_presets);
  cJSON_AddNumberToObject(widget_limits, "maxVars", 8);
  cJSON_AddNumberToObject(widget_limits, "maxStates", 6);
  cJSON_AddNumberToObject(widget_limits, "maxPages", 4);
  cJSON_AddNumberToObject(widget_limits, "maxTransitions", 12);
  cJSON_AddNumberToObject(widget_limits, "maxTicks", 8);
  cJSON_AddNumberToObject(widget_limits, "maxButtons", 8);
  cJSON_AddNumberToObject(widget_limits, "maxSceneEntities", PET_P4_GAME_MAX_ENTITIES);
  cJSON_AddNumberToObject(widget_limits, "maxSceneRules", PET_P4_GAME_MAX_RULES);
  cJSON_AddNumberToObject(widget_limits, "maxSceneOpsPerRule", PET_P4_GAME_MAX_OPS_PER_RULE);
  cJSON_AddNumberToObject(widget_limits, "maxSceneSprites", PET_P4_MINIAPP_SPRITE_MAX);
  cJSON_AddNumberToObject(widget_limits, "maxSceneSpritePixels", 4096);
  cJSON_AddNumberToObject(widget_limits, "maxWidgetJsonBytes", 4095);
  cJSON_AddNumberToObject(widget_limits, "maxButtonsJsonBytes", 2047);
  cJSON_AddBoolToObject(widget_limits, "fetchers", false);
  cJSON_AddBoolToObject(widget_limits, "readers", false);
  cJSON_AddItemToObject(capabilities, "widgetLimits", widget_limits);
  bool audio_ready = pet_p4_audio_ready();
  bool audio_playback_ready = pet_p4_audio_playback_ready();
  bool touch_ready = pet_p4_touch_ready();
  cJSON_AddBoolToObject(capabilities, "voice", audio_ready);
  cJSON_AddBoolToObject(capabilities, "stats", true);
  cJSON_AddBoolToObject(capabilities, "inputs", true);
  cJSON_AddBoolToObject(capabilities, "inputConfig", true);
  cJSON_AddBoolToObject(capabilities, "diagnostics", true);
  cJSON_AddBoolToObject(capabilities, "resetReporting", true);
  cJSON_AddBoolToObject(capabilities, "audio", audio_ready || audio_playback_ready);
  cJSON_AddBoolToObject(capabilities, "touch", touch_ready);
  cJSON_AddStringToObject(audio_capture, "codec", pet_p4_audio_codec_name());
  cJSON_AddStringToObject(audio_capture, "format", "pcm_s16le");
  cJSON_AddNumberToObject(audio_capture, "sampleRate", PET_P4_AUDIO_SAMPLE_RATE);
  cJSON_AddNumberToObject(audio_capture, "channels", PET_P4_AUDIO_CHANNELS);
  cJSON_AddNumberToObject(audio_capture, "bitsPerSample", PET_P4_AUDIO_BITS_PER_SAMPLE);
  cJSON_AddNumberToObject(audio_capture, "frameMs", PET_P4_AUDIO_FRAME_MS);
  cJSON_AddStringToObject(audio_capture, "transport", "usb-jsonl-pcm-v1");
  cJSON_AddNumberToObject(audio_capture, "maxCaptureMs", 30000);
  cJSON_AddBoolToObject(audio_capture, "playback", audio_playback_ready);
  cJSON_AddItemToObject(capabilities, "audioCapture", audio_capture);
  cJSON_AddStringToObject(touch_input, "controller", touch_ready ? "GT911" : "unavailable");
  cJSON_AddBoolToObject(touch_input, "ready", touch_ready);
  cJSON_AddStringToObject(touch_input, "coordinates", "logical-640x480");
  cJSON_AddBoolToObject(touch_input, "tap", true);
  cJSON_AddBoolToObject(touch_input, "longPress", true);
  cJSON_AddBoolToObject(touch_input, "swipe", true);
  cJSON_AddItemToObject(capabilities, "touchInput", touch_input);
  cJSON_AddBoolToObject(capabilities, "firmwareOta", true);
  cJSON_AddStringToObject(firmware_update, "transport", "usb-jsonl-ota-v1");
  cJSON_AddNumberToObject(firmware_update, "slots", 2);
  cJSON_AddNumberToObject(firmware_update, "maxImageBytes", 0x280000);
  cJSON_AddNumberToObject(firmware_update, "chunkBytes", 4 * 1024);
  cJSON_AddStringToObject(firmware_update, "checksum", "sha256");
  cJSON_AddStringToObject(firmware_update, "projectName", "pet_manager_p4_runtime");
  cJSON_AddBoolToObject(firmware_update, "rollback", true);
  cJSON_AddNumberToObject(firmware_update, "validationMs", 8000);
  cJSON_AddItemToObject(capabilities, "firmwareUpdate", firmware_update);
  cJSON_AddBoolToObject(capabilities, "protocolAck", true);
  cJSON_AddBoolToObject(capabilities, "screenshot", true);
  cJSON_AddNumberToObject(display, "width", 640);
  cJSON_AddNumberToObject(display, "height", 480);
  cJSON_AddNumberToObject(display, "physicalWidth", 480);
  cJSON_AddNumberToObject(display, "physicalHeight", 640);
  cJSON_AddStringToObject(display, "pixelFormat", "rgb565");
  cJSON_AddItemToObject(capabilities, "display", display);
  cJSON_AddItemToArray(asset_formats, cJSON_CreateString("p4-mjpeg-v1"));
  cJSON_AddItemToArray(asset_formats, cJSON_CreateString("p4-h264-v1"));
  cJSON_AddItemToArray(asset_formats, cJSON_CreateString("p4-pcm-wav-v1"));
  cJSON_AddItemToObject(capabilities, "assetFormats", asset_formats);

  cJSON_AddBoolToObject(transport, "usbOnly", true);
  cJSON_AddBoolToObject(transport, "usbUart", true);
  cJSON_AddNumberToObject(transport, "usbUartBaud", 4000000);
  cJSON_AddBoolToObject(transport, "rawAssetChunks", true);
  cJSON_AddBoolToObject(transport, "rawAssetReadyAck", true);
  cJSON_AddNumberToObject(transport, "rawAssetChunkBytes", PET_P4_RAW_ASSET_CHUNK_LIMIT);
  cJSON_AddBoolToObject(transport, "nativeUsb", true);
  cJSON_AddBoolToObject(transport, "bulk", true);
  cJSON_AddStringToObject(transport, "nativeProtocol", "pet-usb-native-v1");
  cJSON_AddItemToObject(capabilities, "transport", transport);

  cJSON_AddItemToArray(appearance_formats, cJSON_CreateString("p4-mjpeg-v1"));
  cJSON_AddItemToArray(appearance_formats, cJSON_CreateString("p4-h264-v1"));
  cJSON_AddItemToArray(appearance_formats, cJSON_CreateString("p4-pcm-wav-v1"));
  cJSON_AddItemToObject(appearance, "formats", appearance_formats);
  cJSON_AddNumberToObject(appearance, "maxSlots", 2);
  cJSON_AddBoolToObject(appearance, "slotReuse", true);
  cJSON_AddNumberToObject(appearance, "builtinSlot", 0);
  cJSON_AddNumberToObject(appearance, "customSlot", PET_P4_RAW_APPEARANCE_SLOT);
  cJSON_AddBoolToObject(appearance, "builtinProtected", true);
  cJSON_AddBoolToObject(
    appearance,
    "rawSlot0",
    pet_p4_raw_assets_slot_available(PET_P4_BUILTIN_APPEARANCE_SLOT)
  );
  cJSON_AddBoolToObject(appearance, "rawSlot1", pet_p4_raw_assets_available());
  cJSON_AddNumberToObject(
    appearance,
    "builtinCapacityBytes",
    (double) pet_p4_raw_assets_slot_capacity_bytes(PET_P4_BUILTIN_APPEARANCE_SLOT)
  );
  cJSON_AddNumberToObject(
    appearance,
    "customCapacityBytes",
    (double) pet_p4_raw_assets_capacity_bytes()
  );
  cJSON_AddBoolToObject(appearance, "mp4", false);
  cJSON_AddBoolToObject(appearance, "audio", audio_playback_ready);
  cJSON_AddItemToObject(capabilities, "appearance", appearance);

  cJSON_AddBoolToObject(features, "mainPage", true);
  cJSON_AddBoolToObject(features, "statsPage", false);
  cJSON_AddBoolToObject(features, "screenshot", true);
  cJSON_AddBoolToObject(features, "controls", true);
  cJSON_AddBoolToObject(features, "widgets", true);
  cJSON_AddBoolToObject(features, "widgetDelete", true);
  cJSON_AddBoolToObject(features, "widgetInventory", true);
  cJSON_AddBoolToObject(features, "miniAppPage", true);
  cJSON_AddBoolToObject(features, "miniAppState", true);
  cJSON_AddBoolToObject(features, "componentCenter", true);
  cJSON_AddBoolToObject(features, "voiceCapture", audio_ready);
  cJSON_AddBoolToObject(features, "audioPlayback", audio_playback_ready);
  cJSON_AddBoolToObject(features, "touchInput", touch_ready);
  cJSON_AddBoolToObject(features, "touchGestures", touch_ready);
  cJSON_AddBoolToObject(features, "firmwareOta", true);
  cJSON_AddBoolToObject(features, "protocolAck", true);
  cJSON_AddBoolToObject(features, "diagnostics", true);
  cJSON_AddBoolToObject(features, "safeReboot", true);
  cJSON_AddItemToObject(capabilities, "features", features);

  cJSON_AddNumberToObject(controls, "sw1", 50);
  cJSON_AddNumberToObject(controls, "sw2", 49);
  cJSON_AddNumberToObject(controls, "sw3", 5);
  cJSON_AddNumberToObject(controls, "encoderPress", 4);
  cJSON_AddNumberToObject(controls, "encoderB", 3);
  cJSON_AddNumberToObject(controls, "encoderA", 2);
  cJSON_AddBoolToObject(controls, "activeLow", true);
  cJSON_AddBoolToObject(controls, "configPersistent", true);
  cJSON_AddBoolToObject(controls, "touchscreen", touch_ready);
  cJSON_AddItemToObject(capabilities, "controls", controls);

  cJSON_AddItemToArray(screen_pages, cJSON_CreateString("main"));
  cJSON_AddItemToArray(screen_pages, cJSON_CreateString("components"));
  cJSON_AddItemToArray(screen_pages, cJSON_CreateString("app"));
  cJSON_AddItemToObject(capabilities, "screenPages", screen_pages);
  cJSON_AddItemToObject(payload, "capabilities", capabilities);

  send_topic_payload(send_line, ctx, "hello", payload);
}

static void handle_state_topic(pet_p4_runtime_state_t *state, const char *topic, const cJSON *payload) {
  bool was_done = lifecycle_is(runtime_lifecycle(state), "done");
  const char *agent = "";
  const char *slash = strchr(topic, '/');
  if (slash && slash[1]) agent = slash + 1;
  copy_text(state->active_agent, sizeof(state->active_agent), agent);
  copy_text(state->current_state, sizeof(state->current_state), json_string(payload, "state"));
  if (!state->current_state[0]) copy_text(state->current_state, sizeof(state->current_state), "working");
  copy_text(state->current_event, sizeof(state->current_event), json_string(payload, "event"));
  copy_text(state->current_status, sizeof(state->current_status), json_string(payload, "status"));
  if (!state->current_status[0]) copy_text(state->current_status, sizeof(state->current_status), state->current_state);
  copy_text(state->current_status_text, sizeof(state->current_status_text), json_string(payload, "statusText"));
  pet_p4_stats_update(&state->stats, payload, agent);
  if (state->stats.session_title[0]) {
    copy_text(
      state->current_session_title,
      sizeof(state->current_session_title),
      state->stats.session_title
    );
  }
  pet_p4_miniapp_sync_stats(&state->stats);
  state->last_update_ms = json_u64(payload, "tsMs", state->last_update_ms);
  mark_current_session_terminal(
    state,
    state->current_session_id,
    runtime_lifecycle(state),
    (unsigned long long) (esp_timer_get_time() / 1000ULL)
  );
  note_remote_lifecycle(state, was_done);
}

static void handle_speech(pet_p4_runtime_state_t *state, const cJSON *payload) {
  bool was_done = lifecycle_is(runtime_lifecycle(state), "done");
  const char *title_keys[] = {"displayTitle", "sessionName", "title", "agent"};
  const char *content_keys[] = {"displayContent", "text", "message"};
  const char *title = json_first_string(payload, title_keys, sizeof(title_keys) / sizeof(title_keys[0]));
  const char *content = json_first_string(payload, content_keys, sizeof(content_keys) / sizeof(content_keys[0]));
  const char *session_id = json_string(payload, "sessionId");
  if (title[0]) copy_text(state->current_title, sizeof(state->current_title), title);
  copy_text(state->current_speech, sizeof(state->current_speech), content);
  copy_text(state->current_status, sizeof(state->current_status), json_string(payload, "status"));
  if (!state->current_status[0] && state->current_state[0]) {
    copy_text(state->current_status, sizeof(state->current_status), state->current_state);
  }
  copy_text(state->current_status_text, sizeof(state->current_status_text), json_string(payload, "statusText"));
  if (session_id[0]) {
    pet_p4_session_queue_item_t *item = find_session_queue_item(
      state->session_queue,
      state->session_queue_count,
      session_id
    );
    if (item) {
      if (content[0]) copy_text(item->content, sizeof(item->content), content);
      if (title[0]) copy_text(item->title, sizeof(item->title), title);
      if (state->current_status[0]) {
        copy_text(item->state, sizeof(item->state), state->current_status);
        if (lifecycle_is_active(item->state)) item->terminal_until_ms = 0;
      }
    }
  }
  state->last_update_ms = json_u64(payload, "tsMs", state->last_update_ms);
  mark_current_session_terminal(
    state,
    session_id,
    runtime_lifecycle(state),
    (unsigned long long) (esp_timer_get_time() / 1000ULL)
  );
  note_remote_lifecycle(state, was_done);
}

static void handle_stats_update(pet_p4_runtime_state_t *state, const cJSON *payload) {
  char *raw = cJSON_PrintUnformatted((cJSON *) payload);
  if (raw) {
    copy_text(state->stats_json, sizeof(state->stats_json), raw);
    cJSON_free(raw);
  }
  pet_p4_stats_update(&state->stats, payload, state->active_agent);
  if (state->stats.session_title[0]) {
    copy_text(
      state->current_session_title,
      sizeof(state->current_session_title),
      state->stats.session_title
    );
  }
  pet_p4_miniapp_sync_stats(&state->stats);
  state->last_update_ms = json_u64(payload, "tsMs", state->last_update_ms);
}

static void handle_session_current(pet_p4_runtime_state_t *state, const cJSON *payload) {
  const char *session_id = json_string(payload, "sessionId");
  const char *agent_id = json_string(payload, "agentId");
  const char *title = json_string(payload, "title");
  const char *notice = json_string(payload, "notice");
  const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(payload, "sessions");
  const cJSON *active_session_ids =
    cJSON_GetObjectItemCaseSensitive(payload, "activeSessionIds");
  bool has_active_session_ids = cJSON_IsArray(active_session_ids);
  const cJSON *display_enabled_value =
    cJSON_GetObjectItemCaseSensitive(payload, "displayEnabled");
  bool display_enabled = !cJSON_IsBool(display_enabled_value)
    || cJSON_IsTrue(display_enabled_value);
  unsigned long long index = json_u64(payload, "index", 0);
  unsigned long long count = json_u64(payload, "count", 0);
  unsigned long long now_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  bool agent_changed = strcmp(state->current_session_agent, agent_id) != 0;

  state->session_snapshot_last_seen_ms = now_ms;

  copy_text(state->current_session_title, sizeof(state->current_session_title), title);
  copy_text(state->current_session_id, sizeof(state->current_session_id), session_id);
  copy_text(state->current_session_agent, sizeof(state->current_session_agent), agent_id);
  copy_text(state->stats.session_title, sizeof(state->stats.session_title), title);
  state->current_session_count = count > 999ULL ? 999U : (unsigned int) count;
  state->current_session_index = index > state->current_session_count
    ? state->current_session_count
    : (unsigned int) index;
  memset(state->session_queue_staging, 0, sizeof(state->session_queue_staging));
  unsigned int next_count = 0;
  if (display_enabled && cJSON_IsArray(sessions)) {
    const cJSON *session = NULL;
    cJSON_ArrayForEach(session, sessions) {
      if (next_count >= PET_P4_SESSION_QUEUE_MAX) break;
      if (!cJSON_IsObject(session)) continue;
      pet_p4_session_queue_item_t candidate = {0};
      copy_text(
        candidate.id,
        sizeof(candidate.id),
        json_string(session, "id")
      );
      copy_text(
        candidate.title,
        sizeof(candidate.title),
        json_string(session, "title")
      );
      copy_text(
        candidate.content,
        sizeof(candidate.content),
        json_string(session, "content")
      );
      copy_text(
        candidate.state,
        sizeof(candidate.state),
        json_string(session, "state")
      );
      candidate.transition_revision = json_bounded_u64(
        session,
        "transitionRevision",
        PET_P4_JSON_SAFE_INTEGER_MAX,
        0
      );
      unsigned long long terminal_remaining_ms = json_bounded_u64(
        session,
        "terminalRemainingMs",
        PET_P4_DONE_HOLD_MS,
        0
      );
      if (!candidate.id[0] || !candidate.title[0]) continue;
      if (find_session_queue_item(
            state->session_queue_staging,
            next_count,
            candidate.id
          )) {
        continue;
      }

      pet_p4_session_queue_item_t *previous = agent_changed
        ? NULL
        : find_session_queue_item(
            state->session_queue,
            state->session_queue_count,
            candidate.id
          );
      if (previous && !candidate.content[0]) {
        copy_text(candidate.content, sizeof(candidate.content), previous->content);
      }
      bool revision_is_stale = previous
        && previous->transition_revision > 0
        && candidate.transition_revision > 0
        && candidate.transition_revision < previous->transition_revision;
      bool revision_is_same = previous
        && previous->transition_revision > 0
        && candidate.transition_revision == previous->transition_revision;
      if (revision_is_stale) {
        candidate = *previous;
      } else if (lifecycle_is_active(candidate.state)) {
        if (revision_is_same && !lifecycle_is_active(previous->state)) {
          candidate = *previous;
        } else {
          if (candidate.transition_revision == 0 && previous) {
            candidate.transition_revision = previous->transition_revision;
          }
          candidate.terminal_until_ms = 0;
        }
      } else if (lifecycle_is_terminal(candidate.state)) {
        copy_text(
          candidate.state,
          sizeof(candidate.state),
          lifecycle_is(candidate.state, "error") ? "error" : "done"
        );
        if (candidate.transition_revision > 0) {
          if (revision_is_same) {
            if (!lifecycle_is_terminal(previous->state)
                || previous->terminal_until_ms <= now_ms) {
              candidate = *previous;
            } else {
              candidate.terminal_until_ms = previous->terminal_until_ms;
            }
          } else if (terminal_remaining_ms > 0) {
            candidate.terminal_until_ms = now_ms + terminal_remaining_ms;
          } else {
            continue;
          }
        } else {
          if (!previous) continue;
          candidate.transition_revision = previous->transition_revision;
          if (lifecycle_is_active(previous->state)) {
            candidate.terminal_until_ms = now_ms + PET_P4_DONE_HOLD_MS;
          } else if (lifecycle_is_terminal(previous->state)
                     && previous->terminal_until_ms > now_ms) {
            candidate.terminal_until_ms = previous->terminal_until_ms;
          } else {
            continue;
          }
        }
      } else {
        continue;
      }
      state->session_queue_staging[next_count] = candidate;
      next_count += 1;
    }
  }
  if (display_enabled && !agent_changed) {
    unsigned int ordered_count = 0;
    for (unsigned int i = 0; i < state->session_queue_count; i += 1) {
      pet_p4_session_queue_item_t *previous = &state->session_queue[i];
      unsigned int matched_index = next_count;
      for (unsigned int j = ordered_count; j < next_count; j += 1) {
        if (session_ids_match(state->session_queue_staging[j].id, previous->id)) {
          matched_index = j;
          break;
        }
      }
      if (matched_index < next_count) {
        pet_p4_session_queue_item_t updated_item =
          state->session_queue_staging[matched_index];
        if (matched_index > ordered_count) {
          memmove(
            &state->session_queue_staging[ordered_count + 1],
            &state->session_queue_staging[ordered_count],
            sizeof(state->session_queue_staging[0]) * (matched_index - ordered_count)
          );
          state->session_queue_staging[ordered_count] = updated_item;
        }
        ordered_count += 1;
        continue;
      }
      bool retain_visible_active = has_active_session_ids
        && lifecycle_is_active(previous->state)
        && json_session_id_array_contains(active_session_ids, previous->id);
      bool retain_terminal = lifecycle_is_terminal(previous->state)
        && previous->terminal_until_ms > now_ms;
      if (!retain_visible_active && !retain_terminal) {
        continue;
      }
      if (next_count >= PET_P4_SESSION_QUEUE_MAX) continue;
      if (ordered_count < next_count) {
        memmove(
          &state->session_queue_staging[ordered_count + 1],
          &state->session_queue_staging[ordered_count],
          sizeof(state->session_queue_staging[0]) * (next_count - ordered_count)
        );
      }
      state->session_queue_staging[ordered_count] = *previous;
      next_count += 1;
      ordered_count += 1;
    }
  }
  memcpy(state->session_queue, state->session_queue_staging, sizeof(state->session_queue));
  state->session_queue_count = next_count;
  pet_p4_session_queue_item_t *selected_item = find_session_queue_item(
    state->session_queue,
    state->session_queue_count,
    session_id
  );
  if (selected_item) {
    state->current_session_index =
      (unsigned int) (selected_item - state->session_queue) + 1;
  } else if (state->current_session_index > state->session_queue_count) {
    state->current_session_index = state->session_queue_count;
  }
  state->current_session_count = state->session_queue_count;
  if (state->session_queue_count == 0
      && (!display_enabled
          || (has_active_session_ids
              && cJSON_GetArraySize(active_session_ids) == 0))) {
    restore_idle_session_view(state, "session_queue_empty");
  }
  if (notice[0]) {
    copy_text(state->current_session_notice, sizeof(state->current_session_notice), notice);
    state->session_notice_until_ms = now_ms + PET_P4_SESSION_NOTICE_MS;
  }
}

static bool handle_widget_topic(
  pet_p4_runtime_state_t *state,
  const char *topic,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *transfer_id = json_string(payload, "transferId");
  char error[160] = {0};
  bool ok = false;
  const char *phase = "unknown";
  if (strcmp(topic, "widget/begin") == 0) {
    phase = "begin";
    ok = pet_p4_miniapp_install_begin(payload, error, sizeof(error));
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error);
  } else if (strcmp(topic, "widget/chunk") == 0) {
    phase = "chunk";
    ok = pet_p4_miniapp_install_chunk(payload, error, sizeof(error));
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error);
  } else if (strcmp(topic, "widget/commit") == 0) {
    phase = "commit";
    ok = pet_p4_miniapp_install_commit(payload, error, sizeof(error));
    if (ok && state) {
      pet_p4_miniapp_sync_stats(&state->stats);
      copy_text(state->screen_page, sizeof(state->screen_page), "app");
      state->last_update_ms += 1;
    }
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error);
  } else if (strcmp(topic, "widget/list") == 0) {
    ok = send_widget_inventory(state, payload, send_line, ctx);
  } else if (strcmp(topic, "widget/delete") == 0) {
    phase = "delete";
    if (!transfer_id[0] || strlen(transfer_id) >= 64) {
      copy_text(error, sizeof(error), "missing/invalid transferId");
      ok = false;
    } else {
      ok = pet_p4_miniapp_remove(json_string(payload, "widgetId"), error, sizeof(error));
    }
    if (ok && state) {
      copy_text(state->screen_page, sizeof(state->screen_page), "main");
      state->last_update_ms += 1;
    }
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error);
  } else {
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, false, "unsupported widget phase");
  }
  return ok;
}

bool pet_p4_handle_line(
  pet_p4_runtime_state_t *state,
  const char *line,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  cJSON *root;
  cJSON *payload;
  const char *topic;
  if (!state || !line || !line[0]) return false;
  root = cJSON_Parse(line);
  if (!cJSON_IsObject(root)) {
    send_protocol_ack(send_line, ctx, "", NULL, false, "invalid_json", "message must be a JSON object");
    cJSON_Delete(root);
    return false;
  }
  topic = json_string(root, "topic");
  payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
  if (!topic[0] || !cJSON_IsObject(payload)) {
    send_protocol_ack(
      send_line,
      ctx,
      topic,
      cJSON_IsObject(payload) ? payload : NULL,
      false,
      "invalid_message",
      "topic and object payload are required"
    );
    cJSON_Delete(root);
    return false;
  }
  pet_p4_state_note_host_activity(
    state,
    (unsigned long long) (esp_timer_get_time() / 1000ULL)
  );

  if (strcmp(topic, "ack") == 0 || strcmp(topic, "bind") == 0) {
    copy_text(state->desktop_device_id, sizeof(state->desktop_device_id), json_string(payload, "desktopDeviceId"));
    pet_p4_send_hello(state, send_line, ctx);
  } else if (strncmp(topic, "state/", 6) == 0) {
    handle_state_topic(state, topic, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "speech/text") == 0) {
    handle_speech(state, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "session/current") == 0) {
    handle_session_current(state, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "control/screen-page") == 0) {
    const char *page = json_string(payload, "page");
    if (strcmp(page, "main") != 0
        && strcmp(page, "components") != 0
        && strcmp(page, "app") != 0) {
      send_protocol_ack(
        send_line,
        ctx,
        topic,
        payload,
        false,
        "invalid_page",
        "page must be main, components, or app"
      );
      cJSON_Delete(root);
      return false;
    }
    if (strcmp(page, "app") == 0 && !pet_p4_miniapp_active()) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "miniapp_unavailable", "no mini-app is installed");
      cJSON_Delete(root);
      return false;
    }
    copy_text(state->screen_page, sizeof(state->screen_page), page);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "stats/update") == 0 || strcmp(topic, "runtime/stats") == 0) {
    handle_stats_update(state, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "input/config-query") == 0) {
    if (!pet_p4_input_send_config_state(
          payload,
          state ? state->board_device_id : "",
          send_line,
          ctx
        )) {
      cJSON_Delete(root);
      return false;
    }
  } else if (strcmp(topic, "input/config") == 0) {
    if (!pet_p4_input_handle_config(payload, false, send_line, ctx)) {
      cJSON_Delete(root);
      return false;
    }
  } else if (strcmp(topic, "control/command") == 0
             && strcmp(json_string(payload, "type"), "button_config") == 0) {
    if (!pet_p4_input_handle_config(payload, true, send_line, ctx)) {
      cJSON_Delete(root);
      return false;
    }
  } else if (strcmp(topic, "control/command") == 0
             && strcmp(json_string(payload, "type"), "audio_bridge") == 0) {
    const char *action = json_string(payload, "action");
    esp_err_t audio_err = ESP_ERR_INVALID_ARG;
    if (strcmp(action, "start") == 0) {
      audio_err = pet_p4_audio_set_enabled(true);
    } else if (strcmp(action, "stop") == 0) {
      (void) pet_p4_audio_capture_stop();
      audio_err = pet_p4_audio_set_enabled(false);
    }
    if (audio_err != ESP_OK) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_unavailable",
                        "P4 microphone is unavailable or audio action is invalid");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/control") == 0) {
    const char *action = json_string(payload, "action");
    esp_err_t audio_err = ESP_ERR_INVALID_ARG;
    if (strcmp(action, "start") == 0) {
      audio_err = pet_p4_audio_set_enabled(true);
      if (audio_err == ESP_OK) audio_err = pet_p4_audio_capture_start(state->session_queue_count == 0);
    } else if (strcmp(action, "stop") == 0) {
      audio_err = pet_p4_audio_capture_stop();
    }
    if (audio_err != ESP_OK) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_control_failed",
                        "audio action is invalid or capture is not ready");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/query") == 0) {
    pet_p4_audio_send_status();
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "system/heartbeat") == 0) {
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (pet_p4_diagnostics_handle_topic(topic, payload, state, send_line, ctx)) {
    state->last_update_ms += 1;
  } else if (pet_p4_ota_handle_topic(topic, payload, send_line, ctx)) {
    state->last_update_ms += 1;
  } else if (strncmp(topic, "asset/", 6) == 0) {
    handle_asset_topic(state, topic, payload, send_line, ctx);
  } else if (strncmp(topic, "widget/", 7) == 0) {
    if (!handle_widget_topic(state, topic, payload, send_line, ctx)) {
      cJSON_Delete(root);
      return false;
    }
  } else if (strcmp(topic, "miniapp/event") == 0) {
    const char *action = json_string(payload, "action");
    if (!pet_p4_miniapp_dispatch_action(action, json_u64(payload, "tsMs", 0))) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "miniapp_event_unhandled",
                        "action is not handled by the active mini-app state");
      cJSON_Delete(root);
      return false;
    }
    state->last_update_ms += 1;
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "miniapp/query") == 0) {
    send_miniapp_state(send_line, ctx);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else {
    bool known = topic_is_known_but_unsupported(topic);
    send_protocol_ack(
      send_line,
      ctx,
      topic,
      payload,
      false,
      known ? "unsupported_topic" : "unknown_topic",
      known ? "topic is not supported by this firmware" : "topic is not recognized"
    );
    cJSON_Delete(root);
    return false;
  }

  cJSON_Delete(root);
  return true;
}
