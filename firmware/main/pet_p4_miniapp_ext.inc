/* Track C capability-expansion hooks (p4-sequence/samples/clip/big-image/events).
 * Included by pet_p4_miniapp.c after every static it uses. Owns one mutex for the
 * device-wide event bus / hidden alarm table so it never nests inside the model lock. */
#if PET_P4_CAPABILITY_EXT
#include "freertos/semphr.h"
#include <stdatomic.h>
static _Atomic(SemaphoreHandle_t) g_ext_lock;
static void ext_lock(void) {
  SemaphoreHandle_t mutex = atomic_load(&g_ext_lock);
  if (!mutex) {
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex(); if (!candidate) abort();
    SemaphoreHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong(&g_ext_lock, &expected, candidate)) { vSemaphoreDelete(candidate); mutex = expected; }
    else mutex = candidate;
  }
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) abort();
}
static void ext_unlock(void) { xSemaphoreGive(atomic_load(&g_ext_lock)); }
#if PET_P4_EVENTS
static EXT_RAM_BSS_ATTR pet_cev_bus_t g_ext_bus;
typedef struct { char widget_id[PET_CEV_WIDGET_ID_MAX]; char title[64]; pet_cev_alarm_set_t alarms; uint64_t revision,serial; bool used,dirty; } ext_alarm_entry_t;
static EXT_RAM_BSS_ATTR ext_alarm_entry_t g_ext_alarms[PET_CEV_COMPONENTS];
static uint64_t g_ext_alarm_serial;
static bool miniapp_ext_hidden_checkpoint(void);
static bool g_ext_background_notify; /* PC-authorised per boot through component/alarm_policy */
static uint64_t g_ext_alarm_policy_revision;
static char g_ext_notice_id[PET_CEV_WIDGET_ID_MAX], g_ext_notice_alarm[PET_CEV_NAME_MAX];
static char g_ext_notice_title[64], g_ext_notice_text[PET_CEV_ALARM_TEXT_MAX];
static bool g_ext_notice_pending;
static ext_alarm_entry_t *ext_alarm_entry(const char *widget_id, bool create) {
  ext_alarm_entry_t *free_slot = NULL;
  for (unsigned i = 0; i < PET_CEV_COMPONENTS; i++) {
    if (g_ext_alarms[i].used && !strcmp(g_ext_alarms[i].widget_id, widget_id)) return &g_ext_alarms[i];
    if (!g_ext_alarms[i].used && !free_slot) free_slot = &g_ext_alarms[i];
  }
  if (!create || !free_slot || g_ext_alarm_serial==UINT64_MAX) return NULL;
  memset(free_slot, 0, sizeof(*free_slot)); copy_utf8(free_slot->widget_id, sizeof(free_slot->widget_id), widget_id); free_slot->used = true;free_slot->serial=++g_ext_alarm_serial;
  return free_slot;
}
/* Legacy trusted RAM setter retained; PC grants use the fallible worker hook. */
void pet_p4_miniapp_alarm_policy(bool background_notify) {
  ext_lock();
  if (g_ext_alarm_policy_revision != UINT64_MAX) { g_ext_alarm_policy_revision++; g_ext_background_notify = background_notify; }
  else if (!background_notify) g_ext_background_notify = false;
  if (!background_notify) { g_ext_notice_pending = false; g_ext_notice_id[0] = 0; }
  ext_unlock();
}
bool pet_p4_miniapp_alarm_notification(char title[64], char text[PET_CEV_ALARM_TEXT_MAX]) {
  ext_lock();
  bool pending = g_ext_notice_pending; g_ext_notice_pending = false; g_ext_notice_id[0] = 0;
  if (pending) { copy_utf8(title, 64, g_ext_notice_title); copy_utf8(text, PET_CEV_ALARM_TEXT_MAX, g_ext_notice_text); }
  ext_unlock();
  return pending;
}
/* Explicit user cancel only; no program execution. Caller checkpoints the affected
 * private save after success (hidden saves require the storage owner hook). */
