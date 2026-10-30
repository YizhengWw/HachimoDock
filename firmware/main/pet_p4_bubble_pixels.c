#include "pet_p4_bubble_pixels.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#define CACHE_SLOTS 32
#define CACHE_BYTES (1024U * 1024U)
#define IMAGE_BYTES (128U * 1024U)
#define MAX_PAGES 16
#define VIEW_SLOTS 12
#define WAIT_MS 3000U

typedef struct {
  uint64_t key;
  uint8_t *data;
  size_t size;
  uint32_t offsets[MAX_PAGES + 1];
  unsigned style, pages, age;
} text_image_t;
static text_image_t cache[CACHE_SLOTS], staging;
static size_t received, cache_bytes;
static uint32_t expected_checksum;
static unsigned clock_age;
static atomic_uint revision;
static SemaphoreHandle_t mutex;
typedef struct {
  uint64_t owner, target, pending_since, seen_ms, title_key, body_key;
  unsigned style;
  bool occupied, waiting, degraded;
  pet_bubble_text_view_t shown;
} text_view_t;
static text_view_t views[VIEW_SLOTS];
static bool expect_pixels;
static uint64_t view_now_ms;

static unsigned width_for(unsigned style) {
  return style == PET_BUBBLE_TITLE ? 474 : (style == PET_BUBBLE_BODY || style == PET_BUBBLE_BODY_CLIPPED) ? 496 : 348;
}
static unsigned height_for(unsigned style) { return style == PET_BUBBLE_BODY || style == PET_BUBBLE_BODY_CLIPPED ? 54 : 38; }
static uint64_t text_key(unsigned style, const char *text) {
  uint64_t hash = (UINT64_C(14695981039346656037) ^ style) * UINT64_C(1099511628211);
  for (const unsigned char *p = (const unsigned char *)text; p && *p; p++)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  return hash;
}
static const char *string_value(const cJSON *json, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
static bool number_value(const cJSON *json, const char *name, unsigned maximum, unsigned *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0
      || item->valuedouble > maximum || floor(item->valuedouble) != item->valuedouble) return false;
  *out = (unsigned)item->valuedouble;
  return true;
}
static bool hex_value(const char *value, unsigned digits, uint64_t *out) {
  if (strlen(value) != digits) return false;
  uint64_t result = 0;
  for (unsigned i = 0; i < digits; i++) {
    unsigned char ch = (unsigned char)value[i];
    unsigned digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : 16;
    if (digit > 15) return false;
    result = (result << 4) | digit;
  }
  *out = result;
  return true;
}
static void discard(text_image_t *image) { free(image->data); memset(image, 0, sizeof(*image)); }
static int find(uint64_t key, unsigned style) {
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].data && cache[i].key == key && cache[i].style == style) return i;
  return -1;
}
static void evict(int slot) { cache_bytes -= cache[slot].size; discard(&cache[slot]); }
static bool pinned(const text_image_t *image) {
  for (unsigned i = 0; i < VIEW_SLOTS; i++) {
    const text_view_t *view = &views[i];
    if (!view->occupied || !view->shown.pixels || view_now_ms - view->seen_ms > 5000) continue;
    if ((image->style == PET_BUBBLE_TITLE && view->shown.title[0]
          && image->key == view->title_key)
        || (image->style == view->style && view->shown.body[0]
          && image->key == view->body_key)) return true;
  }
  return false;
}
static int oldest(int excluded) {
  int slot = -1;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (i != excluded && cache[i].data && !pinned(&cache[i])
        && (slot < 0 || cache[i].age < cache[slot].age)) slot = i;
  return slot;
}
bool pet_bubble_pixels_init(void) {
  if (!mutex) mutex = xSemaphoreCreateMutex();
  return mutex != NULL;
}

