/*
 * Optional alpha assets: latest text is authoritative, never held for pixels. One
 * route/client/boot-owned stage; whole-asset scheduling is host-owned. Cache mutex protects
 * metadata; pinned immutable pages survive retirement.
 */
#include "pet_p4_bubble_pixels.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#define CACHE_SLOTS 8
#define CACHE_BYTES (256U * 1024U)
#define IMAGE_BYTES (32U * 1024U)
#define VIEW_SLOTS 12
#define IDLE_MS 3000U
#define ABSOLUTE_MS 10000U
#define SCHEMA "p4-text-alpha-v2:rle4:1:"
typedef struct {
  uint64_t        key;
  uint8_t        *data;
  size_t          size;
  uint32_t        checksum;
  unsigned        style, page, pages, age, readers;
  bool            retired;
} text_image_t;
typedef struct {
  uint64_t        key;
  unsigned        style;
} desired_asset_t;
typedef struct {
  uint64_t        owner, version, scope;
  desired_asset_t assets[4];
  unsigned        asset_count;
} desired_t;
typedef struct {
  uint64_t        owner, target, since_ms, seen_ms, title_key, body_key;
  unsigned        style;
  bool            occupied;
} text_view_t;
static text_image_t cache[CACHE_SLOTS], staging;
static desired_t desired[VIEW_SLOTS];
static unsigned desired_count;
static text_view_t views[VIEW_SLOTS];
static size_t received, cache_bytes;
static unsigned stage_pixels, clock_age;
static uint32_t stage_hash;
static atomic_uint revision;
static uint64_t stage_owner, stage_version, stage_started, stage_seen, cache_now_ms;
static uint32_t lifecycle_epoch;
typedef struct {
  uint64_t        key;
  unsigned        style, page;
} need_t;
static need_t needs[8];
static unsigned need_count;
static uint64_t last_need_ms;
static char     boot[17], client[33], transfer[33];
static uintptr_t route;
static bool     blocked;
static SemaphoreHandle_t mutex;
static unsigned
width_for(unsigned s)
{
  return s == 1 ? 474 : (s == 2 || s == 4) ? 496 : 348;
}
static unsigned
height_for(unsigned s)
{
  return s == 2 || s == 4 ? 54 : 38;
}
static uint64_t hash_bytes(uint64_t hash, const char *s){
  for (const unsigned char *p = (const unsigned char *)s; p && *p; p++)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  return hash;
}
static uint64_t text_key(unsigned style, const char *text){
  uint64_t        h = hash_bytes(UINT64_C(14695981039346656037), SCHEMA);
  return hash_bytes((h ^ style) * UINT64_C(1099511628211), text);
}
static const char *string_value(const cJSON * j, const char *n){
  const           cJSON *x = cJSON_GetObjectItemCaseSensitive(j, n);
  return cJSON_IsString(x) && x->valuestring ? x->valuestring : "";
}
static bool     number_value(const cJSON * j, const char *n, unsigned max, unsigned *out){
  const           cJSON *x = cJSON_GetObjectItemCaseSensitive(j, n);
  if (!cJSON_IsNumber(x) || !isfinite(x->valuedouble) || x->valuedouble < 0 || x->valuedouble > max || floor(x->valuedouble) != x->valuedouble)
    return false;
  *out = (unsigned)x->valuedouble;
  return true;
}
static bool
hex_string(const char *s, unsigned n)
{
  if (strlen(s) != n)
    return false;
  for (unsigned i = 0; i < n; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
      return false;
  return true;
}
static bool     hex_value(const char *s, unsigned n, uint64_t * out){
  if (!hex_string(s, n) || n > 16)
    return false;
  uint64_t        h = 0;
  for (unsigned i = 0; i < n; i++)
    h = (h << 4) | (unsigned)(s[i] <= '9' ? s[i] - '0' : s[i] - 'a' + 10);
  *out = h;
  return true;
}
static void     discard(text_image_t * i) {
  free(i->data);
  memset(i, 0, sizeof(*i));
}
static void     bump(void){
  atomic_fetch_add(&revision, 1);
}
static void     clear_stage(void){
  discard(&staging);
  received = stage_pixels = 0;
  transfer[0] = 0;
  stage_owner = stage_version = stage_started = stage_seen = 0;
}
static void     evict(int n){
  cache_bytes -= cache[n].size;
  discard(&cache[n]);
  bump();
}
static void     retire_cache(void){
  need_count = 0;
  last_need_ms = 0;
  lifecycle_epoch++;
  memset(views, 0, sizeof(views));
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data) {
      if (cache[i].readers)
        cache[i].retired = true;
      else
        evict(i);
    }
  bump();
}
static int
find(uint64_t key, unsigned style, unsigned page)
{
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && !cache[i].retired && cache[i].key == key && cache[i].style == style && cache[i].page == page)
      return i;
  return -1;
}
static bool
pinned(const text_image_t * image)
{
  if (image->readers)
    return true;
  for (unsigned i = 0; i < VIEW_SLOTS; i++) {
    const           text_view_t *v = &views[i];
    if (!v->occupied || cache_now_ms < v->seen_ms || cache_now_ms - v->seen_ms > 5000)
      continue;
    if ((image->style == 1 && image->key == v->title_key) || (image->style == v->style && image->key == v->body_key))
      return true;
  }
  return false;
}
static int
oldest(void)
{
  int             n = -1;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && !pinned(&cache[i]) && (n < 0 || cache[i].age < cache[n].age))
      n = i;
  return n;
}
static void
missing(uint64_t key, unsigned style, unsigned page)
{
  if (blocked || !client[0] || need_count >= 8)
    return;
  for (unsigned i = 0; i < need_count; i++)
    if (needs[i].key == key && needs[i].style == style && needs[i].page == page)
      return;
  needs[need_count++] = (need_t) {
    key, style, page
  };
}
/* One small coalesced notice per second, outside the cache lock/renderer. */
bool
pet_bubble_pixels_next_need(uint64_t now, pet_bubble_need_t * out)
{
  if (!mutex || !out)
    return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (blocked || !client[0] || (last_need_ms && now - last_need_ms < 1000)) {
    xSemaphoreGive(mutex);
    return false;
  }
  while (need_count) {
    need_t          n = needs[0];
    memmove(needs, needs + 1, (--need_count) * sizeof(*needs));
    if (find(n.key, n.style, n.page) >= 0)
      continue;
    snprintf(out->id, sizeof(out->id), "%016" PRIx64, n.key);
    snprintf(out->boot, sizeof(out->boot), "%s", boot);
    snprintf(out->client, sizeof(out->client), "%s", client);
    out->style = n.style;
    out->page = n.page;
    out->route = (const void *)route;
    last_need_ms = now ? now : 1;
    xSemaphoreGive(mutex);
    return true;
  }
  xSemaphoreGive(mutex);
  return false;
}
static bool
current_asset(uint64_t owner, uint64_t version, uint64_t key, unsigned style)
{
  for (unsigned i = 0; i < desired_count; i++)
    if (desired[i].owner == owner && desired[i].version == version)
      for (unsigned k = 0; k < desired[i].asset_count; k++)
        if (desired[i].assets[k].key == key && desired[i].assets[k].style == style)
          return true;
  return false;
}
static void     expire(uint64_t now) {
  cache_now_ms = now;
  if (staging.data && (now < stage_started || now < stage_seen || now - stage_started >= ABSOLUTE_MS || now - stage_seen >= IDLE_MS || blocked || !current_asset(stage_owner, stage_version, staging.key, staging.style)))
    clear_stage();
}
bool            pet_bubble_pixels_init(void){
  if (!mutex) {
    mutex = xSemaphoreCreateMutex();
    if (mutex) {
      uint64_t        b = ((uint64_t) esp_random() << 32) | esp_random();
      snprintf(boot, sizeof(boot), "%016" PRIx64, b ? b : 1);
    }
  } return mutex != NULL;
}
const char     *
pet_bubble_pixels_boot(void)
{
  return boot;
}
void            pet_bubble_pixels_maintain(uint64_t now, bool pause){
  if (!mutex)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  blocked = pause;
  expire(now);
  xSemaphoreGive(mutex);
}
/*
 * Called only with accepted text state, before that state becomes visible. Payload tokens are
 * bounded capabilities for lifecycle correlation, not auth.
 */