bool pet_p4_miniapp_alarm_cancel(const char *widget_id, const char *alarm_id) {
  if (!widget_id || !alarm_id) return false;
  bool cancelled = false;
  miniapp_runtime_lock();
  ext_lock();
  if (g_runtime.active && !strcmp(g_runtime.widget_id, widget_id)) {
    pet_cev_alarm_set_t *set = &g_runtime.cext.alarms;
    for (unsigned i = 0; i < set->count; i++) if (!strcmp(set->items[i].id, alarm_id)) { pet_cev_alarms_cancel(set, i); cancelled = true; }
  }
  ext_alarm_entry_t *entry = ext_alarm_entry(widget_id, false);
  if (entry && entry->revision!=UINT64_MAX) for (unsigned i = 0; i < entry->alarms.count; i++) if (!strcmp(entry->alarms.items[i].id, alarm_id)) { pet_cev_alarms_cancel(&entry->alarms, i); entry->dirty=true;entry->revision++;cancelled = true; }
  if (!strcmp(g_ext_notice_id, widget_id) && !strcmp(g_ext_notice_alarm, alarm_id)) { g_ext_notice_pending = false; g_ext_notice_id[0] = 0; }
  ext_unlock();
  miniapp_runtime_unlock();
  if(cancelled)(void)miniapp_ext_hidden_checkpoint();
  return cancelled;
}
#endif
static int ext_find_int_var(void *ctx, const char *name) {
  const miniapp_runtime_t *r = ctx; int v = find_var(r, name);
  return v >= 0 && r->vars[v].type == MINIAPP_VAR_INT ? v : -1;
}
/* Alarm actions are checked against the raw transitions array: miniapp_ext_parse runs
 * after sfx and before parse_transitions so transition ops can reference the profiles. */
