/* Bounded, RAM-only captions. Agent session state is deliberately never modified. */
#include "pet_p4_conversation.h"
#include <string.h>

static void copy_utf8(char *out, size_t size, const char *text) {
  if (!text) text = "";
  size_t len = strlen(text);
  if (len >= size) {
    len = size - 1;
    while (len && (((unsigned char) text[len] & 0xc0) == 0x80)) len--;
  }
  memcpy(out, text, len);
  out[len] = '\0';
}

bool pet_p4_conversation_active(const pet_p4_runtime_state_t *s) {
  return s && s->conversation_state[0] && strcmp(s->conversation_state, "ended") != 0;
}

void pet_p4_conversation_clear(pet_p4_runtime_state_t *s) {
  if (!s) return;
  memset(s->conversation_state, 0, sizeof(s->conversation_state));
  memset(s->conversation_name, 0, sizeof(s->conversation_name));
  memset(s->conversation_text, 0, sizeof(s->conversation_text));
  s->conversation_id[0] = s->conversation_playback_id[0] = '\0';
  s->conversation_history_count = s->conversation_cue_count = 0;
  s->conversation_history_cursor = -1;
  s->conversation_error_until_ms = s->conversation_caption_since_ms = 0;
  s->conversation_user = false;
  memset(s->conversation_history, 0, sizeof(s->conversation_history));
  memset(s->conversation_cues, 0, sizeof(s->conversation_cues));
  s->last_update_ms++;
}

static void caption(pet_p4_runtime_state_t *s, const char *text, bool user, bool final,
  unsigned long long now_ms) {
  copy_utf8(s->conversation_text, sizeof(s->conversation_text), text);
  s->conversation_user = user;
  s->conversation_caption_since_ms = now_ms;
  s->conversation_history_cursor = -1;
  if (final && s->conversation_text[0]) {
    if (s->conversation_history_count == PET_P4_CONVERSATION_HISTORY_MAX) {
      memmove(s->conversation_history, s->conversation_history + 1,
        (PET_P4_CONVERSATION_HISTORY_MAX - 1) * sizeof(s->conversation_history[0]));
      s->conversation_history_count--;
    }
    pet_p4_conversation_caption_t *entry = &s->conversation_history[s->conversation_history_count++];
    copy_utf8(entry->text, sizeof(entry->text), s->conversation_text);
    entry->user = user;
  }
  s->last_update_ms++;
}

bool pet_p4_conversation_update(pet_p4_runtime_state_t *s, const char *id,
  const char *phase, const char *name, const char *text, bool user, bool final,
  unsigned long long now_ms) {
  if (!s || !id || !id[0] || strlen(id) >= sizeof(s->conversation_id) || !phase || !name || !text) return false;
  if (strcmp(phase, "preparing") && strcmp(phase, "listening") && strcmp(phase, "thinking")
      && strcmp(phase, "speaking") && strcmp(phase, "error") && strcmp(phase, "ended")) return false;
  if (!pet_p4_conversation_active(s) && strcmp(phase, "preparing") && strcmp(phase, "error")
      && strcmp(phase, "ended")) return false; // Late packets cannot reopen an ended conversation.
  if (!strcmp(phase, "preparing") && strcmp(s->conversation_id, id)) pet_p4_conversation_clear(s);
  if (s->conversation_id[0] && strcmp(s->conversation_id, id)) return false;
  if (!strcmp(phase, "ended")) { pet_p4_conversation_clear(s); return true; }
  copy_utf8(s->conversation_id, sizeof(s->conversation_id), id);
  copy_utf8(s->conversation_state, sizeof(s->conversation_state), phase);
  if (name[0]) copy_utf8(s->conversation_name, sizeof(s->conversation_name), name);
  s->conversation_error_until_ms = !strcmp(phase, "error") ? now_ms + 8000ULL : 0;
  if (!strcmp(phase, "error") || !strcmp(phase, "listening")) {
    s->conversation_cue_count = 0;
    memset(s->conversation_cues, 0, sizeof(s->conversation_cues));
  }
  caption(s, text, user, final, now_ms);
  return true;
}

void pet_p4_conversation_move(pet_p4_runtime_state_t *s, int direction, unsigned long long now_ms) {
  if (!pet_p4_conversation_active(s) || !s->conversation_history_count || !direction) return;
  int cursor = s->conversation_history_cursor;
  if (cursor < 0) {
    if (direction > 0) return;
    cursor = (int) s->conversation_history_count - 1;
    if (cursor > 0 && !strcmp(s->conversation_text, s->conversation_history[cursor].text)
        && s->conversation_user == s->conversation_history[cursor].user) cursor--;
  } else {
    cursor += direction < 0 ? -1 : 1;
    if (cursor < 0) cursor = 0;
    if (cursor >= (int) s->conversation_history_count) cursor = -1;
  }
  s->conversation_history_cursor = cursor;
  s->conversation_caption_since_ms = now_ms;
  s->last_update_ms++;
}

void pet_p4_conversation_playback_begin(pet_p4_runtime_state_t *s, const char *id) {
  s->conversation_cue_count = 0;
  memset(s->conversation_cues, 0, sizeof(s->conversation_cues));
  copy_utf8(s->conversation_playback_id, sizeof(s->conversation_playback_id), id);
}

bool pet_p4_conversation_queue_cue(pet_p4_runtime_state_t *s, const char *id,
  unsigned int offset_bytes, const char *text) {
  if (!pet_p4_conversation_active(s) || !id || !text || !text[0]
      || strcmp(s->conversation_playback_id, id) || (offset_bytes & 1)
      || s->conversation_cue_count >= PET_P4_CONVERSATION_CUES_MAX) return false;
  if (s->conversation_cue_count && offset_bytes < s->conversation_cues[s->conversation_cue_count - 1].offset_bytes) return false;
  pet_p4_conversation_cue_t *cue = &s->conversation_cues[s->conversation_cue_count++];
  cue->offset_bytes = offset_bytes;
  copy_utf8(cue->text, sizeof(cue->text), text);
  return true;
}

void pet_p4_conversation_advance(pet_p4_runtime_state_t *s, unsigned int played_bytes,
  bool playing, unsigned long long now_ms) {
  if (!pet_p4_conversation_active(s) || (!playing && !played_bytes)) return;
  while (s->conversation_cue_count && s->conversation_cues[0].offset_bytes <= played_bytes) {
    copy_utf8(s->conversation_state, sizeof(s->conversation_state), "speaking");
    caption(s, s->conversation_cues[0].text, false, true, now_ms);
    s->conversation_cue_count--;
    memmove(s->conversation_cues, s->conversation_cues + 1,
      s->conversation_cue_count * sizeof(s->conversation_cues[0]));
    memset(&s->conversation_cues[s->conversation_cue_count], 0, sizeof(s->conversation_cues[0]));
  }
}
