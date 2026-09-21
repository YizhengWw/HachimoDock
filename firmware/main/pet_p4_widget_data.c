/* Validates full snapshots before atomic replacement; expires by monotonic time. */
#include "pet_p4_widget_data.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
typedef struct {
  bool used;
  char source[48], status[8], message[91], date[11];
  uint64_t received_ms;
  uint32_t ttl_ms;
  uint8_t count;
  pet_p4_data_row_t rows[PET_P4_DATA_MAX_ROWS];
} data_snapshot_t;
static data_snapshot_t g_sources[4];
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool text(const cJSON *obj, const char *key, char *out, size_t size) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= size) return false;
  for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++) if (*p < 32 || *p == 127) return false;
  strcpy(out, v->valuestring);
  return true;
}
static bool integer(const cJSON *obj, const char *key, int min, int max, int *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max || v->valuedouble != v->valueint) return false;
  *out = v->valueint; return true;
}
static bool safe_source(const char *s) {
  if (!s[0]) return false;
  for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-')) return false;
  return true;
}
bool pet_p4_widget_data_apply(const cJSON *payload, uint64_t now_ms) {
  int schema, ttl;
  if (!cJSON_IsObject(payload) || !integer(payload, "schema", 1, 1, &schema)
      || !integer(payload, "ttlMs", 5000, 300000, &ttl)) return false;
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(payload, "rows");
  if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > PET_P4_DATA_MAX_ROWS) return false;
  data_snapshot_t *next = calloc(1, sizeof(*next));
  if (!next) return false;
  bool ok = false;
  if (!text(payload, "source", next->source, sizeof(next->source)) || !safe_source(next->source)
      || !text(payload, "status", next->status, sizeof(next->status))
      || !text(payload, "message", next->message, sizeof(next->message))) goto done;
  if (strcmp(next->status, "ok") && strcmp(next->status, "error") && strcmp(next->status, "empty")) goto done;
  if (cJSON_HasObjectItem(payload, "date")) {
    if (!text(payload, "date", next->date, sizeof(next->date))) goto done;
    if (next->date[0]) {
      if (strlen(next->date) != 10) goto done;
      for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (next->date[i] != '-') goto done; }
        else if (next->date[i] < '0' || next->date[i] > '9') goto done;
      }
    }
  }
  next->count = cJSON_GetArraySize(rows);
  for (int i = 0; i < next->count; i++) {
    const cJSON *row = cJSON_GetArrayItem(rows, i);
    pet_p4_data_row_t *r = &next->rows[i];
    int tone;
    if (!text(row, "id", r->id, sizeof(r->id)) || !r->id[0]
        || !text(row, "label", r->label, sizeof(r->label))
        || !text(row, "value", r->value, sizeof(r->value))
        || !text(row, "detail", r->detail, sizeof(r->detail))
        || !text(row, "meta", r->meta, sizeof(r->meta))
        || !integer(row, "tone", -1, 1, &tone)) goto done;
    for (int j = 0; j < i; j++) if (!strcmp(next->rows[j].id, r->id)) goto done;
    r->tone = tone;
  }
  next->used = true; next->ttl_ms = ttl; next->received_ms = now_ms;
  portENTER_CRITICAL(&g_lock);
  int slot = -1;
  for (int i = 0; i < 4; i++) if (g_sources[i].used && !strcmp(g_sources[i].source, next->source)) { slot = i; break; }
  if (slot < 0) for (int i = 0; i < 4; i++) if (!g_sources[i].used) { slot = i; break; }
  if (slot >= 0) { g_sources[slot] = *next; ok = true; }
  portEXIT_CRITICAL(&g_lock);
done:
  free(next); return ok;
}
void pet_p4_widget_data_view(const char *source, int32_t page, uint64_t now_ms, pet_p4_data_view_t *out) {
  memset(out, 0, sizeof(*out));
  out->enabled = source && source[0]; out->pages = 1;
  if (!out->enabled) return;
  portENTER_CRITICAL(&g_lock);
  for (int i = 0; i < 4; i++) {
    const data_snapshot_t *s = &g_sources[i];
    if (!s->used || strcmp(s->source, source)) continue;
    out->received = true;
    out->stale = now_ms < s->received_ms || now_ms - s->received_ms > s->ttl_ms;
    out->pages = s->count ? (s->count + PET_P4_DATA_PAGE_ROWS - 1) / PET_P4_DATA_PAGE_ROWS : 1;
    out->page = ((page % out->pages) + out->pages) % out->pages;
    strcpy(out->status, s->status); strcpy(out->message, s->message);
    strcpy(out->date, s->date);
    int start = out->page * PET_P4_DATA_PAGE_ROWS;
    out->count = s->count > start ? s->count - start : 0;
    if (out->count > PET_P4_DATA_PAGE_ROWS) out->count = PET_P4_DATA_PAGE_ROWS;
    memcpy(out->rows, s->rows + start, out->count * sizeof(out->rows[0]));
    break;
  }
  portEXIT_CRITICAL(&g_lock);
}
