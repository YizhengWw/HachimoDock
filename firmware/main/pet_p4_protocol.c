/*
 * [Input] JSON-line control/state envelopes from USB-UART and native USB.
 * [Output] validated runtime updates, ACK/NACK responses, persisted input
 *          configuration snapshots, transfer routing, deterministic appearance
 *          pack discovery/reactivation across both flash slots, compatible
 *          v3/v4 component capabilities with bounded sprite limits, restart-safe
 *          slot cleanup, idle-transfer recovery and legacy-pack migration, 60-second per-session
 *          terminal conversation visibility across active-queue refreshes,
 *          stable first-seen card ordering, selected-card lifecycle authority for
 *          appearance playback, and exact retained-card selection preservation
 *          after host refreshes; queue resets preserve physical voice-hold feedback;
 *          DSH R5 approval pending state with fail-closed expiry and one-decision
 *          idempotency; actual chip revision for safe firmware selection.
 * [Pos] shared ESP32-P4 protocol dispatcher.
 * [Sync] If this file changes, update firmware/protocol.md and .folder.md.
 */

#include "pet_p4_protocol.h"
#include "pet_p4_usb_native.h"
#include "pet_p4_variable_owners.h"
#include "pet_p4_conversation.h"
#include "pet_p4_audio.h"
#include "pet_p4_audio_patch.h"
#include "pet_p4_system_cues.h"
#include "esp_attr.h"
#include "pet_p4_transport_config.h"
#include "pet_p4_control_integrity.h"
#include "pet_p4_control_session.h"
#include "pet_p4_screenshot_close.h"
#include "pet_p4_evidence_identity.h"
#include "pet_p4_component_program.h"
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_resources_device.h"

#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "pet_p4_json_source.h"
#include "mbedtls/base64.h"
#include "bsp/display.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_p4_audio.h"
#include "pet_p4_audio_volume.h"
#include "pet_p4_audio_frontend.h"
#include "pet_p4_build_info.h"
#include "pet_p4_diagnostics.h"
#include "pet_p4_input.h"
#include "pet_p4_miniapp.h"
#include "pet_p4_boot_trace.h"
#include "pet_p4_lcd_profile.h"
#include "pet_p4_widget_trace.h"
#include "pet_p4_media.h"
#include "pet_p4_app_host.h"
#include "pet_p4_ota.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_renderer.h"
#include "pet_p4_bubble_pixels.h"
#include "pet_p4_touch.h"
#include "pet_p4_notify.h"

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
/* DSH approval deadlines: the plugin/PC bridge use Unix epoch milliseconds,
 * while the device only has a boot-relative monotonic clock. 2020-01-01 is far
 * beyond any plausible boot-relative value, so it cleanly separates the two. */
#define PET_P4_WALL_CLOCK_EPOCH_MIN_MS 1577836800000ULL
#define PET_P4_APPROVAL_MAX_HOLD_MS 300000ULL

static const char *TAG = "pet-p4-protocol";
static pet_boot_identity_t g_boot_identity;
void pet_p4_boot_identity_start(uint32_t first,uint32_t second){(void)pet_boot_identity_start(&g_boot_identity,first,second);}
const char *pet_p4_boot_identity(void){return g_boot_identity.ready?g_boot_identity.id:"";}
/* Renderer observes this outside the runtime-state mutex. */
static _Atomic int g_asset_active_slot = -1;
static int g_asset_transfer_slot = -1;
static char g_asset_transfer_id[PET_P4_DEVICE_ID_MAX];
static unsigned long long g_asset_transfer_last_activity_ms;
static pet_p4_send_line_fn g_asset_sender;
static void *g_asset_route;
static pet_appearance_session_t g_appearance_session;
bool pet_p4_appearance_claim(pet_p4_send_line_fn send,void *ctx,const char *id,unsigned kind){
  /* Serialize status-cue admission BEFORE either serial/native owner can start IO.
   * A refused claim never releases another owner's gate. Main confirms quiescence. */
  pet_p4_audio_status_cues_defer(true);
  return pet_appearance_claim(&g_appearance_session,send,ctx,id,kind);
}
bool pet_p4_appearance_owned(pet_p4_send_line_fn send,void *ctx,const char *id,unsigned kind){return pet_appearance_matches(&g_appearance_session,send,ctx,id,kind);}
bool pet_p4_appearance_end(pet_p4_send_line_fn send,void *ctx,const char *id,unsigned kind){return pet_appearance_release(&g_appearance_session,send,ctx,id,kind);}
bool pet_p4_appearance_active(void){return g_appearance_session.active;}

bool pet_p4_appearance_native_active(void){return g_appearance_session.active&&g_appearance_session.kind==PET_APPEARANCE_NATIVE;}
static _Atomic bool g_asset_cleanup_due;
static bool audio_patch_protocol_active(void);
static bool audio_patch_idle_due(uint64_t now);
static void audio_patch_timeout_worker(pet_p4_runtime_state_t *state);

static unsigned long long g_host_wall_clock_ms;
static unsigned long long g_host_wall_clock_at_ms;
static bool g_asset_phase_write_attempted;
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

static const char *selected_session_lifecycle(const pet_p4_runtime_state_t *state) {
  if (!state || state->session_queue_count == 0) return NULL;
  unsigned int selected = state->current_session_index > 0
    ? state->current_session_index - 1
    : 0;
  if (selected >= state->session_queue_count) selected = 0;
  const char *lifecycle = state->session_queue[selected].state;
  return lifecycle[0] ? lifecycle : NULL;
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
  return lifecycle_is(value, "done") || lifecycle_is(value, "error")
    || lifecycle_is(value, "stopped");
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
  /* Active lifecycle from any agent must survive an empty session snapshot. */
  if (lifecycle_is_active(runtime_lifecycle(state))) return;
  /* The bridge has no DSH session snapshot; an empty snapshot must not erase
   * the working/tool/done state carried by state/deepseek-harness. */
  if (strcmp(state->active_agent, "deepseek-harness") == 0
      || strncmp(state->current_event, "tool/", 5) == 0
      || strncmp(state->current_event, "turn/", 5) == 0
      || strncmp(state->current_event, "session", 7) == 0) return;
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
  // Queue refresh/expiry does not end a physical push-to-talk hold.
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

static bool close_asset_chunk_file(void) {
  FILE *file = g_asset_chunk_file;
  g_asset_chunk_file = NULL;
  if(file)g_asset_phase_write_attempted=true;
  if (file && fclose(file) != 0) {
    g_asset_chunk_stat_valid = false;
    snprintf(g_asset_chunk_error, sizeof(g_asset_chunk_error), "transferred file close failed");
    return false;
  }
  return true;
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
  if(!cJSON_IsNumber(item)||!isfinite(item->valuedouble)||item->valuedouble<0
      ||item->valuedouble>PET_P4_JSON_SAFE_INTEGER_MAX)return fallback;
  unsigned long long value=(unsigned long long)item->valuedouble;
  return (double)value==item->valuedouble?value:fallback;
}

static unsigned long long json_bounded_u64(
  const cJSON *object,
  const char *key,
  unsigned long long maximum,
  unsigned long long fallback
) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)
      || item->valuedouble < 0
      || item->valuedouble > (double) maximum) {
    return fallback;
  }
  unsigned long long value=(unsigned long long)item->valuedouble;
  return (double)value==item->valuedouble?value:fallback;
}

#if PET_P4_MUTATION_ACK_DEFERRED
static pet_p4_query_reply_t *g_deferred_reply;
static TaskHandle_t g_deferred_reply_owner;
static bool deferrable_topic(const char *topic){
  if(!topic)return false;
  if(!strcmp(topic,"widget-install-ack"))return true;
#if PET_P4_LARGE_REPLY_DEFERRED
  if(!strcmp(topic,"widget/inventory")||!strcmp(topic,"hello")
      ||!strcmp(topic,"widget/catalog_snapshot")||!strcmp(topic,"widget/catalog_reconcile_result"))return true;
#endif
  return false;
}
static void flush_deferred_reply(pet_p4_send_line_fn send_line,void *ctx){
  if(g_deferred_reply&&g_deferred_reply->line){
    char *held=g_deferred_reply->line;g_deferred_reply->line=NULL;
    if(send_line)send_line(held,ctx);
    cJSON_free(held);
  }
}
#endif
/* Best-effort host wall clock from the latest system/heartbeat tsMs. Returns 0
 * until an anchor was received, so callers can fail closed instead of guessing. */
static unsigned long long host_wall_clock_estimate_ms(unsigned long long now_ms) {
  unsigned long long elapsed_ms;
  if (!g_host_wall_clock_ms || !g_host_wall_clock_at_ms) return 0;
  elapsed_ms = now_ms >= g_host_wall_clock_at_ms ? now_ms - g_host_wall_clock_at_ms : 0;
  return g_host_wall_clock_ms + elapsed_ms;
}

/* Converts the optional approval expiresAtMs into a device monotonic deadline.
 * Accepts Unix epoch ms (the plugin/bridge contract) and, defensively, values
 * already expressed in the boot-relative monotonic base. */
static unsigned long long approval_deadline_ms(
  unsigned long long expires_ms,
  unsigned long long now_ms,
  bool *expired
) {
  unsigned long long wall_now_ms;
  unsigned long long remaining_ms;
  *expired = false;
  if (expires_ms == 0) return now_ms + PET_P4_APPROVAL_TIMEOUT_MS;
  if (expires_ms < PET_P4_WALL_CLOCK_EPOCH_MIN_MS) {
    /* Already boot-relative milliseconds (debug/legacy bridge senders). */
    if (expires_ms <= now_ms) *expired = true;
    return expires_ms;
  }
  wall_now_ms = host_wall_clock_estimate_ms(now_ms);
  if (!wall_now_ms) {
    /* No heartbeat anchor yet: keep the bounded local default so a wall-clock
     * value can never turn into an endless modal. */
    return now_ms + PET_P4_APPROVAL_TIMEOUT_MS;
  }
  if (expires_ms <= wall_now_ms) {
    *expired = true;
    return 0;
  }
  remaining_ms = expires_ms - wall_now_ms;
  if (remaining_ms > PET_P4_APPROVAL_MAX_HOLD_MS) {
    remaining_ms = PET_P4_APPROVAL_MAX_HOLD_MS;
  }
  return now_ms + remaining_ms;
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
#if PET_P4_MUTATION_ACK_DEFERRED
  if(line&&send_line&&g_deferred_reply&&g_deferred_reply_owner==xTaskGetCurrentTaskHandle()){
    if(!g_deferred_reply->line&&deferrable_topic(topic)){g_deferred_reply->line=line;cJSON_Delete(root);return;}
    flush_deferred_reply(send_line,ctx);
  }
#endif
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
  PET_WIDGET_TRACE_SCOPE(trace_ack, PET_WTRACE_ACK);
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
  /* Main resumes admission after the serialized storage transition finishes. */
  pet_p4_renderer_set_asset_preparation_suspended(true);
  file = fopen(PET_P4_ASSET_SLOT_MARKER, "wb");
  if (!file) return false;
  bool written=fputc('0'+slot,file)!=EOF&&fputc('\n',file)!=EOF;
  if(fclose(file)!=0)written=false;
  if(!written)return false;
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
  if (!slot_is_valid(slot)) return false;
  pet_p4_renderer_set_asset_preparation_suspended(true);
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
      size_t name_size = strlen(entry->d_name);
      const char base[] = PET_P4_SPIFFS_PREFIX "/";
      if (name_size >= sizeof(fs_path) - (sizeof(base)-1U)) { closedir(dir); return false; }
      memcpy(fs_path, base, sizeof(base)-1U);
      memcpy(fs_path+sizeof(base)-1U, entry->d_name, name_size+1U);
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

static bool ensure_asset_transfer_slot(const char *id) {
  return pet_appearance_id_valid(id)&&slot_is_valid(g_asset_transfer_slot)
    &&!strcmp(g_asset_transfer_id,id);
}

static void reset_asset_transfer_slot(void) {
  if (pet_p4_appearance_mutation_blocked()) return;
  (void)pet_p4_appearance_end(g_asset_sender,g_asset_route,g_asset_transfer_id,PET_APPEARANCE_SERIAL);
  g_asset_sender=NULL;g_asset_route=NULL;
  pet_p4_raw_assets_reset_transfer();
  g_asset_transfer_slot = -1;
  g_asset_transfer_id[0] = '\0';
}

static void note_asset_transfer_activity(void) {
  g_asset_transfer_last_activity_ms =
    (unsigned long long) (esp_timer_get_time() / 1000ULL);
}

static bool abort_serial_asset_transfer(pet_p4_runtime_state_t *state) {
  (void)state;
  if(g_appearance_session.recovery_blocked)return false;
  if(!close_asset_chunk_file())return false;
  g_asset_phase_write_attempted=true;
  if(slot_is_valid(g_asset_transfer_slot)&&!clear_slot_files(g_asset_transfer_slot))return false;
  reset_raw_asset_chunk();reset_asset_chunk_tracking();pet_p4_raw_assets_reset_transfer();return true;
}

void pet_p4_asset_transfer_process(pet_p4_runtime_state_t *state,unsigned long long now_ms) {
  /* Main holds the state lock: latch only, no close/free/scan/erase/TX here.
   * Claims and standalone reads publish true synchronously before admission.
   * Only this complete lock-held retirement observation may publish false. */
  if(state)pet_p4_audio_status_cues_defer(state->asset_transfer_active
    ||g_appearance_session.active||g_appearance_session.operation_active
    ||g_appearance_session.recovery_blocked||g_raw_asset_chunk.active
    ||g_raw_asset_chunk.data||g_asset_chunk_file||g_asset_transfer_slot>=0);
  if(!pet_p4_appearance_mutation_blocked()&&state&&state->asset_transfer_active
      &&g_asset_transfer_slot>=0&&g_asset_transfer_last_activity_ms
      &&now_ms>=g_asset_transfer_last_activity_ms
      &&now_ms-g_asset_transfer_last_activity_ms>=PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS)
    atomic_store_explicit(&g_asset_cleanup_due,true,memory_order_release);
  if(!pet_p4_appearance_mutation_blocked()&&audio_patch_idle_due(now_ms))
    atomic_store_explicit(&g_asset_cleanup_due,true,memory_order_release);
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
  bool closed_ok=!ferror(file);
  if(fclose(file)!=0)closed_ok=false;
  if (!closed_ok || ch != '"' || written == 0) {
    out[0] = '\0';
    return false;
  }
  out[written] = '\0';
  return true;
}

static bool read_ready_slot_pack_id(int slot, char *out, size_t out_size) {
  char manifest_pack_id[PET_P4_PACK_ID_MAX];
  char marker_pack_id[PET_P4_PACK_ID_MAX+2];
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
  marker_len=fread(marker_pack_id,1,sizeof(marker_pack_id),file);
  bool closed_ok=!ferror(file)&&fgetc(file)==EOF&&!ferror(file);
  if(fclose(file)!=0)closed_ok=false;
  bool exact=marker_len==PET_P4_PACK_ID_MAX-1
    ||(marker_len==PET_P4_PACK_ID_MAX&&marker_pack_id[marker_len-1]=='\n')
    ||(marker_len==PET_P4_PACK_ID_MAX+1&&marker_pack_id[marker_len-2]=='\r'&&marker_pack_id[marker_len-1]=='\n');
  if(!closed_ok||!exact)return false;
  marker_pack_id[PET_P4_PACK_ID_MAX-1]='\0';
  if(strcmp(marker_pack_id,manifest_pack_id))return false;
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

static bool write_ready_pack_exact(int slot,const char *pack,bool *changed);
bool pet_p4_asset_mark_slot_ready(int slot) {
  char pack[PET_P4_PACK_ID_MAX];bool changed=false;
  return read_slot_pack_id(slot,pack,sizeof(pack))
    &&write_ready_pack_exact(slot,pack,&changed);
}

bool pet_p4_asset_slot_has_pack_id(int slot) {
  char pack_id[PET_P4_PACK_ID_MAX];
  return read_slot_pack_id(slot, pack_id, sizeof(pack_id));
}

static bool (*g_asset_query_io_begin)(void *);
static void (*g_asset_query_io_end)(void *);
static void *g_asset_query_io_context;

static bool appearance_phase_try_begin(pet_appearance_phase_t *p,
    pet_p4_runtime_state_t *state,pet_p4_send_line_fn send,void *route,
    const char *id,unsigned kind,int slot,const pet_appearance_commit_io_t *io,
    bool *storage_busy) {
  if(storage_busy)*storage_busy=false;
  if(!p||!state||!io||!io->begin||!io->end||pet_p4_appearance_mutation_blocked()
      ||!pet_p4_appearance_owned(send,route,id,kind)
      ||(kind==PET_APPEARANCE_SERIAL&&send==pet_p4_native_usb_send_json_line&&!pet_p4_native_usb_route_current(route))||(io->current&&!io->current(io->context)))return false;
  memset(p,0,sizeof(*p));
  pet_resource_result_t opened=pet_resources_device_owner_open(
    PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&p->owner);
  pet_resource_result_t acquired=opened==PET_RESOURCE_OK
    ?pet_resources_device_acquire(p->owner,PET_RESOURCE_STORAGE_WORK,1,&p->lease)
    :PET_RESOURCE_UNAVAILABLE;
  if(opened!=PET_RESOURCE_OK||acquired!=PET_RESOURCE_OK){
    /* Only physical occupancy is transient here. Protected audio, stale owner,
     * owner-table exhaustion and invalid inputs are not admission retries. */
    if(storage_busy)*storage_busy=opened==PET_RESOURCE_OK&&acquired==PET_RESOURCE_BUSY;
    pet_resources_device_revoke(p->owner);return false;
  }
  p->generation=g_appearance_session.generation;p->revision=state->asset_revision;
  p->active_slot=g_asset_active_slot;p->target_slot=slot;p->kind=kind;p->sender=send;p->route=route;
  copy_text(p->id,sizeof(p->id),id);p->io=*io;
  g_appearance_session.operation_active=true;
  if(!io->begin(io->context)){
    g_appearance_session.operation_active=false;
    (void)pet_resources_device_finish(p->lease);pet_resources_device_revoke(p->owner);return false;
  }
  /* begin borrowed the state lock; only captured route/atomic current is safe here. */
  if((p->kind==PET_APPEARANCE_SERIAL&&p->sender==pet_p4_native_usb_send_json_line
        &&!pet_p4_native_usb_route_current(p->route))
      ||(p->io.current&&!p->io.current(p->io.context))){
    p->io.end(p->io.context);
    if(g_appearance_session.generation==p->generation)g_appearance_session.operation_active=false;
    (void)pet_resources_device_finish(p->lease);pet_resources_device_revoke(p->owner);
    return false;
  }
  p->active=true;return true;
}
bool pet_p4_appearance_phase_begin(pet_appearance_phase_t *p,
    pet_p4_runtime_state_t *state,pet_p4_send_line_fn send,void *route,
    const char *id,unsigned kind,int slot,const pet_appearance_commit_io_t *io){
  return appearance_phase_try_begin(p,state,send,route,id,kind,slot,io,NULL);
}
/* The serial raw header has not run and no ready ACK/body has been granted.
 * Yield only a BUSY storage admission, without retaining owner/lease/state lock.
 * This 1s budget fits inside the existing PC 5s ready-ACK deadline; it never
 * retries an entered phase, an allocator failure, a protected audio reservation
 * or any physical mutation. */
static bool serial_raw_phase_begin_wait(pet_appearance_phase_t *p,
    pet_p4_runtime_state_t *state,pet_p4_send_line_fn send,void *route,
    const char *id,int slot,const pet_appearance_commit_io_t *io){
  if(!state||!io||!io->begin||!io->end)return false;
  uint64_t generation=g_appearance_session.generation;
  unsigned revision=state->asset_revision;
  int active_slot=g_asset_active_slot;
  uint64_t started=(uint64_t)esp_timer_get_time()/1000ULL;
  for(;;){
    bool storage_busy=false;
    if(appearance_phase_try_begin(p,state,send,route,id,PET_APPEARANCE_SERIAL,slot,io,&storage_busy))return true;
    uint64_t now=(uint64_t)esp_timer_get_time()/1000ULL;
    if(!storage_busy||now<started||now-started>=1000ULL)return false;
    if(!io->begin(io->context))return false;
    TickType_t ticks=pdMS_TO_TICKS(10);
    vTaskDelay(ticks?ticks:1);
    io->end(io->context);
    /* Re-admission is about the same unexecuted header, never a newer owner. */
    now=(uint64_t)esp_timer_get_time()/1000ULL;
    if(now<started||now-started>=1000ULL
        ||g_appearance_session.generation!=generation
        ||state->asset_revision!=revision||g_asset_active_slot!=active_slot
        ||g_asset_transfer_slot!=slot||!ensure_asset_transfer_slot(id)
        ||pet_p4_appearance_mutation_blocked()||g_raw_asset_chunk.active
        ||!pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL)
        ||(send==pet_p4_native_usb_send_json_line&&!pet_p4_native_usb_route_current(route))
        ||(io->current&&!io->current(io->context)))return false;
  }
}
pet_appearance_commit_result_t pet_p4_appearance_phase_end(pet_appearance_phase_t *p,
    pet_p4_runtime_state_t *state,int slot,bool ok,bool attempted) {
  if(!p||!p->active)return PET_APPEARANCE_COMMIT_FAILED;
  p->io.end(p->io.context);p->active=false;
  bool current=g_appearance_session.operation_active&&!g_appearance_session.recovery_blocked
    &&g_appearance_session.generation==p->generation
    &&pet_p4_appearance_owned(p->sender,p->route,p->id,p->kind)
    &&state->asset_revision==p->revision&&g_asset_active_slot==p->active_slot
    &&slot==p->target_slot&&pet_resources_device_current(p->lease)
    &&(p->kind!=PET_APPEARANCE_SERIAL||p->sender!=pet_p4_native_usb_send_json_line||pet_p4_native_usb_route_current(p->route))
    &&(!p->io.current||p->io.current(p->io.context));
  pet_appearance_commit_result_t result=current&&ok?PET_APPEARANCE_COMMIT_OK:PET_APPEARANCE_COMMIT_FAILED;
  if(!current||(!ok&&attempted)){
    g_appearance_session.recovery_blocked=true;result=PET_APPEARANCE_COMMIT_UNKNOWN;
  }
  if(g_appearance_session.generation==p->generation)g_appearance_session.operation_active=false;
  (void)pet_resources_device_finish(p->lease);pet_resources_device_revoke(p->owner);return result;
}

#define APPEARANCE_OBSERVATION_SCHEMA "p4-appearance-quiescent-v1"
static bool (*g_asset_query_pending)(void *);
static void *g_asset_query_pending_context;
bool pet_p4_protocol_asset_query_pending_configure(bool (*pending)(void *),void *context) {
  if(!pending||g_asset_query_pending)return false;
  g_asset_query_pending=pending;g_asset_query_pending_context=context;return true;
}
typedef struct {
  bool asset_transfer,raw_receiving,appearance_owner,file_open,slot_pending;
  bool native_transfer,native_receiving,native_pending,worker_pending,storage_pending,firmware_ota,reboot_pending;
  uint32_t native_epoch;
} appearance_io_observation_t;
static appearance_io_observation_t appearance_io_observe(const pet_p4_runtime_state_t *state) {
  pet_p4_native_io_snapshot_t native=pet_p4_native_usb_io_snapshot();
  bool busy=pet_p4_appearance_mutation_blocked();
  appearance_io_observation_t s={
    .asset_transfer=state->asset_transfer_active,.raw_receiving=busy||g_raw_asset_chunk.active,
    .appearance_owner=pet_p4_appearance_active(),.file_open=busy||g_asset_chunk_file!=NULL,
    .slot_pending=busy||g_asset_transfer_slot>=0,.native_transfer=native.transfer_active,
    .native_receiving=native.receiving,.native_pending=native.worker_pending,
    .worker_pending=!g_asset_query_pending||g_asset_query_pending(g_asset_query_pending_context),
    .storage_pending=pet_p4_miniapp_storage_busy(),
    .firmware_ota=pet_p4_ota_transfer_active(),.reboot_pending=pet_p4_diagnostics_reboot_pending(),
    .native_epoch=native.epoch
  };return s;
}
static bool appearance_io_busy(appearance_io_observation_t s) {
  return s.asset_transfer||s.raw_receiving||s.appearance_owner||s.file_open||s.slot_pending
    ||s.native_transfer||s.native_receiving||s.native_pending||s.worker_pending||s.storage_pending||s.firmware_ota||s.reboot_pending;
}

bool pet_p4_protocol_asset_query_io_configure(bool (*begin)(void *),
    void (*end)(void *), void *context) {
  if (!begin || !end || g_asset_query_io_begin) return false;
  g_asset_query_io_begin=begin;g_asset_query_io_end=end;
  g_asset_query_io_context=context;return true;
}
static void asset_query_refuse(pet_p4_send_line_fn send,void *route,const char *id,const char *error){
  if(g_asset_query_io_begin&&g_asset_query_io_end&&g_asset_query_io_begin(g_asset_query_io_context)){
    send_asset_ack(send,route,id,"slot-query",NULL,false,error);g_asset_query_io_end(g_asset_query_io_context);
  }
}
/* Modern requests cannot downgrade to an unbound legacy read. Only immutable
 * request fields and caller-mutex-owned session metadata are read here. */
static bool asset_query_modern(const cJSON *request){
  static const char *const keys[]={"observationSchema","queryNonce","expectedBoardDeviceId","expectedBootId"};
  for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++)
    if(cJSON_GetObjectItemCaseSensitive(request,keys[i]))return true;
  return false;
}
static bool asset_query_bound(const pet_p4_runtime_state_t *state,const cJSON *request,const char *id){
  static const char *const keys[]={"observationSchema","queryNonce","expectedBoardDeviceId","expectedBootId","transferId"};
  if(!state||!cJSON_IsObject(request)||!pet_appearance_id_valid(id))return false;
  for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++){
    unsigned count=0;const cJSON *item=NULL;
    cJSON_ArrayForEach(item,request)if(item->string&&!strcmp(item->string,keys[i]))count++;
    if(count!=1||!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(request,keys[i])))return false;
  }
  return !strcmp(json_string(request,"observationSchema"),APPEARANCE_OBSERVATION_SCHEMA)
    &&pet_appearance_id_valid(json_string(request,"queryNonce"))
    &&!strcmp(json_string(request,"transferId"),id)
    &&state->board_device_id[0]&&pet_p4_boot_identity()[0]
    &&!strcmp(json_string(request,"expectedBoardDeviceId"),state->board_device_id)
    &&!strcmp(json_string(request,"expectedBootId"),pet_p4_boot_identity());
}
static void asset_query_not_proof(const pet_p4_runtime_state_t *state,const cJSON *request,
    pet_p4_send_line_fn send,void *route,const char *id,bool unknown){
  /* Snapshot identity before borrowing IO. Never inspect private transfer/file
   * tracking, sample storage, or synthesize slot contents on this fast path. */
  char board[sizeof(state->board_device_id)],boot[128];
  const char *current_boot=pet_p4_boot_identity();
  if(strlen(state->board_device_id)>=sizeof(board)||strlen(current_boot)>=sizeof(boot))return;
  memcpy(board,state->board_device_id,strlen(state->board_device_id)+1);
  memcpy(boot,current_boot,strlen(current_boot)+1);
  if(!g_asset_query_io_begin(g_asset_query_io_context))return;
  cJSON *payload=cJSON_CreateObject();
  bool ok=payload&&cJSON_AddStringToObject(payload,"transferId",id)
    &&cJSON_AddStringToObject(payload,"phase","slot-query")
    &&cJSON_AddBoolToObject(payload,"ok",false)
    &&cJSON_AddStringToObject(payload,"observationSchema",APPEARANCE_OBSERVATION_SCHEMA)
    &&cJSON_AddStringToObject(payload,"queryNonce",json_string(request,"queryNonce"))
    &&cJSON_AddStringToObject(payload,"boardDeviceId",board)
    &&cJSON_AddStringToObject(payload,"bootId",boot)
    &&cJSON_AddBoolToObject(payload,"serialized",true)
    &&cJSON_AddBoolToObject(payload,"proof",false)
    &&cJSON_AddBoolToObject(payload,"quiescent",false)
    &&cJSON_AddStringToObject(payload,"errorCode",unknown?
      "appearance_query_outcome_unknown":"appearance_query_busy");
  if(ok){send_topic_payload(send,route,"asset/ack",payload);payload=NULL;}
  cJSON_Delete(payload);g_asset_query_io_end(g_asset_query_io_context);
}
/* RAM-only manifest snapshot while the state mutex is held. Header metadata
 * reads/JSON allocation happen only in the existing borrowed storage window. */
