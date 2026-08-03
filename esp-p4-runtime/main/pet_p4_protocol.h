/*
 * [Input] Shared ESP32-P4 protocol payload/state limits and lifecycle timing constants.
 * [Output] Runtime state structures, per-session terminal retention, and public
 *          protocol/state-processing declarations.
 * [Pos] Shared protocol contract header for the ESP32-P4 firmware.
 * [Sync] If this file changes, update esp-p4-runtime/protocol.md and .folder.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pet_p4_stats.h"
#include "pet_p4_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_DEVICE_ID_MAX 64
#define PET_P4_AGENT_MAX 64
#define PET_P4_STATE_MAX 32
#define PET_P4_TITLE_MAX 128
#define PET_P4_SPEECH_MAX 512
#define PET_P4_STATUS_TEXT_MAX 96
#define PET_P4_SESSION_TITLE_MAX 256
#define PET_P4_SESSION_NOTICE_MAX 96
#define PET_P4_SESSION_QUEUE_MAX 8
#define PET_P4_SESSION_QUEUE_ID_MAX 128
#define PET_P4_SESSION_QUEUE_TITLE_MAX 192
#define PET_P4_SESSION_QUEUE_CONTENT_MAX 384
#define PET_P4_SESSION_QUEUE_STATE_MAX 24
#define PET_P4_PAGE_MAX 16
#define PET_P4_STATS_MAX 768
#define PET_P4_ASSET_MANIFEST_MAX 65536
#define PET_P4_DONE_HOLD_MS 60000ULL
#define PET_P4_HOST_HEARTBEAT_TIMEOUT_MS 6000ULL
#define PET_P4_SESSION_SNAPSHOT_TIMEOUT_MS 30000ULL

typedef void (*pet_p4_send_line_fn)(const char *line, void *ctx);

typedef struct {
  char id[PET_P4_SESSION_QUEUE_ID_MAX];
  char title[PET_P4_SESSION_QUEUE_TITLE_MAX];
  char content[PET_P4_SESSION_QUEUE_CONTENT_MAX];
  char state[PET_P4_SESSION_QUEUE_STATE_MAX];
  unsigned long long transition_revision;
  unsigned long long terminal_until_ms;
} pet_p4_session_queue_item_t;

typedef struct {
  char board_device_id[PET_P4_DEVICE_ID_MAX];
  char desktop_device_id[PET_P4_DEVICE_ID_MAX];
  char active_agent[PET_P4_AGENT_MAX];
  char current_state[PET_P4_STATE_MAX];
  char current_event[PET_P4_STATE_MAX];
  char current_title[PET_P4_TITLE_MAX];
  char current_speech[PET_P4_SPEECH_MAX];
  char current_status[PET_P4_STATE_MAX];
  char current_status_text[PET_P4_STATUS_TEXT_MAX];
  char current_session_title[PET_P4_SESSION_TITLE_MAX];
  char current_session_id[PET_P4_SESSION_QUEUE_ID_MAX];
  char current_session_agent[PET_P4_AGENT_MAX];
  char current_session_notice[PET_P4_SESSION_NOTICE_MAX];
  unsigned int current_session_index;
  unsigned int current_session_count;
  unsigned long long session_notice_until_ms;
  pet_p4_session_queue_item_t session_queue[PET_P4_SESSION_QUEUE_MAX];
  pet_p4_session_queue_item_t session_queue_staging[PET_P4_SESSION_QUEUE_MAX];
  unsigned int session_queue_count;
  bool session_voice_active;
  unsigned long long session_snapshot_last_seen_ms;
  char screen_page[PET_P4_PAGE_MAX];
  char stats_json[PET_P4_STATS_MAX];
  pet_p4_stats_model_t stats;
  char asset_manifest_json[PET_P4_ASSET_MANIFEST_MAX];
  pet_p4_asset_catalog_t asset_catalog;
  unsigned int asset_family_count;
  unsigned int asset_revision;
  bool asset_transfer_active;
  char local_lifecycle[PET_P4_STATE_MAX];
  unsigned long long local_lifecycle_started_ms;
  unsigned long long local_lifecycle_until_ms;
  unsigned long long touch_feedback_until_ms;
  unsigned long long done_until_ms;
  unsigned long long host_last_seen_ms;
  bool native_usb_mounted;
  int local_touch_x;
  int local_touch_y;
  unsigned long long last_update_ms;
} pet_p4_runtime_state_t;

void pet_p4_state_init(pet_p4_runtime_state_t *state, const char *board_device_id);
void pet_p4_state_process(pet_p4_runtime_state_t *state, unsigned long long now_ms);
bool pet_p4_state_request_touch(
  pet_p4_runtime_state_t *state,
  const char *family,
  int x,
  int y,
  unsigned long long duration_ms,
  unsigned long long now_ms
);
const char *pet_p4_state_effective_lifecycle(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
bool pet_p4_state_touch_feedback_active(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
void pet_p4_state_note_host_activity(
  pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
void pet_p4_state_set_native_usb_mounted(
  pet_p4_runtime_state_t *state,
  bool mounted,
  unsigned long long now_ms
);
bool pet_p4_state_host_connected(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
void pet_p4_load_asset_manifest(pet_p4_runtime_state_t *state);
bool pet_p4_asset_fs_path(const char *logical_path, char *out, size_t out_size);
bool pet_p4_asset_read_all(
  const char *logical_path,
  unsigned char *out,
  size_t expected_size
);
bool pet_p4_asset_fs_path_for_slot(int slot, const char *logical_path, bool tmp, char *out, size_t out_size);
int pet_p4_asset_active_slot(void);
int pet_p4_asset_inactive_slot(void);
bool pet_p4_asset_set_active_slot(int slot);
bool pet_p4_asset_slot_has_pack_id(int slot);
bool pet_p4_asset_mark_slot_ready(int slot);
void pet_p4_asset_clean_legacy_files(void);
void pet_p4_send_hello(const pet_p4_runtime_state_t *state, pet_p4_send_line_fn send_line, void *ctx);
bool pet_p4_raw_asset_chunk_active(void);
size_t pet_p4_consume_raw_asset_bytes(
  const unsigned char *data,
  size_t len,
  pet_p4_send_line_fn send_line,
  void *ctx
);
bool pet_p4_handle_line(
  pet_p4_runtime_state_t *state,
  const char *line,
  pet_p4_send_line_fn send_line,
  void *ctx
);

#ifdef __cplusplus
}
#endif