static const cJSON *g_ext_parse_transitions;
static bool ext_has_action_raw(void *ctx, const char *action) {
  (void) ctx; const cJSON *t;
  if (!action || !action[0] || !g_ext_parse_transitions) return false;
  cJSON_ArrayForEach(t, g_ext_parse_transitions) if (!strcmp(json_string(t, "on"), action)) return true;
  return false;
}
static bool miniapp_ext_parse(miniapp_runtime_t *parsed, const cJSON *root, bool runtime_v4, char *error, size_t error_size) {
  pet_cext_parse_env_t env = {
    .runtime_v4 = runtime_v4,
    .exclusive_audio_owner = cJSON_HasObjectItem(root, "instrument") || cJSON_HasObjectItem(root, "media") || cJSON_HasObjectItem(root, "local") || cJSON_HasObjectItem(root, "program"),
    .persist = cJSON_HasObjectItem(root, "persist"),
    .surface_app = !strcmp(json_string(cJSON_GetObjectItemCaseSensitive(root, "surface"), "engine"), "p4-app-surface-v1"),
    .ctx = parsed, .find_int_var = ext_find_int_var, .has_action = ext_has_action_raw,
  };
  pet_cext_runtime_t *ext = &parsed->cext;
  g_ext_parse_transitions = cJSON_GetObjectItemCaseSensitive(root, "transitions");
  bool ok = pet_cext_parse(root, ext, &env, error, error_size);
  g_ext_parse_transitions = NULL;
  if (!ok) return false;
#if !PET_P4_SEQUENCE
  if (ext->sequence) { set_error(error, error_size, "sequence capability is unavailable"); return false; }
#endif
#if !PET_P4_SAMPLES
  if (ext->samples) { set_error(error, error_size, "samples capability is unavailable"); return false; }
#endif
#if !PET_P4_EVENTS
  if (ext->events || ext->alarms.count) { set_error(error, error_size, "events capability is unavailable"); return false; }
#endif
  const cJSON *surface = cJSON_GetObjectItemCaseSensitive(root, "surface");
  if (surface && !pet_cext_parse_surface_media(surface, ext, error, error_size)) return false;
#if !PET_P4_CLIP
  if (ext->clip) { set_error(error, error_size, "clip capability is unavailable"); return false; }
#endif
#if !PET_P4_BIG_IMAGE
  if (ext->big) { set_error(error, error_size, "big image capability is unavailable"); return false; }
#endif
  if ((ext->clip || ext->big) && !env.surface_app) { set_error(error, error_size, "clip and big image nodes need the app surface"); return false; }
  return true;
}
static bool miniapp_ext_validate_bindings(const miniapp_runtime_t *runtime, const cJSON *root, const char *buttons_json, char *error, size_t size) {
  if (!runtime->cext.events && !runtime->cext.alarms.count) return true;
  cJSON *buttons = cJSON_ParseWithOpts(buttons_json && buttons_json[0] ? buttons_json : "[]", NULL, true);
  bool ok = pet_cext_binding_coverage(&runtime->cext, root, buttons, error, size);
  cJSON_Delete(buttons); return ok;
}
static void miniapp_ext_fire(miniapp_runtime_t *runtime, const pet_cext_ops_t *ops) { pet_cext_fire(&runtime->cext, ops); }
#include "pet_p4_component_media_clients.h"
static bool g_ext_media_transition; /* protected by ext; no stale audio during commit */
/* Generation is copied by pet_cext_audio_take while the runtime lock is held. */
static void miniapp_ext_audio_apply(const pet_cext_audio_t *audio, const pet_seq_program_t *program) {
  miniapp_ext_audio_apply_snapshot(audio, program, audio->generation);
}
void miniapp_ext_audio_apply_snapshot(const pet_cext_audio_t *audio, const pet_seq_program_t *program, uint32_t generation) {
#if PET_P4_SEQUENCE || PET_P4_SAMPLES
  ext_lock();
  if (g_ext_media_transition || !generation || !pet_cmedia_generation_try(generation)) { ext_unlock(); return; }
  pet_p4_audio_cext_set(audio->enabled, program, audio->sequence_active, audio->sequence_volume, audio->samples_volume);
  for (unsigned i = 0; audio->sample_pending && i < PET_SAMPLE_MAX_SLOTS; i++) if (audio->sample_pending & (1u << i)) {
    pet_cmedia_sample_lease_t lease = {0};
    if (pet_cmedia_sample_pin_try_at(i, generation, &lease)) {
      (void)pet_p4_audio_cext_sample_lease(&lease);
      (void)pet_cmedia_unpin_deferred(lease.token);
    }
  }
  ext_unlock();
#else
  (void) generation; (void) audio; (void) program;
#endif
}
#if PET_P4_EVENTS
/* Alarm bookkeeping for the active runtime (model lock held by the caller). */
static void ext_alarms_observe_locked(uint64_t now_ms,const pet_cev_clock_snapshot_t *clock) {
  pet_cev_alarm_set_t *set = &g_runtime.cext.alarms;
  for (unsigned i = 0; i < set->count; i++) if (set->items[i].at_var >= 0 && (unsigned)set->items[i].at_var < (unsigned)g_runtime.var_count)
    pet_cev_alarms_observe_var_clock(set, i, g_runtime.vars[set->items[i].at_var].int_value, now_ms,clock);
  pet_cev_alarms_start_clock(set, now_ms,clock);
}
#endif
static void miniapp_ext_process(uint64_t now_ms, bool visible) {
  (void)pet_cmedia_collect_try(); /* busy writer: retry on the next main cycle */
#if PET_P4_EVENTS
  char actions[PET_CEV_QUEUE + PET_CEV_ALARMS][PET_P4_MINIAPP_ACTION_MAX]; unsigned action_count = 0;
  char emits[PET_CEV_NAMES_PER_PACKAGE][PET_CEV_NAME_MAX]; unsigned emit_count = 0;
  char active_id[PET_CEV_WIDGET_ID_MAX] = {0};
  pet_cev_clock_snapshot_t clock=miniapp_utc_snapshot(now_ms);
  miniapp_runtime_lock();
  if (g_runtime.active && g_runtime.cext.events) {
    copy_utf8(active_id, sizeof(active_id), g_runtime.widget_id);
    for (unsigned i = 0; i < g_runtime.cext.emit_count; i++) if (g_runtime.cext.emit_pending & (1u << i)) copy_utf8(emits[emit_count++], PET_CEV_NAME_MAX, g_runtime.cext.emit_names[i]);
    g_runtime.cext.emit_pending = 0;
    ext_alarms_observe_locked(now_ms,&clock);
    (void) pet_cev_alarms_tick_clock(&g_runtime.cext.alarms, now_ms,&clock);
    if (!visible) {
      ext_lock();
      for (unsigned i = 0; i < g_runtime.cext.alarms.count; i++) {
        pet_cev_alarm_t *alarm = &g_runtime.cext.alarms.items[i];
        if (alarm->pending && !alarm->waiting_for_clock && alarm->background && g_ext_background_notify && !g_ext_notice_pending) {
          copy_utf8(g_ext_notice_title, sizeof(g_ext_notice_title), g_runtime.view.title);
          copy_utf8(g_ext_notice_text, sizeof(g_ext_notice_text), alarm->text);
          copy_utf8(g_ext_notice_id, sizeof(g_ext_notice_id), g_runtime.widget_id);
          copy_utf8(g_ext_notice_alarm, sizeof(g_ext_notice_alarm), alarm->id);
          g_ext_notice_pending = true; alarm->pending = false;
        }
      }
      ext_unlock();
    }
    if (visible) {
      unsigned due = pet_cev_alarms_take_pending(&g_runtime.cext.alarms);
      for (unsigned i = 0; i < g_runtime.cext.alarms.count; i++) if (due & (1u << i)) copy_utf8(actions[action_count++], PET_P4_MINIAPP_ACTION_MAX, g_runtime.cext.alarms.items[i].action);
    }
  }
  miniapp_runtime_unlock();
  ext_lock();
  for (unsigned i = 0; i < emit_count; i++) (void) pet_cev_bus_emit(&g_ext_bus, emits[i]);
  if (active_id[0] && visible) {
    char name[PET_CEV_NAME_MAX];
    while (action_count < PET_CEV_QUEUE + PET_CEV_ALARMS && pet_cev_bus_pop(&g_ext_bus, active_id, name)) snprintf(actions[action_count++], PET_P4_MINIAPP_ACTION_MAX, "event:%s", name);
  }
  /* Hidden components: tick their table copy; background alarms raise one bubble each. */
  for (unsigned i = 0; i < PET_CEV_COMPONENTS; i++) {
    ext_alarm_entry_t *e = &g_ext_alarms[i];
    if (!e->used || (active_id[0] && !strcmp(e->widget_id, active_id))) continue;
    uint32_t prior=e->alarms.clock_changes;
    (void)pet_cev_alarms_tick_clock(&e->alarms, now_ms,&clock);
    if(prior!=e->alarms.clock_changes&&e->revision!=UINT64_MAX){e->dirty=true;e->revision++;}
    for (unsigned a = 0; a < e->alarms.count; a++) if (e->alarms.items[a].pending && !e->alarms.items[a].waiting_for_clock && e->alarms.items[a].background && g_ext_background_notify && !g_ext_notice_pending) {
      copy_utf8(g_ext_notice_title, sizeof(g_ext_notice_title), e->title); copy_utf8(g_ext_notice_text, sizeof(g_ext_notice_text), e->alarms.items[a].text); copy_utf8(g_ext_notice_id, sizeof(g_ext_notice_id), e->widget_id); copy_utf8(g_ext_notice_alarm, sizeof(g_ext_notice_alarm), e->alarms.items[a].id); g_ext_notice_pending = true; e->alarms.items[a].pending = false;if(e->revision!=UINT64_MAX){e->dirty=true;e->revision++;}
    }
  }
  ext_unlock();
  for (unsigned i = 0; i < action_count; i++) (void) pet_p4_miniapp_dispatch_action(actions[i], now_ms);
#else
  (void) now_ms; (void) visible;
#endif
}
static pet_cext_publish_ticket_t *g_ext_pending_publish;
static enum { EXT_PUBLISH_IDLE, EXT_PUBLISH_PREPARING, EXT_PUBLISH_READY, EXT_PUBLISH_FINISHING } g_ext_publish_phase;
/* Worker caller owns its ticket until completion. Bus reservation is bounded;
 * no media/store call, allocation, stream wait or model lock while ext is held. */