typedef struct {char path[PET_P4_ASSET_PATH_MAX];uint32_t size;} asset_query_video_t;
static EXT_RAM_BSS_ATTR asset_query_video_t g_asset_query_videos[PET_P4_ASSET_CATALOG_MAX_FAMILIES];
static unsigned g_asset_query_video_count;
static bool asset_query_snapshot_videos(const pet_p4_asset_catalog_t *catalog){
  g_asset_query_video_count=0;
  if(!catalog||!catalog->count||catalog->count>PET_P4_ASSET_CATALOG_MAX_FAMILIES)return false;
  for(unsigned i=0;i<catalog->count;i++){
    const pet_p4_asset_entry_t *entry=&catalog->entries[i];
    if(!memchr(entry->path,0,sizeof(entry->path))||!entry->path[0]||!entry->stream_bytes)return false;
    unsigned j=0;
    for(;j<g_asset_query_video_count;j++)if(!strcmp(g_asset_query_videos[j].path,entry->path))break;
    if(j<g_asset_query_video_count){if(g_asset_query_videos[j].size!=entry->stream_bytes)return false;continue;}
    copy_text(g_asset_query_videos[j].path,sizeof(g_asset_query_videos[j].path),entry->path);
    g_asset_query_videos[j].size=entry->stream_bytes;g_asset_query_video_count++;
  }
  return true;
}
static cJSON *asset_query_video_proof(int slot,const char *pack){
  cJSON *videos=cJSON_CreateArray();if(!videos)return NULL;
  for(unsigned i=0;i<g_asset_query_video_count;i++){
    uint32_t size=0;unsigned long long checksum=0;char hex[17];
    if(!pet_p4_raw_assets_stat_for_slot(slot,pack,g_asset_query_videos[i].path,
        g_asset_query_videos[i].size,&size,&checksum)){cJSON_Delete(videos);return NULL;}
    snprintf(hex,sizeof(hex),"%016llx",checksum);
    cJSON *entry=cJSON_CreateObject();
    bool ok=entry&&cJSON_AddStringToObject(entry,"path",g_asset_query_videos[i].path)
      &&cJSON_AddNumberToObject(entry,"size",size)&&cJSON_AddStringToObject(entry,"checksum",hex);
    if(ok&&cJSON_AddItemToArray(videos,entry))entry=NULL;else ok=false;
    cJSON_Delete(entry);if(!ok){cJSON_Delete(videos);return NULL;}
  }
  return videos;
}
static void send_asset_slot_query_ack(pet_p4_runtime_state_t *state,const cJSON *request,
    pet_p4_send_line_fn send,void *route,const char *id){
  bool observation=asset_query_modern(request);
  const char *nonce=json_string(request,"queryNonce");
  if(!state||!g_asset_query_io_begin||!g_asset_query_io_end
      ||(observation&&(!g_asset_query_pending||!asset_query_bound(state,request,id)))){
    asset_query_refuse(send,route,id,"appearance observation binding unavailable");return;
  }
  /* UNKNOWN wins over concurrent operation flags. It is not retryable busy.
   * Both refusals precede FS, broker acquisition and private tracking reads. */
  if(g_appearance_session.recovery_blocked){
    if(observation)asset_query_not_proof(state,request,send,route,id,true);
    else asset_query_refuse(send,route,id,"appearance outcome unknown");
    return;
  }
  if(pet_p4_appearance_mutation_blocked()||pet_p4_appearance_active()||state->asset_transfer_active){
    if(observation)asset_query_not_proof(state,request,send,route,id,false);
    else asset_query_refuse(send,route,id,"appearance IO active");
    return;
  }
  pet_p4_audio_status_cues_defer(true); /* Read-only storage also needs cue exclusion. */
  pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
  pet_resource_result_t admission=pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&owner);
  if(admission==PET_RESOURCE_OK)admission=pet_resources_device_acquire(owner,PET_RESOURCE_STORAGE_WORK,1,&lease);
  if(admission!=PET_RESOURCE_OK){
    pet_resources_device_revoke(owner);
    if(observation&&admission==PET_RESOURCE_BUSY)asset_query_not_proof(state,request,send,route,id,false);
    else asset_query_refuse(send,route,id,"appearance storage unavailable");
    return;
  }
  appearance_io_observation_t before=appearance_io_observe(state);
  int slot=atomic_load_explicit(&g_asset_active_slot,memory_order_acquire);
  unsigned revision=state->asset_revision;uint64_t generation=g_appearance_session.generation;
  bool video_snapshot=slot==PET_P4_RAW_APPEARANCE_SLOT&&asset_query_snapshot_videos(&state->asset_catalog);
  /* The read owns exclusion too; it is not an active appearance mutation and
   * does not create a durable result/ACK. Own exclusion is removed before the
   * second sample, while the caller still holds the state mutex. */
  g_appearance_session.operation_active=true;state->asset_transfer_active=true;
  if(!g_asset_query_io_begin(g_asset_query_io_context)){
    g_appearance_session.operation_active=false;state->asset_transfer_active=false;
    (void)pet_resources_device_finish(lease);pet_resources_device_revoke(owner);return;
  }
  cJSON *payload=cJSON_CreateObject(),*slots=cJSON_CreateArray();
  bool ok=payload&&slots;
  if(ok){
    ok=cJSON_AddStringToObject(payload,"transferId",id?id:"")
      &&cJSON_AddStringToObject(payload,"phase","slot-query")
      &&cJSON_AddBoolToObject(payload,"builtinProtected",true)
      &&cJSON_AddBoolToObject(payload,"systemCues",true)
      &&cJSON_AddBoolToObject(payload,"ok",true);
    if(ok&&slot_is_valid(slot))ok=cJSON_AddNumberToObject(payload,"activeSlot",slot)!=NULL;
    for(int i=0;ok&&i<PET_P4_ASSET_SLOT_COUNT;i++){
      char pack[PET_P4_PACK_ID_MAX];bool valid=read_ready_slot_pack_id(i,pack,sizeof(pack));
      cJSON *entry=cJSON_CreateObject();
      ok=entry&&cJSON_AddNumberToObject(entry,"slot",i)&&cJSON_AddBoolToObject(entry,"valid",valid);
      if(ok&&valid)ok=cJSON_AddStringToObject(entry,"packId",pack)!=NULL;
      if(ok&&valid&&i==slot&&video_snapshot){
        cJSON *videos=asset_query_video_proof(slot,pack);
        /* Missing proof never authorizes an audio-only custom-pack mismatch. */
        if(videos&&!cJSON_AddItemToObject(payload,"videos",videos)){cJSON_Delete(videos);ok=false;}
      }
      if(ok&&cJSON_AddItemToArray(slots,entry))entry=NULL;else ok=false;
      cJSON_Delete(entry);
    }
    if(ok&&cJSON_AddItemToObject(payload,"slots",slots))slots=NULL;else ok=false;
  }
  g_asset_query_io_end(g_asset_query_io_context);
  bool current=g_appearance_session.generation==generation&&g_appearance_session.operation_active
    &&!g_appearance_session.active&&!g_appearance_session.recovery_blocked
    &&state->asset_revision==revision&&g_asset_active_slot==slot&&pet_resources_device_current(lease);
  g_appearance_session.operation_active=false;state->asset_transfer_active=false;
  appearance_io_observation_t after=appearance_io_observe(state);
  bool changed=!current||before.native_epoch!=after.native_epoch;
  (void)pet_resources_device_finish(lease);pet_resources_device_revoke(owner);
  if(!g_asset_query_io_begin(g_asset_query_io_context)){
    /* Adapter failure: bounded, private JSON metadata only; no file handle,
     * lease or payload escapes and no success is emitted. */
    cJSON_Delete(slots);cJSON_Delete(payload);return;
  }
  if(observation&&ok){
    ok=cJSON_AddStringToObject(payload,"observationSchema",APPEARANCE_OBSERVATION_SCHEMA)
      &&cJSON_AddStringToObject(payload,"queryNonce",nonce)
      &&cJSON_AddStringToObject(payload,"boardDeviceId",state->board_device_id)
      &&cJSON_AddStringToObject(payload,"bootId",pet_p4_boot_identity())
      &&cJSON_AddBoolToObject(payload,"serialized",true)
      &&cJSON_AddBoolToObject(payload,"snapshotChanged",changed)
      &&cJSON_AddBoolToObject(payload,"quiescent",!changed&&!appearance_io_busy(before)&&!appearance_io_busy(after));
#define OBS(name,field) if(ok)ok=cJSON_AddBoolToObject(payload,name,before.field||after.field)!=NULL
    OBS("assetTransferActive",asset_transfer);OBS("rawReceiving",raw_receiving);OBS("appearanceOwnerActive",appearance_owner);
    OBS("assetFileOpen",file_open);OBS("assetSlotPending",slot_pending);OBS("nativeTransferActive",native_transfer);
    OBS("nativeReceiving",native_receiving);OBS("nativeWorkerPending",native_pending);OBS("protocolWorkerPending",worker_pending);
    OBS("storageWorkerPending",storage_pending);OBS("firmwareOtaActive",firmware_ota);OBS("rebootPending",reboot_pending);
#undef OBS
    if(ok)ok=cJSON_AddNumberToObject(payload,"nativeIoEpoch",after.native_epoch)!=NULL;
  }
  cJSON_Delete(slots);
  if(ok&&current){send_topic_payload(send,route,"asset/ack",payload);payload=NULL;}
  else if(payload)send_asset_ack(send,route,id,"slot-query",NULL,false,"appearance read snapshot unavailable");
  cJSON_Delete(payload);g_asset_query_io_end(g_asset_query_io_context);
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
  bool ok=!ferror(file);if(fclose(file)!=0)ok=false;
  if(!ok)return false;
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
    g_asset_phase_write_attempted=true;
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
    if(!close_asset_chunk_file())return false;
    reset_asset_chunk_tracking();
  }
  if (!g_asset_chunk_file) {
    g_asset_phase_write_attempted=true;
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
  void *ctx,
  const char **error_code
) {
  if(error_code)*error_code="raw_ready_ack_unconfirmed";
  const char *transfer_id = json_string(payload, "transferId");
  const char *path = json_string(payload, "path");
  const char *index = json_string(payload, "index");
  const char *checksum_text = json_string(payload, "checksum");
  unsigned long long expected_size = json_u64(payload, "size", 0);
  char *checksum_end = NULL;
  unsigned long long expected_checksum;

  if(g_raw_asset_chunk.active){
    if(error_code)*error_code="raw_already_receiving";
    send_asset_ack_indexed(send_line,ctx,transfer_id,"raw-ready",path,index,false,"raw chunk still active");return false;
  }
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
    if(error_code)*error_code="invalid_raw_header";
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
    if(error_code)*error_code="transfer_slot_mismatch";
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
    if(error_code)*error_code="raw_buffer_unavailable";
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
  if(error_code)*error_code=NULL;
  g_raw_asset_chunk.active = true;
  g_raw_asset_chunk.expected_size = (size_t) expected_size;
  g_raw_asset_chunk.expected_checksum = expected_checksum;
  copy_text(g_raw_asset_chunk.transfer_id, sizeof(g_raw_asset_chunk.transfer_id), transfer_id);
  copy_text(g_raw_asset_chunk.logical_path, sizeof(g_raw_asset_chunk.logical_path), path);
  copy_text(g_raw_asset_chunk.index, sizeof(g_raw_asset_chunk.index), index);
  (void)state; /* caller publishes transfer state under its existing mutex */
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

bool pet_p4_raw_asset_chunk_active_for_route(pet_p4_send_line_fn send,void *ctx){
  return !pet_p4_appearance_mutation_blocked()&&g_raw_asset_chunk.active
    &&pet_p4_appearance_owned(send,ctx,g_raw_asset_chunk.transfer_id,PET_APPEARANCE_SERIAL);
}
bool pet_p4_raw_asset_chunk_active(void){
  return !pet_p4_appearance_mutation_blocked()&&g_raw_asset_chunk.active;
}

size_t pet_p4_consume_raw_asset_bytes(const unsigned char *data,size_t len,
    pet_p4_send_line_fn send,void *route) {
  /* Legacy callers have no physical-task lock adapter: fail closed. Main uses
   * the scoped entry; never borrow a protocol-worker grant from an RX task. */
  (void)data;(void)len;(void)send;(void)route;return 0;
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
    send_asset_ack(send_line, ctx, transfer_id, "stat", path, false, "asset stat unavailable");
    return;
  }
  send_asset_stat_ack(send_line, ctx, transfer_id, path, size, checksum);
}

