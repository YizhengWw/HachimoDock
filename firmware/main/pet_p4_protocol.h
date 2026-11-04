/*
 * [Input] Shared ESP32-P4 protocol payload/state limits and lifecycle timing constants.
 * [Output] Runtime state structures, per-session terminal retention, and public
 *          protocol/state-processing declarations.
 * [Pos] Shared protocol contract header for the ESP32-P4 firmware.
 * [Sync] If this file changes, update firmware/protocol.md and .folder.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pet_p4_stats.h"
#include "pet_p4_assets.h"
#include "pet_p4_slice_core.h"
#include "pet_p4_query_reply.h"
#include "pet_p4_appearance_session.h"
#include "pet_p4_resources.h"

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
#define PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS 15000ULL
#define PET_P4_CONVERSATION_HISTORY_MAX 8
#define PET_P4_CONVERSATION_CUES_MAX 12
/* DSH approval modal (R5): one pending request at a time, bounded strings. */
#define PET_P4_APPROVAL_REQUEST_ID_MAX 96
#define PET_P4_APPROVAL_AGENT_MAX 64
#define PET_P4_APPROVAL_TOOL_MAX 64
#define PET_P4_APPROVAL_REASON_MAX 192
#define PET_P4_APPROVAL_TIMEOUT_MS 30000ULL

typedef struct {
  char text[PET_P4_SPEECH_MAX];
  bool user;
} pet_p4_conversation_caption_t;

typedef struct {
  unsigned int offset_bytes;
  char text[PET_P4_SPEECH_MAX];
} pet_p4_conversation_cue_t;

typedef void (*pet_p4_send_line_fn)(const char *line, void *ctx);
/* Boot-configured paired callbacks. begin admits only the current locked
 * protocol command; on success the slot-query owns an unlocked read/TX window
 * and must call end before returning to its dispatcher. No new IO owner. */
bool pet_p4_protocol_asset_query_io_configure(bool (*begin)(void *),
  void (*end)(void *), void *context);

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
  /* DSH 审批上屏（R5）：PC bridge 下发的单条 pending approval。active 时渲染
   * 覆盖层；一次 SW1/SW3 短按产生唯一 decision 并清空 pending 字段。
   * approval_last_decided_id 保留最近一次已回复的 requestId，用于幂等去重。 */
  bool approval_active;
  char approval_request_id[PET_P4_APPROVAL_REQUEST_ID_MAX];
  char approval_agent[PET_P4_APPROVAL_AGENT_MAX];
  char approval_tool[PET_P4_APPROVAL_TOOL_MAX];
  char approval_reason[PET_P4_APPROVAL_REASON_MAX];
  unsigned long long approval_expires_ms;
  char approval_last_decided_id[PET_P4_APPROVAL_REQUEST_ID_MAX];
  /* Ephemeral native touch diagnostic; never persisted or advertised as PetUI. */
  pet_p4_slice_t touch_test;
  unsigned long long touch_test_until_ms;
  /* 实时对话（2026-09-18）：PC 经 ui/conversation 驱动的 HUD 与动画覆盖；
   * state 为空或 "ended" 时不生效。 */
  char conversation_state[PET_P4_STATE_MAX];
  char conversation_name[PET_P4_AGENT_MAX];
  char conversation_text[PET_P4_SPEECH_MAX];
  unsigned long long conversation_error_until_ms;
  char conversation_id[PET_P4_DEVICE_ID_MAX];
  char conversation_playback_id[128];
  bool conversation_user;
  unsigned long long conversation_caption_since_ms;
  pet_p4_conversation_caption_t conversation_history[PET_P4_CONVERSATION_HISTORY_MAX];
  unsigned int conversation_history_count;
  int conversation_history_cursor; /* -1 follows the current speaker */
  pet_p4_conversation_cue_t conversation_cues[PET_P4_CONVERSATION_CUES_MAX];
  unsigned int conversation_cue_count;
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
/* Trusted single-thread startup only. Repeated starts cannot replace a boot ID. */
void pet_p4_boot_identity_start(uint32_t first,uint32_t second);
const char *pet_p4_boot_identity(void);
bool pet_p4_protocol_asset_query_pending_configure(bool (*pending)(void *), void *context);
void pet_p4_state_process(pet_p4_runtime_state_t *state, unsigned long long now_ms);
/* DSH R5 approval pending state: set by approval/request, consumed exactly once
 * by SW1/SW3, cleared without an outgoing decision when it expires. */
