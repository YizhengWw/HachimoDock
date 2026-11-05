/* Track C component media store; see pet_p4_component_media.h. */
#include "pet_p4_component_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "pet_p4_resources_device.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "p4-cmedia";
typedef struct { pet_cext_media_kind_t kind; char id[PET_CEXT_ID_MAX]; uint32_t offset, size, next_index, last_offset, last_size; bool complete; } cmedia_file_t;
#define CMEDIA_FILES (PET_SAMPLE_MAX_SLOTS + PET_CEXT_CLIP_MAX + PET_CEXT_BIG_MAX)
static struct { uint8_t *arena; uint32_t used; cmedia_file_t files[CMEDIA_FILES]; unsigned count; } g_stage;
typedef struct {
  uint8_t *samples; uint32_t sample_offset[PET_SAMPLE_MAX_SLOTS], sample_frames[PET_SAMPLE_MAX_SLOTS]; unsigned sample_count;
  uint8_t *clip; size_t clip_bytes; pet_cext_clip_t clip_meta; bool clip_ready;
  char big_path[PET_CEXT_BIG_MAX][32]; pet_cext_big_t big_meta[PET_CEXT_BIG_MAX]; uint8_t *big_table[PET_CEXT_BIG_MAX]; unsigned big_count;
  uint32_t generation;
  unsigned pins;
} cmedia_bank_t;
static cmedia_bank_t g_banks[PET_CMEDIA_BANKS];
static unsigned g_current;
static uint32_t g_generation;
static bool g_generation_exhausted;
static struct { uint64_t serial; uint32_t generation; unsigned bank; bool empty; } g_pending;
static uint64_t g_prepare_serial;
#define g_active g_banks[g_current]
static struct { _Atomic(uint32_t) token; unsigned bank; } g_leases[PET_CMEDIA_LEASES];
static uint32_t g_token, g_reclaimed, g_deferred_reclaimed;
#define CMEDIA_RELEASED (UINT32_C(1) << 31)
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(unsigned) == 4, "audio release requires lock-free 32-bit atomics");
static void reap_locked(void);
static _Atomic(SemaphoreHandle_t) g_lock;
static void lock(void) {
  SemaphoreHandle_t mutex = atomic_load(&g_lock);
  if (!mutex) {
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) abort();
    SemaphoreHandle_t expected = NULL;
    if (!atomic_compare_exchange_strong(&g_lock, &expected, candidate)) {
      vSemaphoreDelete(candidate); mutex = expected;
    } else mutex = candidate;
  }
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) abort();
  reap_locked();
}
/* Frame/main probes never allocate a mutex or collect/free implicitly. */
static bool try_lock(void) {
  SemaphoreHandle_t mutex = atomic_load(&g_lock);
  return mutex && xSemaphoreTake(mutex, 0) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(atomic_load(&g_lock)); }