/* One protocol command owns each activation across all unlocked phases.
 * Native appearance mutations are excluded by the route/ID owner; component
 * and save writes are excluded by the same STORAGE_WORK broker as miniapp.
 * An uncertain marker result retains its appearance owner until reboot:
 * no timeout, retry, cleanup or slot-query echo may resolve a mutation. */
static uint64_t g_asset_activation_generation;
typedef struct {
  char manifest[PET_P4_ASSET_MANIFEST_MAX];
  pet_p4_asset_catalog_t catalog;
  char pack_id[PET_P4_PACK_ID_MAX];
} asset_activation_snapshot_t;

static bool asset_phase_send_current(pet_p4_send_line_fn send,void *route);

static void send_asset_activation_ack(pet_p4_send_line_fn send_line, void *ctx,
    const char *id, bool ok, const char *error) {
  /* Same native FIFO command borrows the state lock for exactly one TX.
   * Missing/invalid dispatcher binding fails closed without sending success. */
  if (!g_asset_query_io_begin || !g_asset_query_io_end
      || !g_asset_query_io_begin(g_asset_query_io_context)) return;
  send_asset_ack(send_line, ctx, id, "activate", NULL, ok, error);
  g_asset_query_io_end(g_asset_query_io_context);
}

static bool prepare_asset_snapshot(int slot,const char *expected,
    asset_activation_snapshot_t *next) {
  char path[PET_P4_FS_PATH_MAX];
  if (!p4_asset_manifest_path_for_candidate(slot,path,sizeof(path))) return false;
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  size_t n = fread(next->manifest, 1, sizeof(next->manifest) - 1, file);
  bool ok = !ferror(file) && fgetc(file) == EOF && !ferror(file);
  if (fclose(file) != 0) ok = false;
  next->manifest[n] = '\0';
  if (!ok || !n || memchr(next->manifest, '\0', n) || !pet_p4_protocol_source_valid(next->manifest)) return false;
  cJSON *root = cJSON_ParseWithOpts(next->manifest, NULL, true);
  const cJSON *pack = root ? cJSON_GetObjectItemCaseSensitive(root, "packId") : NULL;
  unsigned count = 0; const cJSON *field;
  cJSON_ArrayForEach(field, root) if (field->string && !strcmp(field->string, "packId")) count++;
  ok = cJSON_IsObject(root) && count == 1 && cJSON_IsString(pack)
    && strlen(pack->valuestring)==PET_P4_PACK_ID_MAX-1;
  if(ok) for(const char *p=pack->valuestring;*p;p++)
    if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f'))) {ok=false;break;}
  if(ok && expected)ok=!strcmp(pack->valuestring,expected);
  if(ok)copy_text(next->pack_id,sizeof(next->pack_id),pack->valuestring);
  cJSON_Delete(root);
  return ok && pet_p4_asset_catalog_parse(&next->catalog, next->manifest)
    && next->catalog.count > 0;
}
static bool prepare_asset_activation(int slot,const char *expected,
    asset_activation_snapshot_t *next) {
  char actual[PET_P4_PACK_ID_MAX];
  return read_ready_slot_pack_id(slot,actual,sizeof(actual))&&!strcmp(actual,expected)
    &&prepare_asset_snapshot(slot,expected,next);
}

static bool write_asset_activation_marker(int slot) {
  FILE *file = fopen(PET_P4_ASSET_SLOT_MARKER, "wb");
  if (!file) return false;
  bool ok = fputc('0' + slot, file) != EOF && fputc('\n', file) != EOF;
  if (fclose(file) != 0) ok = false;
  if (!ok) return false;
  /* No repair/rewrite on a failed readback. A completed but uncertain write
   * must not be represented as an untouched old slot or automatically retried. */
  file = fopen(PET_P4_ASSET_SLOT_MARKER, "rb");
  if (!file) return false;
  ok = fgetc(file) == '0' + slot && fgetc(file) == '\n'
    && fgetc(file) == EOF && !ferror(file);
  if (fclose(file) != 0) ok = false;
  return ok;
}

static void handle_asset_activate(
  pet_p4_runtime_state_t *state, const cJSON *payload,
  pet_p4_send_line_fn send_line, void *ctx
) {
  const char *id = json_string(payload, "transferId");
  const char *expected = json_string(payload, "packId");
  const cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(payload, "slot");
  uint32_t number = 0;
  const char *error = "appearance activation unavailable";
  if (!state || !g_asset_query_io_begin || !g_asset_query_io_end
      || !asset_phase_send_current(send_line, ctx)) return;
  if (!pet_appearance_size(cJSON_IsNumber(slot_item) ? slot_item->valuedouble : -1, &number)
      || !slot_is_valid((int)number) || !pet_appearance_id_valid(id)
      || strlen(expected) != PET_P4_PACK_ID_MAX - 1) {
    send_asset_activation_ack(send_line, ctx, id, false, "invalid appearance slot, transfer or pack id"); return;
  }
  for (size_t i=0; expected[i]; i++) if (!((expected[i]>='0' && expected[i]<='9')
      || (expected[i]>='a' && expected[i]<='f'))) {
    send_asset_activation_ack(send_line, ctx, id, false, "invalid appearance pack id"); return;
  }
  appearance_io_observation_t observed = appearance_io_observe(state);
  /* Later FIFO commands do not own IO; only actual in-flight actors exclude
   * this command. Requiring an empty FIFO here could starve user activation. */
  observed.worker_pending = false;
  if (appearance_io_busy(observed) || g_asset_activation_generation == UINT64_MAX) {
    send_asset_activation_ack(send_line, ctx, id, false, "appearance or component storage is busy"); return;
  }
  pet_p4_audio_status_cues_defer(true); /* Before broker admission, not after claim. */
  pet_resource_owner_t owner = {0};
  pet_resource_lease_t lease = {0};
  if (pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK), &owner) != PET_RESOURCE_OK
      || pet_resources_device_acquire(owner, PET_RESOURCE_STORAGE_WORK, 1, &lease) != PET_RESOURCE_OK) {
    pet_resources_device_revoke(owner);
    send_asset_activation_ack(send_line, ctx, id, false, "appearance storage owner unavailable"); return;
  }
  if (!pet_p4_appearance_claim(send_line, ctx, id, PET_APPEARANCE_SERIAL)) {
    (void)pet_resources_device_finish(lease); pet_resources_device_revoke(owner);
    send_asset_activation_ack(send_line, ctx, id, false, "appearance owner unavailable"); return;
  }
  const uint64_t generation = ++g_asset_activation_generation;
  const int slot = (int)number, old_slot = g_asset_active_slot;
  const unsigned revision = state->asset_revision;
  asset_activation_snapshot_t *next = NULL;
  bool ok = false, marker_attempted = false, unlocked = false, same = false;
  if (!g_asset_query_io_begin(g_asset_query_io_context)) goto done;
  unlocked = true;
  next = calloc(1, sizeof(*next));
  if (!next) {error = "appearance activation workspace unavailable"; goto relock;}
  if (!pet_resources_device_current(lease)
      || !prepare_asset_activation(slot, expected, next)) {
    error = "appearance pack is not present in requested slot or failed verification"; goto relock;
  }
  /* Reacquire before granting the only persistent write. Payload and physical
   * resources have not changed, even while main/input remained runnable. */
  g_asset_query_io_end(g_asset_query_io_context); unlocked = false;
  if (!asset_phase_send_current(send_line, ctx)
      || generation != g_asset_activation_generation
      || !pet_p4_appearance_owned(send_line, ctx, id, PET_APPEARANCE_SERIAL)
      || g_asset_active_slot != old_slot || state->asset_revision != revision
      || !pet_resources_device_current(lease)) {
    error = "appearance activation ownership changed"; goto done;
  }
  same = slot == old_slot && state->asset_family_count == next->catalog.count
    && !strcmp(state->asset_manifest_json, next->manifest);
  if (!g_asset_query_io_begin(g_asset_query_io_context)) goto done;
  unlocked = true;
  /* Same-slot activation still verifies disk bytes, but need not rewrite an
   * already published exact snapshot. Local/cache IDs alone never admit it. */
  if (!same) {
    if (!asset_phase_send_current(send_line, ctx)) {error = "appearance route retired before marker"; goto relock;}
    marker_attempted = true;
    if (!write_asset_activation_marker(slot)) {
      error = "appearance marker outcome unknown; do not replay"; goto relock;
    }
  }
  g_asset_query_io_end(g_asset_query_io_context); unlocked = false;
  if (!asset_phase_send_current(send_line, ctx)
      || generation != g_asset_activation_generation
      || !pet_p4_appearance_owned(send_line, ctx, id, PET_APPEARANCE_SERIAL)
      || g_asset_active_slot != old_slot || state->asset_revision != revision
      || !pet_resources_device_current(lease)) {
    error = "appearance publication outcome unknown; do not replay"; goto done;
  }
  /* Publication is only a bounded RAM snapshot; no parser, IO, free or TX. */
  g_asset_active_slot = slot;
  memcpy(state->asset_manifest_json, next->manifest, strlen(next->manifest) + 1);
  state->asset_catalog = next->catalog;
  state->asset_family_count = next->catalog.count;
  state->asset_revision++;
  ok = true;
  goto done;
relock:
  if (unlocked) {g_asset_query_io_end(g_asset_query_io_context); unlocked = false;}
done:
  /* Keep an uncertain persistent operation quarantined. Releasing a different
   * generation's owner or replaying a write is never a recovery strategy. */
  if (!ok && marker_attempted) {
    g_appearance_session.recovery_blocked = true;
    error = "appearance activation outcome unknown; do not replay";
  }
  if (ok || !marker_attempted) (void)pet_p4_appearance_end(send_line, ctx, id, PET_APPEARANCE_SERIAL);
  (void)pet_resources_device_finish(lease); pet_resources_device_revoke(owner);
  /* Heap release and one ACK also run without retaining the main mutex. */
  if (g_asset_query_io_begin(g_asset_query_io_context)) {
    free(next);
    send_asset_ack(send_line, ctx, id, "activate", NULL, ok, ok ? NULL : error);
    g_asset_query_io_end(g_asset_query_io_context);
  } else free(next); /* Broken dispatcher: no success, retry, or leaked workspace. */
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
  if (!close_asset_chunk_file()) {
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false, "transferred file close failed");
    return;
  }
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
    reset_asset_chunk_tracking();
    send_asset_ack(send_line, ctx, transfer_id, "file", path, false, error);
    return;
  }
  reset_asset_chunk_tracking();
  send_asset_ack(send_line, ctx, transfer_id, "file", path, true, NULL);
}

static void send_serial_commit_ack(pet_p4_send_line_fn send,void *route,
    const char *id,bool patch,bool ok,const char *error) {
  if(!g_asset_query_io_begin||!g_asset_query_io_end
      ||!g_asset_query_io_begin(g_asset_query_io_context))return;
  send_asset_ack(send,route,id,patch?"patch":"commit",NULL,ok,error);
  g_asset_query_io_end(g_asset_query_io_context);
}
static void handle_serial_asset_commit(pet_p4_runtime_state_t *state,
    pet_p4_send_line_fn send,void *route,const char *id,bool patch) {
  const pet_appearance_commit_io_t io={g_asset_query_io_begin,g_asset_query_io_end,
    NULL,g_asset_query_io_context};
  char error[128]={0};
  pet_appearance_commit_result_t result=pet_p4_appearance_commit_slot(state,
    g_asset_transfer_slot,send,route,id,PET_APPEARANCE_SERIAL,patch,&io,error,sizeof(error));
  if(result==PET_APPEARANCE_COMMIT_OK) {
    reset_raw_asset_chunk();reset_asset_chunk_tracking();reset_asset_transfer_slot();
    state->asset_transfer_active=false;g_asset_transfer_last_activity_ms=0;
  }
  send_serial_commit_ack(send,route,id,patch,result==PET_APPEARANCE_COMMIT_OK,
    result==PET_APPEARANCE_COMMIT_OK?NULL:error);
}

/* Reply capture is private to this synchronous phase; no route is retained.
 * Real ACK delivery happens once, only after lock-held final owner validation. */
enum {
  RAW_PHASE_OPERATION=1u<<0,RAW_PHASE_RECOVERY=1u<<1,RAW_PHASE_GENERATION=1u<<2,
  RAW_PHASE_OWNER=1u<<3,RAW_PHASE_REVISION=1u<<4,RAW_PHASE_ACTIVE_SLOT=1u<<5,
  RAW_PHASE_TARGET_SLOT=1u<<6,RAW_PHASE_LEASE=1u<<7,RAW_PHASE_ROUTE=1u<<8,
  RAW_PHASE_IO=1u<<9,RAW_PHASE_CAPTURE=1u<<10,RAW_PHASE_BUFFER=1u<<11,
  RAW_PHASE_RECEIVED=1u<<12,RAW_PHASE_PHYSICAL_IO=1u<<13,RAW_PHASE_ROLLBACK=1u<<14
};
typedef struct {char *line;bool ok,seen,invalid;const char *error_code;uint32_t reason_bits;const char *first_cause;} asset_phase_reply_t;
static void raw_phase_note(asset_phase_reply_t *r,uint32_t bit,const char *cause){
  r->reason_bits|=bit;if(!r->first_cause)r->first_cause=cause;
}
/* Same current gates as phase_end, sampled while holding the state mutex. */
static uint32_t raw_header_phase_bits(const pet_appearance_phase_t *p,
    const pet_p4_runtime_state_t *state,int slot){
  uint32_t bits=0;
  if(!g_appearance_session.operation_active)bits|=RAW_PHASE_OPERATION;
  if(g_appearance_session.recovery_blocked)bits|=RAW_PHASE_RECOVERY;
  if(g_appearance_session.generation!=p->generation)bits|=RAW_PHASE_GENERATION;
  if(!pet_p4_appearance_owned(p->sender,p->route,p->id,p->kind))bits|=RAW_PHASE_OWNER;
  if(state->asset_revision!=p->revision)bits|=RAW_PHASE_REVISION;
  if(g_asset_active_slot!=p->active_slot)bits|=RAW_PHASE_ACTIVE_SLOT;
  if(slot!=p->target_slot||g_asset_transfer_slot!=slot||!ensure_asset_transfer_slot(p->id))bits|=RAW_PHASE_TARGET_SLOT;
  if(!pet_resources_device_current(p->lease))bits|=RAW_PHASE_LEASE;
  if(p->kind==PET_APPEARANCE_SERIAL&&p->sender==pet_p4_native_usb_send_json_line&&!pet_p4_native_usb_route_current(p->route))bits|=RAW_PHASE_ROUTE;
  if(p->io.current&&!p->io.current(p->io.context))bits|=RAW_PHASE_IO;
  return bits;
}
static const char *raw_phase_first_cause(uint32_t bits){
  if(bits&RAW_PHASE_OPERATION)return "operation_not_active";
  if(bits&RAW_PHASE_RECOVERY)return "recovery_blocked";
  if(bits&RAW_PHASE_GENERATION)return "generation_changed";
  if(bits&RAW_PHASE_OWNER)return "owner_changed";
  if(bits&RAW_PHASE_REVISION)return "asset_revision_changed";
  if(bits&RAW_PHASE_ACTIVE_SLOT)return "active_slot_changed";
  if(bits&RAW_PHASE_TARGET_SLOT)return "target_slot_changed";
  if(bits&RAW_PHASE_LEASE)return "storage_lease_not_current";
  if(bits&RAW_PHASE_ROUTE)return "route_retired";
  if(bits&RAW_PHASE_IO)return "io_context_retired";
  return "header_rejected";
}
/* Only called before asset_phase_deliver: the positive ready exists solely in
 * the private capture, never in a transport. No raw/body IO is retried here. */
static pet_appearance_commit_result_t finish_unpublished_raw_header(
    pet_appearance_phase_t *p,pet_p4_runtime_state_t *state,int slot,
    const cJSON *payload,unsigned char *new_buffer,asset_phase_reply_t *reply,bool attempted){
  if(!p||!p->active)return PET_APPEARANCE_COMMIT_FAILED;
  p->io.end(p->io.context);
  uint32_t bits=raw_header_phase_bits(p,state,slot);
  if(bits)raw_phase_note(reply,bits,raw_phase_first_cause(bits));
  bool ok=reply->seen&&reply->ok&&!reply->invalid;
  if(!reply->seen||reply->invalid)raw_phase_note(reply,RAW_PHASE_CAPTURE,"ready_capture_failed");
  if(attempted)raw_phase_note(reply,RAW_PHASE_PHYSICAL_IO,"physical_io_attempted");
  bool exact=new_buffer&&g_raw_asset_chunk.active&&g_raw_asset_chunk.data==new_buffer
    &&g_raw_asset_chunk.expected_size==json_u64(payload,"size",0)
    &&g_raw_asset_chunk.expected_checksum==strtoull(json_string(payload,"checksum"),NULL,16)
    &&!strcmp(g_raw_asset_chunk.transfer_id,p->id)
    &&!strcmp(g_raw_asset_chunk.logical_path,json_string(payload,"path"))
    &&!strcmp(g_raw_asset_chunk.index,json_string(payload,"index"));
  if(new_buffer&&!exact)raw_phase_note(reply,RAW_PHASE_BUFFER,"header_buffer_changed");
  if(new_buffer&&g_raw_asset_chunk.received_size)raw_phase_note(reply,RAW_PHASE_RECEIVED,"body_received_before_ready");
  bool clean_candidate=exact&&!g_raw_asset_chunk.received_size&&!attempted;
  bool rolled_back=false;
  /* A revoked STORAGE_WORK lease still forbids commit. Only this private,
   * zero-body RAM reservation may be cancelled; all identity gates still hold.
   * operation_active pins the old buffer while borrowing the mutex for free. */
  if((!ok||bits)&&clean_candidate&&!(bits&~RAW_PHASE_LEASE)){
    if(p->io.begin(p->io.context)){
      reset_raw_asset_chunk();
      free(reply->line);reply->line=NULL;
      p->io.end(p->io.context);
      uint32_t after=raw_header_phase_bits(p,state,slot);
      if(after)raw_phase_note(reply,after,raw_phase_first_cause(after));
      rolled_back=!(after&~RAW_PHASE_LEASE)&&!g_raw_asset_chunk.active
        &&!g_raw_asset_chunk.data&&!g_raw_asset_chunk.received_size&&!g_raw_asset_chunk.expected_size;
      if(rolled_back){
        /* Discard the old captured success/invalid ACK. Fresh typed NACK
         * construction must still complete atomically in asset_phase_deliver. */
        reply->seen=true;reply->ok=false;reply->invalid=false;
        reply->error_code="phase_admission_rejected";
      }
    }
    if(!rolled_back)raw_phase_note(reply,RAW_PHASE_ROLLBACK,"header_rollback_unconfirmed");
  }
  pet_appearance_commit_result_t result=bits==0&&ok?PET_APPEARANCE_COMMIT_OK:PET_APPEARANCE_COMMIT_FAILED;
  if(rolled_back)result=PET_APPEARANCE_COMMIT_FAILED;
  else if(bits||attempted||(reply->reason_bits&RAW_PHASE_ROLLBACK)
      ||(new_buffer&&(!exact||g_raw_asset_chunk.received_size))){
    result=PET_APPEARANCE_COMMIT_UNKNOWN;
    if(g_appearance_session.generation==p->generation
        &&pet_p4_appearance_owned(p->sender,p->route,p->id,p->kind))g_appearance_session.recovery_blocked=true;
  }
  p->active=false;
  if(g_appearance_session.generation==p->generation
      &&pet_p4_appearance_owned(p->sender,p->route,p->id,p->kind))g_appearance_session.operation_active=false;
  (void)pet_resources_device_finish(p->lease);pet_resources_device_revoke(p->owner);
  return result;
}

