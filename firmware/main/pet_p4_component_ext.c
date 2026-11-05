/* Track C capability-expansion parser/state core; see pet_p4_component_ext.h. */
#include "pet_p4_component_ext.h"
#include <stdio.h>
#include <string.h>
static void fail(char *error, size_t size, const char *message) { if (error && size) snprintf(error, size, "%s", message); }
static const char *str(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
static bool integer(const cJSON *o, const char *k, long lo, long hi, long *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  if (!cJSON_IsNumber(v) || v->valuedouble != (double) v->valueint || v->valueint < lo || v->valueint > hi) return false;
  *out = v->valueint; return true;
}
static bool keys_allowed(const cJSON *o, const char *const *allowed, size_t count) {
  if (!cJSON_IsObject(o)) return false;
  for (const cJSON *c = o->child; c; c = c->next) {
    bool ok = false;
    for (size_t i = 0; i < count && !ok; i++) ok = c->string && !strcmp(c->string, allowed[i]);
    if (!ok) return false;
  }
  return true;
}
static bool safe_id(const char *v, size_t max) {
  if (!v || !v[0] || strlen(v) >= max) return false;
  for (const char *p = v; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
  return true;
}
/* "assets/<id>.<ext>" -> id (basename without extension, <=15 safe chars). */
static bool asset_id(const char *asset, const char *ext, char out[PET_CEXT_ID_MAX]) {
  if (!asset || strncmp(asset, "assets/", 7)) return false;
  const char *name = asset + 7; size_t len = strlen(name), elen = strlen(ext);
  if (len <= elen || strcmp(name + len - elen, ext) || len - elen >= PET_CEXT_ID_MAX || strchr(name, '/')) return false;
  memcpy(out, name, len - elen); out[len - elen] = 0;
  return safe_id(out, PET_CEXT_ID_MAX);
}
static int find_id(const char (*ids)[PET_CEXT_ID_MAX], size_t stride_unused, unsigned count, const char *id) {
  (void) stride_unused;
  for (unsigned i = 0; i < count; i++) if (!strcmp(ids[i], id)) return (int) i;
  return -1;
}
static bool parse_sequence(const cJSON *seq, pet_cext_runtime_t *out, const pet_cext_parse_env_t *env, char *error, size_t size) {
  static const char *const keys[] = {"engine", "bpm", "volume", "tracks", "loop"};
  static const char *const track_keys[] = {"wave", "volume", "notes"};
  long bpm, volume;
  if (!env->runtime_v4 || env->exclusive_audio_owner || !keys_allowed(seq, keys, 5) || strcmp(str(seq, "engine"), PET_CEXT_SEQUENCE_ENGINE)
      || !integer(seq, "bpm", PET_SEQ_BPM_MIN, PET_SEQ_BPM_MAX, &bpm) || !integer(seq, "volume", 0, 100, &volume)) {
    fail(error, size, "sequence requires p4-bounded-runtime-v4, p4-sequence-v1, bpm 40-240 and volume 0-100 without instrument/media/local/program"); return false;
  }
  const cJSON *loop = cJSON_GetObjectItemCaseSensitive(seq, "loop"), *tracks = cJSON_GetObjectItemCaseSensitive(seq, "tracks"), *track;
  if (loop && !cJSON_IsBool(loop)) { fail(error, size, "sequence loop must be a boolean"); return false; }
  if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) < 1 || cJSON_GetArraySize(tracks) > (int) PET_SEQ_TRACKS) { fail(error, size, "sequence needs 1-2 tracks"); return false; }
  out->program.bpm = (uint16_t) bpm; out->program.loop = cJSON_IsTrue(loop); out->sequence_volume = (uint8_t) volume;
  cJSON_ArrayForEach(track, tracks) {
    pet_seq_track_t *t = &out->program.tracks[out->program.track_count];
    long tvol; const char *wave = str(track, "wave");
    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(track, "notes"), *note;
    if (!keys_allowed(track, track_keys, 3) || !integer(track, "volume", 0, 100, &tvol) || !cJSON_IsArray(notes)
        || cJSON_GetArraySize(notes) < 1 || cJSON_GetArraySize(notes) > (int) PET_SEQ_NOTES) { fail(error, size, "sequence track needs wave, volume and 1-64 notes"); return false; }
    if (!strcmp(wave, "square")) t->wave = 0; else if (!strcmp(wave, "triangle")) t->wave = 1; else if (!strcmp(wave, "noise")) t->wave = 2;
    else { fail(error, size, "sequence wave must be square, triangle or noise"); return false; }
    t->volume = (uint8_t) tvol;
    cJSON_ArrayForEach(note, notes) {
      const cJSON *midi = cJSON_IsArray(note) ? cJSON_GetArrayItem(note, 0) : NULL, *beats = cJSON_IsArray(note) ? cJSON_GetArrayItem(note, 1) : NULL;
      if (cJSON_GetArraySize(note) != 2 || !cJSON_IsNumber(midi) || !cJSON_IsNumber(beats) || midi->valuedouble != midi->valueint || beats->valuedouble != beats->valueint
          || (midi->valueint && (midi->valueint < (int) PET_SEQ_MIDI_MIN || midi->valueint > (int) PET_SEQ_MIDI_MAX)) || beats->valueint < 1 || beats->valueint > (int) PET_SEQ_BEATS_MAX) {
        fail(error, size, "sequence note must be [midi 36-96 or 0, beats 1-16]"); return false;
      }
      t->midi[t->count] = (uint8_t) midi->valueint; t->beats[t->count++] = (uint8_t) beats->valueint;
    }
    out->program.track_count++;
  }
  out->sequence = pet_seq_program_valid(&out->program);
  if (!out->sequence) fail(error, size, "sequence program is out of bounds");
  return out->sequence;
}
static bool parse_samples(const cJSON *samples, pet_cext_runtime_t *out, const pet_cext_parse_env_t *env, char *error, size_t size) {
  static const char *const keys[] = {"engine", "volume", "items"};
  static const char *const item_keys[] = {"id", "asset"};
  long volume;
  if (!env->runtime_v4 || env->exclusive_audio_owner || !keys_allowed(samples, keys, 3) || strcmp(str(samples, "engine"), PET_CEXT_SAMPLES_ENGINE) || !integer(samples, "volume", 0, 100, &volume)) {
    fail(error, size, "samples requires p4-bounded-runtime-v4, p4-samples-v1 and volume 0-100 without instrument/media/local/program"); return false;
  }
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(samples, "items"), *item;
  if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) < 1 || cJSON_GetArraySize(items) > (int) PET_SAMPLE_MAX_SLOTS) { fail(error, size, "samples needs 1-4 items"); return false; }
  out->samples_volume = (uint8_t) volume;
  cJSON_ArrayForEach(item, items) {
    char file_id[PET_CEXT_ID_MAX];
    const char *id = str(item, "id");
    if (!keys_allowed(item, item_keys, 2) || !safe_id(id, PET_CEXT_ID_MAX) || !asset_id(str(item, "asset"), ".wav", file_id) || strcmp(id, file_id)) {
      fail(error, size, "sample needs a unique safe id matching assets/<id>.wav"); return false;
    }
    for (unsigned i = 0; i < out->sample_count; i++) if (!strcmp(out->sample[i].id, id)) { fail(error, size, "duplicate sample id"); return false; }
    strcpy(out->sample[out->sample_count++].id, id);
  }
  out->samples = true;
  return true;
}
static bool parse_events(const cJSON *events, pet_cext_runtime_t *out, const pet_cext_parse_env_t *env, char *error, size_t size) {
  static const char *const keys[] = {"engine", "emit"};
  if (!env->runtime_v4 || !keys_allowed(events, keys, 2) || strcmp(str(events, "engine"), PET_CEXT_EVENTS_ENGINE)) { fail(error, size, "events requires p4-bounded-runtime-v4 and p4-events-v1"); return false; }
  const cJSON *emit = cJSON_GetObjectItemCaseSensitive(events, "emit"), *name;
  if (emit && (!cJSON_IsArray(emit) || cJSON_GetArraySize(emit) > (int) PET_CEV_NAMES_PER_PACKAGE)) { fail(error, size, "events emit lists at most 8 names"); return false; }
  cJSON_ArrayForEach(name, emit) {
    if (!cJSON_IsString(name) || !pet_cev_name_valid(name->valuestring)) { fail(error, size, "event name must be 1-15 safe ASCII characters"); return false; }
    for (unsigned i = 0; i < out->emit_count; i++) if (!strcmp(out->emit_names[i], name->valuestring)) { fail(error, size, "duplicate event name"); return false; }
    strcpy(out->emit_names[out->emit_count++], name->valuestring);
  }
  out->events = true;
  return true;
}
static bool parse_alarms(const cJSON *alarms, pet_cext_runtime_t *out, const pet_cext_parse_env_t *env, char *error, size_t size) {
  static const char *const keys[] = {"id", "every_ms", "at_ms_var", "action", "background", "text"};
  const cJSON *item;
  if (!out->events || !env->persist || !cJSON_IsArray(alarms) || cJSON_GetArraySize(alarms) < 1 || cJSON_GetArraySize(alarms) > (int) PET_CEV_ALARMS) {
    fail(error, size, "alarms need events p4-events-v1, persist and 1-4 items"); return false;
  }
  cJSON_ArrayForEach(item, alarms) {
    pet_cev_alarm_t *a = &out->alarms.items[out->alarms.count];
    const char *id = str(item, "id"), *action = str(item, "action"), *text = str(item, "text"), *var = str(item, "at_ms_var");
    const cJSON *background = cJSON_GetObjectItemCaseSensitive(item, "background"), *every = cJSON_GetObjectItemCaseSensitive(item, "every_ms");
    long every_ms = 0;
    bool periodic = every != NULL;
    if (!keys_allowed(item, keys, 6) || !pet_cev_name_valid(id) || (!safe_id(action, PET_CEV_ACTION_MAX) && !(!strncmp(action, "event:", 6) && pet_cev_name_valid(action + 6))) || !env->has_action || !env->has_action(env->ctx, action) || strlen(action) >= PET_CEV_ACTION_MAX
        || (background && !cJSON_IsBool(background)) || strlen(text) >= PET_CEV_ALARM_TEXT_MAX || periodic == (var[0] != 0)
        || (periodic && !integer(item, "every_ms", PET_CEV_ALARM_MIN_MS, 7L * 86400000L, &every_ms))) {
      fail(error, size, "alarm needs id, a declared action, text <= 40 bytes and exactly one of every_ms >= 60000 or at_ms_var"); return false;
    }
    int var_index = -1;
    if (!periodic) { var_index = env->find_int_var ? env->find_int_var(env->ctx, var) : -1; if (var_index < 0) { fail(error, size, "alarm at_ms_var must name an int var"); return false; } }
    for (unsigned i = 0; i < out->alarms.count; i++) if (!strcmp(out->alarms.items[i].id, id)) { fail(error, size, "duplicate alarm id"); return false; }
    strcpy(a->id, id); strcpy(a->action, action); strcpy(a->text, text[0] ? text : id);
    a->every_ms = (uint32_t) every_ms; a->at_var = (int8_t) var_index; a->background = cJSON_IsTrue(background); a->last_var = 0;
    out->alarms.count++;
  }
  return true;
}
bool pet_cext_parse(const cJSON *root, pet_cext_runtime_t *out, const pet_cext_parse_env_t *env, char *error, size_t size) {
  if (!root || !out || !env) return false;
  memset(out, 0, sizeof(*out));
  const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "sequence"), *samples = cJSON_GetObjectItemCaseSensitive(root, "samples");
  const cJSON *events = cJSON_GetObjectItemCaseSensitive(root, "events"), *alarms = cJSON_GetObjectItemCaseSensitive(root, "alarms");
  if (seq && !parse_sequence(seq, out, env, error, size)) return false;
  if (samples && !parse_samples(samples, out, env, error, size)) return false;
  if (events && !parse_events(events, out, env, error, size)) return false;
  if (alarms && !parse_alarms(alarms, out, env, error, size)) return false;
  return true;
}
bool pet_cext_parse_surface_media(const cJSON *surface, pet_cext_runtime_t *out, char *error, size_t size) {
  static const char *const big_keys[] = {"id", "asset", "width", "height"};
  if (!surface || !out) return true;
  const cJSON *bigs = cJSON_GetObjectItemCaseSensitive(surface, "big_images"), *item, *elements = cJSON_GetObjectItemCaseSensitive(surface, "elements");
  if (bigs) {
    if (!cJSON_IsArray(bigs) || cJSON_GetArraySize(bigs) < 1 || cJSON_GetArraySize(bigs) > (int) PET_CEXT_BIG_MAX) { fail(error, size, "big_images needs 1-2 items"); return false; }
    cJSON_ArrayForEach(item, bigs) {
      pet_cext_big_t *b = &out->bigs[out->big_count];
      long w, h; char file_id[PET_CEXT_ID_MAX];
      const char *id = str(item, "id");
      if (!keys_allowed(item, big_keys, 4) || !safe_id(id, PET_CEXT_ID_MAX) || !asset_id(str(item, "asset"), ".png", file_id) || strcmp(id, file_id)
          || !integer(item, "width", 241, PET_CEXT_BIG_MAX_W, &w) || !integer(item, "height", 1, PET_CEXT_BIG_MAX_H, &h)) {
        fail(error, size, "big image needs a safe id matching assets/<id>.png, width 241-1280 and height 1-960"); return false;
      }
      unsigned cols = ((unsigned) w + PET_CEXT_BIG_TILE - 1) / PET_CEXT_BIG_TILE, rows = ((unsigned) h + PET_CEXT_BIG_TILE - 1) / PET_CEXT_BIG_TILE;
      if (cols * rows > PET_CEXT_BIG_MAX_TILES) { fail(error, size, "big image exceeds 16 tiles"); return false; }
      for (unsigned i = 0; i < out->big_count; i++) if (!strcmp(out->bigs[i].id, id)) { fail(error, size, "duplicate big image id"); return false; }
      strcpy(b->id, id); b->w = (uint16_t) w; b->h = (uint16_t) h; b->cols = (uint8_t) cols; b->rows = (uint8_t) rows;
      b->tile_w = (uint16_t) (((unsigned) w + cols - 1) / cols); b->tile_h = (uint16_t) (((unsigned) h + rows - 1) / rows);
      out->big_count++;
    }
    out->big = true;
  }
  cJSON_ArrayForEach(item, elements) {
    if (strcmp(str(item, "type"), "clip")) continue;
    char id[PET_CEXT_ID_MAX];
    if (out->clip_count >= PET_CEXT_CLIP_MAX || !asset_id(str(item, "asset"), ".mp4", id)) { fail(error, size, "at most one clip node with assets/<id>.mp4"); return false; }
    strcpy(out->clips[out->clip_count].id, id); out->clips[out->clip_count].fps = PET_CEXT_CLIP_FPS; out->clip_count++; out->clip = true;
  }
  return true;
}
bool pet_cext_add_subscription(pet_cext_runtime_t *out, const char *on) {
  if (!out || !on || strncmp(on, "event:", 6)) return true;
  if (!out->events || !pet_cev_name_valid(on + 6)) return false;
  for (unsigned i = 0; i < out->sub_count; i++) if (!strcmp(out->sub_names[i], on + 6)) return true;
  if (out->sub_count >= PET_CEV_NAMES_PER_PACKAGE) return false;
  strcpy(out->sub_names[out->sub_count++], on + 6);
  return true;
}
bool pet_cext_parse_ops(const pet_cext_runtime_t *r, const cJSON *object, pet_cext_ops_t *out, char *error, size_t size) {
  out->sample = -1; out->emit = -1; out->sequence = 0;
  if (!r || !object) return true;
  const cJSON *seq = cJSON_GetObjectItemCaseSensitive(object, "sequence"), *sample = cJSON_GetObjectItemCaseSensitive(object, "sample"), *emit = cJSON_GetObjectItemCaseSensitive(object, "emit");
  if (seq) {
    const char *v = cJSON_IsString(seq) ? seq->valuestring : "";
    if (!r->sequence || (strcmp(v, "play") && strcmp(v, "stop"))) { fail(error, size, r->sequence ? "sequence op must be play or stop" : "sequence op requires the p4-sequence-v1 profile"); return false; }
    out->sequence = (uint8_t) (!strcmp(v, "play") ? 1 : 2);
  }
  if (sample) {
    int index = -1;
    if (cJSON_IsString(sample) && r->samples) for (unsigned i = 0; i < r->sample_count; i++) if (!strcmp(r->sample[i].id, sample->valuestring)) index = (int) i;
    if (index < 0) { fail(error, size, r->samples ? "sample references an unknown id" : "sample requires the p4-samples-v1 profile"); return false; }
    out->sample = (int8_t) index;
  }
  if (emit) {
    int index = cJSON_IsString(emit) && r->events ? find_id(r->emit_names, 0, r->emit_count, emit->valuestring) : -1;
    if (index < 0) { fail(error, size, r->events ? "emit references an undeclared event name" : "emit requires the p4-events-v1 profile"); return false; }
    out->emit = (int8_t) index;
  }
  return true;
}
void pet_cext_fire(pet_cext_runtime_t *r, const pet_cext_ops_t *ops) {
  if (!r || !ops) return;
  if (ops->sequence == 1) r->sequence_playing = true; else if (ops->sequence == 2) r->sequence_playing = false;
  if (ops->sample >= 0) r->sample_pending |= (uint8_t) (1u << ops->sample);
  if (ops->emit >= 0) r->emit_pending |= (uint8_t) (1u << ops->emit);
}
void pet_cext_audio_take(pet_cext_runtime_t *r, bool audible, pet_cext_audio_t *out) {
  memset(out, 0, sizeof(*out));
  if (!r) return;
  out->generation = r->media_generation;
  if (!audible) { r->sequence_playing = false; r->sample_pending = 0; return; }
  out->enabled = r->sequence || r->samples;
  out->sequence_active = r->sequence && r->sequence_playing;
  out->sequence_volume = r->sequence_volume;
  out->sample_pending = r->samples ? r->sample_pending : 0;
  out->samples_volume = r->samples_volume;
  r->sample_pending = 0;
}
/* ---- media containers ---- */
pet_cext_media_kind_t pet_cext_media_path_kind(const char *path, char id[PET_CEXT_ID_MAX]) {
  static const struct { const char *prefix; const char *ext; pet_cext_media_kind_t kind; } kinds[] = {
    {"runtime/samples/", ".pcm", PET_CEXT_MEDIA_SAMPLE}, {"runtime/clips/", ".h264", PET_CEXT_MEDIA_CLIP}, {"runtime/big/", ".p4t", PET_CEXT_MEDIA_BIG},
  };
  if (!path) return PET_CEXT_MEDIA_NONE;
  for (unsigned k = 0; k < 3; k++) {
    size_t plen = strlen(kinds[k].prefix), elen = strlen(kinds[k].ext);
    if (strncmp(path, kinds[k].prefix, plen)) continue;
    const char *name = path + plen; size_t len = strlen(name);
    if (len <= elen || strcmp(name + len - elen, kinds[k].ext) || len - elen >= PET_CEXT_ID_MAX) return PET_CEXT_MEDIA_NONE;
    char buffer[PET_CEXT_ID_MAX]; memcpy(buffer, name, len - elen); buffer[len - elen] = 0;
    if (!safe_id(buffer, PET_CEXT_ID_MAX)) return PET_CEXT_MEDIA_NONE;
    if (id) strcpy(id, buffer);
    return kinds[k].kind;
  }
  return PET_CEXT_MEDIA_NONE;
}
static uint32_t u32le(const uint8_t *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static uint16_t u16le(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
bool pet_cext_sample_header(const uint8_t *d, size_t size, pet_cext_sample_t *out) {
  if (!d || size < PET_CEXT_SAMPLE_HEADER || memcmp(d, "P4A1", 4)) return false;
  uint32_t frames = u32le(d + 4);
  if (!frames || frames > PET_SAMPLE_MAX_BYTES / 2 || (size_t) frames * 2 + PET_CEXT_SAMPLE_HEADER != size || size > PET_SAMPLE_MAX_BYTES + PET_CEXT_SAMPLE_HEADER) return false;
  if (out) { out->frames = frames; out->bytes = (uint32_t) size; }
  return true;
}
bool pet_cext_clip_header(const uint8_t *d, size_t size, pet_cext_clip_t *out) {
  if (!d || size < PET_CEXT_CLIP_HEADER || memcmp(d, "P4V1", 4)) return false;
  uint16_t w = u16le(d + 4), h = u16le(d + 6); uint8_t fps = d[8]; uint16_t frames = u16le(d + 10); uint32_t bytes = u32le(d + 12);
  bool small = w == 160 && h == 120, medium = w == 320 && h == 240;
  if ((!small && !medium) || fps != PET_CEXT_CLIP_FPS || !frames || frames > PET_CEXT_CLIP_MAX_FRAMES || !bytes || (size_t) bytes + PET_CEXT_CLIP_HEADER != size || size > PET_CEXT_CLIP_MAX_BYTES) return false;
  if (out) { out->w = w; out->h = h; out->fps = fps; out->frames = frames; out->bytes = (uint32_t) size; }
  return true;
}
/* Big-image edge tiles may be 1px wide/high; unlike ordinary surface
 * images their dimensions come from the exact P4T1 grid intersection. */
bool pet_cext_big_image_valid(const uint8_t *p,size_t n,unsigned w,unsigned h){
  if(!p||!w||!h||w>PET_CEXT_BIG_TILE||h>PET_CEXT_BIG_TILE||n<1036||n>PET_CEXT_BIG_TILE_MAX_BYTES||memcmp(p,"P4I1",4)||p[4]!=w||p[5]!=h||p[6]!=1||p[7]!=0||u32le(p+8)!=n)return false;
  size_t out=0,pos=1036,count=(size_t)w*h;
  while(pos<n){unsigned tag=p[pos++],len=(tag&127)+1;if(len>count-out)return false;
    if(tag&128){if(pos>=n)return false;pos++;}else{if(len>n-pos)return false;pos+=len;}
    out+=len;
  }
  return out==count;
}
bool pet_cext_big_header(const uint8_t *d, size_t size, pet_cext_big_t *out) {
  if (!d || size < PET_CEXT_BIG_HEADER || memcmp(d, "P4T1", 4)) return false;
  uint8_t cols = d[4], rows = d[5]; uint16_t tw = u16le(d + 6), th = u16le(d + 8), w = u16le(d + 10), h = u16le(d + 12); uint8_t count = d[14];
  if (!cols || !rows || (unsigned) cols * rows != count || count > PET_CEXT_BIG_MAX_TILES || !tw || !th || tw > PET_CEXT_BIG_TILE || th > PET_CEXT_BIG_TILE
      || w < 241 || w > PET_CEXT_BIG_MAX_W || !h || h > PET_CEXT_BIG_MAX_H || (unsigned) tw * cols < w || (unsigned) th * rows < h || size > PET_CEXT_BIG_BUDGET_BYTES
      || (unsigned)tw*(cols-1)>=w || (unsigned)th*(rows-1)>=h
      || size < PET_CEXT_BIG_HEADER + (size_t) count * 8) return false;
  uint32_t end = PET_CEXT_BIG_HEADER + (uint32_t)count * 8;
  for (unsigned i = 0; i < count; i++) {
    uint32_t offset, bytes;
    if (!pet_cext_big_tile(d, size, i, &offset, &bytes) || offset < end || bytes < 12 || bytes > PET_CEXT_BIG_TILE_MAX_BYTES || memcmp(d + offset, "P4I1", 4)) return false;
    unsigned col=i%cols,row=i/cols;
    unsigned ew=w-col*tw,eh=h-row*th;if(ew>tw)ew=tw;if(eh>th)eh=th;
    if(!pet_cext_big_image_valid(d+offset,bytes,ew,eh))return false;
    end = offset + bytes;
  }
  if (out) { out->w = w; out->h = h; out->tile_w = tw; out->tile_h = th; out->cols = cols; out->rows = rows; out->bytes = (uint32_t) size; }
  return true;
}
bool pet_cext_big_tile(const uint8_t *d, size_t size, unsigned index, uint32_t *offset, uint32_t *bytes) {
  if (!d || size < PET_CEXT_BIG_HEADER || index >= d[14]) return false;
  if (size < PET_CEXT_BIG_HEADER + ((size_t)index + 1) * 8) return false;
  const uint8_t *entry = d + PET_CEXT_BIG_HEADER + (size_t) index * 8;
  uint32_t o = u32le(entry), b = u32le(entry + 4);
  if (!b || o < PET_CEXT_BIG_HEADER || o > size || b > size - o) return false;
  if (offset) *offset = o;
  if (bytes) *bytes = b;
  return true;
}
unsigned pet_cext_big_visible_tiles(const pet_cext_big_t *big, int32_t ox, int32_t oy, unsigned node_w, unsigned node_h, uint8_t out[PET_CEXT_BIG_MAX_TILES]) {
  if (!big || !big->cols || !big->rows || !big->tile_w || !big->tile_h || !node_w || !node_h) return 0;
  int32_t max_x = (int32_t) big->w - (int32_t) node_w, max_y = (int32_t) big->h - (int32_t) node_h;
  if (ox < 0) ox = 0;
   if (max_x > 0 && ox > max_x) ox = max_x;
  if (max_x <= 0) ox = 0;
  if (oy < 0) oy = 0;
   if (max_y > 0 && oy > max_y) oy = max_y;
  if (max_y <= 0) oy = 0;
  unsigned c0 = (unsigned) ox / big->tile_w, c1 = ((unsigned) ox + node_w - 1) / big->tile_w;
  unsigned r0 = (unsigned) oy / big->tile_h, r1 = ((unsigned) oy + node_h - 1) / big->tile_h;
  if (c1 >= big->cols) c1 = big->cols - 1;
  if (r1 >= big->rows) r1 = big->rows - 1;
  unsigned n = 0;
  for (unsigned r = r0; r <= r1 && r < big->rows; r++) for (unsigned c = c0; c <= c1 && c < big->cols; c++) {
    if (n >= PET_CEXT_BIG_MAX_TILES) return n;
    if (out) out[n] = (uint8_t) (r * big->cols + c);
    n++;
  }
  return n;
}
int pet_cext_clip_target(pet_cext_clip_state_t *s, const pet_cext_clip_t *clip, bool visible, uint64_t now_ms) {
  if (!s || !clip || !clip->frames || !clip->fps) return -1;
  if (!visible) { if (s->visible) { s->hidden_ms = now_ms; s->visible = false; } return -1; }
  if (!s->visible) {
    if (!s->started_ms) s->started_ms = now_ms;
    else if (s->hidden_ms && now_ms >= s->hidden_ms) s->started_ms += now_ms - s->hidden_ms; /* paused time never advances the clip */
    s->visible = true;
  }
  uint64_t elapsed = now_ms >= s->started_ms ? now_ms - s->started_ms : 0;
  uint64_t frame = elapsed * clip->fps / 1000u;
  return (int) (frame % clip->frames);
}

bool pet_cext_internal_action(const pet_cext_runtime_t *r, const char *action) {
  if (!r || !r->events || !action || !action[0]) return false;
  if (!strncmp(action, "event:", 6) && pet_cev_name_valid(action + 6)) {
    for (unsigned i = 0; i < r->sub_count && i < PET_CEV_NAMES_PER_PACKAGE; i++) if (!strcmp(action + 6, r->sub_names[i])) return true;
  }
  for (unsigned i = 0; i < r->alarms.count && i < PET_CEV_ALARMS; i++) if (!strcmp(action, r->alarms.items[i].action)) return true;
  return false;
}
static bool action_in_array(const cJSON *array, const char *key, const char *action) {
  const cJSON *item; cJSON_ArrayForEach(item, array) if (!strcmp(str(item, key), action)) return true;
  return false;
}
bool pet_cext_binding_coverage(const pet_cext_runtime_t *r, const cJSON *root, const cJSON *buttons, char *error, size_t size) {
  if (!r || !root || !cJSON_IsArray(buttons)) return false;
  if (!r->events && !r->alarms.count) return true; /* existing legacy policy */
  const cJSON *transitions = cJSON_GetObjectItemCaseSensitive(root, "transitions");
  const cJSON *input = cJSON_GetObjectItemCaseSensitive(root, "input");
  const cJSON *surface = cJSON_GetObjectItemCaseSensitive(root, "surface");
  const cJSON *elements = cJSON_GetObjectItemCaseSensitive(surface, "elements");
  const cJSON *swipe = cJSON_GetObjectItemCaseSensitive(input, "swipe"), *item;
  if (!cJSON_IsArray(transitions) || cJSON_GetArraySize(transitions) > 48 || cJSON_GetArraySize(buttons) > 8 || (elements && (!cJSON_IsArray(elements) || cJSON_GetArraySize(elements) > 64))) return false;
  bool touch = !strcmp(str(input, "engine"), "p4-input-v1");
  cJSON_ArrayForEach(item, transitions) {
    const char *action = str(item, "on");
    bool bound = action[0] && (pet_cext_internal_action(r, action) || action_in_array(buttons, "action", action));
    if (!bound && touch) {
      bound = action_in_array(elements, "action", action);
      const cJSON *value; cJSON_ArrayForEach(value, swipe) if (cJSON_IsString(value) && !strcmp(value->valuestring, action)) bound = true;
    }
    if (!bound) { fail(error, size, "transition action needs a physical/touch binding or declared event/alarm scope"); return false; }
  }
  cJSON_ArrayForEach(item, buttons) if (!action_in_array(transitions, "on", str(item, "action"))) { fail(error, size, "button action has no transition"); return false; }
  return true;
}
