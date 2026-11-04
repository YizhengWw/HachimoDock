#include "pet_p4_view.h"
#include "pet_p4_conversation.h"

#include <string.h>

static bool text_is_empty(const char *value) {
  return !value || !value[0];
}

static pet_p4_view_status_t status_from_state(const pet_p4_runtime_state_t *state) {
  const char *status = state ? state->current_status : "";
  const char *life = state ? state->current_state : "";
  if (strcmp(status, "done") == 0 || strcmp(life, "done") == 0) {
    return PET_P4_VIEW_STATUS_DONE;
  }
  if (strcmp(status, "error") == 0 || strcmp(life, "error") == 0) {
    return PET_P4_VIEW_STATUS_ERROR;
  }
  if (strcmp(status, "stopped") == 0 || strcmp(life, "stopped") == 0) {
    return PET_P4_VIEW_STATUS_STOPPED;
  }
  if (strcmp(status, "waiting_user") == 0 || strcmp(life, "waiting_user") == 0) {
    return PET_P4_VIEW_STATUS_WAITING;
  }
  if (strcmp(status, "speaking") == 0 || strcmp(life, "speaking") == 0 ||
      strcmp(status, "working") == 0 || strcmp(life, "working") == 0 ||
      strcmp(life, "tool_running") == 0) {
    return PET_P4_VIEW_STATUS_WORKING;
  }
  return PET_P4_VIEW_STATUS_IDLE;
}

/* The small main-page pet badge describes lifecycle only. Agent/session
 * identity remains available to routing and full Session cards, never as a
 * fallback label when a state event has no speech body. */
static const char *compact_pet_status_label(pet_p4_view_status_t status) {
  switch (status) {
    case PET_P4_VIEW_STATUS_WORKING: return "工作中";
    case PET_P4_VIEW_STATUS_WAITING: return "等待操作";
    case PET_P4_VIEW_STATUS_DONE: return "已完成";
    case PET_P4_VIEW_STATUS_ERROR: return "出错了";
    case PET_P4_VIEW_STATUS_STOPPED: return "已停止";
    case PET_P4_VIEW_STATUS_IDLE:
    default: return "休息中";
  }
}

static bool should_compact_bubble(const pet_p4_runtime_state_t *state) {
  if (!state) return true;
  if (text_is_empty(state->current_title) && strlen(state->current_speech) <= 36) {
    return true;
  }
  if (strcmp(state->current_state, "idle") == 0 && strlen(state->current_speech) <= 36) {
    return true;
  }
  return false;
}

static bool is_dsh_state_event(const pet_p4_runtime_state_t *state) {
  return state
    && (strncmp(state->current_event, "tool/", 5) == 0
        || strncmp(state->current_event, "turn/", 5) == 0
        || strncmp(state->current_event, "session", 7) == 0);
}

static bool should_wait_for_session_card(const pet_p4_runtime_state_t *state) {
  if (!state
      || state->session_queue_count > 0
      || state->session_snapshot_last_seen_ms == 0
      || state->session_voice_active) {
    return false;
  }
  /* DSH has no P4 session-queue snapshot; its state/tool events are the
   * authoritative status source. Do not replace working/tool_running with the
   * "休息中" fallback. */
  if (strcmp(state->active_agent, "deepseek-harness") == 0 || is_dsh_state_event(state)) {
    return false;
  }
  /* Never replace an active lifecycle with the idle "休息中" placeholder just
   * because a session card has not arrived. This affected DSH and, after
   * switching, Claude/Codex too. */
  return false;
}

static const char *conversation_hint(const char *conversation_state) {
  /* Status labels omit trailing punctuation; U+2026 is not in the device font. */
  if (!strcmp(conversation_state, "listening")) return "我在听";
  if (!strcmp(conversation_state, "thinking")) return "想一想";
  if (!strcmp(conversation_state, "speaking")) return "准备回答";
  return "准备中";
}

void pet_p4_build_view_model(const pet_p4_runtime_state_t *state, pet_p4_view_model_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->page = state && state->screen_page[0] ? state->screen_page : "main";
  out->agent = state ? state->active_agent : "";
  if (pet_p4_conversation_active(state)) {
    /* 实时对话独占气泡：只显示当前说话方，不与 Agent 会话叠加。 */
    out->page = "main";
    out->agent = "";
    out->realtime_conversation = true;
    out->caption_since_ms = state->conversation_caption_since_ms;
    out->title = state->conversation_user ? "你" : state->conversation_name[0] ? state->conversation_name : "宠物";
    out->body = state->conversation_text[0]
      ? state->conversation_text
      : conversation_hint(state->conversation_state);
    out->stats_json = "";
    int cursor = state->conversation_history_cursor;
    if (cursor >= 0 && cursor < (int) state->conversation_history_count) {
      out->conversation_history = true;
      out->title = state->conversation_history[cursor].user ? "你" : state->conversation_name;
      out->body = state->conversation_history[cursor].text;
    }
    out->status = !strcmp(state->conversation_state, "error") ? PET_P4_VIEW_STATUS_ERROR : !strcmp(state->conversation_state, "listening")
      ? PET_P4_VIEW_STATUS_WAITING
      : PET_P4_VIEW_STATUS_WORKING;
    out->show_bubble = true;
    out->compact_bubble = false;
    return;
  }
  out->title = state && state->current_title[0] ? state->current_title : out->agent;
  out->body = state ? state->current_speech : "";
  out->stats_json = state ? state->stats_json : "";
  out->status = status_from_state(state);
  out->show_voice_overlay = state && state->session_voice_active
    && state->session_queue_count == 0 && strcmp(out->page, "main") == 0;
  if (out->show_voice_overlay) return;
  if (should_wait_for_session_card(state)) {
    out->title = "";
    out->body = "休息中";
    out->status = PET_P4_VIEW_STATUS_IDLE;
    out->show_bubble = true;
    out->compact_bubble = true;
    return;
  }
  out->show_bubble = !text_is_empty(out->title) || !text_is_empty(out->body);
  out->compact_bubble = should_compact_bubble(state);
  if (text_is_empty(out->body)) {
    /* Terminal lifecycle metadata is already represented by its Session card.
     * Do not synthesize another compact completion notice. Nonempty actual
     * replies, including short replies, remain unchanged. */
    if (out->status == PET_P4_VIEW_STATUS_DONE) {
      out->title = "";
      out->body = "";
      out->show_bubble = false;
      out->compact_bubble = true;
      return;
    }
    /* Missing content is lifecycle metadata, never an Agent/session-name
     * caption. Keep all nonempty task/speech content verbatim. */
    out->title = "";
    out->body = compact_pet_status_label(out->status);
    out->compact_bubble = true;
  }
}