static void asset_phase_capture(const char *line,void *context){
  asset_phase_reply_t *r=context;
  if(!r||r->seen){if(r){r->invalid=true;raw_phase_note(r,RAW_PHASE_CAPTURE,"ready_capture_failed");}return;}
  r->seen=true;
  cJSON *root=cJSON_ParseWithOpts(line,NULL,true);
  const cJSON *p=cJSON_GetObjectItemCaseSensitive(root,"payload");
  const cJSON *ok=cJSON_GetObjectItemCaseSensitive(p,"ok");
  if(!cJSON_IsBool(ok))r->invalid=true;
  else r->ok=cJSON_IsTrue(ok);
  cJSON_Delete(root);
  size_t n=strlen(line)+1;r->line=malloc(n);
  if(r->line)memcpy(r->line,line,n);else r->invalid=true;
  if(r->invalid)raw_phase_note(r,RAW_PHASE_CAPTURE,"ready_capture_failed");
}
static bool asset_phase_send_current(pet_p4_send_line_fn send,void *route){
  return send!=pet_p4_native_usb_send_json_line||pet_p4_native_usb_route_current(route);
}
/* Snapshotted under the state mutex. Build/send the bounded ACK only after
 * borrowing the mutex, so TX and allocation do not extend the state lock. */
typedef struct {
  bool owner_matched,phase_current,raw_idle,header_accepted;
  size_t remaining;
  char board_id[PET_P4_DEVICE_ID_MAX],boot_id[sizeof(g_boot_identity.id)];
  const char *error_code,*first_cause;
  uint32_t reason_bits;
} raw_ready_rejection_t;
static raw_ready_rejection_t raw_ready_rejection_snapshot(const pet_p4_runtime_state_t *state,
    pet_p4_send_line_fn send,void *route,const char *id,asset_phase_reply_t *reply,pet_appearance_commit_result_t result){
  raw_ready_rejection_t proof={0};
  proof.reason_bits=reply?reply->reason_bits:0;
  proof.first_cause=reply&&reply->first_cause?reply->first_cause:"not_recorded";
  proof.owner_matched=pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL);
  bool coherent=g_raw_asset_chunk.active
    ?g_raw_asset_chunk.data&&g_raw_asset_chunk.expected_size>0
      &&g_raw_asset_chunk.expected_size<=PET_P4_RAW_ASSET_CHUNK_LIMIT
      &&g_raw_asset_chunk.received_size<=g_raw_asset_chunk.expected_size
    :!g_raw_asset_chunk.data&&!g_raw_asset_chunk.expected_size&&!g_raw_asset_chunk.received_size;
  proof.raw_idle=coherent&&!g_raw_asset_chunk.active;
  proof.header_accepted=!proof.raw_idle; /* Never claim rejection means idle. */
  proof.remaining=coherent&&g_raw_asset_chunk.active
    ?g_raw_asset_chunk.expected_size-g_raw_asset_chunk.received_size:0;
  proof.phase_current=coherent&&proof.owner_matched&&asset_phase_send_current(send,route)
    &&!pet_p4_appearance_mutation_blocked()&&result==PET_APPEARANCE_COMMIT_FAILED
    &&!(reply&&reply->invalid)&&state&&state->board_device_id[0]&&pet_p4_boot_identity()[0];
  copy_text(proof.board_id,sizeof(proof.board_id),state?state->board_device_id:"");
  copy_text(proof.boot_id,sizeof(proof.boot_id),pet_p4_boot_identity());
  proof.error_code=result==PET_APPEARANCE_COMMIT_UNKNOWN?"phase_outcome_unknown"
    :reply&&reply->error_code?reply->error_code
    :reply&&reply->invalid?"raw_ready_ack_unconfirmed"
    :g_raw_asset_chunk.active?"raw_already_receiving"
    :!proof.owner_matched?"appearance_owner_mismatch":"phase_admission_rejected";
  return proof;
}
static const char *raw_ready_rejection_message(const char *code){
  if(!strcmp(code,"raw_already_receiving"))return "raw chunk still active";
  if(!strcmp(code,"invalid_raw_header"))return "invalid raw asset chunk header";
  if(!strcmp(code,"transfer_slot_mismatch"))return "slot prepare failed";
  if(!strcmp(code,"raw_buffer_unavailable"))return "no memory";
  if(!strcmp(code,"raw_ready_ack_unconfirmed"))return "raw readiness acknowledgement unavailable";
  if(!strcmp(code,"appearance_owner_mismatch"))return "appearance owner does not match";
  if(!strcmp(code,"phase_outcome_unknown"))return "appearance phase outcome unknown; do not replay";
  return "appearance phase admission rejected";
}
static void send_raw_ready_rejection(pet_p4_send_line_fn send,void *route,
    const char *id,const char *path,const char *index,const raw_ready_rejection_t *proof){
  cJSON *p=cJSON_CreateObject();
  bool ok=p&&cJSON_AddStringToObject(p,"transferId",id?id:"")
    &&cJSON_AddStringToObject(p,"phase","raw-ready")
    &&cJSON_AddStringToObject(p,"path",path?path:"")
    &&cJSON_AddStringToObject(p,"index",index?index:"")
    &&cJSON_AddBoolToObject(p,"ok",false)
    &&cJSON_AddStringToObject(p,"rawReadyProof","p4-raw-ready-proof-v1")
    &&cJSON_AddStringToObject(p,"boardDeviceId",proof->board_id)
    &&cJSON_AddStringToObject(p,"bootId",proof->boot_id)
    &&cJSON_AddBoolToObject(p,"ownerMatched",proof->owner_matched)
    &&cJSON_AddBoolToObject(p,"phaseCurrent",proof->phase_current)
    &&cJSON_AddBoolToObject(p,"rawIdle",proof->raw_idle)
    &&cJSON_AddNumberToObject(p,"rawRemaining",(double)proof->remaining)
    &&cJSON_AddBoolToObject(p,"headerAccepted",proof->header_accepted)
    &&cJSON_AddNumberToObject(p,"phaseReasonBits",proof->reason_bits)
    &&cJSON_AddStringToObject(p,"phaseFirstCause",proof->first_cause)
    &&cJSON_AddStringToObject(p,"errorCode",proof->error_code)
    &&cJSON_AddStringToObject(p,"error",raw_ready_rejection_message(proof->error_code));
  if(ok)send_topic_payload(send,route,"asset/ack",p);else cJSON_Delete(p);
}
static void asset_phase_deliver(const pet_p4_runtime_state_t *state,
    const pet_appearance_commit_io_t *io,
    pet_p4_send_line_fn send,void *route,asset_phase_reply_t *reply,
    const char *id,const char *phase,const char *path,const char *index,
    pet_appearance_commit_result_t result){
  bool raw_rejected=phase&&!strcmp(phase,"raw-ready")
    &&result!=PET_APPEARANCE_COMMIT_OK;
  raw_ready_rejection_t proof={0};
  if(raw_rejected)proof=raw_ready_rejection_snapshot(state,send,route,id,reply,result);
  /* An idle boundary proof cannot borrow the mutex and then allow a different
   * appearance owner to replace its snapshot. Pin only this quiescent owner;
   * never block draining an already-active raw receiver. No storage lease is
   * held by this metadata/TX pin. The client still rechecks connection+boot. */
  uint64_t reply_generation=g_appearance_session.generation;
  bool reply_pinned=raw_rejected&&proof.phase_current&&proof.raw_idle;
  if(reply_pinned)g_appearance_session.operation_active=true;
  if(!io||!io->begin||!io->end||!io->begin(io->context)){
    if(reply_pinned&&g_appearance_session.generation==reply_generation
        &&pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL))
      g_appearance_session.operation_active=false;
    if(reply){free(reply->line);reply->line=NULL;}return;
  }
  if(asset_phase_send_current(send,route)&&send){
    if(raw_rejected)send_raw_ready_rejection(send,route,id,path,index,&proof);
    else if(result==PET_APPEARANCE_COMMIT_OK&&reply&&reply->line&&!reply->invalid)send(reply->line,route);
    else send_asset_ack_indexed(send,route,id,phase,path,index,false,
      result==PET_APPEARANCE_COMMIT_UNKNOWN?"appearance phase outcome unknown; read-only reconciliation required; do not replay":"appearance phase rejected; no successful result");
  }
  if(reply){free(reply->line);reply->line=NULL;}
  io->end(io->context);
  if(reply_pinned&&g_appearance_session.generation==reply_generation
      &&pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL))
    g_appearance_session.operation_active=false;
}
static void serial_retire_locked(pet_p4_runtime_state_t *state){
  /* Physical close/free/reset was already completed outside the state lock. */
  (void)pet_p4_appearance_end(g_asset_sender,g_asset_route,g_asset_transfer_id,PET_APPEARANCE_SERIAL);
  g_asset_sender=NULL;g_asset_route=NULL;g_asset_transfer_slot=-1;g_asset_transfer_id[0]=0;
  g_asset_transfer_last_activity_ms=0;state->asset_transfer_active=false;
}
#include "pet_p4_protocol_audio_patch.inc"

static void handle_asset_topic(pet_p4_runtime_state_t *state,const char *topic,
    const cJSON *payload,pet_p4_send_line_fn send,void *route){
  if(!strncmp(topic,"asset/audio-",12)){handle_audio_patch_topic(state,topic,payload,send,route);return;}
  if(audio_patch_protocol_active()&&strcmp(topic,"asset/slot-query")){
    const pet_appearance_commit_io_t locked_io={g_asset_query_io_begin,g_asset_query_io_end,NULL,g_asset_query_io_context};
    asset_phase_deliver(state,&locked_io,send,route,NULL,json_string(payload,"transferId"),topic+6,NULL,NULL,PET_APPEARANCE_COMMIT_FAILED);return;
  }
  const char *id=json_string(payload,"transferId"),*path=json_string(payload,"path");
  const char *index=json_string(payload,"index");
  const pet_appearance_commit_io_t io={g_asset_query_io_begin,g_asset_query_io_end,NULL,g_asset_query_io_context};
  bool begin=!strcmp(topic,"asset/begin"),stat=!strcmp(topic,"asset/stat");
  bool abort=!strcmp(topic,"asset/abort"),raw=!strcmp(topic,"asset/raw-chunk");
  const char *phase=raw?"raw-ready":topic+6;
  if(!strcmp(topic,"asset/slot-query")){send_asset_slot_query_ack(state,payload,send,route,id);return;}
  if(!strcmp(topic,"asset/activate")){handle_asset_activate(state,payload,send,route);return;}
  if(!strcmp(topic,"asset/commit")||!strcmp(topic,"asset/patch-commit")){
    if(pet_p4_appearance_mutation_blocked()||!asset_phase_send_current(send,route)
        ||!pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL)||g_raw_asset_chunk.active){
      asset_phase_deliver(state,&io,send,route,NULL,id,!strcmp(topic,"asset/patch-commit")?"patch":"commit",path,index,PET_APPEARANCE_COMMIT_FAILED);return;
    }
    handle_serial_asset_commit(state,send,route,id,!strcmp(topic,"asset/patch-commit"));return;
  }
  bool chunk=!strcmp(topic,"asset/chunk"),file=!strcmp(topic,"asset/file");
  if(!state||!asset_phase_send_current(send,route)||pet_p4_appearance_mutation_blocked()
      ||!pet_appearance_id_valid(id)||(!begin&&!stat&&!abort&&!raw&&!chunk&&!file)
      ||(begin&&pet_p4_appearance_active())
      ||(stat&&pet_p4_appearance_active()&&!pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL))
      ||(!begin&&!stat&&!pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL))
      ||(g_raw_asset_chunk.active&&!abort)){
    asset_phase_deliver(state,&io,send,route,NULL,id,phase,path,index,PET_APPEARANCE_COMMIT_FAILED);return;
  }
  uint32_t total=0,raw_size=0;
  if(begin){
    const cJSON *t=cJSON_GetObjectItemCaseSensitive(payload,"totalBytes"),*r=cJSON_GetObjectItemCaseSensitive(payload,"rawBytes");
    if(pet_p4_diagnostics_reboot_pending()||pet_p4_ota_transfer_active()
        ||!cJSON_IsNumber(t)||!pet_appearance_size(t->valuedouble,&total)||!total
        ||(r&&(!cJSON_IsNumber(r)||!pet_appearance_size(r->valuedouble,&raw_size)))){
      asset_phase_deliver(state,&io,send,route,NULL,id,phase,path,index,PET_APPEARANCE_COMMIT_FAILED);return;
    }
    if(!r)raw_size=total;
  }
  bool temporary=stat&&!pet_p4_appearance_active();
  if((begin||temporary)&&!pet_p4_appearance_claim(send,route,id,PET_APPEARANCE_SERIAL)){
    asset_phase_deliver(state,&io,send,route,NULL,id,phase,path,index,PET_APPEARANCE_COMMIT_FAILED);return;
  }
  bool previous=state->asset_transfer_active;
  if(begin){g_asset_sender=send;g_asset_route=route;g_asset_transfer_slot=PET_P4_RAW_APPEARANCE_SLOT;copy_text(g_asset_transfer_id,sizeof(g_asset_transfer_id),id);}
  int slot=stat?g_asset_transfer_slot:PET_P4_RAW_APPEARANCE_SLOT;
  state->asset_transfer_active=true; /* suspension stays asserted for native/RX too */
  pet_appearance_phase_t op={0};
  uint64_t phase_generation=g_appearance_session.generation;
  unsigned phase_revision=state->asset_revision;
  int phase_active_slot=g_asset_active_slot;
  bool phase_started=raw
    ?serial_raw_phase_begin_wait(&op,state,send,route,id,slot,&io)
    :pet_p4_appearance_phase_begin(&op,state,send,route,id,PET_APPEARANCE_SERIAL,slot,&io);
  if(!phase_started){
    if(begin)serial_retire_locked(state);
    else if(temporary)(void)pet_p4_appearance_end(send,route,id,PET_APPEARANCE_SERIAL);
    bool original_current=g_appearance_session.generation==phase_generation
      &&state->asset_revision==phase_revision&&g_asset_active_slot==phase_active_slot
      &&pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL)
      &&asset_phase_send_current(send,route);
    if(!raw||original_current)state->asset_transfer_active=previous;
    /* Do not certify a newer owner, even if it reused the same transferId. */
    asset_phase_deliver(state,&io,send,route,NULL,id,phase,path,index,
      raw&&!original_current?PET_APPEARANCE_COMMIT_UNKNOWN:PET_APPEARANCE_COMMIT_FAILED);return;
  }
  asset_phase_reply_t reply={0};g_asset_phase_write_attempted=false;
  bool raw_was_clean=raw&&!g_raw_asset_chunk.active&&!g_raw_asset_chunk.data
    &&!g_raw_asset_chunk.expected_size&&!g_raw_asset_chunk.received_size;
  unsigned char *new_raw_buffer=NULL;
  /* Only this pinned physical actor touches serial tracking while unlocked. */
  if(begin){
    bool ok=!pet_p4_raw_assets_available()||raw_size<=pet_p4_raw_assets_capacity_bytes();
    if(ok){
      reset_raw_asset_chunk();ok=close_asset_chunk_file();
      if(ok){reset_asset_chunk_tracking();g_asset_phase_write_attempted=true;
        ok=clear_slot_files(slot);
        if(ok&&pet_p4_raw_assets_available())ok=pet_p4_raw_assets_prepare(raw_size,g_asset_chunk_error,sizeof(g_asset_chunk_error));
        if(ok)ok=run_spiffs_gc_yielding()==ESP_OK;
      }
    }
    send_asset_ack(asset_phase_capture,&reply,id,"begin",NULL,ok,ok?NULL:"appearance preparation failed");
  }else if(abort){
    bool ok=abort_serial_asset_transfer(NULL);
    send_asset_ack(asset_phase_capture,&reply,id,"abort",NULL,ok,ok?NULL:"appearance cleanup failed");
  }else if(chunk){
    bool ok=handle_asset_chunk(payload);
    send_asset_ack_indexed(asset_phase_capture,&reply,id,"chunk",path,index,ok,ok?NULL:g_asset_chunk_error);
  }else if(raw){
    bool prepared=begin_raw_asset_chunk(NULL,payload,asset_phase_capture,&reply,&reply.error_code);
    if(prepared&&raw_was_clean)new_raw_buffer=g_raw_asset_chunk.data;
  }
  else if(file)handle_asset_file(payload,asset_phase_capture,&reply);
  else if(stat)handle_asset_stat(payload,asset_phase_capture,&reply);
  bool ok=reply.seen&&reply.ok&&!reply.invalid;
  pet_appearance_commit_result_t result=raw
    ?finish_unpublished_raw_header(&op,state,slot,payload,new_raw_buffer,&reply,g_asset_phase_write_attempted)
    :pet_p4_appearance_phase_end(&op,state,slot,ok,g_asset_phase_write_attempted);
  if(result==PET_APPEARANCE_COMMIT_OK){
    if(abort)serial_retire_locked(state);
    else if(stat){state->asset_transfer_active=previous;if(temporary)(void)pet_p4_appearance_end(send,route,id,PET_APPEARANCE_SERIAL);}
    else note_asset_transfer_activity();
  }else if(result==PET_APPEARANCE_COMMIT_FAILED&&begin)serial_retire_locked(state);
  else if(temporary&&result!=PET_APPEARANCE_COMMIT_UNKNOWN){(void)pet_p4_appearance_end(send,route,id,PET_APPEARANCE_SERIAL);state->asset_transfer_active=previous;}
  asset_phase_deliver(state,&io,send,route,&reply,id,phase,path,index,result);
}
void pet_p4_asset_transfer_cleanup_worker(pet_p4_runtime_state_t *state){
  if(!atomic_exchange_explicit(&g_asset_cleanup_due,false,memory_order_acq_rel))return;
  if(audio_patch_protocol_active()){audio_patch_timeout_worker(state);return;}
  unsigned long long now=(unsigned long long)esp_timer_get_time()/1000ULL;
  if(!state||pet_p4_appearance_mutation_blocked()||!state->asset_transfer_active
      ||!g_asset_transfer_last_activity_ms||now<g_asset_transfer_last_activity_ms
      ||now-g_asset_transfer_last_activity_ms<PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS)return;
  const pet_appearance_commit_io_t io={g_asset_query_io_begin,g_asset_query_io_end,NULL,g_asset_query_io_context};
  pet_appearance_phase_t op={0};int slot=g_asset_transfer_slot;
  if(!pet_p4_appearance_phase_begin(&op,state,g_asset_sender,g_asset_route,g_asset_transfer_id,PET_APPEARANCE_SERIAL,slot,&io))return;
  g_asset_phase_write_attempted=false;bool ok=abort_serial_asset_transfer(NULL);
  pet_appearance_commit_result_t result=pet_p4_appearance_phase_end(&op,state,slot,ok,g_asset_phase_write_attempted);
  if(result==PET_APPEARANCE_COMMIT_OK)serial_retire_locked(state);
}
size_t pet_p4_consume_raw_asset_bytes_scoped(pet_p4_runtime_state_t *state,
    const unsigned char *data,size_t len,pet_p4_send_line_fn send,void *route,
    const pet_appearance_commit_io_t *io){
  if(!state||!data||!len||!pet_p4_raw_asset_chunk_active_for_route(send,route))return 0;
  size_t remaining=g_raw_asset_chunk.expected_size-g_raw_asset_chunk.received_size;
  size_t used=len<remaining?len:remaining;
  /* The RX task remains the owner; metadata-only short copy for partial reads.
   * Terminal FNV and driver IO borrow THIS RX mutex, never protocol task flags. */
  if(used<remaining){memcpy(g_raw_asset_chunk.data+g_raw_asset_chunk.received_size,data,used);g_raw_asset_chunk.received_size+=used;note_asset_transfer_activity();return used;}
  int slot=g_asset_transfer_slot;pet_appearance_phase_t op={0};
  if(!pet_p4_appearance_phase_begin(&op,state,send,route,g_raw_asset_chunk.transfer_id,PET_APPEARANCE_SERIAL,slot,io)){
    /* A granted raw-ready frame must be drained exactly once even when another
     * storage actor wins admission. Returning zero with raw still active would
     * spin the RX loop and implicitly retry this same terminal chunk. No FNV or
     * driver write is attempted. Pin only its private RAM during free/NACK. */
    char id[64],path[PET_P4_LOGICAL_PATH_MAX],index[24];
    copy_text(id,sizeof(id),g_raw_asset_chunk.transfer_id);copy_text(path,sizeof(path),g_raw_asset_chunk.logical_path);copy_text(index,sizeof(index),g_raw_asset_chunk.index);
    uint64_t generation=g_appearance_session.generation;
    g_raw_asset_chunk.received_size=g_raw_asset_chunk.expected_size;
    g_appearance_session.operation_active=true;
    if(io&&io->begin&&io->end&&(!io->current||io->current(io->context))&&io->begin(io->context)){
      reset_raw_asset_chunk(); /* RAM only; does not borrow another actor's storage lease */
      send_asset_ack_indexed(send,route,id,"raw-chunk",path,index,false,"raw terminal storage admission failed before write; not retried");
      io->end(io->context);
      if(g_appearance_session.generation!=generation||!pet_p4_appearance_owned(send,route,id,PET_APPEARANCE_SERIAL)
          ||(io->current&&!io->current(io->context)))g_appearance_session.recovery_blocked=true;
    }else g_appearance_session.recovery_blocked=true;
    if(g_appearance_session.generation==generation)g_appearance_session.operation_active=false;
    return used;
  }
  memcpy(g_raw_asset_chunk.data+g_raw_asset_chunk.received_size,data,used);g_raw_asset_chunk.received_size+=used;
  g_asset_phase_write_attempted=false;
  unsigned long long checksum=fnv1a64_bytes(g_raw_asset_chunk.data,g_raw_asset_chunk.expected_size);
  bool ok=checksum==g_raw_asset_chunk.expected_checksum;
  if(ok)ok=write_asset_chunk_bytes(g_raw_asset_chunk.transfer_id,g_raw_asset_chunk.logical_path,g_raw_asset_chunk.index,g_raw_asset_chunk.data,g_raw_asset_chunk.expected_size);
  asset_phase_reply_t reply={0};
  char id[64],path[PET_P4_LOGICAL_PATH_MAX],index[24];
  copy_text(id,sizeof(id),g_raw_asset_chunk.transfer_id);copy_text(path,sizeof(path),g_raw_asset_chunk.logical_path);copy_text(index,sizeof(index),g_raw_asset_chunk.index);
  send_asset_ack_indexed(asset_phase_capture,&reply,id,"raw-chunk",path,index,ok,ok?NULL:"raw chunk verification/write failed");
  bool attempted=g_asset_phase_write_attempted;
  /* Failed physical writes retain exact staging, including raw bytes. */
  if(ok||!attempted)reset_raw_asset_chunk();
  pet_appearance_commit_result_t result=pet_p4_appearance_phase_end(&op,state,slot,ok&&!reply.invalid,attempted);
  if(result==PET_APPEARANCE_COMMIT_OK)note_asset_transfer_activity();
  asset_phase_deliver(state,io,send,route,&reply,id,"raw-chunk",path,index,result);return used;
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

void pet_p4_state_clear_approval(pet_p4_runtime_state_t *state) {
  if (!state) return;
  state->approval_active = false;
  state->approval_request_id[0] = '\0';
  state->approval_agent[0] = '\0';
  state->approval_tool[0] = '\0';
  state->approval_reason[0] = '\0';
  state->approval_expires_ms = 0;
}

bool pet_p4_state_approval_pending(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
) {
  if (!state || !state->approval_active || !state->approval_request_id[0]) return false;
  /* A zero deadline is treated as "no deadline" so an older/odd request cannot
   * make the modal invisible; normal paths always set a bound deadline. */
  return state->approval_expires_ms == 0 || now_ms < state->approval_expires_ms;
}

bool pet_p4_state_begin_approval(
  pet_p4_runtime_state_t *state,
  const char *request_id,
  const char *agent,
  const char *tool,
  const char *reason,
  unsigned long long expires_ms
) {
  if (!state || !request_id || !request_id[0]) return false;
  /* A new request always replaces the pending one: the device only ever shows
   * one modal, and the previous request can no longer be answered locally. */
  copy_text(state->approval_request_id, sizeof(state->approval_request_id), request_id);
  copy_text(state->approval_agent, sizeof(state->approval_agent), agent);
  copy_text(state->approval_tool, sizeof(state->approval_tool), tool);
  copy_text(state->approval_reason, sizeof(state->approval_reason), reason);
  state->approval_expires_ms = expires_ms;
  state->approval_active = true;
  return true;
}

bool pet_p4_state_decide_approval(
  pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  char *request_id_out,
  size_t request_id_out_size
) {
  if (!state || !request_id_out || request_id_out_size == 0) return false;
  if (!pet_p4_state_approval_pending(state, now_ms)) {
    /* Never emit a late decision for an expired modal; drop it fail-closed. */
    pet_p4_state_clear_approval(state);
    return false;
  }
  copy_text(request_id_out, request_id_out_size, state->approval_request_id);
  copy_text(
    state->approval_last_decided_id,
    sizeof(state->approval_last_decided_id),
    state->approval_request_id
  );
  pet_p4_state_clear_approval(state);
  return true;
}

void pet_p4_state_process(pet_p4_runtime_state_t *state, unsigned long long now_ms) {
  if (!state) return;
  if (state->approval_active && state->approval_expires_ms > 0
      && now_ms >= state->approval_expires_ms) {
    /* Timeout is fail-closed: hide the modal but never send a decision. */
    pet_p4_state_clear_approval(state);
    (void) pet_p4_audio_play_event_cue("permission_expired");
    state->last_update_ms += 1;
  }
  if (state->conversation_error_until_ms && now_ms >= state->conversation_error_until_ms) {
    pet_p4_conversation_clear(state);
  }
  if (pet_p4_conversation_active(state) && state->host_last_seen_ms
      && now_ms > state->host_last_seen_ms + PET_P4_HOST_HEARTBEAT_TIMEOUT_MS) {
    (void) pet_p4_audio_conversation_set(false, true, NULL);
    pet_p4_conversation_clear(state);
  }
  pet_p4_conversation_advance(state, pet_p4_audio_stream_played_bytes(),
    pet_p4_audio_stream_playing(), now_ms);
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
  /* 实时对话覆盖动画：听→waiting_user，想→working，说→welcome。 */
  if (state && state->conversation_state[0] && strcmp(state->conversation_state, "ended") != 0) {
    if (!strcmp(state->conversation_state, "listening")) return "notification";
    if (!strcmp(state->conversation_state, "thinking")) return "thinking";
    if (!strcmp(state->conversation_state, "speaking")) return "welcome";
    return "idle";
  }
  const char *lifecycle = runtime_lifecycle(state);
  const char *session_lifecycle = selected_session_lifecycle(state);
  if (session_lifecycle) lifecycle = session_lifecycle;
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
  PET_WIDGET_TRACE_SCOPE(trace_ack, PET_WTRACE_ACK);
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "transferId", transfer_id ? transfer_id : "");
  cJSON_AddStringToObject(payload, "phase", phase ? phase : "");
  const char *path = json_string(request, "path");
  const cJSON *index = cJSON_GetObjectItemCaseSensitive(request, "index");
  if (path[0]) cJSON_AddStringToObject(payload, "path", path);
  if (index) cJSON_AddItemToObject(payload, "index", cJSON_Duplicate(index, true));
  cJSON_AddBoolToObject(payload, "ok", ok);
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  if(ok&&phase&&!strcmp(phase,"commit"))cJSON_AddBoolToObject(payload,"unchanged",pet_p4_miniapp_install_unchanged());
#endif
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
  const size_t selected_index = pet_p4_miniapp_catalog_selected();
  char selected_widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX] = {0};
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
  cJSON_AddNumberToObject(payload, "maxInstalled", pet_p4_miniapp_catalog_capacity());
  cJSON_AddBoolToObject(payload, "complete", !pet_p4_miniapp_catalog_recovery_blocked());
  cJSON_AddBoolToObject(payload, "recoveryBlocked", pet_p4_miniapp_catalog_recovery_blocked());
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
      if (index == selected_index) copy_text(selected_widget_id, sizeof(selected_widget_id), entry.widget_id);
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
      cJSON_AddStringToObject(item, "manifestState", entry.resource_state == 1 ? "valid" : "resources-incomplete");
      cJSON_AddStringToObject(item, "resourceState", entry.resource_state == 1 ? "ready"
        : entry.resource_state == 2 ? "bootstrap" : entry.resource_state == 3 ? "damaged" : "unchecked");
      cJSON_AddNumberToObject(item, "imageCount", entry.image_count);
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
  /* Selection is the visible catalog cursor, independent of the active app.
   * Use the same visible projection as items; an empty catalog has no index. */
  if (selected_widget_id[0]) {
    cJSON_AddStringToObject(payload, "selectedWidgetId", selected_widget_id);
    cJSON_AddNumberToObject(payload, "selectedIndex", (double) selected_index);
  } else {
    cJSON_AddNullToObject(payload, "selectedWidgetId");
    cJSON_AddNullToObject(payload, "selectedIndex");
  }
  cJSON_AddItemToObject(payload, "items", items);
  cJSON_AddItemToObject(payload, "warnings", warnings);
  send_topic_payload(send_line, ctx, "widget/inventory", payload);
  return request_id[0] && strlen(request_id) < 96;
}