void
pet_bubble_pixels_desire(const cJSON * payload, const void *ctx, const char *scope, uint64_t now)
{
  if (!mutex)
    return;
  const           cJSON *m = cJSON_GetObjectItemCaseSensitive(payload, "textPixels");
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (!cJSON_IsObject(m)) {
    if (client[0]) {
      clear_stage();
      retire_cache();
      client[0] = 0;
      desired_count = 0;
    }
  xSemaphoreGive(mutex);
    return;
  }
  const char     *b = string_value(m, "boot"), *c = string_value(m, "client");
  const           cJSON *owners = cJSON_GetObjectItemCaseSensitive(m, "owners");
  int             n = cJSON_GetArraySize(owners);
  desired_t       next[VIEW_SLOTS];
  (void)scope; /* owner list is a complete native snapshot across display roles */
  bool            valid = !strcmp(b, boot) && hex_string(c, 32) && cJSON_IsArray(owners) && n >= 0 && n <= VIEW_SLOTS;
  for (int i = 0; valid && i < n; i++) {
    const           cJSON *v = cJSON_GetArrayItem(owners, i);
    valid = hex_value(string_value(v, "id"), 16, &next[i].owner) && hex_value(string_value(v, "version"), 16, &next[i].version);
    for (int k = 0; k < i; k++)
      if (next[k].owner == next[i].owner)
        valid = false;
    const           cJSON *assets = cJSON_GetObjectItemCaseSensitive(v, "assets");
    int             count = cJSON_GetArraySize(assets);
    if (!cJSON_IsArray(assets) || count < 0 || count > 4)
      valid = false;
    next[i].asset_count = valid ? (unsigned)count : 0;
    for (unsigned k = 0; valid && k < next[i].asset_count; k++) {
      const           cJSON *asset = cJSON_GetArrayItem(assets, (int)k);
      valid = hex_value(string_value(asset, "id"), 16, &next[i].assets[k].key) && number_value(asset, "style", 4, &next[i].assets[k].style) && next[i].assets[k].style > 0;
    }
  }
  if (valid) {
    if (strcmp(c, client) || route != (uintptr_t) ctx) {
      clear_stage();
      retire_cache();
      desired_count = 0;
      snprintf(client, sizeof(client), "%s", c);
      route = (uintptr_t) ctx;
    }
    desired_count = (unsigned)n;
    memcpy(desired,next,desired_count*sizeof(*desired));
    expire(now);
  }
  xSemaphoreGive(mutex);
}
static bool
handle_locked(const cJSON * p, const void *ctx, uint64_t now, cJSON * reply, const char **error)
{
  expire(now);
  const char     *phase = string_value(p, "phase");
  uint64_t        key, owner, version;
  if (strcmp(string_value(p, "boot"), boot) || strcmp(string_value(p, "client"), client) || route != (uintptr_t) ctx || !client[0]
      || !hex_value(string_value(p, "owner"), 16, &owner) || !hex_value(string_value(p, "version"), 16, &version)
  || !hex_value(string_value(p, "id"), 16, &key) || !hex_string(string_value(p, "transfer"), 32)) {
    *error = "stale_peer";
    return false;
  }
  bool            matching = staging.data && stage_owner == owner && stage_version == version && staging.key == key && !strcmp(transfer, string_value(p, "transfer"));
  if (!strcmp(phase, "abort")) {
    if (matching)
      clear_stage();
    return true;
  }
  unsigned        frame_style;
  if (!number_value(p, "style", 4, &frame_style) || !current_asset(owner, version, key, frame_style)) {
    if (matching)
      clear_stage();
    *error = "stale_content";
    return false;
  }
  if (!strcmp(phase, "status")) {
    cJSON_AddNumberToObject(reply, "offset", matching ? received : 0);
    cJSON_AddBoolToObject(reply, "receiving", matching);
    return true;
  }
  if (blocked) {
    if (matching)
      clear_stage();
    *error = "protected_audio_or_update";
    return false;
  }
  if (!strcmp(phase, "begin")) {
    if (staging.data) {
      *error = "stage_busy";
      return false;
    } unsigned      style, size, page, pages;
    uint64_t        sum;
    const           cJSON *sizes = cJSON_GetObjectItemCaseSensitive(p, "pageSizes");
    const           cJSON *only = cJSON_GetArrayItem(sizes, 0);
    if (!number_value(p, "style", 4, &style) || !style || !number_value(p, "totalBytes", IMAGE_BYTES, &size) || !size
        || !number_value(p, "pageIndex", 15, &page) || !number_value(p, "pageCount", 16, &pages) || !pages || page >= pages || (style != 2 && (page || pages != 1))
        || !hex_value(string_value(p, "checksum"), 8, &sum) || !cJSON_IsArray(sizes) || cJSON_GetArraySize(sizes) != 1 || !cJSON_IsNumber(only) || only->valuedouble != size) {
      *error = "invalid_metadata";
      return false;
    }
    int             existing = find(key, style, page);
    if (existing >= 0) {
      text_image_t   *i = &cache[existing];
      if (i->size != size || i->checksum != (uint32_t) sum || i->pages != pages) {
        *error = "asset_conflict";
        return false;
      } cJSON_AddBoolToObject(reply, "cached", true);
      return true;
    }
    staging.data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!staging.data) {
      *error = "no_memory";
      return false;
    }
    staging.key = key;
    staging.size = size;
    staging.style = style;
    staging.page = page;
    staging.pages = pages;
    staging.checksum = (uint32_t) sum;
    stage_owner = owner;
    stage_version = version;
    stage_started = stage_seen = now;
    stage_hash = 2166136261U;
    stage_pixels = received = 0;
    snprintf(transfer, sizeof(transfer), "%s", string_value(p, "transfer"));
    cJSON_AddNumberToObject(reply, "offset", 0);
    return true;
  }
  if (!matching) {
    *error = "no_matching_stage";
    return false;
  }
  if (!strcmp(phase, "chunk")) {
    unsigned        offset;
    const char     *data = string_value(p, "data");
    uint8_t         decoded[512];
    size_t          bytes = 0;
    if (!number_value(p, "offset", IMAGE_BYTES, &offset) || offset != received || !data[0] || strlen(data) > 684
        || mbedtls_base64_decode(decoded, sizeof(decoded), &bytes, (const unsigned char *)data, strlen(data)) || !bytes || bytes > staging.size - received) {
      clear_stage();
      *error = "invalid_chunk";
      return false;
    }
    unsigned        pixels = stage_pixels;
    uint32_t        hash = stage_hash;
    for (size_t i = 0; i < bytes; i++) {
      pixels += (decoded[i] & 15U) + 1U;
      hash = (hash ^ decoded[i]) * 16777619U;
    }
    if (pixels > width_for(staging.style) * height_for(staging.style)) {
      clear_stage();
      *error = "invalid_geometry";
      return false;
    }
    memcpy(staging.data + received, decoded, bytes);
    received += bytes;
    stage_pixels = pixels;
    stage_hash = hash;
    stage_seen = now;
    cJSON_AddNumberToObject(reply, "offset", received);
    return true;
  }
  if (strcmp(phase, "end") || received != staging.size || stage_hash != staging.checksum || stage_pixels != width_for(staging.style) * height_for(staging.style)) {
    clear_stage();
    *error = "incomplete_or_invalid";
    return false;
  }
  while (cache_bytes + staging.size > CACHE_BYTES) {
    int             v = oldest();
    if (v < 0) {
      clear_stage();
      *error = "cache_pinned";
      return false;
    } evict(v);
  }
  int             slot = -1;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (!cache[i].data) {
      slot = i;
      break;
  }
  if (slot < 0)
    slot = oldest();
  if (slot < 0) {
    clear_stage();
    *error = "cache_pinned";
    return false;
  } if (cache[slot].data)
    evict(slot);
  staging.age = ++clock_age;
  cache[slot] = staging;
  cache_bytes += staging.size;
  memset(&staging, 0, sizeof(staging));
  received = 0;
  transfer[0] = 0;
  bump();
  return true;
}
bool
pet_bubble_pixels_handle_owned(const cJSON * p, const void *ctx, uint64_t now, cJSON * reply)
{
  if (!mutex || !reply)
    return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const char     *error = "invalid_request";
  bool            ok = handle_locked(p, ctx, now, reply, &error);
  cJSON_AddBoolToObject(reply, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(reply, "error", error);
  cJSON_AddStringToObject(reply, "boot", boot);
  cJSON_AddNumberToObject(reply, "cacheRevision", atomic_load(&revision));
  xSemaphoreGive(mutex);
  return ok;
}
uint32_t        pet_bubble_pixels_revision(void){
  return atomic_load(&revision);
}
static void     unpin(int slot){
  if (slot >= 0 && slot < CACHE_SLOTS && cache[slot].readers) {
    cache[slot].readers--;
    if (!cache[slot].readers && cache[slot].retired)
      evict(slot);
  }
}
void            pet_bubble_pixels_release(pet_bubble_text_view_t * out) {
  if (!mutex || !out)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  unpin(out->title_pin);
  unpin(out->body_pin);
  out->title_pin = out->body_pin = -1;
  out->pixels = false;
  xSemaphoreGive(mutex);
}
void
pet_bubble_pixels_pin_page(pet_bubble_text_view_t * out, unsigned page)
{
  if (!mutex || !out)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (out->epoch != lifecycle_epoch) {
    out->pixels = false;
    xSemaphoreGive(mutex);
    return;
  } unpin(out->body_pin);
  out->body_pin = -1;
  int             slot = find(text_key(out->style, out->body), out->style, page);
  if (slot >= 0) {
    out->body_pin = slot;
    cache[slot].readers++;
  } else if (out->body[0])
    missing(text_key(out->style, out->body), out->style, page);
  out->pixels = (!out->title[0] || out->title_pin >= 0) && (!out->body[0] || out->body_pin >= 0);
  xSemaphoreGive(mutex);
}
void
pet_bubble_pixels_select(const char *owner, unsigned style, const char *title, const char *body, uint64_t now, pet_bubble_text_view_t * out)
{
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  out->title_pin = out->body_pin = -1;
  out->style = style;
  snprintf(out->title, sizeof(out->title), "%s", title ? title : "");
  snprintf(out->body, sizeof(out->body), "%s", body ? body : "");
  if (!mutex)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cache_now_ms = now;
  out->epoch = lifecycle_epoch;
  uint64_t        tk = text_key(1, out->title), bk = text_key(style, out->body), id = text_key(style, owner ? owner : "");
  text_view_t    *v = NULL, *victim = &views[0];
  for (unsigned i = 0; i < VIEW_SLOTS; i++) {
    if (views[i].occupied && views[i].owner == id) {
      v = &views[i];
      break;
    } if (!views[i].occupied || views[i].seen_ms < victim->seen_ms)
      victim = &views[i];
  }
  if (!v) {
    v = victim;
    memset(v, 0, sizeof(*v));
    v->owner = id;
    v->occupied = true;
  }
  uint64_t        target = tk ^ (bk * UINT64_C(1099511628211));
  if (v->target != target) {
    v->target = target;
    v->since_ms = now;
  }
  v->seen_ms = now;
  v->title_key = tk;
  v->body_key = bk;
  v->style = style;
  out->since_ms = v->since_ms;
  int             a = find(tk, 1, 0), b = find(bk, style, 0);
  if (out->title[0] && a < 0)
    missing(tk, 1, 0);
  if (out->body[0] && b < 0)
    missing(bk, style, 0);
  if (out->title[0] && a >= 0) {
    out->title_pin = a;
    cache[a].readers++;
  } if (out->body[0] && b >= 0) {
    out->body_pin = b;
    cache[b].readers++;
  }
  out->pixels = (!out->title[0] || a >= 0) && (!out->body[0] || b >= 0);
  xSemaphoreGive(mutex);
}
unsigned        pet_bubble_pixels_pages(unsigned style, const char *text){
  if (!mutex || !text || !*text)
    return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  uint64_t        key = text_key(style, text);
  unsigned        pages = 0;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && !cache[i].retired && cache[i].key == key && cache[i].style == style) {
      pages = cache[i].pages;
      break;
    }
  xSemaphoreGive(mutex);
  return pages;
}
static uint16_t blend(uint16_t background, uint16_t color, unsigned alpha){
  if (alpha == 15)
    return color;
  unsigned        inverse = 15 - alpha;
  unsigned        r = (((color >> 11) & 31) * alpha + ((background >> 11) & 31) * inverse + 7) / 15;
  unsigned        g = (((color >> 5) & 63) * alpha + ((background >> 5) & 63) * inverse + 7) / 15;
  unsigned        b = ((color & 31) * alpha + (background & 31) * inverse + 7) / 15;
  return (uint16_t) ((r << 11) | (g << 5) | b);
}

