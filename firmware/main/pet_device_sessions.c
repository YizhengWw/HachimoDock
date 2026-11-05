/* Bounded, platform-independent subset of the HachimoDock session contract.
 * P4 retains its extended dispatcher; new boards share this small model.
 */
#include "pet_device_sessions.h"
#include "pet_device_lifecycle.h"
#include <math.h>
#include <string.h>

const char *pet_device_json_string(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
void pet_device_text_copy(char *dst, size_t cap, const char *src) {
  if (!cap) return;
  if (!src) src = "";
  size_t n = strlen(src);
  if (n >= cap) {
    n = cap - 1;
    while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
  }
  memcpy(dst, src, n); dst[n] = 0;
}
static uint64_t number(const cJSON *obj, const char *key, uint64_t max) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
      v->valuedouble < 0 || v->valuedouble > (double)max) return 0;
  uint64_t n = (uint64_t)v->valuedouble;
  return (double)n == v->valuedouble ? n : 0;
}
static bool terminal(const char *state) {
  const char *s = pet_device_canonical_lifecycle(state);
  return !strcmp(s,"done") || !strcmp(s,"error") || !strcmp(s,"stopped");
}
static bool active(const char *state) {
  const char *s = pet_device_canonical_lifecycle(state);
  return !strcmp(s,"working") || !strcmp(s,"waiting_user") || !strcmp(s,"speaking");
}
static const char *first_string(const cJSON *p, const char *a, const char *b, const char *c) {
  const char *value = pet_device_json_string(p,a);
  if (!value[0]) value = pet_device_json_string(p,b);
  if (!value[0]) value = pet_device_json_string(p,c);
  return value;
}
static void update_card_state(pet_device_session_t *card, const char *state, uint64_t now) {
  if (!state[0]) return;
  const char *life = pet_device_canonical_lifecycle(state);
  if (!active(life) && !terminal(life)) return;
  if (terminal(life)) {
    if (!terminal(card->state)) card->terminal_until_ms = now+PET_DEVICE_TERMINAL_MS;
  } else card->terminal_until_ms = 0;
  pet_device_text_copy(card->state,sizeof(card->state),life);
}
static int find(const pet_device_session_t *cards, unsigned count, const char *id) {
  for (unsigned i = 0; i < count; ++i) if (!strcmp(cards[i].id,id)) return (int)i;
  return -1;
}
static void snapshot(pet_device_sessions_t *m, const cJSON *p, uint64_t now) {
  const char *agent = pet_device_json_string(p,"agentId");
  bool changed = strcmp(m->agent,agent) != 0;
  char selected[128];
  pet_device_text_copy(selected,sizeof(selected),pet_device_json_string(p,"sessionId"));
  if ((!selected[0] || !strcmp(selected,"auto")) && !changed && m->count)
    pet_device_text_copy(selected,sizeof(selected),m->sessions[m->selected].id);
  if (changed) {
    m->count = m->selected = 0;
    m->title[0] = m->content[0] = 0;
    pet_device_text_copy(m->state,sizeof(m->state),"idle");
  }
  pet_device_text_copy(m->agent,sizeof(m->agent),agent);
  m->snapshot_seen = true; m->last_snapshot_ms = now;
  unsigned count = 0;
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(p,"sessions");
  if (cJSON_IsArray(items) && !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"displayEnabled"))) {
    const cJSON *item;
    cJSON_ArrayForEach(item,items) {
      if (count == PET_DEVICE_SESSION_MAX) break;
      const char *id = pet_device_json_string(item,"id");
      const char *title = pet_device_json_string(item,"title");
      const char *state = pet_device_canonical_lifecycle(pet_device_json_string(item,"state"));
      if (!id[0] || strlen(id) >= sizeof(m->staging[0].id) || !title[0] ||
          (!active(state) && !terminal(state)) || find(m->staging,count,id) >= 0) continue;
      int old = find(m->sessions,m->count,id);
      pet_device_session_t *next = &m->staging[count];
      memset(next,0,sizeof(*next));
      pet_device_text_copy(next->id,sizeof(next->id),id);
      pet_device_text_copy(next->title,sizeof(next->title),title);
      pet_device_text_copy(next->content,sizeof(next->content),pet_device_json_string(item,"content"));
      pet_device_text_copy(next->state,sizeof(next->state),state);
      next->revision = number(item,"transitionRevision",9007199254740991ULL);
      if (old >= 0) {
        const pet_device_session_t *previous = &m->sessions[old];
        if ((next->revision && previous->revision > next->revision) ||
            (terminal(previous->state) && next->revision == previous->revision)) {
          *next = *previous;
        } else if (terminal(state)) {
          uint64_t ttl = number(item,"terminalRemainingMs",PET_DEVICE_TERMINAL_MS);
          next->terminal_until_ms = now + (next->revision ? ttl : PET_DEVICE_TERMINAL_MS);
        }
      } else if (terminal(state)) {
        // Cold snapshots may only restore explicitly revisioned, unexpired cards.
        uint64_t ttl = number(item,"terminalRemainingMs",PET_DEVICE_TERMINAL_MS);
        if (!next->revision || !ttl) continue;
        next->terminal_until_ms = now + ttl;
      }
      if (terminal(next->state) && next->terminal_until_ms <= now) continue;
      ++count;
    }
    // Keep first-seen ordering across periodic snapshots.
    for (unsigned old = 0, at = 0; old < m->count; ++old) {
      int i = find(m->staging,count,m->sessions[old].id);
      if (i >= 0) {
        pet_device_session_t card = m->staging[i];
        memmove(&m->staging[at+1],&m->staging[at],((unsigned)i-at)*sizeof(card));
        m->staging[at++] = card;
      }
    }
  }
  memcpy(m->sessions,m->staging,count*sizeof(m->sessions[0]));
  m->count = count;
  int selected_index = find(m->sessions,count,selected);
  m->selected = selected_index < 0 ? 0 : (unsigned)selected_index;
  if (!count && (!active(m->state) || changed ||
      cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"displayEnabled")))) {
    pet_device_text_copy(m->state,sizeof(m->state),"idle");
    m->title[0] = m->content[0] = 0;
  }
}
bool pet_device_sessions_handle(pet_device_sessions_t *m, const char *topic,
                                const cJSON *p, uint64_t now) {
  if (!m || !topic || !cJSON_IsObject(p)) return false;
  if (!strcmp(topic,"session/current")) {
    if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(p,"sessions")) ||
        strlen(pet_device_json_string(p,"agentId")) >= sizeof(m->agent)) return false;
    snapshot(m,p,now);
  }
  else if (!strncmp(topic,"state/",6)) {
    if (strcmp(m->agent,topic+6)) {
      m->count = m->selected = 0;
      m->title[0] = m->content[0] = 0;
    }
    if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"displayEnabled"))) {
      m->count = m->selected = 0; m->title[0] = m->content[0] = 0;
      pet_device_text_copy(m->state,sizeof(m->state),"idle");
    } else {
      const char *state = first_string(p,"status","state","lifecycle");
      pet_device_text_copy(m->state,sizeof(m->state),
        pet_device_canonical_lifecycle(state[0] ? state : "working"));
      pet_device_text_copy(m->agent,sizeof(m->agent),topic+6);
      int i = find(m->sessions,m->count,pet_device_json_string(p,"sessionId"));
      if (i >= 0) update_card_state(&m->sessions[i],state,now);
      if (!strcmp(topic+6,"deepseek-harness")) {
        pet_device_text_copy(m->title,sizeof(m->title),first_string(p,"title","sessionTitle","displayTitle"));
        m->content[0] = 0;
      }
    }
  } else if (!strcmp(topic,"speech/text")) {
    const char *id = pet_device_json_string(p,"sessionId");
    const char *content = first_string(p,"displayContent","text","message");
    const char *title = first_string(p,"displayTitle","sessionName","title");
    const char *status = pet_device_json_string(p,"status");
    int i = find(m->sessions,m->count,id);
    if (i >= 0) {
      if (content[0]) pet_device_text_copy(m->sessions[i].content,sizeof(m->sessions[i].content),content);
      if (title[0]) pet_device_text_copy(m->sessions[i].title,sizeof(m->sessions[i].title),title);
      update_card_state(&m->sessions[i],status,now);
    } else if (!id[0] && !m->count) {
      pet_device_text_copy(m->title,sizeof(m->title),title);
      pet_device_text_copy(m->content,sizeof(m->content),content);
      if (status[0]) pet_device_text_copy(m->state,sizeof(m->state),pet_device_canonical_lifecycle(status));
    }
  } else return false;
  m->host_seen = true; m->last_host_ms = now;
  return true;
}
void pet_device_sessions_tick(pet_device_sessions_t *m, uint64_t now) {
  char selected[128] = "";
  if (m->count) pet_device_text_copy(selected,sizeof(selected),m->sessions[m->selected].id);
  unsigned n = 0;
  for (unsigned i = 0; i < m->count; ++i) {
    pet_device_session_t *card = &m->sessions[i];
    bool expired = terminal(card->state) ? now >= card->terminal_until_ms :
      (m->snapshot_seen && now-m->last_snapshot_ms >= PET_DEVICE_SESSION_TIMEOUT_MS);
    if (!expired) m->sessions[n++] = *card;
  }
  bool cleared = m->count && !n;
  m->count = n;
  int i = find(m->sessions,n,selected);
  m->selected = i < 0 ? 0 : (unsigned)i;
  if (cleared || (!n && m->host_seen && now-m->last_host_ms >= PET_DEVICE_SESSION_TIMEOUT_MS)) {
    pet_device_text_copy(m->state,sizeof(m->state),"idle");
    m->title[0] = m->content[0] = 0;
  }
}
const pet_device_session_t *pet_device_sessions_selected(const pet_device_sessions_t *m) {
  return m->count ? &m->sessions[m->selected] : NULL;
}
bool pet_device_sessions_select(pet_device_sessions_t *m, int direction) {
  if (!m->count) return false;
  m->selected = (m->selected + m->count + (direction < 0 ? -1 : 1)) % m->count;
  return true;
}
const char *pet_device_sessions_lifecycle(const pet_device_sessions_t *m) {
  const pet_device_session_t *card = pet_device_sessions_selected(m);
  return card ? card->state : m->state[0] ? m->state : "idle";
}