/* Strict, opt-in metadata administration. Never enters package cleanup or
 * activation. Failed/unknown acknowledgements authorize no replay. */
static bool send_catalog_admin(pet_p4_runtime_state_t *state,const char *topic,
    const cJSON *request,pet_p4_send_line_fn send_line,void *ctx){
  const bool mutate=!strcmp(topic,"widget/catalog_reconcile");
  const char *fields[]={"requestId","expectedBoardDeviceId","expectedBootId","expectedCatalogSeq",
    "expectedCatalogDigest","removeVisibleIds","survivorVisibleOrder","preserveSave"};
  unsigned seen=0;bool valid=cJSON_IsObject(request);
  const cJSON *field;
  cJSON_ArrayForEach(field,request){
    unsigned at=0;while(at<(mutate?8U:1U)&&(!field->string||strcmp(field->string,fields[at])))at++;
    if(at==(mutate?8U:1U)||(seen&(1U<<at)))valid=false;
    else seen|=1U<<at;
  }
  const char *id=json_string(request,"requestId");
  valid=valid&&seen==(mutate?255U:1U)&&id[0]&&strlen(id)<96;
  const char *commit="not_committed";char error[128]={0};
  if(!valid)copy_text(error,sizeof(error),"invalid bounded catalog request");
  else if(mutate&&(!state||!pet_p4_boot_identity()[0]
      ||strcmp(json_string(request,"expectedBoardDeviceId"),state->board_device_id)
      ||strcmp(json_string(request,"expectedBootId"),pet_p4_boot_identity())))
    copy_text(error,sizeof(error),"board or boot identity changed; inspect again");
  else if(mutate)commit=pet_p4_miniapp_catalog_reconcile(request,error,sizeof(error));
  cJSON *response=pet_p4_miniapp_catalog_inspect();
  if(!response)return false;
  if(!cJSON_AddStringToObject(response,"requestId",id)
      ||!cJSON_AddStringToObject(response,"boardDeviceId",state?state->board_device_id:"")
      ||!cJSON_AddStringToObject(response,"bootId",pet_p4_boot_identity())){cJSON_Delete(response);return false;}
  bool ok=mutate?(!strcmp(commit,"committed")||!strcmp(commit,"unchanged")):valid&&!pet_p4_miniapp_catalog_recovery_blocked();
  cJSON_DeleteItemFromObjectCaseSensitive(response,"ok");
  cJSON_AddBoolToObject(response,"ok",ok);
  if(mutate){
    const char *drop[]={"items","complete","recoveryBlocked","physicalCount","activeWidgetId","selectedWidgetId"};
    for(size_t i=0;i<sizeof(drop)/sizeof(drop[0]);i++)cJSON_DeleteItemFromObjectCaseSensitive(response,drop[i]);
    cJSON_AddStringToObject(response,"commitState",commit);
    if(ok&&state&&!strcmp(commit,"committed"))state->last_update_ms+=1;
  }
  if(error[0])cJSON_AddStringToObject(response,"error",error);
  send_topic_payload(send_line,ctx,mutate?"widget/catalog_reconcile_result":"widget/catalog_snapshot",response);
  return ok;
}

static void send_miniapp_state(pet_p4_send_line_fn send_line, void *ctx, const cJSON *request) {
  pet_p4_miniapp_view_t snapshot = {0};
  pet_p4_miniapp_observation_t observation;
  bool available = pet_p4_miniapp_observe(&observation,&snapshot);
  const pet_p4_miniapp_view_t *view = available ? &snapshot : NULL;
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload,"requestId",json_string(request,"requestId"));
  cJSON_AddStringToObject(payload,"bootId",pet_p4_boot_identity());
  cJSON_AddStringToObject(payload,"widgetId",available&&snapshot.active?snapshot.widget_id:"");
  bool coherent=available;
  cJSON_AddBoolToObject(payload,"observationAvailable",coherent);
  if(coherent){
    char generation[9],persist[9],cells[9],vm[9];
    snprintf(generation,sizeof(generation),"%08x",(unsigned)observation.instance_generation);
    snprintf(persist,sizeof(persist),"%08x",(unsigned)observation.persist_digest);
    snprintf(cells,sizeof(cells),"%08x",(unsigned)observation.cells_digest);
    snprintf(vm,sizeof(vm),"%08x",(unsigned)observation.vm_digest);
    cJSON_AddStringToObject(payload,"instanceGeneration",generation);
    cJSON_AddStringToObject(payload,"observationEngine","p4-miniapp-observation-fnv1a32-v1");
    cJSON_AddStringToObject(payload,"persistProjectionDigest",persist);
    cJSON_AddStringToObject(payload,"durability","not_observed");
    if(observation.has_cells){
      cJSON *grid=cJSON_AddObjectToObject(payload,"cellsDigest");
      cJSON_AddNumberToObject(grid,"cols",observation.cols);cJSON_AddNumberToObject(grid,"rows",observation.rows);
      cJSON_AddNumberToObject(grid,"bits",observation.bits);cJSON_AddNumberToObject(grid,"count",observation.cells_count);
      cJSON_AddStringToObject(grid,"hash",cells);
    }
    if(observation.has_vm){
      cJSON *program=cJSON_AddObjectToObject(payload,"vmDigest");
      cJSON_AddNumberToObject(program,"schema",observation.vm_schema);cJSON_AddNumberToObject(program,"cells",observation.vm_cells);
      cJSON_AddNumberToObject(program,"revision",observation.vm_revision);cJSON_AddStringToObject(program,"hash",vm);
    }
  }
  bool active = view && view->active;
  cJSON_AddBoolToObject(payload, "active", active);
  if (active) {
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
    pet_p4_miniapp_program_status_t program_status;
    if(pet_p4_miniapp_program_status(&program_status)){
      cJSON *program=cJSON_AddObjectToObject(payload,"boundedProgram");
      cJSON_AddStringToObject(program,"engine",PET_COMPONENT_PROGRAM_ENGINE);
      cJSON_AddNumberToObject(program,"phase",program_status.phase);cJSON_AddBoolToObject(program,"live",program_status.live);
      cJSON_AddBoolToObject(program,"retiring",program_status.retiring);cJSON_AddBoolToObject(program,"reportValid",program_status.report_valid);
      cJSON_AddNumberToObject(program,"revision",program_status.revision);cJSON_AddNumberToObject(program,"result",program_status.result);
      cJSON_AddNumberToObject(program,"usedFuel",program_status.used_fuel);cJSON_AddNumberToObject(program,"lastStepUs",program_status.last_step_us);
      cJSON_AddNumberToObject(program,"maxStepUs",program_status.max_step_us);
      cJSON_AddNumberToObject(program,"sliceBudgetUs",program_status.slice_budget_us);cJSON_AddBoolToObject(program,"sliceStalled",program_status.slice_stalled);
    }
    pet_p4_miniapp_side_status_t side;
    if(pet_p4_miniapp_side_status(&side)){
      cJSON *game=cJSON_AddObjectToObject(payload,"sideScene");
      cJSON_AddStringToObject(game,"engine","p4-side-scene-v1");
      cJSON_AddNumberToObject(game,"phase",side.phase);
      cJSON_AddNumberToObject(game,"x",side.x/256.0);cJSON_AddNumberToObject(game,"y",side.y/256.0);
      cJSON_AddNumberToObject(game,"vx",side.vx/256.0);cJSON_AddNumberToObject(game,"vy",side.vy/256.0);
      cJSON_AddNumberToObject(game,"direction",side.direction);
      cJSON_AddBoolToObject(game,"grounded",side.grounded);cJSON_AddBoolToObject(game,"jumpDown",side.jump_down);
      cJSON_AddNumberToObject(game,"score",side.score);cJSON_AddNumberToObject(game,"coins",side.coins);
      cJSON_AddNumberToObject(game,"lives",side.lives);cJSON_AddNumberToObject(game,"checkpoint",side.checkpoint);
      cJSON_AddNumberToObject(game,"elapsedMs",side.elapsed_ms);cJSON_AddNumberToObject(game,"revision",side.revision);
      cJSON_AddNumberToObject(game,"lastStepUs",side.last_step_us);cJSON_AddNumberToObject(game,"maxStepUs",side.max_step_us);
    }
  }
  send_topic_payload(send_line, ctx, "miniapp/state", payload);
}

bool pet_p4_appearance_mutation_blocked(void) {
  return g_appearance_session.operation_active||g_appearance_session.recovery_blocked;
}
static bool commit_owner_current(pet_p4_runtime_state_t *state,int old_slot,
    unsigned revision,uint64_t generation,pet_p4_send_line_fn send,void *route,
    const char *id,unsigned kind,pet_resource_lease_t lease,
    const pet_appearance_commit_io_t *io) {
  return g_appearance_session.operation_active&&!g_appearance_session.recovery_blocked
    &&g_appearance_session.generation==generation
    &&pet_p4_appearance_owned(send,route,id,kind)
    &&(kind!=PET_APPEARANCE_SERIAL||asset_phase_send_current(send,route))
    &&state->asset_revision==revision&&g_asset_active_slot==old_slot
    &&pet_resources_device_current(lease)&&(!io->current||io->current(io->context));
}
static bool write_ready_pack_exact(int slot,const char *pack,bool *changed) {
  char path[PET_P4_FS_PATH_MAX],actual[PET_P4_PACK_ID_MAX];
  if(!p4_asset_ready_marker_path(slot,path,sizeof(path)))return false;
  *changed=true; /* fopen(wb) itself may truncate before reporting failure. */
  FILE *file=fopen(path,"wb");if(!file)return false;
  bool ok=fprintf(file,"%s\n",pack)==PET_P4_PACK_ID_MAX;
  if(fclose(file)!=0)ok=false;
  /* Never remove a possibly durable ready marker on a close/readback error. */
  return ok&&read_ready_slot_pack_id(slot,actual,sizeof(actual))&&!strcmp(actual,pack);
}
pet_appearance_commit_result_t pet_p4_appearance_commit_slot(
    pet_p4_runtime_state_t *state,int slot,pet_p4_send_line_fn send,void *route,
    const char *id,unsigned kind,bool patch,const pet_appearance_commit_io_t *io,
    char *error,size_t capacity) {
  const char *why="appearance commit unavailable";
  pet_appearance_commit_result_t result=PET_APPEARANCE_COMMIT_FAILED;
  pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
  asset_activation_snapshot_t *next=NULL;
  bool unlocked=false,changed=false,raw_changed=false;
  if(!state||!slot_is_valid(slot)||!io||!io->begin||!io->end
      ||pet_p4_appearance_mutation_blocked()
      ||!pet_p4_appearance_owned(send,route,id,kind)
      ||(kind==PET_APPEARANCE_SERIAL&&!asset_phase_send_current(send,route)))goto reject;
  if(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&owner)!=PET_RESOURCE_OK
      ||pet_resources_device_acquire(owner,PET_RESOURCE_STORAGE_WORK,1,&lease)!=PET_RESOURCE_OK) {
    why="appearance storage owner unavailable";goto reject;
  }
  const uint64_t generation=g_appearance_session.generation;
  const unsigned revision=state->asset_revision;const int old_slot=g_asset_active_slot;
  g_appearance_session.operation_active=true;
  if(!io->begin(io->context))goto finish;
  unlocked=true;
  if(kind==PET_APPEARANCE_SERIAL&&!asset_phase_send_current(send,route)) {
    why="appearance route retired before commit IO";goto relock;
  }
  /* A close error can follow physical writes. Do not publish or erase staging. */
  if(kind==PET_APPEARANCE_SERIAL&&!close_asset_chunk_file()) {
    changed=true;why="appearance staged close outcome unknown; do not replay";goto relock;
  }
  if(!patch) {
    next=calloc(1,sizeof(*next));
    if(!next){why="appearance commit workspace unavailable";goto relock;}
    if(!prepare_asset_snapshot(slot,NULL,next)) {
      why="appearance manifest verification failed";goto relock;
    }
  }
  io->end(io->context);unlocked=false;
  if(!commit_owner_current(state,old_slot,revision,generation,send,route,id,kind,lease,io)) {
    why="appearance commit ownership changed";goto finish;
  }
  if(patch){result=PET_APPEARANCE_COMMIT_OK;goto finish;}
  if(!io->begin(io->context))goto finish;
  unlocked=true;
  if(kind==PET_APPEARANCE_SERIAL&&!asset_phase_send_current(send,route)) {
    why="appearance route retired before ready IO";goto relock;
  }
  if(slot==PET_P4_RAW_APPEARANCE_SLOT&&pet_p4_raw_assets_available()) {
    char raw_pack[PET_P4_PACK_ID_MAX];
    bool present=pet_p4_raw_assets_committed_pack_id_for_slot(slot,raw_pack,sizeof(raw_pack))
      &&!strcmp(raw_pack,next->pack_id);
    if(!present&&!pet_p4_raw_assets_commit_observed(next->pack_id,error,capacity,&raw_changed)) {
      changed|=raw_changed;why="appearance raw header commit failed";goto relock;
    }
    changed|=raw_changed;
  }
  if(kind==PET_APPEARANCE_SERIAL&&!asset_phase_send_current(send,route)) {
    why="appearance route retired after raw commit";goto relock;
  }
  if(!write_ready_pack_exact(slot,next->pack_id,&changed)) {
    why="appearance ready marker outcome unknown; do not replay";goto relock;
  }
  io->end(io->context);unlocked=false;
  if(!commit_owner_current(state,old_slot,revision,generation,send,route,id,kind,lease,io)) {
    why="appearance commit publication ownership changed";goto finish;
  }
  if(!io->begin(io->context))goto finish;
  unlocked=true;
  if(kind==PET_APPEARANCE_SERIAL&&!asset_phase_send_current(send,route)) {
    why="appearance route retired before activation marker";goto relock;
  }
  changed=true;
  if(!write_asset_activation_marker(slot)) {
    why="appearance activation marker outcome unknown; do not replay";goto relock;
  }
  io->end(io->context);unlocked=false;
  if(!commit_owner_current(state,old_slot,revision,generation,send,route,id,kind,lease,io)) {
    why="appearance commit publication outcome unknown; do not replay";goto finish;
  }
  /* Only a bounded staged RAM snapshot is published under the state lock. */
  g_asset_active_slot=slot;
  memcpy(state->asset_manifest_json,next->manifest,strlen(next->manifest)+1);
  state->asset_catalog=next->catalog;state->asset_family_count=next->catalog.count;
  state->asset_revision++;
  result=PET_APPEARANCE_COMMIT_OK;
  goto finish;