static bool handle_locked(const cJSON *payload) {
  const char *phase = string_value(payload, "phase");
  uint64_t key;
  if (!hex_value(string_value(payload, "id"), 16, &key)) return false;
  if (!strcmp(phase, "begin")) {
    unsigned style, size;
    uint64_t checksum;
    const cJSON *sizes = cJSON_GetObjectItemCaseSensitive(payload, "pageSizes");
    if (!number_value(payload, "style", 4, &style) || !style
        || !number_value(payload, "totalBytes", IMAGE_BYTES, &size) || !size
        || !hex_value(string_value(payload, "checksum"), 8, &checksum)
        || !cJSON_IsArray(sizes)) return false;
    int pages = cJSON_GetArraySize(sizes);
    if (pages < 1 || pages > (style == PET_BUBBLE_BODY ? MAX_PAGES : 1)) return false;
    uint32_t offsets[MAX_PAGES + 1] = {0};
    for (int i = 0; i < pages; i++) {
      const cJSON *item = cJSON_GetArrayItem(sizes, i);
      if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 1
          || item->valuedouble > size || floor(item->valuedouble) != item->valuedouble) return false;
      offsets[i + 1] = offsets[i] + (unsigned)item->valuedouble;
      if (offsets[i + 1] > size) return false;
    }
    if (offsets[pages] != size) return false;
    discard(&staging); received = 0;
    staging.data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!staging.data) return false;
    staging.size = size; staging.key = key; staging.style = style; staging.pages = (unsigned)pages;
    memcpy(staging.offsets, offsets, sizeof(offsets));
    expected_checksum = (uint32_t)checksum;
    return true;
  }
  if (!staging.data || staging.key != key) return false;
  if (!strcmp(phase, "chunk")) {
    unsigned offset;
    const char *data = string_value(payload, "data");
    if (!number_value(payload, "offset", IMAGE_BYTES, &offset) || offset != received
        || !data[0] || strlen(data) > 1368) return false;
    size_t bytes = 0;
    if (mbedtls_base64_decode(staging.data + received, staging.size - received,
        &bytes, (const unsigned char *)data, strlen(data)) || !bytes || bytes > 1024) return false;
    received += bytes;
    return true;
  }
  if (strcmp(phase, "end") || received != staging.size) return false;
  uint32_t checksum = UINT32_C(2166136261);
  for (size_t i = 0; i < staging.size; i++) checksum = (checksum ^ staging.data[i]) * UINT32_C(16777619);
  if (checksum != expected_checksum) { discard(&staging); return false; }
  for (unsigned page = 0; page < staging.pages; page++) {
    unsigned pixels = 0;
    for (size_t i = staging.offsets[page]; i < staging.offsets[page + 1]; i++)
      pixels += (staging.data[i] & 15U) + 1U;
    if (pixels != width_for(staging.style) * height_for(staging.style)) {
      discard(&staging); return false;
    }
  }
  int slot = find(key, staging.style);
  size_t replaced_size = slot >= 0 ? cache[slot].size : 0;
  while (cache_bytes - replaced_size + staging.size > CACHE_BYTES) {
    int victim = oldest(slot);
    if (victim < 0) { discard(&staging); return false; }
    evict(victim);
  }
  if (slot < 0) for (int i = 0; i < CACHE_SLOTS; i++) if (!cache[i].data) { slot = i; break; }
  if (slot < 0) slot = oldest(-1);
  if (slot < 0) { discard(&staging); return false; }
  if (cache[slot].data) evict(slot);
  staging.age = ++clock_age;
  cache[slot] = staging;
  cache_bytes += staging.size;
  memset(&staging, 0, sizeof(staging));
  atomic_fetch_add(&revision, 1);
  return true;
}
bool pet_bubble_pixels_handle(const cJSON *payload, char *error, size_t error_size) {
  if (!mutex) { snprintf(error, error_size, "text cache unavailable"); return false; }
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = handle_locked(payload);
  xSemaphoreGive(mutex);
  if (!ok) snprintf(error, error_size, "invalid or incomplete text image");
  return ok;
}
uint32_t pet_bubble_pixels_revision(void) { return atomic_load(&revision); }
void pet_bubble_pixels_expect(bool enabled, uint64_t now_ms) {
  if (!mutex) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  view_now_ms = now_ms;
  if (expect_pixels != enabled) { expect_pixels = enabled; atomic_fetch_add(&revision, 1); }
  xSemaphoreGive(mutex);
}
void pet_bubble_pixels_select(const char *owner, unsigned style, const char *title,
    const char *body, uint64_t now_ms, pet_bubble_text_view_t *out) {
  if (!title) title = "";
  if (!body) body = "";
  if (!mutex) {
    if (out) { memset(out, 0, sizeof(*out)); snprintf(out->title, sizeof(out->title), "%s", title);
      snprintf(out->body, sizeof(out->body), "%s", body); }
    return;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  view_now_ms = now_ms;
  uint64_t owner_key = text_key(style, owner ? owner : "");
  text_view_t *view = NULL, *victim = &views[0];
  for (unsigned i = 0; i < VIEW_SLOTS; i++) {
    if (views[i].occupied && views[i].owner == owner_key) { view = &views[i]; break; }
    if (!views[i].occupied || (victim->occupied && views[i].seen_ms < victim->seen_ms)) victim = &views[i];
  }
  if (!view) { view = victim; memset(view, 0, sizeof(*view)); view->occupied = true;
    view->owner = owner_key; view->style = style; }
  view->seen_ms = now_ms;
  uint64_t target = text_key(PET_BUBBLE_TITLE, title) ^ (text_key(style, body) * UINT64_C(1099511628211));
  bool ready = (!title[0] || find(text_key(PET_BUBBLE_TITLE, title), PET_BUBBLE_TITLE) >= 0)
    && (!body[0] || find(text_key(style, body), style) >= 0);
  if (view->target != target) {
    view->target = target;
    // Keep the first deadline during streaming: newer chunks cannot postpone fallback forever.
    if (!view->waiting) view->pending_since = now_ms;
    view->waiting = true;
  }
  if (ready || !expect_pixels || view->degraded || now_ms - view->pending_since >= WAIT_MS) {
    bool changed = strcmp(view->shown.title, title) || strcmp(view->shown.body, body)
      || view->shown.pixels != ready;
    if (changed) {
      snprintf(view->shown.title, sizeof(view->shown.title), "%s", title);
      snprintf(view->shown.body, sizeof(view->shown.body), "%s", body);
      view->shown.pixels = ready; view->shown.since_ms = now_ms;
      view->title_key = text_key(PET_BUBBLE_TITLE, title);
      view->body_key = text_key(style, body);
      atomic_fetch_add(&revision, 1);
    }
    view->waiting = false;
    view->degraded = expect_pixels && !ready;
  }
  if (out) *out = view->shown;
  xSemaphoreGive(mutex);
}
unsigned pet_bubble_pixels_pages(unsigned style, const char *text) {
  if (!mutex || !text || !*text) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  int slot = find(text_key(style, text), style);
  unsigned pages = slot < 0 ? 0 : cache[slot].pages;
  xSemaphoreGive(mutex);
  return pages;
}
static uint16_t blend(uint16_t background, uint16_t color, unsigned alpha) {
  if (alpha == 15) return color;
  unsigned inverse = 15 - alpha;
  unsigned r = (((color >> 11) & 31) * alpha + ((background >> 11) & 31) * inverse + 7) / 15;
  unsigned g = (((color >> 5) & 63) * alpha + ((background >> 5) & 63) * inverse + 7) / 15;
  unsigned b = ((color & 31) * alpha + (background & 31) * inverse + 7) / 15;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
bool pet_bubble_pixels_draw(unsigned style, const char *text, unsigned page,
    uint16_t *framebuffer, int x, int y, int clip_width, uint16_t color) {
  if (!mutex || !text || !*text || !framebuffer) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  int slot = find(text_key(style, text), style);
  if (slot < 0) { xSemaphoreGive(mutex); return false; }
  text_image_t *image = &cache[slot];
  image->age = ++clock_age;
  page %= image->pages;
  unsigned width = width_for(style), pixel = 0;
  for (size_t i = image->offsets[page]; i < image->offsets[page + 1]; i++) {
    unsigned alpha = image->data[i] >> 4, run = (image->data[i] & 15U) + 1U;
    for (unsigned end = pixel + run; pixel < end; pixel++) {
      if (!alpha) continue;
      int column = (int)(pixel % width), dx = x + column, dy = y + (int)(pixel / width);
      if (column >= clip_width || dx < 0 || dx >= 640 || dy < 0 || dy >= 480) continue;
      uint16_t *out = framebuffer + dy * 640 + dx;
      *out = blend(*out, color, alpha);
    }
  }
  xSemaphoreGive(mutex);
  return true;
}