void pet_p4_state_clear_approval(pet_p4_runtime_state_t *state);
bool pet_p4_state_approval_pending(
  const pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
bool pet_p4_state_begin_approval(
  pet_p4_runtime_state_t *state,
  const char *request_id,
  const char *agent,
  const char *tool,
  const char *reason,
  unsigned long long expires_ms
);
bool pet_p4_state_decide_approval(
  pet_p4_runtime_state_t *state,
  unsigned long long now_ms,
  char *request_id_out,
  size_t request_id_out_size
);
void pet_p4_asset_transfer_process(
  pet_p4_runtime_state_t *state,
  unsigned long long now_ms
);
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
/* Read-only audio overlay lookup; caller owns physical playback realtime IO. */
bool pet_p4_asset_audio_override_path(const char *family,char *out,size_t capacity);
int pet_p4_asset_inactive_slot(void);
bool pet_p4_asset_set_active_slot(int slot);
bool pet_p4_asset_slot_has_pack_id(int slot);
bool pet_p4_asset_mark_slot_ready(int slot);
void pet_p4_asset_clean_legacy_files(void);
void pet_p4_send_hello(const pet_p4_runtime_state_t *state, pet_p4_send_line_fn send_line, void *ctx);
/* Immutable native board identity only; no runtime/model lock during TX. */
void pet_p4_send_hello_for_board(const char *board_id, pet_p4_send_line_fn send_line, void *ctx);
/* All calls below require the existing state mutex. Separate serial and
 * native transactions share one physical appearance mutation owner. */
enum {PET_APPEARANCE_SERIAL=1,PET_APPEARANCE_NATIVE=2};
bool pet_p4_appearance_claim(pet_p4_send_line_fn send,void *ctx,const char *id,unsigned kind);
bool pet_p4_appearance_owned(pet_p4_send_line_fn send,void *ctx,const char *id,unsigned kind);
bool pet_p4_appearance_end(pet_p4_send_line_fn send,void *ctx,const char *id,unsigned kind);
bool pet_p4_appearance_active(void);
/* All callbacks use the caller's existing state mutex; begin borrows it for
 * physical IO, end reacquires it. current is a RAM-only connection-generation
 * check. No caller may retire the route/slot while operation_active is set.
 * UNKNOWN retains the appearance owner and blocks mutation/timeout cleanup. */
typedef struct {
  bool (*begin)(void *); void (*end)(void *);
  bool (*current)(void *); void *context;
} pet_appearance_commit_io_t;
typedef enum {PET_APPEARANCE_COMMIT_FAILED, PET_APPEARANCE_COMMIT_OK,
              PET_APPEARANCE_COMMIT_UNKNOWN} pet_appearance_commit_result_t;
bool pet_p4_appearance_mutation_blocked(void);
pet_appearance_commit_result_t pet_p4_appearance_commit_slot(
  pet_p4_runtime_state_t *state,int slot,pet_p4_send_line_fn send,void *route,
  const char *id,unsigned kind,bool patch,const pet_appearance_commit_io_t *io,
  char *error,size_t capacity);

/* Every appearance upload phase uses the caller's actual task/mutex adapter.
 * The operation owns all mutable staging bytes until end revalidates the exact
 * route/generation/slot/revision/connection and physical STORAGE_WORK lease.
 * No callbacks or IO run under the state lock except the short broker calls. */
typedef struct {
  pet_resource_owner_t owner; pet_resource_lease_t lease;
  pet_appearance_commit_io_t io;
  pet_p4_send_line_fn sender; void *route;
  uint64_t generation; unsigned revision,kind; int active_slot,target_slot;
  char id[64]; bool active;
} pet_appearance_phase_t;
bool pet_p4_appearance_phase_begin(pet_appearance_phase_t *phase,
  pet_p4_runtime_state_t *state,pet_p4_send_line_fn send,void *route,
  const char *id,unsigned kind,int target_slot,const pet_appearance_commit_io_t *io);
pet_appearance_commit_result_t pet_p4_appearance_phase_end(pet_appearance_phase_t *phase,
  pet_p4_runtime_state_t *state,int target_slot,bool ok,bool write_attempted);
bool pet_p4_appearance_native_active(void);
void pet_p4_asset_transfer_cleanup_worker(pet_p4_runtime_state_t *state);
size_t pet_p4_consume_raw_asset_bytes_scoped(pet_p4_runtime_state_t *state,
  const unsigned char *data,size_t len,pet_p4_send_line_fn send,void *route,
  const pet_appearance_commit_io_t *io);

bool pet_p4_raw_asset_chunk_active_for_route(pet_p4_send_line_fn send,void *ctx);
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
/* Same parser, host registration and dispatch. Only the immutable diagnostic
 * query response may be detached; all mutation/audio/media paths stay direct.
 * An optional reply must be empty; NULL retains the synchronous old behavior.
 * Caller retains the original send/ctx route.
 * On return, release the state lock before sending/freeing reply->line once. */
bool pet_p4_handle_line_with_reply(
  pet_p4_runtime_state_t *state,
  const char *line,
  pet_p4_send_line_fn send_line,
  void *ctx,
  pet_p4_query_reply_t *reply
);
/* Same nonrecursive bounded Unicode/depth preflight as the ordinary entry.
 * Pure source representation check, not schema validation or authorization. */
bool pet_p4_protocol_source_valid(const char *line);
/* State-locked RAM query lane; send/free reply after unlocking. */
bool pet_p4_handle_storage_query(pet_p4_runtime_state_t *state,
  const char *line, pet_p4_query_reply_t *reply);
#if PET_P4_MUTATION_ACK_DEFERRED
/* Dispatcher-owned slot for the current locked dispatch; see pet_p4_query_reply.h. */
void pet_p4_protocol_set_deferred_reply(pet_p4_query_reply_t *reply);
#endif

#ifdef __cplusplus
}
#endif