relock:
  if(unlocked){io->end(io->context);unlocked=false;}
finish:
  if(result!=PET_APPEARANCE_COMMIT_OK&&changed) {
    result=PET_APPEARANCE_COMMIT_UNKNOWN;
    g_appearance_session.recovery_blocked=true;
    why="appearance commit outcome unknown; read-only reconciliation required; do not replay";
  }
  /* Free in the borrowed IO window while owner and operation remain pinned. */
  if(next) {
    if(io->begin(io->context)){free(next);next=NULL;io->end(io->context);}
    else { /* Broken dispatcher: keep ownership and do not claim completion. */
      g_appearance_session.recovery_blocked=true;
      result=PET_APPEARANCE_COMMIT_UNKNOWN;why="appearance commit dispatcher unavailable; do not replay";
    }
  }
  if(g_appearance_session.generation!=generation
      ||!pet_p4_appearance_owned(send,route,id,kind)
      ||(kind==PET_APPEARANCE_SERIAL&&!asset_phase_send_current(send,route))
      ||(io->current&&!io->current(io->context))) {
    g_appearance_session.recovery_blocked=true;
    result=PET_APPEARANCE_COMMIT_UNKNOWN;why="appearance commit owner/connection changed; do not replay";
  }
  if(g_appearance_session.generation==generation)g_appearance_session.operation_active=false;
reject:
  (void)pet_resources_device_finish(lease);pet_resources_device_revoke(owner);
  if(result!=PET_APPEARANCE_COMMIT_OK)copy_text(error,capacity,why);
  return result;
}

/* Cold capability construction only; do not change live audio dispatch flags. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("Os")))
#endif
void pet_p4_send_hello_for_board(const char *board_id, pet_p4_send_line_fn send_line, void *ctx) {
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
  cJSON_AddStringToObject(payload, "boardDeviceId", board_id ? board_id : "p4-unknown");
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
  cJSON_AddStringToObject(capabilities,"appearanceTransaction","p4-appearance-owner-v1");
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
  cJSON_AddBoolToObject(capabilities, "widgetDeletePreserveSave", true);
  cJSON_AddBoolToObject(capabilities, "widgetInventory", true);
  cJSON_AddBoolToObject(capabilities, "componentCatalogGeneration", true);
  cJSON_AddStringToObject(capabilities, "widgetRuntime", "p4-bounded-runtime-v4");
  cJSON_AddStringToObject(capabilities, "widgetResourceIntegrity", "p4-resource-integrity-v1");
  cJSON_AddItemToArray(widget_runtimes, cJSON_CreateString("p4-bounded-runtime-v3"));
  cJSON_AddItemToArray(widget_runtimes, cJSON_CreateString("p4-bounded-runtime-v4"));
  cJSON_AddItemToObject(capabilities, "widgetRuntimes", widget_runtimes);
  cJSON_AddStringToObject(capabilities, "widgetScene", "p4-grid-scene-v2");
  cJSON_AddItemToArray(widget_scenes, cJSON_CreateString("p4-grid-scene-v1"));
  cJSON_AddItemToArray(widget_scenes, cJSON_CreateString("p4-grid-scene-v2"));
  cJSON_AddItemToArray(widget_scenes, cJSON_CreateString("p4-touch-slice-v1"));
  cJSON_AddItemToArray(widget_scenes, cJSON_CreateString("p4-side-scene-v1"));
  cJSON_AddStringToObject(capabilities,"widgetPointer","p4-pointer-v1");
  cJSON_AddNumberToObject(capabilities,"widgetSliceRevision",2);
  cJSON_AddItemToObject(capabilities, "widgetScenes", widget_scenes);
  cJSON_AddBoolToObject(capabilities, "widgetSprites", true);
  cJSON_AddStringToObject(capabilities, "widgetData", "p4-data-list-v1");
  cJSON_AddStringToObject(capabilities, "widgetDataExpiry", "p4-data-expiry-v1");
  cJSON_AddNumberToObject(capabilities, "widgetDataSources", 8);
  cJSON_AddStringToObject(capabilities, "widgetMedia", "p4-media-v1");
  cJSON_AddStringToObject(capabilities, "widgetMediaSurface", "p4-media-surface-v1");
  cJSON_AddNumberToObject(capabilities, "widgetMediaQueueMax", 128);
  cJSON_AddStringToObject(capabilities, "widgetMediaLifecycle", "p4-media-lifecycle-v1");
  cJSON_AddStringToObject(capabilities, "widgetSurfaceImages", "p4-surface-images-v1");
  cJSON_AddStringToObject(capabilities, "widgetAppSurface", "p4-app-surface-v1");
  cJSON_AddStringToObject(capabilities, "widgetAppGraphics", "p4-app-graphics-v1");
  cJSON_AddStringToObject(capabilities, "widgetAppTypography", "p4-app-type-v1");
  cJSON_AddStringToObject(capabilities, "widgetGlyphCells", "p4-glyph-cells-v1");
  cJSON_AddStringToObject(capabilities, "widgetAppInteraction", "p4-app-interaction-v1");
  cJSON_AddStringToObject(capabilities, "widgetGameSurface", "p4-game-surface-v1");
  cJSON_AddStringToObject(capabilities, "widgetDataSurface", "p4-data-surface-v1");
  cJSON_AddStringToObject(capabilities, "widgetSceneSurface", "p4-scene-surface-v1");
  cJSON_AddStringToObject(capabilities, "widgetFlappyTuning", "p4-flappy-tuning-v1");
  cJSON_AddStringToObject(capabilities, "widgetLifecycle", "p4-lifecycle-v1");
  cJSON_AddStringToObject(capabilities, "widgetInput", "p4-input-v1");
  cJSON_AddStringToObject(capabilities, "widgetLocalTools", "p4-local-tools-v1");
  cJSON_AddStringToObject(capabilities, "widgetLocalToolsMasks", "p4-local-masks-v2");
  cJSON_AddStringToObject(capabilities, "widgetLogic", "p4-logic-v1");
#if PET_P4_QUOTA_V3
  /* v3 supersedes v2 in the single-valued key; v2 packages stay accepted and are listed in widgetQuotas. */
  cJSON_AddStringToObject(capabilities, "widgetQuota", "p4-quota-v3");
  {cJSON *quotas=cJSON_CreateArray();cJSON_AddItemToArray(quotas,cJSON_CreateString("p4-quota-v2"));cJSON_AddItemToArray(quotas,cJSON_CreateString("p4-quota-v3"));cJSON_AddItemToObject(capabilities,"widgetQuotas",quotas);}
#else
  cJSON_AddStringToObject(capabilities, "widgetQuota", "p4-quota-v2");
#endif
#if PET_P4_CELLS_V1
  cJSON_AddStringToObject(capabilities, "widgetCells", PET_CELLS_ENGINE);
#endif
#if PET_P4_TEXT_TABLE
  cJSON_AddStringToObject(capabilities, "widgetTexts", "p4-texts-v1");
#endif
  cJSON_AddStringToObject(capabilities, "widgetSfx", "p4-sfx-v1");
#if PET_P4_SEQUENCE
  cJSON_AddStringToObject(capabilities, "widgetSequence", "p4-sequence-v1");
#endif
#if PET_P4_SAMPLES
  cJSON_AddStringToObject(capabilities, "widgetSamples", "p4-samples-v1");
#endif
#if PET_P4_CLIP
  cJSON_AddStringToObject(capabilities, "widgetClip", "p4-clip-v1");
#endif
#if PET_P4_BIG_IMAGE
  cJSON_AddStringToObject(capabilities, "widgetBigImage", "p4-big-image-v1");
#endif
#if PET_P4_EVENTS
  cJSON_AddStringToObject(capabilities, "widgetEvents", "p4-events-v1");
#endif
  cJSON_AddStringToObject(capabilities, "widgetMotion", "p4-motion-v1");
  cJSON_AddStringToObject(capabilities, "widgetPlatformTrack", "p4-platform-track-v1");
  cJSON_AddStringToObject(capabilities, "widgetSideScene", "p4-side-scene-v1");
  cJSON_AddNumberToObject(capabilities, "widgetSideSceneMaxActors", PET_SIDE_ACTORS);
  cJSON_AddNumberToObject(capabilities, "widgetSideSceneMaxWidth", PET_SIDE_WIDTH);
  cJSON_AddNumberToObject(capabilities, "widgetSideSceneMaxHeight", PET_SIDE_HEIGHT);
  cJSON_AddStringToObject(capabilities, "widgetMediaTouch", "p4-media-touch-v1");
  cJSON_AddStringToObject(capabilities, "widgetSurfaceAtlas", "p4-wide-atlas-v1");
  cJSON_AddStringToObject(capabilities, "widgetAppHost", "p4-app-host-v1");
  cJSON_AddStringToObject(capabilities, "widgetVariableBindings", "p4-variable-bindings-v1");
#if PET_CANVAS_ENABLED
  cJSON_AddStringToObject(capabilities,"widgetCanvas","p4-canvas-v1");
#endif
#if PET_P4_CANVAS_V2
  cJSON_ReplaceItemInObject(capabilities,"widgetCanvas",cJSON_CreateString("p4-canvas-v2"));
#endif
#if PET_P4_GRAPHICS_V2
  cJSON_AddStringToObject(capabilities,"widgetGraphicsV2","p4-graphics-v2");
#endif
#if PET_P4_PARTICLES
  cJSON_AddStringToObject(capabilities,"widgetParticles","p4-particles-v1");
#endif
#if PET_P4_KEYFRAME_BG
  if(pet_p4_renderer_keyframe_ready())cJSON_AddStringToObject(capabilities,"widgetKeyframeBackground","p4-keyframe-bg-v1");
#endif
#if PET_P4_BODIES
  /* Default-off candidate grant. Physical admission remains source-bound HIL,
   * never inferred from this implementation capability. */
  cJSON_AddStringToObject(capabilities,"widgetBodies","p4-bodies-v1");
#endif
  cJSON_AddStringToObject(capabilities,"widgetHostContext","p4-host-context-v1");
#if PET_SERVICE_BRIDGE_ENABLED
  cJSON_AddStringToObject(capabilities,"widgetServiceBridge","p4-service-bridge-v1");
  cJSON_AddNumberToObject(capabilities,"widgetServiceBridgeImages",2);
  cJSON_AddNumberToObject(capabilities,"widgetServiceBridgeImageSide",192);
#endif
  cJSON_AddStringToObject(capabilities,"widgetAppNavigation","p4-app-navigation-v1");
  cJSON_AddStringToObject(capabilities,"widgetActivation","p4-widget-activation-v1");
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
  cJSON_AddStringToObject(capabilities,"widgetPreserveUnchanged","p4-preserve-unchanged-v1");
#endif
  cJSON_AddStringToObject(capabilities,"widgetCatalogMetadata","p4-catalog-metadata-v1");
  cJSON_AddStringToObject(capabilities,"widgetAppViewport","p4-app-viewport-v1");
#if PET_P4_SERIAL_CONTROL_INTEGRITY
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
  pet_control_session_capabilities(ctx,capabilities);
#else
  cJSON_AddStringToObject(capabilities,"serialControlIntegrity",PET_CONTROL_INTEGRITY_ENGINE);
  cJSON_AddNumberToObject(capabilities,"serialControlMaxBytes",PET_CONTROL_INTEGRITY_MAX);
#endif
#endif
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
  cJSON_AddStringToObject(capabilities,"storageCompletedObservations","p4-storage-completed-v1");
#endif
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  cJSON_AddStringToObject(capabilities,"renderLcdSplit","p4-lcd-split-v1");
#endif
  cJSON_AddStringToObject(capabilities,"widgetAppViewports","p4-app-viewports-v1");
  cJSON_AddStringToObject(capabilities,"widgetStateObservation","p4-miniapp-observation-fnv1a32-v1");
#if PET_HOST_COLLECTION_ENABLED
  cJSON_AddStringToObject(capabilities,"widgetHostCollection",PET_HOST_COLLECTION_ENGINE);
#endif
#if PET_VARIABLE_OWNERS_ENABLED
  cJSON_AddStringToObject(capabilities,"widgetVariableOwners",PET_VARIABLE_OWNERS_ENGINE);
#endif
#if PET_COMPONENT_PROGRAM_ENABLED && PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED
#if PET_P4_PROGRAM_V2
  /* v2 supersedes v1 in the single-valued key; v1 images stay valid and are listed in widgetPrograms. */
  cJSON_AddStringToObject(capabilities,"widgetProgram",PET_COMPONENT_PROGRAM_ENGINE_V2);
  {cJSON *programs=cJSON_CreateArray();cJSON_AddItemToArray(programs,cJSON_CreateString(PET_COMPONENT_PROGRAM_ENGINE));cJSON_AddItemToArray(programs,cJSON_CreateString(PET_COMPONENT_PROGRAM_ENGINE_V2));cJSON_AddItemToObject(capabilities,"widgetPrograms",programs);}
  cJSON_AddNumberToObject(capabilities,"widgetProgramMaxCode",PET_PROGRAM_CODE);
  cJSON_AddNumberToObject(capabilities,"widgetProgramMaxCells",PET_PROGRAM_CELLS);
#else
  cJSON_AddStringToObject(capabilities,"widgetProgram",PET_COMPONENT_PROGRAM_ENGINE);
#endif
  cJSON_AddNumberToObject(capabilities,"widgetProgramMaxFuel",PET_PROGRAM_FUEL);
  cJSON_AddNumberToObject(capabilities,"widgetProgramQuantum",PET_PROGRAM_QUANTUM);
#if PET_COMPONENT_BACKGROUND_ENABLED
  if(pet_p4_miniapp_background_available()){
    cJSON_AddStringToObject(capabilities,"widgetProgramBackground",PET_P4_MINIAPP_BACKGROUND_ENGINE);
    cJSON_AddStringToObject(capabilities,"widgetProgramInspect","p4-program-inspect-v1");
    cJSON_AddNumberToObject(capabilities,"widgetProgramBackgroundMaxActive",pet_resources_limit(PET_RESOURCE_PROGRAM).leases);
#if PET_COMPONENT_PROGRAM_PARENT_UNIFIED
    cJSON_AddStringToObject(capabilities,"widgetProgramParent","p4-program-parent-v1");
    cJSON_AddNumberToObject(capabilities,"widgetProgramAggregateQuantum",64);
    cJSON_AddNumberToObject(capabilities,"widgetProgramAggregateFuel",pet_resources_limit(PET_RESOURCE_PROGRAM).units);
#endif
  }
#endif
#endif
  cJSON_AddStringToObject(capabilities,"widgetAppForm","p4-app-form-v1");
  if(g_boot_identity.ready){
    cJSON_AddStringToObject(capabilities,"debugEvidenceIdentity","p4-evidence-identity-v1");
    cJSON_AddStringToObject(capabilities,"debugBootId",g_boot_identity.id);
  }
#if PET_FORM_V2_ENABLED
  cJSON_AddStringToObject(capabilities,"widgetAppFormV2","p4-app-form-v2");
#endif
  /* Track D app-shell grants: advertised only when the default-off flag is on. */
#if PET_FORM_V3_ENABLED
  cJSON_AddStringToObject(capabilities,"widgetAppFormV3","p4-app-form-v3");
#endif
#if PET_P4_APP_SHELL
  cJSON_AddStringToObject(capabilities,"widgetAppShell","p4-app-shell-v1");
#endif
#if PET_P4_DATA_ROWS
  cJSON_AddStringToObject(capabilities,"widgetDataRows","p4-data-rows-v1");
#endif
#if PET_P4_LONGTEXT
  cJSON_AddStringToObject(capabilities,"widgetLongText","p4-longtext-v1");
#endif
#if PET_P4_NOTIFY
  cJSON_AddStringToObject(capabilities,"widgetNotify","p4-notify-v1");
#endif
  cJSON_AddStringToObject(capabilities, "widgetState", "p4-app-state-v1");
  cJSON_AddBoolToObject(capabilities, "widgetMediaNotice", true);
  cJSON_AddStringToObject(capabilities, "widgetInstrument", "p4-instrument-v1");
  cJSON_AddStringToObject(capabilities, "widgetLyrics", "p4-lrc-v1");
  cJSON_AddNumberToObject(capabilities, "componentCatalogMax", PET_P4_MINIAPP_CATALOG_MAX);
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("blocks"));
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("snake"));
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("flappy"));
  cJSON_AddItemToArray(widget_games, cJSON_CreateString("gomoku"));
  cJSON_AddItemToObject(capabilities, "widgetGames", widget_games);
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("blocks"));
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("snake"));
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("flappy"));
  cJSON_AddItemToArray(widget_game_presets, cJSON_CreateString("gomoku"));
  cJSON_AddItemToObject(capabilities, "widgetGamePresets", widget_game_presets);
  cJSON_AddNumberToObject(widget_limits, "maxVars", 8);
  cJSON_AddNumberToObject(widget_limits, "maxStates", 6);
  cJSON_AddNumberToObject(widget_limits, "maxPages", 4);
  // Preserve the legacy/base limit for clients that do not understand profiles.
  cJSON_AddNumberToObject(widget_limits, "maxTransitions", 12);
  cJSON_AddNumberToObject(widget_limits, "maxInputTransitions", 24);
  cJSON_AddNumberToObject(widget_limits, "maxLegacyTransitions", 12);
  cJSON_AddNumberToObject(widget_limits, "maxSurfaceNodes", 32);
  cJSON_AddNumberToObject(widget_limits, "maxLegacySurfaceNodes", 20);
  cJSON_AddNumberToObject(widget_limits, "maxTicks", 8);
  cJSON_AddNumberToObject(widget_limits, "maxConditions", 2);
  cJSON_AddNumberToObject(widget_limits, "maxSfxCues", 8);