static bool miniapp_ext_prepare(miniapp_runtime_t *next, size_t slot, uint8_t generation, pet_cext_publish_ticket_t *ticket) {
  if (!ticket) return false;
  ext_lock();
  if (g_ext_pending_publish || ticket->media.serial || ticket->next) { ext_unlock(); return false; }
  g_ext_pending_publish = ticket; g_ext_publish_phase = EXT_PUBLISH_PREPARING;
  ext_unlock();
  /* Original blocking worker preparation, never under the periodic bus lock. */
  bool prepared = pet_cmedia_prepare(slot, generation, next ? &next->cext : NULL, &ticket->media);
  ext_lock();
  bool reserved = g_ext_pending_publish == ticket && g_ext_publish_phase == EXT_PUBLISH_PREPARING;
  if (!reserved || !prepared) {
    if (reserved) { g_ext_pending_publish = NULL; g_ext_publish_phase = EXT_PUBLISH_IDLE; }
    ext_unlock();
    if (prepared) (void)pet_cmedia_abort(&ticket->media);
    return false;
  }
  ticket->next = next; g_ext_publish_phase = EXT_PUBLISH_READY;
  ext_unlock(); return true;
}
static bool miniapp_ext_abort(pet_cext_publish_ticket_t *ticket) {
  ext_lock();
  if (!ticket || g_ext_pending_publish != ticket || g_ext_publish_phase != EXT_PUBLISH_READY) { ext_unlock(); return false; }
  g_ext_publish_phase = EXT_PUBLISH_FINISHING;
  ext_unlock();
  bool aborted = pet_cmedia_abort(&ticket->media);
  ext_lock();
  if (g_ext_pending_publish != ticket || g_ext_publish_phase != EXT_PUBLISH_FINISHING) { ext_unlock(); return false; }
  if (aborted) { ticket->next = NULL; g_ext_pending_publish = NULL; g_ext_publish_phase = EXT_PUBLISH_IDLE; }
  else g_ext_publish_phase = EXT_PUBLISH_READY;
  ext_unlock(); return aborted;
}
static bool miniapp_ext_commit(pet_cext_publish_ticket_t *ticket) {
#if PET_P4_EVENTS
  uint64_t now_ms = esp_timer_get_time() / 1000ULL;
#endif
  ext_lock();
  if (!ticket || g_ext_pending_publish != ticket || g_ext_publish_phase != EXT_PUBLISH_READY) { ext_unlock(); return false; }
  g_ext_publish_phase = EXT_PUBLISH_FINISHING; g_ext_media_transition = true;
  miniapp_runtime_t *next = ticket->next;
  ext_unlock();
  /* Blocking store/stream operations are worker-only and outside ext/model. */
  bool committed = pet_cmedia_commit(&ticket->media);
  if (committed) {
    if (next) next->cext.media_generation = pet_cmedia_generation();
#if PET_P4_SEQUENCE || PET_P4_SAMPLES
    pet_p4_audio_cext_set(false, NULL, false, 0, 0);
#endif
    pet_cmedia_collect();
  }
  ext_lock();
  if (g_ext_pending_publish != ticket || g_ext_publish_phase != EXT_PUBLISH_FINISHING) { ext_unlock(); return false; }
  g_ext_media_transition = false;
  if (!committed) { g_ext_publish_phase = EXT_PUBLISH_READY; ext_unlock(); return false; }
  ticket->next = NULL; g_ext_pending_publish = NULL; g_ext_publish_phase = EXT_PUBLISH_IDLE;
  ext_unlock();
#if PET_P4_EVENTS
  /* Park the outgoing runtime's alarms so they keep running hidden, adopt the incoming one's. */
  miniapp_runtime_lock();
  char outgoing[PET_CEV_WIDGET_ID_MAX] = {0}; pet_cev_alarm_set_t parked = {0}; char title[64] = {0};
  bool park = g_runtime.active && g_runtime.cext.alarms.count && (!next || strcmp(g_runtime.widget_id, next->widget_id));
  if (park) { copy_utf8(outgoing, sizeof(outgoing), g_runtime.widget_id); copy_utf8(title, sizeof(title), g_runtime.view.title); parked = g_runtime.cext.alarms; }
  miniapp_runtime_unlock();
  ext_lock();
  if (park) { ext_alarm_entry_t *e = ext_alarm_entry(outgoing, true); if (e) { e->alarms = parked;e->dirty=true;if(e->revision!=UINT64_MAX)e->revision++; copy_utf8(e->title, sizeof(e->title), title); } }
  if (next && next->cext.events) {
    (void) pet_cev_bus_subscribe(&g_ext_bus, next->widget_id, next->cext.sub_names, next->cext.sub_count);
    ext_alarm_entry_t *e = ext_alarm_entry(next->widget_id, false);
    if (e && next->cext.alarms.count) {
      /* RAM state is fresher than the restored save: keep ids/actions from the package. */
      for (unsigned i = 0; i < next->cext.alarms.count; i++) for (unsigned j = 0; j < e->alarms.count; j++)
        if (!strcmp(next->cext.alarms.items[i].id, e->alarms.items[j].id)) { pet_cev_alarm_t *a = &next->cext.alarms.items[i]; const pet_cev_alarm_t *s = &e->alarms.items[j]; a->armed = s->armed; a->pending = s->pending; a->deadline_ms = s->deadline_ms; a->last_var = s->last_var;a->utc_deadline_ms=s->utc_deadline_ms;a->waiting_ms=s->waiting_ms;a->utc_deadline=s->utc_deadline;a->waiting_for_clock=s->waiting_for_clock;a->cancelled=s->cancelled; }
      e->used = false;
    }
    pet_cev_clock_snapshot_t clock=miniapp_utc_snapshot(now_ms);
    pet_cev_alarms_start_clock(&next->cext.alarms, now_ms,&clock);
  }
  ext_unlock();
#endif
  return true;
}
static bool miniapp_ext_publish(miniapp_runtime_t *next, size_t slot, uint8_t generation) {
  pet_cext_publish_ticket_t ticket = {0};
  return miniapp_ext_prepare(next, slot, generation, &ticket) && miniapp_ext_commit(&ticket);
}
static void miniapp_ext_installed(const miniapp_runtime_t *next) {
#if PET_P4_EVENTS
  if (!next || !next->cext.events) return;
  ext_lock(); (void) pet_cev_bus_subscribe(&g_ext_bus, next->widget_id, next->cext.sub_names, next->cext.sub_count); ext_unlock();
#else
  (void) next;
#endif
}
static void miniapp_ext_removed(const char *widget_id, size_t slot) {
#if PET_P4_EVENTS
  ext_lock();
  if (!strcmp(g_ext_notice_id, widget_id)) { g_ext_notice_pending = false; g_ext_notice_id[0] = 0; }
  pet_cev_bus_unsubscribe(&g_ext_bus, widget_id);
  ext_alarm_entry_t *e = ext_alarm_entry(widget_id, false); if (e) memset(e, 0, sizeof(*e));
  ext_unlock();
#else
  (void) widget_id;
#endif
  pet_cmedia_remove(slot);
}
static bool miniapp_ext_install_chunk(const cJSON *payload, const char *path, const char *data, long index, char *error, size_t error_size) {
  char id[PET_CEXT_ID_MAX];
  pet_cext_media_kind_t kind = pet_cext_media_path_kind(path, id);
  if (kind == PET_CEXT_MEDIA_NONE) { set_error(error, error_size, "unsupported component media path"); return false; }
  size_t encoded = strlen(data), capacity = encoded / 4 * 3 + 3, decoded_size = 0;
  if (capacity > 16384) { set_error(error, error_size, "component media chunk is too large"); return false; }
  uint8_t *decoded = malloc(capacity);
  if (!decoded || mbedtls_base64_decode(decoded, capacity, &decoded_size, (const unsigned char *) data, encoded) != 0 || !decoded_size) {
    free(decoded); set_error(error, error_size, "component media chunk is not valid base64"); return false;
  }
  bool ok = verify_chunk_integrity(payload, decoded, decoded_size, error, error_size)
    && pet_cmedia_stage_chunk(kind, id, (uint32_t) index, decoded, decoded_size, error, error_size);
  free(decoded);
  return ok;
}
#if PET_P4_EVENTS
/* Storage-owner checkpoint of one parked component. Only RAM save IO is
 * enqueued, never a hidden runtime/action/program. Workspaces are per-call,
 * allocated outside model/ext locks; stale snapshots cannot overwrite cancel. */
