#include "pet_p4_view.h"

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

static bool should_wait_for_session_card(const pet_p4_runtime_state_t *state) {
  if (!state
      || state->session_queue_count > 0
      || state->session_snapshot_last_seen_ms == 0
      || state->session_voice_active) {
    return false;
  }
  return status_from_state(state) != PET_P4_VIEW_STATUS_IDLE;
}

void pet_p4_build_view_model(const pet_p4_runtime_state_t *state, pet_p4_view_model_t *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->page = state && state->screen_page[0] ? state->screen_page : "main";
  out->agent = state ? state->active_agent : "";
  out->title = state && state->current_title[0] ? state->current_title : out->agent;
  out->body = state ? state->current_speech : "";
  out->stats_json = state ? state->stats_json : "";
  out->status = status_from_state(state);
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
}