#if PET_P4_CAPABILITY_EXT
  {
    /* Track C budgets, advertised only with their capability strings. */
    cJSON *ext = cJSON_CreateObject();
    cJSON_AddNumberToObject(ext, "sequenceTracks", 2);
    cJSON_AddNumberToObject(ext, "sequenceNotes", 64);
    cJSON_AddNumberToObject(ext, "sampleSlots", 4);
    cJSON_AddNumberToObject(ext, "sampleBytes", 65536);
    cJSON_AddNumberToObject(ext, "sampleBudgetBytes", 262144);
    cJSON_AddNumberToObject(ext, "clipBytes", 524288);
    cJSON_AddNumberToObject(ext, "clipSeconds", 5);
    cJSON_AddNumberToObject(ext, "bigImageTiles", 16);
    cJSON_AddNumberToObject(ext, "bigImageBudgetBytes", 1048576);
    cJSON_AddNumberToObject(ext, "eventNames", 8);
    cJSON_AddNumberToObject(ext, "eventQueue", 8);
    cJSON_AddNumberToObject(ext, "alarms", 4);
    cJSON_AddNumberToObject(ext, "alarmMinMs", 60000);
    cJSON_AddItemToObject(widget_limits, "capabilityExt", ext);
  }
#endif
  cJSON_AddNumberToObject(widget_limits, "maxButtons", 8);
  cJSON_AddNumberToObject(widget_limits, "maxSceneEntities", PET_P4_GAME_MAX_ENTITIES);
  cJSON_AddNumberToObject(widget_limits, "maxSceneRules", PET_P4_GAME_MAX_RULES);
  cJSON_AddNumberToObject(widget_limits, "maxSceneOpsPerRule", PET_P4_GAME_MAX_OPS_PER_RULE);
  /* The base limits stay the v1 contract for every package without graphics:"p4-graphics-v2". */
  cJSON_AddNumberToObject(widget_limits, "maxSceneSprites", PET_P4_MINIAPP_SPRITE_V1_MAX);
  cJSON_AddNumberToObject(widget_limits, "maxSceneSpritePixels", PET_P4_MINIAPP_SPRITE_V1_PIXELS);
  cJSON_AddNumberToObject(widget_limits, "maxWidgetJsonBytes", 8191);
  cJSON_AddNumberToObject(widget_limits, "maxLegacyWidgetJsonBytes", 4095);
  cJSON_AddNumberToObject(widget_limits, "maxButtonsJsonBytes", 2047);
  {
    /* Ceilings for packages that declare quota p4-quota-v2; every value above
     * stays the v1 contract for packages that do not. */
    cJSON *quota = cJSON_CreateObject();
    cJSON_AddStringToObject(quota, "profile", "p4-quota-v2");
    cJSON_AddNumberToObject(quota, "maxVars", 16);
    cJSON_AddNumberToObject(quota, "maxStates", 8);
    cJSON_AddNumberToObject(quota, "maxPages", 6);
    cJSON_AddNumberToObject(quota, "maxTransitions", 32);
    cJSON_AddNumberToObject(quota, "maxTicks", 12);
    cJSON_AddNumberToObject(quota, "maxSwitchChoices", 8);
    cJSON_AddNumberToObject(quota, "maxSurfaceNodes", 48);
    cJSON_AddNumberToObject(quota, "maxLocalItems", 24);
    cJSON_AddNumberToObject(quota, "maxSaveVars", 16);
    cJSON_AddNumberToObject(quota, "maxWidgetJsonBytes", 8191);
    cJSON_AddItemToObject(widget_limits, "v2", quota);
  }
#if PET_P4_QUOTA_V3
  {
    /* Ceilings for packages that declare quota p4-quota-v3. */
    cJSON *quota = cJSON_CreateObject();
    cJSON_AddStringToObject(quota, "profile", "p4-quota-v3");
    cJSON_AddNumberToObject(quota, "maxVars", 32);
    cJSON_AddNumberToObject(quota, "maxStates", 16);
    cJSON_AddNumberToObject(quota, "maxPages", 12);
    cJSON_AddNumberToObject(quota, "maxTransitions", 48);
    cJSON_AddNumberToObject(quota, "maxTicks", 16);
    cJSON_AddNumberToObject(quota, "maxSwitchChoices", 12);
    cJSON_AddNumberToObject(quota, "maxSurfaceNodes", 64);
    cJSON_AddNumberToObject(quota, "maxLocalItems", 64);
    cJSON_AddNumberToObject(quota, "maxSaveVars", 32);
    cJSON_AddNumberToObject(quota, "maxStringVarBytes", 127);
    cJSON_AddNumberToObject(quota, "maxWidgetJsonBytes", 12287);
    cJSON_AddItemToObject(widget_limits, "v3", quota);
  }
#endif
#if PET_P4_CELLS_V1
  {
    cJSON *cells = cJSON_CreateObject();
    cJSON_AddNumberToObject(cells, "maxSide", PET_CELLS_MAX_SIDE);
    cJSON_AddNumberToObject(cells, "maxCells", PET_CELLS_MAX);
    cJSON_AddNumberToObject(cells, "maxOpsPerBranch", 4);
    cJSON_AddItemToObject(widget_limits, "cells", cells);
  }
#endif
#if PET_P4_TEXT_TABLE
  {
    cJSON *texts = cJSON_CreateObject();
    cJSON_AddNumberToObject(texts, "maxEntries", 64);
    cJSON_AddNumberToObject(texts, "maxEntryBytes", 120);
    cJSON_AddNumberToObject(texts, "maxTotalBytes", 4096);
    cJSON_AddItemToObject(widget_limits, "texts", texts);
  }
#endif
#if PET_P4_GRAPHICS_V2
  {
    /* Ceilings for packages that declare graphics:"p4-graphics-v2". */
    cJSON *graphics = cJSON_CreateObject();
    cJSON_AddStringToObject(graphics, "profile", "p4-graphics-v2");
    cJSON_AddNumberToObject(graphics, "maxSceneSprites", PET_P4_MINIAPP_SPRITE_DECLARED_MAX);
    cJSON_AddNumberToObject(graphics, "maxSceneSpritePixels", PET_P4_MINIAPP_SPRITE_V2_PIXELS);
    cJSON_AddNumberToObject(graphics, "maxSurfaceImages", PET_SURFACE_IMAGES);
    cJSON_AddNumberToObject(graphics, "maxSurfaceImagePixels", PET_SURFACE_IMAGE_PIXELS);
    cJSON_AddNumberToObject(graphics, "maxSurfaceImageBytes", PET_SURFACE_IMAGE_BYTES);
    cJSON_AddNumberToObject(graphics, "maxFonts", PET_SURFACE_FONTS);
    cJSON_AddNumberToObject(graphics, "maxFontChars", PET_SURFACE_FONT_CHARS);
#if PET_P4_KEYFRAME_BG
    cJSON_AddNumberToObject(graphics, "maxKeyframes", PET_SURFACE_KEYFRAMES);
    cJSON_AddNumberToObject(graphics, "maxKeyframeBytes", 400000);
#endif
#if PET_P4_PARTICLES
    cJSON_AddNumberToObject(graphics, "maxParticleEmitters", PET_SURFACE_PARTICLE_EMITTERS);
    cJSON_AddNumberToObject(graphics, "maxParticleDots", PET_SURFACE_PARTICLE_DOTS);
#endif
    cJSON_AddItemToObject(widget_limits, "graphicsV2", graphics);
  }
#endif
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
  if (audio_playback_ready && pet_audio_volume_ready())
    cJSON_AddStringToObject(capabilities,"audioMasterVolume",PET_AUDIO_MASTER_VOLUME_ENGINE);
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
  cJSON_AddStringToObject(touch_input, "controller", touch_ready ? pet_p4_touch_controller() : "unavailable");
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
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  cJSON_AddNumberToObject(firmware_update, "chipRevision", chip_info.revision);
  cJSON_AddBoolToObject(firmware_update, "rollback", true);
  cJSON_AddNumberToObject(firmware_update, "validationMs", 8000);
  cJSON_AddItemToObject(capabilities, "firmwareUpdate", firmware_update);
  cJSON_AddBoolToObject(capabilities, "protocolAck", true);
  cJSON_AddStringToObject(capabilities, "bubblePixels", "p4-text-alpha-v2");
  cJSON_AddStringToObject(capabilities, "bubblePixelsBoot", pet_bubble_pixels_boot());
  cJSON_AddBoolToObject(capabilities, "screenshot", true);
  cJSON_AddStringToObject(capabilities, "screenshotTransfer", "pull-fnv1a32-v1");
#if PET_P4_SCREENSHOT_CLOSE_ACK
  cJSON_AddStringToObject(capabilities,"screenshotCloseAck",PET_SCREENSHOT_CLOSE_ACK_ENGINE);
#endif
#if PET_P4_STORAGE_IO_TRACE_ENABLE
  if(pet_storage_io_ready())cJSON_AddStringToObject(capabilities,"storageIoTrace","p4-storage-io-trace-v1");
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
  cJSON_AddBoolToObject(capabilities,"storageUniformCacheApplying",PET_P4_STORAGE_UNIFORM_CACHE_APPLY!=0);
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
  cJSON_AddBoolToObject(capabilities,"storagePageCacheApplying",PET_P4_STORAGE_PAGE_CACHE_APPLY!=0);
  cJSON_AddStringToObject(capabilities,"storagePageCacheHash",PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH?"stride4096-v1":"mixed-v1");
  cJSON_AddBoolToObject(capabilities,"storagePageCacheAutomatic",PET_P4_STORAGE_CACHE_SCOPE_ENABLED!=0);
#if PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED
  cJSON_AddBoolToObject(capabilities,"storageSaveCacheAutomatic",true);
#endif
#if PET_P4_BOOT_TRACE_ENABLE
  cJSON_AddStringToObject(capabilities,"bootTrace","p4-boot-trace-v1");
  cJSON_AddBoolToObject(capabilities,"storageBootCacheAutomatic",PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED!=0);
#endif
#endif
#endif
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
  cJSON_AddNumberToObject(transport, "usbUartBaud", PET_P4_UART_BAUD);
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
  if(g_asset_query_io_begin&&g_asset_query_pending)
    cJSON_AddStringToObject(appearance,"quiescentQuery",APPEARANCE_OBSERVATION_SCHEMA);
  cJSON_AddNumberToObject(appearance, "builtinSlot", 0);
  cJSON_AddNumberToObject(appearance, "customSlot", PET_P4_RAW_APPEARANCE_SLOT);
  cJSON_AddBoolToObject(appearance, "builtinProtected", true);
  cJSON_AddBoolToObject(appearance, "systemCues", true);
  if(g_asset_query_io_begin&&g_asset_query_io_end&&g_asset_query_pending){
    cJSON_AddStringToObject(capabilities,"appearanceAudioPatch","p4-audio-patch-v1");
    cJSON_AddStringToObject(capabilities,"globalAudioCues","p4-global-audio-cues-v1");
    cJSON_AddNumberToObject(capabilities,"appearanceAudioPatchMaxBytes",PET_AUDIO_PATCH_MAX_BYTES);
    cJSON_AddNumberToObject(capabilities,"appearanceAudioPatchChunkBytes",PET_AUDIO_PATCH_CHUNK_BYTES);
  }
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
  cJSON_AddBoolToObject(features, "conversationSubtitles", true);
  cJSON_AddBoolToObject(features, "audioAec", pet_p4_afe_ready());
  cJSON_AddBoolToObject(features, "audioVad", pet_p4_afe_ready());
  cJSON_AddBoolToObject(features, "audioFullDuplex", pet_p4_afe_ready());
  cJSON_AddBoolToObject(features, "audioAecRecovery", audio_ready && audio_playback_ready);
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

void pet_p4_send_hello(const pet_p4_runtime_state_t *state, pet_p4_send_line_fn send_line, void *ctx) {
  pet_p4_send_hello_for_board(state ? state->board_device_id : NULL, send_line, ctx);
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
  if (strcmp(agent, "deepseek-harness") == 0) {
    /* DSH has no speech/title events on the USB protocol. Clear the previous
     * idle "休息中" body (and any stale title) so a working/tool state does not
     * keep rendering the old bubble text. */
    state->current_speech[0] = '\0';
    state->current_title[0] = '\0';
    const char *dsh_title = json_string(payload, "title");
    if (!dsh_title[0]) dsh_title = json_string(payload, "sessionTitle");
    if (dsh_title[0]) {
      copy_text(state->current_title, sizeof(state->current_title), dsh_title);
    }
  }
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

/* R5 DSH approval: the PC bridge forwards one pending permission request. The
 * device only shows a modal and answers with a single SW1/SW3 decision; policy
 * (30s plugin timeout, fail-closed) stays on the host side. */
static bool handle_approval_request(
  pet_p4_runtime_state_t *state,
  const cJSON *payload,
  pet_p4_send_line_fn send_line,
  void *ctx
) {
  const char *request_id = json_string(payload, "requestId");
  const char *agent = json_string(payload, "agent");
  const char *tool = json_string(payload, "tool");
  const char *reason = json_string(payload, "reason");
  unsigned long long now_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
  unsigned long long expires_ms = json_bounded_u64(
    payload,
    "expiresAtMs",
    PET_P4_JSON_SAFE_INTEGER_MAX,
    0
  );
  unsigned long long deadline_ms;
  bool expired = false;
  const cJSON *never_expires = cJSON_GetObjectItemCaseSensitive(payload, "neverExpires");
  if (!request_id[0]) {
    send_protocol_ack(send_line, ctx, "approval/request", payload, false,
                      "invalid_approval_request", "requestId is required");
    return false;
  }
  if (strcmp(request_id, state->approval_last_decided_id) == 0) {
    /* The decision already crossed the wire. A replayed request must not
     * re-open the modal or allow a second decision for the same requestId. */
    send_protocol_ack(send_line, ctx, "approval/request", payload, false,
                      "approval_already_decided", "requestId was already decided");
    return true;
  }
  deadline_ms = cJSON_IsTrue(never_expires)
    ? 0
    : approval_deadline_ms(expires_ms, now_ms, &expired);
  if (expired) {
    /* Requests that expired before arrival are ignored, never shown. */
    (void) pet_p4_audio_play_event_cue("permission_expired");
    send_protocol_ack(send_line, ctx, "approval/request", payload, false,
                      "approval_expired", "request is already expired");
    return true;
  }
  if (!pet_p4_state_begin_approval(state, request_id, agent, tool, reason, deadline_ms)) {
    send_protocol_ack(send_line, ctx, "approval/request", payload, false,
                      "invalid_approval_request", "requestId is required");
    return false;
  }
  state->last_update_ms += 1;
  (void) pet_p4_audio_play_event_cue("permission_request");
  send_protocol_ack(send_line, ctx, "approval/request", payload, true, "ok", NULL);
  ESP_LOGI(TAG, "approval pending tool=%s expires_ms=%llu",
           state->approval_tool, state->approval_expires_ms);
  return true;
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

static char g_last_stopped_cue_session[PET_P4_SESSION_QUEUE_ID_MAX];
static unsigned long long g_last_stopped_cue_revision;

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
        bool stopped_transition = lifecycle_is(candidate.state, "stopped");
        copy_text(
          candidate.state,
          sizeof(candidate.state),
          lifecycle_is(candidate.state, "error")
            ? "error"
            : stopped_transition ? "stopped" : "done"
        );
        if (candidate.transition_revision > 0) {
          if (revision_is_same) {
            if (stopped_transition && lifecycle_is_active(previous->state)) {
              candidate.terminal_until_ms = now_ms + PET_P4_DONE_HOLD_MS;
            } else if (!lifecycle_is_terminal(previous->state)
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
      if (lifecycle_is(candidate.state, "stopped")
          && (!previous || !lifecycle_is(previous->state, "stopped"))) {
        bool already_played = g_last_stopped_cue_revision == candidate.transition_revision
          && strcmp(g_last_stopped_cue_session, candidate.id) == 0;
        if (!already_played) {
          snprintf(
            g_last_stopped_cue_session,
            sizeof(g_last_stopped_cue_session),
            "%s",
            candidate.id
          );
          g_last_stopped_cue_revision = candidate.transition_revision;
          (void) pet_p4_audio_play_event_cue("session_terminated");
        }
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

#include "pet_p4_background_protocol.inc"

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
  pet_widget_trace_kind_t trace_kind = !strcmp(topic,"widget/commit") ? PET_WTRACE_COMMIT
    : !strcmp(topic,"widget/delete") ? PET_WTRACE_DELETE
    : !strcmp(topic,"widget/activate") ? PET_WTRACE_ACTIVATE : PET_WTRACE_NONE;
  PET_WIDGET_TRACE_OPERATION(trace_operation, trace_kind);
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
    const bool repair_resources = pet_p4_miniapp_resource_repair_active();
    ok = pet_p4_miniapp_install_commit(payload, error, sizeof(error));
    bool unchanged=false;
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
    unchanged=ok&&pet_p4_miniapp_install_unchanged();
#endif
    if (ok && state && !unchanged) {
      pet_p4_miniapp_sync_stats(&state->stats);
      if (!repair_resources) copy_text(state->screen_page, sizeof(state->screen_page), "app");
      state->last_update_ms += 1;
    }
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error);
  } else if (strcmp(topic, "widget/list") == 0) {
    ok = send_widget_inventory(state, payload, send_line, ctx);
  } else if (!strcmp(topic,"widget/program-query")||!strcmp(topic,"widget/program-start")||!strcmp(topic,"widget/program-stop")||!strcmp(topic,"widget/program-inspect")) {
    ok=handle_background_program_topic(state,topic,payload,send_line,ctx);
  } else if (!strcmp(topic,"widget/catalog_inspect")||!strcmp(topic,"widget/catalog_reconcile")) {
    ok=send_catalog_admin(state,topic,payload,send_line,ctx);
  } else if (strcmp(topic,"widget/select_by_id")==0) {
    const char *request_id=json_string(payload,"requestId"),*widget_id=json_string(payload,"widgetId");
    unsigned request_fields=0,id_fields=0;
    bool fields_ok=cJSON_IsObject(payload)&&request_id[0]&&strlen(request_id)<96
      &&widget_id[0]&&strlen(widget_id)<PET_P4_MINIAPP_WIDGET_ID_MAX;
    const cJSON *field;
    cJSON_ArrayForEach(field,payload){
      if(field->string&&!strcmp(field->string,"requestId"))request_fields++;
      else if(field->string&&!strcmp(field->string,"widgetId"))id_fields++;
      else fields_ok=false;
    }
    if(!fields_ok||request_fields!=1||id_fields!=1){
      send_protocol_ack(send_line,ctx,topic,payload,false,"invalid_selection","selection accepts one bounded requestId and widgetId only");return false;
    }
    if(pet_p4_miniapp_catalog_recovery_blocked()){
      send_protocol_ack(send_line,ctx,topic,payload,false,"catalog_unavailable","catalog recovery is blocked; selection unchanged");return false;
    }
    /* Only move the existing visible catalog cursor. Never activate/load a
     * component, change the screen page, or persist/restart its runtime. */
    const size_t count=pet_p4_miniapp_catalog_count();
    for(size_t index=0;index<count;index++){
      pet_p4_miniapp_catalog_entry_t entry={0};
      if(!pet_p4_miniapp_catalog_get(index,&entry))break;
      if(!strcmp(entry.widget_id,widget_id)){ok=pet_p4_miniapp_catalog_select_durable(index);break;}
    }
    if(ok&&state)state->last_update_ms+=1;
    send_protocol_ack(send_line,ctx,topic,payload,ok,ok?"":"selection_failed",ok?"":"existing visible component not found; selection unchanged");
  } else if (strcmp(topic,"widget/activate")==0) {
    const cJSON *show=cJSON_GetObjectItemCaseSensitive(payload,"show");
    bool fields_ok=!show||cJSON_IsBool(show);
    const cJSON *field; cJSON_ArrayForEach(field,payload){if(!field->string||(strcmp(field->string,"widgetId")&&strcmp(field->string,"show")&&strcmp(field->string,"requestId")))fields_ok=false;}
    if(!fields_ok){send_protocol_ack(send_line,ctx,topic,payload,false,"invalid_activation","activation accepts widgetId, show and requestId only");return false;}
    if(pet_p4_audio_active()||pet_p4_audio_playback_active()||pet_p4_audio_conversation_active()||pet_p4_audio_music_active()){
      send_protocol_ack(send_line,ctx,topic,payload,false,"audio_busy","component navigation deferred while audio is active");return false;
    }
    ok=pet_p4_miniapp_catalog_activate_id(json_string(payload,"widgetId"));
    if(ok&&state){pet_p4_miniapp_sync_stats(&state->stats);if(cJSON_IsTrue(show))copy_text(state->screen_page,sizeof(state->screen_page),"app");state->last_update_ms+=1;}
    send_protocol_ack(send_line,ctx,topic,payload,ok,ok?"":"activation_failed",ok?"":"installed visible component could not be opened; no package was installed or deleted");
  } else if (strcmp(topic, "widget/delete") == 0) {
    phase = "delete";
    if (!transfer_id[0] || strlen(transfer_id) >= 64) {
      copy_text(error, sizeof(error), "missing/invalid transferId");
      ok = false;
    } else {
      ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload,"preserveSave"))
        ? pet_p4_miniapp_remove_preserving_save(json_string(payload,"widgetId"),error,sizeof(error))
        : pet_p4_miniapp_remove(json_string(payload, "widgetId"), error, sizeof(error));
    }
    if (ok && state) {
      copy_text(state->screen_page, sizeof(state->screen_page), "main");
      state->last_update_ms += 1;
    }
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, ok, error);
  } else {
    send_widget_ack(send_line, ctx, transfer_id, phase, payload, false, "unsupported widget phase");
  }
  pet_widget_trace_end(&trace_operation, ok);
  return ok;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
bool pet_p4_protocol_source_valid(const char *line){
  return pet_json_source_bounded_text(line);
}

bool pet_p4_handle_line_with_reply(
  pet_p4_runtime_state_t *state,
  const char *line,
  pet_p4_send_line_fn send_line,
  void *ctx,
  pet_p4_query_reply_t *reply
) {
  cJSON *root;
  cJSON *payload;
  const char *topic;
  /* A retired physical ingress is consumed, never a fallback dispatch. */
  if (!asset_phase_send_current(send_line, ctx)) return true;
  if (!state || !line || !line[0] || (reply && reply->line)) return false;
  if(!pet_p4_protocol_source_valid(line)){
    send_protocol_ack(send_line,ctx,"",NULL,false,"invalid_json","device JSON text is invalid or exceeds the container depth limit");
    return false;
  }
  pet_app_host_transport(send_line,ctx,state->board_device_id);
  pet_p4_media_transport(send_line,ctx);
  root = cJSON_ParseWithOpts(line, NULL, true);
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
  if (!strncmp(topic, "state/", 6) || !strcmp(topic, "speech/text")
      || !strcmp(topic, "session/current") || !strcmp(topic, "ui/conversation")
      || !strcmp(topic, "audio/play_subtitle")) {
    pet_bubble_pixels_desire(payload, ctx, topic, (uint64_t)(esp_timer_get_time() / 1000));
  }
  pet_p4_state_note_host_activity(
    state,
    (unsigned long long) (esp_timer_get_time() / 1000ULL)
  );

  if (strcmp(topic, "ack") == 0 || strcmp(topic, "bind") == 0) {
#if PET_P4_SERIAL_CONTROL_NEGOTIATED
    if(!pet_control_session_payload(ctx,payload)){
      send_protocol_ack(send_line,ctx,topic,payload,false,"invalid_control_negotiation","invalid bounded control integrity proposal");
      cJSON_Delete(root);return false;
    }
#endif
    copy_text(state->desktop_device_id, sizeof(state->desktop_device_id), json_string(payload, "desktopDeviceId"));
    pet_p4_send_hello(state, send_line, ctx);
  } else if (strcmp(topic,"debug/identity")==0){
    const char *request=pet_evidence_source_valid(line)?pet_evidence_request(payload):NULL;const esp_app_desc_t *app=esp_app_get_description();
    char elf[65]={0};if(app)for(unsigned i=0;i<32;i++)snprintf(elf+2*i,3,"%02x",app->app_elf_sha256[i]);
    int64_t uptime=esp_timer_get_time();
    cJSON *identity=request&&uptime>=0?pet_evidence_identity(&g_boot_identity,request,state->board_device_id,PET_P4_BUILD_ID,PET_P4_BUILD_GIT_SHA,elf,PET_P4_BUILD_DIRTY!=0,PET_P4_PROTOCOL_SCHEMA,(uint64_t)uptime):NULL;
    if(identity)send_topic_payload(send_line,ctx,"debug/identity",identity);
    else send_protocol_ack(send_line,ctx,topic,payload,false,"identity_unavailable","bounded identity request or identity unavailable");
  } else if (strcmp(topic, "approval/cancel") == 0) {
    const char *request_id = json_string(payload, "requestId");
    if (request_id[0]
        && strcmp(request_id, state->approval_request_id) == 0) {
      const char *outcome = json_string(payload, "outcome");
      pet_p4_state_clear_approval(state);
      if (strcmp(outcome, "allowed-once") == 0) {
        (void) pet_p4_audio_play_event_cue("permission_approved");
      } else if (strcmp(outcome, "rejected") == 0) {
        (void) pet_p4_audio_play_event_cue("permission_denied");
      } else {
        (void) pet_p4_audio_play_event_cue("permission_expired");
      }
      state->last_update_ms += 1;
    }
  } else if (strncmp(topic, "state/", 6) == 0) {
    handle_state_topic(state, topic, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "ui/text-pixels") == 0) {
    cJSON *reply = cJSON_CreateObject();
    if (reply) {
      static const char *const fields[]={"requestId","phase","client","owner","version","transfer","id"};
      for(unsigned i=0;i<sizeof(fields)/sizeof(*fields);i++){
        const char *value=json_string(payload,fields[i]);
        cJSON_AddStringToObject(reply,fields[i],strlen(value)<=64?value:"");
      }
      pet_bubble_pixels_maintain((uint64_t)(esp_timer_get_time()/1000),
        pet_p4_audio_active() || pet_p4_audio_playback_active() || pet_p4_audio_music_active()
        || pet_p4_audio_conversation_active() || pet_p4_ota_active() || state->asset_transfer_active);
      (void)pet_bubble_pixels_handle_owned(payload,ctx,(uint64_t)(esp_timer_get_time()/1000),reply);
      send_topic_payload(send_line,ctx,"ui/text-pixels/status",reply);
    }
  } else if (strcmp(topic, "speech/text") == 0) {
    handle_speech(state, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
#if PET_P4_NOTIFY
  } else if (strcmp(topic, PET_NOTIFY_TOPIC) == 0) {
    /* p4-notify-v1: bounded component notification into the main-page bubble.
     * Same payload shape as the device -> PC toast; one per component per 30 s. */
    static pet_notify_limiter_t notify_limiter;
    pet_notify_t notice;
    if (!pet_notify_parse(payload, &notice)) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "invalid_notify", "componentId/title required; text<=120, badge int");
      cJSON_Delete(root);
      return false;
    }
    if (!pet_notify_admit(&notify_limiter, notice.component, (uint64_t) (esp_timer_get_time() / 1000ULL))) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "notify_cooldown", "one notification per component every 30 s");
      cJSON_Delete(root);
      return false;
    }
    pet_notify_bubble_title(&notice, state->current_title, sizeof(state->current_title));
    copy_text(state->current_speech, sizeof(state->current_speech), notice.text);
    copy_text(state->current_status_text, sizeof(state->current_status_text), "通知");
    state->last_update_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