typedef struct {char id[PET_CEV_WIDGET_ID_MAX],alarms[PET_CEV_SAVE_TEXT_MAX],json[PET_APP_SAVE_BYTES];uint64_t revision,serial;} ext_alarm_checkpoint_t;
static unsigned g_ext_checkpoint_cursor;
static bool miniapp_ext_hidden_checkpoint(void){
  bool pending=false;ext_lock();for(unsigned i=0;i<PET_CEV_COMPONENTS;i++)pending|=g_ext_alarms[i].used&&g_ext_alarms[i].dirty;ext_unlock();
  if(!pending)return true;
  ext_alarm_checkpoint_t *w=heap_caps_calloc(1,sizeof(*w),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!w)return false;
  uint64_t now=esp_timer_get_time()/1000ULL;pet_cev_clock_snapshot_t clock=miniapp_utc_snapshot(now);
  bool captured=false;unsigned slot=0;ext_lock();
  for(unsigned n=0;n<PET_CEV_COMPONENTS;n++){
    unsigned i=(g_ext_checkpoint_cursor+n)%PET_CEV_COMPONENTS;ext_alarm_entry_t *e=&g_ext_alarms[i];
    if(!e->used||!e->dirty)continue;
    slot=i;g_ext_checkpoint_cursor=(i+1)%PET_CEV_COMPONENTS;copy_utf8(w->id,sizeof(w->id),e->widget_id);w->revision=e->revision;w->serial=e->serial;
    captured=pet_cev_alarms_serialize_clock(&e->alarms,now,&clock,w->alarms,sizeof(w->alarms))>0;break;
  }
  ext_unlock();
  bool accepted=false;cJSON *saved=NULL;
  if(captured&&pet_app_store_read_cached(w->id,w->json)&&pet_form_source_valid(w->json)){
    saved=cJSON_ParseWithOpts(w->json,NULL,true);
    if(cJSON_IsObject(saved)&&cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(saved,"version"))&&cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(saved,"vars"))){
      cJSON *alarms=cJSON_CreateString(w->alarms);
      if(alarms){
        if(cJSON_HasObjectItem(saved,"alarms"))cJSON_ReplaceItemInObjectCaseSensitive(saved,"alarms",alarms);else cJSON_AddItemToObject(saved,"alarms",alarms);
        if(cJSON_PrintPreallocated(saved,w->json,sizeof(w->json),false)){
          ext_lock();ext_alarm_entry_t *e=&g_ext_alarms[slot];
          if(e->used&&e->serial==w->serial&&e->revision==w->revision&&!strcmp(e->widget_id,w->id)){
            accepted=pet_app_store_update(w->id,w->json);if(accepted)e->dirty=false;
          }
          ext_unlock();
        }
      }
    }
  }
  cJSON_Delete(saved);heap_caps_free(w);return accepted;
}
#endif
static bool miniapp_ext_save_text(char *out, size_t cap) {
#if PET_P4_EVENTS
  uint64_t now_ms = esp_timer_get_time() / 1000ULL;
  pet_cev_clock_snapshot_t clock=miniapp_utc_snapshot(now_ms);
  return g_runtime.cext.alarms.count && pet_cev_alarms_serialize_clock(&g_runtime.cext.alarms, now_ms,&clock,out,cap)>0;
#else
  (void) out; (void) cap; return false;
#endif
}
static bool miniapp_ext_restore_text(miniapp_runtime_t *r, const cJSON *saved) {
#if PET_P4_EVENTS
  if (!saved) return true;
  uint64_t now=esp_timer_get_time()/1000ULL;pet_cev_clock_snapshot_t clock=miniapp_utc_snapshot(now);
  return cJSON_IsString(saved) && r->cext.alarms.count && pet_cev_alarms_restore_clock(&r->cext.alarms,saved->valuestring,now,&clock);
#else
  (void) r; return saved == NULL;
#endif
}
#if PET_P4_EVENTS
#include "pet_p4_miniapp_alarm_boot.inc"
#endif
#endif