static char kind_char(pet_cext_media_kind_t k) { return k == PET_CEXT_MEDIA_SAMPLE ? 's' : k == PET_CEXT_MEDIA_CLIP ? 'c' : 'b'; }
static void media_path(size_t slot, uint8_t generation, pet_cext_media_kind_t kind, unsigned index, bool temporary, char *out, size_t size) {
  snprintf(out, size, temporary ? "/spiffs/p4m%02u-%u-%c%u.tmp" : "/spiffs/p4m%02u-%u-%c%u.bin", (unsigned) slot, (unsigned) generation, kind_char(kind), index);
}
static uint32_t kind_budget(pet_cext_media_kind_t kind) {
  return kind == PET_CEXT_MEDIA_SAMPLE ? PET_SAMPLE_MAX_BYTES + PET_CEXT_SAMPLE_HEADER : kind == PET_CEXT_MEDIA_CLIP ? PET_CEXT_CLIP_MAX_BYTES : PET_CEXT_BIG_BUDGET_BYTES;
}
void pet_cmedia_stage_reset(void) { free(g_stage.arena); g_stage.arena = NULL; g_stage.used = 0; g_stage.count = 0; memset(g_stage.files, 0, sizeof(g_stage.files)); }
static void fail(char *e, size_t n, const char *m) { if (e && n) snprintf(e, n, "%s", m); }
bool pet_cmedia_stage_chunk(pet_cext_media_kind_t kind, const char *id, uint32_t index, const uint8_t *data, size_t len, char *error, size_t size) {
  if (kind < PET_CEXT_MEDIA_SAMPLE || kind > PET_CEXT_MEDIA_BIG || !pet_cev_name_valid(id) || !data || !len || len > kind_budget(kind) || index == UINT32_MAX) { fail(error, size, "invalid component media chunk"); return false; }
  if (!g_stage.arena) g_stage.arena = heap_caps_malloc(PET_CMEDIA_STAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_stage.arena) { fail(error, size, "component media staging memory unavailable"); return false; }
  cmedia_file_t *f = NULL;
  for (unsigned i = 0; i < g_stage.count; i++) if (g_stage.files[i].kind == kind && !strcmp(g_stage.files[i].id, id)) f = &g_stage.files[i];
  if (f && f->next_index && index == f->next_index - 1) {
    bool same = len == f->last_size && !memcmp(g_stage.arena + f->last_offset, data, len);
    if (!same) fail(error, size, "component media retransmission differs");
    return same;
  }
  if (f && (f->complete || f->offset + f->size != g_stage.used)) {
    fail(error, size, "component media files must not interleave"); return false;
  }
  if (!f) {
    if (g_stage.count && !g_stage.files[g_stage.count - 1].complete) { fail(error, size, "previous component media file is incomplete"); return false; }
    if (index != 0 || g_stage.count >= CMEDIA_FILES) { fail(error, size, "component media chunk sequence"); return false; }
    f = &g_stage.files[g_stage.count++]; memset(f, 0, sizeof(*f)); f->kind = kind; strcpy(f->id, id); f->offset = g_stage.used;
  }
  if (index != f->next_index) { fail(error, size, "component media chunk out of sequence"); return false; }
  uint32_t kind_total = f->size + (uint32_t) len;
  for (unsigned i = 0; i < g_stage.count; i++) if (&g_stage.files[i] != f && g_stage.files[i].kind == kind && kind != PET_CEXT_MEDIA_CLIP && kind != PET_CEXT_MEDIA_SAMPLE) kind_total += g_stage.files[i].size;
  if (kind_total > kind_budget(kind) || g_stage.used + len > PET_CMEDIA_STAGE_BYTES) { fail(error, size, "component media exceeds its budget"); return false; }
  if (kind == PET_CEXT_MEDIA_SAMPLE) {
    uint32_t total = len; for (unsigned i = 0; i < g_stage.count; i++) if (g_stage.files[i].kind == kind) total += g_stage.files[i].size;
    if (total > PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER) { fail(error, size, "sample pack exceeds 256 KiB"); return false; }
  }
  f->last_offset = g_stage.used; f->last_size = (uint32_t) len;
  memcpy(g_stage.arena + g_stage.used, data, len);
  g_stage.used += (uint32_t) len; f->size += (uint32_t) len; f->next_index = index + 1;
  /* Headers carry the exact size, so completion needs no end-of-file message. */
  const uint8_t *d = g_stage.arena + f->offset;
  if (kind == PET_CEXT_MEDIA_SAMPLE && f->size >= PET_CEXT_SAMPLE_HEADER && !memcmp(d, "P4A1", 4)) {
    uint32_t frames = (uint32_t) d[4] | (uint32_t) d[5] << 8 | (uint32_t) d[6] << 16 | (uint32_t) d[7] << 24;
    f->complete = frames && frames <= PET_SAMPLE_MAX_BYTES / 2 && f->size == frames * 2 + PET_CEXT_SAMPLE_HEADER;
  } else if (kind == PET_CEXT_MEDIA_CLIP && f->size >= PET_CEXT_CLIP_HEADER && !memcmp(d, "P4V1", 4)) {
    uint32_t bytes = (uint32_t) d[12] | (uint32_t) d[13] << 8 | (uint32_t) d[14] << 16 | (uint32_t) d[15] << 24;
    f->complete = bytes && bytes <= PET_CEXT_CLIP_MAX_BYTES - PET_CEXT_CLIP_HEADER && f->size == bytes + PET_CEXT_CLIP_HEADER;
  } else if (kind == PET_CEXT_MEDIA_BIG && f->size >= PET_CEXT_BIG_HEADER && !memcmp(d, "P4T1", 4)) {
    unsigned count = d[14]; uint32_t end = 0;
    if (f->size >= PET_CEXT_BIG_HEADER + count * 8) for (unsigned i = 0; i < count; i++) { uint32_t o, b; if (pet_cext_big_tile(d, f->size, i, &o, &b) && o + b > end) end = o + b; }
    f->complete = count && count <= PET_CEXT_BIG_MAX_TILES && end && f->size == end;
  }
  return true;
}
static const cmedia_file_t *staged(pet_cext_media_kind_t kind, const char *id) {
  for (unsigned i = 0; i < g_stage.count; i++) if (g_stage.files[i].kind == kind && !strcmp(g_stage.files[i].id, id) && g_stage.files[i].complete) return &g_stage.files[i];
  return NULL;
}
bool pet_cmedia_stage_matches(const pet_cext_runtime_t *r, char *error, size_t size) {
  if (!r) return g_stage.count == 0;
  if (r->sample_count > PET_SAMPLE_MAX_SLOTS || r->clip_count > PET_CEXT_CLIP_MAX || r->big_count > PET_CEXT_BIG_MAX) return false;
  for (unsigned i = 0; i < r->sample_count; i++) {
    const cmedia_file_t *f = staged(PET_CEXT_MEDIA_SAMPLE, r->sample[i].id);
    if (!f || !pet_cext_sample_header(g_stage.arena + f->offset, f->size, NULL)) { fail(error, size, "declared sample is missing or malformed"); return false; }
  }
  for (unsigned i = 0; i < r->clip_count; i++) {
    const cmedia_file_t *f = staged(PET_CEXT_MEDIA_CLIP, r->clips[i].id);
    if (!f || !pet_cext_clip_header(g_stage.arena + f->offset, f->size, NULL)) { fail(error, size, "declared clip is missing or malformed"); return false; }
  }
  for (unsigned i = 0; i < r->big_count; i++) {
    const cmedia_file_t *f = staged(PET_CEXT_MEDIA_BIG, r->bigs[i].id); pet_cext_big_t meta;
    if (!f || !pet_cext_big_header(g_stage.arena + f->offset, f->size, &meta) || meta.w != r->bigs[i].w || meta.h != r->bigs[i].h) { fail(error, size, "declared big image is missing or malformed"); return false; }
  }
  if (g_stage.count != (unsigned)r->sample_count + r->clip_count + r->big_count) { fail(error, size, "undeclared component media file"); return false; }
  return true;
}
static bool write_atomic(const char *tmp, const char *final, const uint8_t *data, size_t len) {
  FILE *f = fopen(tmp, "wb"); if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len; ok = fclose(f) == 0 && ok;
  if (ok) ok = rename(tmp, final) == 0; /* failed rename preserves the previous file */
  if (!ok) unlink(tmp);
  return ok;
}
/* Fixed namespace scan; conservative admission includes old files plus staged bytes. */
static bool disk_budget_ok(void) {
  size_t total = g_stage.used;
  if (total > PET_CMEDIA_DEVICE_BUDGET_BYTES) return false;
  /* An empty media stage cannot increase media storage usage. Ordinary
   * widgets must not pay 896 negative SPIFFS stat lookups on every commit:
   * each missing name scans the filesystem and can starve the protocol task.
   * Staging validation, pending-publication exclusion and spare-generation
   * cleanup still run in pet_cmedia_persist/persist_locked below. */
  if (total == 0) return true;
  char path[32];
  for (size_t slot = 0; slot < PET_CEV_COMPONENTS; slot++)
    for (unsigned gen = 0; gen < 2; gen++)
      for (pet_cext_media_kind_t kind = PET_CEXT_MEDIA_SAMPLE; kind <= PET_CEXT_MEDIA_BIG; kind++) {
        unsigned count = kind == PET_CEXT_MEDIA_SAMPLE ? PET_SAMPLE_MAX_SLOTS : kind == PET_CEXT_MEDIA_CLIP ? PET_CEXT_CLIP_MAX : PET_CEXT_BIG_MAX;
        for (unsigned i = 0; i < count; i++) for (unsigned tmp = 0; tmp < 2; tmp++) {
          struct stat info;
          media_path(slot, gen, kind, i, tmp != 0, path, sizeof(path));
          if (stat(path, &info)) { if (errno == ENOENT) continue; return false; }
          if (!S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > PET_CMEDIA_DEVICE_BUDGET_BYTES - total) return false;
          total += (size_t)info.st_size;
        }
      }
  return true;
}
static bool persist_locked(size_t slot, uint8_t generation, const pet_cext_runtime_t *r);
bool pet_cmedia_persist(size_t slot, uint8_t generation, const pet_cext_runtime_t *r) {
  if (slot >= PET_CEV_COMPONENTS || generation > 1 || !pet_cmedia_stage_matches(r, NULL, 0)) return false;
  lock(); bool ok = !g_pending.serial && disk_budget_ok() && persist_locked(slot, generation, r); unlock(); return ok;
}
static bool persist_locked(size_t slot, uint8_t generation, const pet_cext_runtime_t *r) {
  if (!r) return true;
  if (!pet_cmedia_stage_matches(r, NULL, 0)) return false;
  char path[32], tmp[32];
  for (pet_cext_media_kind_t kind = PET_CEXT_MEDIA_SAMPLE; kind <= PET_CEXT_MEDIA_BIG; kind++) {
    unsigned count = kind == PET_CEXT_MEDIA_SAMPLE ? r->sample_count : kind == PET_CEXT_MEDIA_CLIP ? r->clip_count : r->big_count;
    unsigned max = kind == PET_CEXT_MEDIA_SAMPLE ? PET_SAMPLE_MAX_SLOTS : kind == PET_CEXT_MEDIA_CLIP ? PET_CEXT_CLIP_MAX : PET_CEXT_BIG_MAX;
    for (unsigned i = 0; i < max; i++) {
      media_path(slot, generation, kind, i, false, path, sizeof(path)); media_path(slot, generation, kind, i, true, tmp, sizeof(tmp));
      if (i >= count) { unlink(path); unlink(tmp); continue; }
      const char *id = kind == PET_CEXT_MEDIA_SAMPLE ? r->sample[i].id : kind == PET_CEXT_MEDIA_CLIP ? r->clips[i].id : r->bigs[i].id;
      const cmedia_file_t *f = staged(kind, id);
      if (!f || !write_atomic(tmp, path, g_stage.arena + f->offset, f->size)) { ESP_LOGW(TAG, "component media persist failed %s", path); return false; }
    }
  }
  return true;
}
void pet_cmedia_remove(size_t slot) {
  if (slot >= PET_CEV_COMPONENTS) return;
  lock();
  if (g_pending.serial) { unlock(); return; }
  char path[32];
  for (uint8_t generation = 0; generation < 2; generation++)
    for (pet_cext_media_kind_t kind = PET_CEXT_MEDIA_SAMPLE; kind <= PET_CEXT_MEDIA_BIG; kind++)
      for (unsigned i = 0; i < PET_SAMPLE_MAX_SLOTS; i++) {
        media_path(slot, generation, kind, i, false, path, sizeof(path)); unlink(path);
        media_path(slot, generation, kind, i, true, path, sizeof(path)); unlink(path);
      }
  unlock();
}
static uint8_t *read_file(const char *path, size_t max, size_t *out_size) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  uint8_t *buffer = heap_caps_malloc(max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t n = buffer ? fread(buffer, 1, max, f) : 0; int extra = buffer ? fgetc(f) : EOF; bool io_error = ferror(f); fclose(f);
  if (!buffer || !n || extra != EOF || io_error) { free(buffer); return NULL; }
  *out_size = n; return buffer;
}
static void bank_free(cmedia_bank_t *bank) {
  free(bank->samples); free(bank->clip);
  for (unsigned i = 0; i < PET_CEXT_BIG_MAX; i++) free(bank->big_table[i]);
  memset(bank, 0, sizeof(*bank));
}
static void reap_locked(void) {
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) {
    if (!(atomic_load(&g_leases[i].token) & CMEDIA_RELEASED)) continue;
    unsigned bank = g_leases[i].bank;
    g_reclaimed++; g_deferred_reclaimed++;
    if (--g_banks[bank].pins == 0 && (bank != g_current || (!g_active.sample_count && !g_active.clip_ready && !g_active.big_count))) bank_free(&g_banks[bank]);
    atomic_store(&g_leases[i].token, 0);
  }
}
void pet_cmedia_collect(void) { lock(); unlock(); }
bool pet_cmedia_collect_try(void) {
  if (!try_lock()) return false;
  reap_locked(); unlock(); return true;
}
bool pet_cmedia_generation_try(uint32_t expected) {
  if (!expected || !try_lock()) return false;
  bool ok = expected == g_generation; unlock(); return ok;
}
bool pet_cmedia_unpin_deferred(uint64_t token) {
  if (!token || token >= CMEDIA_RELEASED) return false;
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) {
    uint32_t expected = (uint32_t)token;
    if (atomic_compare_exchange_strong(&g_leases[i].token, &expected, (uint32_t)token | CMEDIA_RELEASED)) return true;
  }
  return false;
}
void pet_cmedia_unload(void) {
  lock();
  if (g_pending.serial) { unlock(); return; }
  /* Detach visibility immediately; pinned buffers retire until their last unpin. */
  if (!g_active.pins) bank_free(&g_active);
  g_active.sample_count = 0; g_active.clip_ready = false; g_active.big_count = 0;
  if (g_generation == UINT32_MAX) { g_generation = 0; g_generation_exhausted = true; }
  else if (!g_generation_exhausted) g_generation++;
  unlock();
}
bool pet_cmedia_load(size_t slot, uint8_t generation, const pet_cext_runtime_t *r) {
  pet_cmedia_ticket_t ticket = {0};
  return pet_cmedia_prepare(slot, generation, r, &ticket) && pet_cmedia_commit(&ticket);
}
bool pet_cmedia_prepare(size_t slot, uint8_t generation, const pet_cext_runtime_t *r, pet_cmedia_ticket_t *ticket) {
  if (!ticket || ticket->serial || slot >= PET_CEV_COMPONENTS || generation > 1) return false;
  bool ok = true; char path[32];
  lock();
  if (g_pending.serial || g_prepare_serial == UINT64_MAX || g_generation_exhausted || g_generation == UINT32_MAX) { unlock(); return false; }
  if (r && (r->sample_count > PET_SAMPLE_MAX_SLOTS || r->clip_count > PET_CEXT_CLIP_MAX || r->big_count > PET_CEXT_BIG_MAX)) { unlock(); return false; }
  if (!r || (!r->sample_count && !r->clip_count && !r->big_count)) {
    g_pending.serial = ++g_prepare_serial; g_pending.generation = g_generation; g_pending.empty = true;
    *ticket = (pet_cmedia_ticket_t){g_pending.serial, g_generation}; unlock(); return true;
  }
  unsigned previous = g_current, target = PET_CMEDIA_BANKS;
  for (unsigned i = 0; i < PET_CMEDIA_BANKS; i++) if (i != previous && !g_banks[i].pins) { target = i; break; }
  if (target == PET_CMEDIA_BANKS) { unlock(); return false; }
  bank_free(&g_banks[target]); g_current = target;
  if (r->sample_count) {
    g_active.samples = heap_caps_malloc(PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t used = 0;
    for (unsigned i = 0; ok && g_active.samples && i < r->sample_count; i++) {
      media_path(slot, generation, PET_CEXT_MEDIA_SAMPLE, i, false, path, sizeof(path));
      size_t n = 0; uint8_t *data = read_file(path, PET_SAMPLE_MAX_BYTES + PET_CEXT_SAMPLE_HEADER, &n); pet_cext_sample_t meta;
      ok = data && pet_cext_sample_header(data, n, &meta) && used + n <= PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER;
      if (ok) { memcpy(g_active.samples + used, data, n); g_active.sample_offset[i] = (uint32_t) used + PET_CEXT_SAMPLE_HEADER; g_active.sample_frames[i] = meta.frames; used += n; g_active.sample_count++; }
      free(data);
    }
    ok = ok && g_active.samples;
  }
  if (ok && r->clip_count) {
    media_path(slot, generation, PET_CEXT_MEDIA_CLIP, 0, false, path, sizeof(path));
    size_t n = 0; g_active.clip = read_file(path, PET_CEXT_CLIP_MAX_BYTES, &n);
    ok = g_active.clip && pet_cext_clip_header(g_active.clip, n, &g_active.clip_meta);
    if (ok) { strcpy(g_active.clip_meta.id, r->clips[0].id); g_active.clip_bytes = n; g_active.clip_ready = true; }
  }
  size_t big_total = 0;
  for (unsigned i = 0; ok && i < r->big_count; i++) {
    media_path(slot, generation, PET_CEXT_MEDIA_BIG, i, false, g_active.big_path[i], sizeof(g_active.big_path[i]));
    FILE *f = fopen(g_active.big_path[i], "rb"); uint8_t header[PET_CEXT_BIG_HEADER + PET_CEXT_BIG_MAX_TILES * 8];
    size_t n = f ? fread(header, 1, sizeof(header), f) : 0; long total = 0;
    if (f) { if (!ferror(f) && fseek(f, 0, SEEK_END) == 0) total = ftell(f); fclose(f); }
    ok = n >= PET_CEXT_BIG_HEADER && total > 0 && (size_t) total <= PET_CEXT_BIG_BUDGET_BYTES;
    if (ok) { big_total += (size_t)total; ok = big_total <= PET_CEXT_BIG_BUDGET_BYTES; }
    if (ok) {
      /* Validate the table against the file length without loading tiles. */
      uint8_t *table = heap_caps_malloc(sizeof(header), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      ok = table != NULL; if (ok) { memcpy(table, header, n); g_active.big_table[i] = table; }
      unsigned count = header[14];
      ok = ok && count && count <= PET_CEXT_BIG_MAX_TILES && n >= PET_CEXT_BIG_HEADER + count * 8 && !memcmp(header, "P4T1", 4);
      unsigned tw = header[6] | (unsigned) header[7] << 8, th = header[8] | (unsigned) header[9] << 8;
      unsigned w = header[10] | (unsigned) header[11] << 8, h = header[12] | (unsigned) header[13] << 8;
      ok = ok && header[4] && header[5] && header[4] * header[5] == count && tw && th && tw <= PET_CEXT_BIG_TILE && th <= PET_CEXT_BIG_TILE
        && w == r->bigs[i].w && h == r->bigs[i].h && tw * header[4] >= w && th * header[5] >= h
        && tw * (header[4]-1) < w && th * (header[5]-1) < h;
      uint32_t end = PET_CEXT_BIG_HEADER + count * 8;
      for (unsigned t = 0; ok && t < count; t++) {
        uint32_t o, b;
        ok = pet_cext_big_tile(header, (size_t) total, t, &o, &b) && o >= end && b >= 12 && b <= PET_CEXT_BIG_TILE_MAX_BYTES;
        if (ok) end = o + b;
      }
      if(ok){
        /* Cold validation shares the exact staging validator, but retains at
         * most one 64KiB tile workspace, never a full 1MiB tiled asset. */
        uint8_t *tile=heap_caps_malloc(PET_CEXT_BIG_TILE_MAX_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        FILE *file=tile?fopen(g_active.big_path[i],"rb"):NULL;ok=file!=NULL;
        for(unsigned t=0;ok&&t<count;t++){
          uint32_t o,bytes;ok=pet_cext_big_tile(header,(size_t)total,t,&o,&bytes);
          unsigned ew=w-(t%header[4])*tw,eh=h-(t/header[4])*th;if(ew>tw)ew=tw;if(eh>th)eh=th;
          ok=ok&&fseek(file,(long)o,SEEK_SET)==0&&fread(tile,1,bytes,file)==bytes
            &&pet_cext_big_image_valid(tile,bytes,ew,eh);
        }
        if(file&&fclose(file)!=0)ok=false;
        free(tile);
      }
      if (ok) { g_active.big_meta[i] = r->bigs[i]; g_active.big_meta[i].tile_w = tw; g_active.big_meta[i].tile_h = th; g_active.big_meta[i].cols = header[4]; g_active.big_meta[i].rows = header[5]; g_active.big_meta[i].bytes = (uint32_t) total; g_active.big_count++; }
    }
  }
  if (ok) {
    g_pending.serial = ++g_prepare_serial; g_pending.generation = g_generation;
    g_pending.bank = target; g_pending.empty = false;
    *ticket = (pet_cmedia_ticket_t){g_pending.serial, g_generation};
  } else bank_free(&g_active);
  g_current = previous; /* publication is exclusively commit's job */
  unlock();
  if (!ok) ESP_LOGW(TAG, "component media prepare failed slot=%u", (unsigned) slot);
  return ok;
}
static bool ticket_current(const pet_cmedia_ticket_t *ticket) {
  return ticket && ticket->serial && ticket->serial == g_pending.serial && ticket->generation == g_pending.generation && ticket->generation == g_generation;
}
bool pet_cmedia_commit(pet_cmedia_ticket_t *ticket) {
  lock();
  if (!ticket_current(ticket)) { unlock(); return false; }
  unsigned previous = g_current;
  if (!g_pending.empty) g_current = g_pending.bank;
  else { g_active.sample_count = 0; g_active.clip_ready = false; g_active.big_count = 0; }
  if (!g_banks[previous].pins) bank_free(&g_banks[previous]);
  g_active.generation = ++g_generation;
  memset(&g_pending, 0, sizeof(g_pending)); memset(ticket, 0, sizeof(*ticket));
  unlock(); return true;
}
bool pet_cmedia_abort(pet_cmedia_ticket_t *ticket) {
  lock();
  if (!ticket_current(ticket)) { unlock(); return false; }
  if (!g_pending.empty) bank_free(&g_banks[g_pending.bank]);
  memset(&g_pending, 0, sizeof(g_pending)); memset(ticket, 0, sizeof(*ticket));
  unlock(); return true;
}
static uint64_t pin_locked(void) {
  if (g_token == CMEDIA_RELEASED - 1) return 0; /* never reuse a token */
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) if (!g_leases[i].token) {
    g_leases[i].bank = g_current; g_active.pins++;
    atomic_store(&g_leases[i].token, ++g_token);
    return g_token;
  }
  return 0;
}
bool pet_cmedia_sample_pin(unsigned index, pet_cmedia_sample_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out)); lock();
  if (index < g_active.sample_count && g_active.samples) {
    out->token = pin_locked();
    if (out->token) { out->pcm = (const int16_t *)(g_active.samples + g_active.sample_offset[index]); out->frames = g_active.sample_frames[index]; out->generation = g_active.generation; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_clip_pin(unsigned index, pet_cmedia_clip_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out)); lock();
  if (!index && g_active.clip_ready) {
    out->token = pin_locked();
    if (out->token) { out->stream = g_active.clip + PET_CEXT_CLIP_HEADER; out->bytes = g_active.clip_bytes - PET_CEXT_CLIP_HEADER; out->meta = g_active.clip_meta; out->generation = g_active.generation; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_sample_pin_at(unsigned index, uint32_t expected, pet_cmedia_sample_lease_t *out) {
  if (!pet_cmedia_sample_pin(index, out)) return false;
  if (expected && out->generation == expected) return true;
  (void)pet_cmedia_unpin(out->token); memset(out, 0, sizeof(*out)); return false;
}
bool pet_cmedia_clip_pin_at(unsigned index, uint32_t expected, pet_cmedia_clip_lease_t *out) {
  if (!pet_cmedia_clip_pin(index, out)) return false;
  if (expected && out->generation == expected) return true;
  (void)pet_cmedia_unpin(out->token); memset(out, 0, sizeof(*out)); return false;
}
bool pet_cmedia_sample_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_sample_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!expected || !try_lock()) return false;
  if (expected == g_active.generation && index < g_active.sample_count && g_active.samples) {
    out->token = pin_locked();
    if (out->token) { out->pcm = (const int16_t *)(g_active.samples + g_active.sample_offset[index]); out->frames = g_active.sample_frames[index]; out->generation = expected; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_clip_pin_try_at(unsigned index, uint32_t expected, pet_cmedia_clip_lease_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!expected || !try_lock()) return false;
  if (expected == g_active.generation && !index && g_active.clip_ready) {
    out->token = pin_locked();
    if (out->token) { out->stream = g_active.clip + PET_CEXT_CLIP_HEADER; out->bytes = g_active.clip_bytes - PET_CEXT_CLIP_HEADER; out->meta = g_active.clip_meta; out->generation = expected; }
  }
  unlock(); return out->token != 0;
}
bool pet_cmedia_unpin(uint64_t token) {
  if (!token) return false;
  lock();
  if (token >= CMEDIA_RELEASED) { unlock(); return false; }
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) {
    uint32_t expected = (uint32_t)token;
    if (!atomic_compare_exchange_strong(&g_leases[i].token, &expected, 0)) continue;
    unsigned bank = g_leases[i].bank;
    g_reclaimed++;
    if (--g_banks[bank].pins == 0 && (bank != g_current || (!g_active.sample_count && !g_active.clip_ready && !g_active.big_count))) bank_free(&g_banks[bank]);
    unlock(); return true;
  }
  unlock(); return false;
}
/* Compatibility entry points fail closed: an untracked borrow is never returned. */
const int16_t *pet_cmedia_sample_pcm(unsigned index, uint32_t *frames) {
  (void)index; if (frames) *frames = 0; return NULL;
}
const uint8_t *pet_cmedia_clip_stream(unsigned index, size_t *bytes, pet_cext_clip_t *meta) {
  (void)index; if (bytes) *bytes = 0; if (meta) memset(meta, 0, sizeof(*meta)); return NULL;
}
bool pet_cmedia_big_meta(unsigned big, pet_cext_big_t *meta) { return pet_cmedia_big_meta_at(big, 0, meta); }
bool pet_cmedia_big_meta_at(unsigned big, uint32_t expected, pet_cext_big_t *meta) {
  lock(); bool ok = (!expected || expected == g_generation) && big < g_active.big_count; if (ok && meta) *meta = g_active.big_meta[big]; unlock(); return ok;
}
bool pet_cmedia_big_tile(unsigned big, unsigned tile, uint8_t *out, size_t capacity, size_t *bytes) { return pet_cmedia_big_tile_at(big, tile, 0, out, capacity, bytes); }
bool pet_cmedia_big_tile_at(unsigned big, unsigned tile, uint32_t expected, uint8_t *out, size_t capacity, size_t *bytes) {
  if (bytes) *bytes = 0;
  lock();
  bool ok = (!expected || expected == g_generation) && big < g_active.big_count && g_active.big_table[big] && out;
  uint32_t offset = 0, size = 0; char path[32];
  if (ok) { ok = pet_cext_big_tile(g_active.big_table[big], g_active.big_meta[big].bytes, tile, &offset, &size) && size <= capacity; strcpy(path, g_active.big_path[big]); }
  if (!ok) { unlock(); return false; }
  FILE *f = fopen(path, "rb"); if (!f) { unlock(); return false; }
  ok = fseek(f, (long) offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size; fclose(f);
  unlock();
  if (ok && bytes) *bytes = size;
  return ok;
}
bool pet_cmedia_big_meta_try_at(unsigned big, uint32_t expected, pet_cext_big_t *meta) {
  if (!meta || !expected || !try_lock()) return false;
  bool ok = expected == g_generation && big < g_active.big_count;
  if (ok) *meta = g_active.big_meta[big];
  unlock(); return ok;
}
bool pet_cmedia_big_tile_try_at(unsigned big, unsigned tile, uint32_t expected, uint8_t *out, size_t capacity, size_t *bytes) {
  if (bytes) *bytes = 0;
#ifdef ESP_PLATFORM
  /* Admission precedes the store lock: never wait behind an admitted writer.
   * Keep occupancy through fclose. Broker calls hold only their short mux. */
  if (!expected || !out) return false;
  pet_resource_owner_t owner = {0}; pet_resource_lease_t lease = {0};
  if (pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK), &owner) != PET_RESOURCE_OK) return false;
  if (pet_resources_device_acquire(owner, PET_RESOURCE_STORAGE_WORK, 1, &lease) != PET_RESOURCE_OK) { pet_resources_device_revoke(owner); return false; }
  bool ok = false;
  if (try_lock()) {
    uint32_t offset = 0, size = 0;
    if (expected == g_generation && big < g_active.big_count && g_active.big_table[big]
        && pet_cext_big_tile(g_active.big_table[big], g_active.big_meta[big].bytes, tile, &offset, &size)
        && size <= capacity && size <= PET_CEXT_BIG_TILE_MAX_BYTES && pet_resources_device_current(lease)) {
      FILE *f = fopen(g_active.big_path[big], "rb");
      if (f) { ok = fseek(f, (long)offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size; fclose(f); }
      if (ok && bytes) *bytes = size;
    }
    unlock();
  }
  (void)pet_resources_device_finish(lease); pet_resources_device_revoke(owner);
  return ok;
#else
  /* Host callers need an explicit broker double; never bypass admission. */
  (void)big; (void)tile; (void)expected; (void)out; (void)capacity; return false;
#endif
}
uint32_t pet_cmedia_generation(void) { lock(); uint32_t g = g_generation; unlock(); return g; }

bool pet_cmedia_stats(pet_cmedia_stats_t *out) {
  if (!out) return false;
  lock(); memset(out, 0, sizeof(*out));
  out->generation = g_generation; out->tokens_issued = g_token;
  out->tokens_reclaimed = g_reclaimed; out->deferred_reclaimed = g_deferred_reclaimed;
  for (unsigned i = 0; i < PET_CMEDIA_BANKS; i++) {
    const cmedia_bank_t *b = &g_banks[i]; bool live = b->samples || b->clip;
    if (b->samples) out->sample_alloc_bytes += PET_SAMPLE_BUDGET_BYTES + PET_SAMPLE_MAX_SLOTS * PET_CEXT_SAMPLE_HEADER;
    if (b->clip) out->clip_alloc_bytes += PET_CEXT_CLIP_MAX_BYTES;
    for (unsigned j = 0; j < PET_CEXT_BIG_MAX; j++) if (b->big_table[j]) {
      live = true; out->table_alloc_bytes += PET_CEXT_BIG_HEADER + PET_CEXT_BIG_MAX_TILES * 8;
    }
    out->live_banks += live; out->pinned_banks += b->pins != 0; out->bank_pins += b->pins;
  }
  for (unsigned i = 0; i < PET_CMEDIA_LEASES; i++) out->live_tokens += atomic_load(&g_leases[i].token) != 0;
  unlock(); return true;
}