#endif
  } else if (strcmp(topic, "session/current") == 0) {
    handle_session_current(state, payload);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "approval/request") == 0) {
    if (!handle_approval_request(state, payload, send_line, ctx)) {
      cJSON_Delete(root);
      return false;
    }
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
    pet_p4_audio_set_transport(send_line, ctx);
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
    pet_p4_audio_set_transport(send_line, ctx);
    const char *action = json_string(payload, "action");
    esp_err_t audio_err = ESP_ERR_INVALID_ARG;
    if (strcmp(action, "start") == 0) {
      audio_err = pet_p4_audio_set_enabled(true);
      if (audio_err == ESP_OK) audio_err = pet_p4_audio_capture_start_owned(state->session_queue_count == 0,json_string(payload,"captureOwner"));
    } else if (strcmp(action, "stop") == 0) {
      const char *owner=json_string(payload,"captureOwner");
      audio_err = owner[0]?pet_p4_audio_capture_stop_owned(owner):pet_p4_audio_capture_stop();
    }
    if (audio_err != ESP_OK) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_control_failed",
                        "audio action is invalid or capture is not ready");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
#if PET_SERVICE_BRIDGE_ENABLED
  } else if (!strcmp(topic,"app/image")) {
    if(!strcmp(json_string(payload,"operation"),"query")){
      bool ready=pet_app_host_image_ready(payload,esp_timer_get_time()/1000ULL);send_protocol_ack(send_line,ctx,topic,payload,true,ready?"ready":"missing",NULL);
    }else{
      bool ok=pet_app_host_image_apply(payload,esp_timer_get_time()/1000ULL);
      send_protocol_ack(send_line,ctx,topic,payload,ok,ok?"ok":"app_image_invalid",ok?"":"image did not match the active view");
    }
    state->last_update_ms+=1;
#endif
  } else if (!strcmp(topic,"app/snapshot")) {
    bool ok=pet_app_host_apply(payload,esp_timer_get_time()/1000ULL);
    if(!ok)send_protocol_ack(send_line,ctx,topic,payload,false,"app_snapshot_invalid","app snapshot did not match the active request");
    else send_protocol_ack_if_requested(send_line,ctx,topic,payload);
    state->last_update_ms+=1;
  } else if (pet_p4_media_handle(topic,payload,send_line,ctx)) {
    if(!strcmp(topic,"media/begin") && pet_p4_audio_music_active()
       && pet_p4_audio_stream_matches(json_string(payload,"sessionId"))) {
      if(!pet_p4_miniapp_media_active()&&!pet_p4_miniapp_host_source(json_string(payload,"source"))) {
        pet_p4_miniapp_catalog_activate_id("music-player");
      }
      if(pet_p4_miniapp_media_active()||pet_p4_miniapp_host_source(json_string(payload,"source")))copy_text(state->screen_page,sizeof(state->screen_page),"app");
    }
    state->last_update_ms += 1;
  } else if (strcmp(topic, "audio/volume") == 0) {
    bool volume_ok=false;
    char *volume_reply=pet_audio_volume_reply(root,state->board_device_id,
      pet_p4_audio_playback_ready(),&volume_ok);
    bool volume_serialized=volume_reply!=NULL;
    if (volume_reply) {
      if (reply) reply->line=volume_reply;
      else {if (send_line) send_line(volume_reply,ctx);cJSON_free(volume_reply);}
    }
    cJSON_Delete(root);
    return volume_serialized && volume_ok;
  } else if (strcmp(topic, "audio/query") == 0) {
    pet_p4_audio_send_status();
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/conversation") == 0) {
    pet_p4_audio_set_transport(send_line, ctx);
    /* 实时对话：持续采集（不再等长按），半双工时播放期间不上行。 */
    const cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
    const cJSON *half_item = cJSON_GetObjectItemCaseSensitive(payload, "halfDuplex");
    bool enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item) : false;
    bool half_duplex = cJSON_IsBool(half_item) ? cJSON_IsTrue(half_item) : true;
    esp_err_t conv_err = pet_p4_audio_conversation_set(enabled, half_duplex, json_string(payload, "sessionId"));
    pet_p4_audio_diagnostic(enabled ? "conversation_start" : "conversation_end", json_string(payload, "sessionId"), 0, conv_err == ESP_OK);
    if (!enabled) {
      pet_p4_conversation_clear(state);
    }
    if (conv_err != ESP_OK) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_conversation_failed",
                        "conversation capture is not ready");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/play_begin") == 0) {
    pet_p4_conversation_playback_begin(state, json_string(payload, "sessionId"));
    esp_err_t play_err = pet_p4_audio_stream_begin(json_string(payload, "sessionId"));
    if (play_err != ESP_OK) {
      pet_p4_audio_diagnostic("playback_rejected", json_string(payload, "sessionId"), 0, false);
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_playback_failed",
                        "speaker playback is not ready");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/play_chunk") == 0) {
    if (!pet_p4_audio_stream_matches(json_string(payload, "sessionId"))) {
      /* A cancelled turn can have already crossed USB. Ignore it, never append
       * old PCM to a newer reply or terminate the new conversation. */
      send_protocol_ack_if_requested(send_line, ctx, topic, payload);
      cJSON_Delete(root);
      return true;
    }
    const char *data = json_string(payload, "data");
    size_t data_len = strlen(data);
    if (data_len == 0 || data_len > PET_P4_AUDIO_STREAM_CHUNK_B64_MAX) {
      pet_p4_audio_diagnostic("chunk_invalid", json_string(payload, "sessionId"), data_len, false);
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_chunk_invalid",
                        "play_chunk data missing or too large");
      cJSON_Delete(root);
      return false;
    }
    static uint8_t decoded[PET_P4_AUDIO_STREAM_CHUNK_MAX];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_len,
                              (const unsigned char *) data, data_len) != 0
        || pet_p4_audio_stream_push(decoded, decoded_len) != ESP_OK) {
      pet_p4_audio_diagnostic("chunk_rejected", json_string(payload, "sessionId"), decoded_len, false);
      send_protocol_ack(send_line, ctx, topic, payload, false, "audio_chunk_rejected",
                        "play_chunk could not be buffered");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/play_end") == 0) {
    if (pet_p4_audio_stream_matches(json_string(payload, "sessionId"))) (void) pet_p4_audio_stream_end();
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/play_subtitle") == 0) {
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(payload, "offsetBytes");
    bool valid = cJSON_IsNumber(offset) && isfinite(offset->valuedouble)
      && offset->valuedouble >= 0 && offset->valuedouble <= 4294967294.0
      && offset->valuedouble == (double) (unsigned int) offset->valuedouble;
    bool accepted = valid && pet_p4_conversation_queue_cue(state, json_string(payload, "sessionId"),
      (unsigned int) offset->valuedouble, json_string(payload, "text"));
    if (!accepted) send_protocol_ack(send_line, ctx, topic, payload, false, "invalid_subtitle", "invalid playback subtitle");
    else send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "audio/play_flush") == 0) {
    pet_p4_conversation_playback_begin(state, "");
    pet_p4_audio_stream_flush();
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
#if PET_P4_EVENTS
  } else if (strcmp(topic, "component/alarm_policy") == 0) {
    /* p4-events-v1: the PC authorises (per boot) whether hidden background alarms may
     * raise a main-page bubble. Default off; never persisted; no alarm data crosses. */
    const cJSON *background = cJSON_GetObjectItemCaseSensitive(payload, "background");
    if (!cJSON_IsBool(background)) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "invalid_alarm_policy", "background must be a boolean");
      cJSON_Delete(root);
      return false;
    }
    if (!pet_p4_miniapp_alarm_policy_prepare(cJSON_IsTrue(background))) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "alarm_restore_unavailable", "saved alarms unavailable; prior policy retained");
      cJSON_Delete(root);
      return false;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
#endif
  } else if (strcmp(topic, "ui/conversation") == 0) {
    bool phase_changed = strcmp(state->conversation_state, json_string(payload, "state")) != 0;
    bool accepted = pet_p4_conversation_update(state, json_string(payload, "sessionId"),
      json_string(payload, "state"), json_string(payload, "name"), json_string(payload, "text"),
      strcmp(json_string(payload, "role"), "user") == 0,
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "final")), esp_timer_get_time() / 1000ULL);
    bool failed = strcmp(state->conversation_state, "error") == 0;
    if (accepted && phase_changed) pet_p4_audio_diagnostic(failed ? "conversation_failed" : "conversation_state", json_string(payload, "sessionId"), 0, !failed);
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "system/heartbeat") == 0) {
    const cJSON *clock=cJSON_GetObjectItemCaseSensitive(payload,"clock");
    unsigned long long heartbeat_now_ms = (unsigned long long) (esp_timer_get_time() / 1000ULL);
    /* The PC bridge sends Unix epoch tsMs every two seconds. Keep it as the
     * wall-clock anchor used to translate approval expiresAtMs deadlines. */
    unsigned long long host_ts_ms = json_bounded_u64(
      payload,
      "tsMs",
      4102444800000ULL,
      0
    );
    if(clock)pet_p4_miniapp_clock_sync(clock,heartbeat_now_ms);
    if (host_ts_ms >= PET_P4_WALL_CLOCK_EPOCH_MIN_MS) {
      g_host_wall_clock_ms = host_ts_ms;
      g_host_wall_clock_at_ms = heartbeat_now_ms;
    }
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (pet_p4_diagnostics_handle_topic_with_reply(topic, payload, state, send_line, ctx, reply)) {
    state->last_update_ms += 1;
  } else if (pet_p4_ota_handle_topic(topic, payload, send_line, ctx)) {
    state->last_update_ms += 1;
  } else if (strncmp(topic, "asset/", 6) == 0) {
    handle_asset_topic(state, topic, payload, send_line, ctx);
  } else if (strcmp(topic, "widget/data") == 0) {
    if (!pet_p4_widget_data_apply(payload, esp_timer_get_time() / 1000ULL)) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "invalid_widget_data", "data snapshot rejected");
      cJSON_Delete(root); return false;
    }
    state->last_update_ms += 1;
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strncmp(topic, "widget/", 7) == 0) {
    if (!handle_widget_topic(state, topic, payload, send_line, ctx)) {
      cJSON_Delete(root);
      return false;
    }
  } else if (strcmp(topic, "miniapp/event") == 0) {
    const char *action = json_string(payload, "action");
    // Input eligibility, host freshness, throttles and confirmations all use
    // device monotonic time. An omitted/desktop wall-clock timestamp must not
    // silently disable actions or expire a freshly opened confirmation.
    if (!pet_p4_miniapp_dispatch_action(action, esp_timer_get_time() / 1000ULL)) {
      send_protocol_ack(send_line, ctx, topic, payload, false, "miniapp_event_unhandled",
                        "action is not handled by the active mini-app state");
      cJSON_Delete(root);
      return false;
    }
    state->last_update_ms += 1;
    send_protocol_ack_if_requested(send_line, ctx, topic, payload);
  } else if (strcmp(topic, "miniapp/query") == 0) {
    send_miniapp_state(send_line, ctx, payload);
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

/* RAM-only service lane during a storage command. Caller owns the state
 * mutex; returned serialization is sent only after releasing it. This never
 * enters the mutation dispatcher, adopts its deferred ACK, or touches files. */
static void storage_query_capture(const char *line, void *ctx) {
  pet_p4_query_reply_t *reply = ctx;
  if (!reply || reply->line || !line) return;
  size_t bytes = strlen(line) + 1;
  reply->line = cJSON_malloc(bytes);
  if (reply->line) memcpy(reply->line, line, bytes);
}
bool pet_p4_handle_storage_query(pet_p4_runtime_state_t *state,
    const char *line, pet_p4_query_reply_t *reply) {
  if (!state || !line || !reply || reply->line || strlen(line) > 1024
      || !pet_p4_protocol_source_valid(line)) return false;
  cJSON *root = cJSON_ParseWithOpts(line, NULL, true);
  const cJSON *payload = root ? cJSON_GetObjectItemCaseSensitive(root, "payload") : NULL;
  const char *topic = json_string(root, "topic");
  bool handled = false;
  if (cJSON_IsObject(root) && cJSON_IsObject(payload)) {
    if (!strcmp(topic, "system/heartbeat")) {
      const cJSON *clock = cJSON_GetObjectItemCaseSensitive(payload, "clock");
      if (clock) (void)pet_p4_miniapp_clock_sync(clock, esp_timer_get_time()/1000ULL);
      send_protocol_ack_if_requested(storage_query_capture, reply, topic, payload);
      handled = true;
    } else if (!strcmp(topic,"widget/catalog_inspect")) {
      (void)send_catalog_admin(state,topic,payload,storage_query_capture,reply);
      handled=true;
    } else if (!strcmp(topic, "widget/list")) {
      (void)send_widget_inventory(state, payload, storage_query_capture, reply);
      handled = true;
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED && PET_P4_OTA_METADATA_CACHED
    } else if (!strcmp(topic, "diagnostics/query")) {
      handled = pet_p4_diagnostics_handle_topic_with_reply(topic, payload, state,
        storage_query_capture, reply, reply);
#endif
    }
    if (handled) pet_p4_state_note_host_activity(state, esp_timer_get_time()/1000ULL);
  }
  cJSON_Delete(root);
  return handled;
}

bool pet_p4_handle_line(
  pet_p4_runtime_state_t *state, const char *line,
  pet_p4_send_line_fn send_line, void *ctx
) {
  return pet_p4_handle_line_with_reply(state, line, send_line, ctx, NULL);
}
#if PET_P4_MUTATION_ACK_DEFERRED
void pet_p4_protocol_set_deferred_reply(pet_p4_query_reply_t *reply){
  /* Dispatcher only, around one locked dispatch; NULL restores the original
   * locked ACK path. Never called from handlers, hosts or other tasks. */
  g_deferred_reply=reply;
  g_deferred_reply_owner=reply?xTaskGetCurrentTaskHandle():NULL;
}
#endif