bool
pet_bubble_pixels_draw_pinned(const pet_bubble_text_view_t * view, unsigned style, unsigned page, uint16_t * framebuffer, int x, int y, int clip_width, uint16_t color)
{
  if (!mutex || !view || !view->pixels || !framebuffer)
    return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  int             slot = style == PET_BUBBLE_TITLE ? view->title_pin : view->body_pin;
  if (slot < 0 || slot >= CACHE_SLOTS || !cache[slot].data || !cache[slot].readers || cache[slot].style != style || cache[slot].page != page) {
    xSemaphoreGive(mutex);
    return false;
  }
  text_image_t   *image = &cache[slot];
  image->age = ++clock_age;
  /* A draw owns a separate pin: a caller may release its view or the peer
   * may retire it after this unlock. Only the final unpin may free the page. */
  image->readers++;
  const uint8_t *data = image->data;
  size_t size = image->size;
  unsigned        width = width_for(style), pixel = 0;
  xSemaphoreGive(mutex);
  for (size_t i = 0; i < size; i++) {
    unsigned        alpha = data[i] >> 4, run = (data[i] & 15U) + 1U;
    for (unsigned end = pixel + run; pixel < end; pixel++) {
      if (!alpha)
        continue;
      int             col = (int)(pixel % width), dx = x + col, dy = y + (int)(pixel / width);
      if (col >= clip_width || dx < 0 || dx >= 640 || dy < 0 || dy >= 480)
        continue;
      uint16_t       *out = framebuffer + dy * 640 + dx;
      *out = blend(*out, color, alpha);
    }
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  unpin(slot);
  xSemaphoreGive(mutex);
  return true;
}
