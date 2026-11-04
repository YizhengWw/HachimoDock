/* Validates full snapshots before atomic replacement; expires by monotonic time. */
#include "pet_p4_widget_data.h"
#if PET_P4_DATA_ROWS
#include "pet_p4_data_rows.h"
#endif
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif
typedef struct {
  bool used, clear_on_expire;
  char source[48], status[8], message[91], date[11];
  uint64_t received_ms;
  uint32_t ttl_ms;
  uint8_t count;
  pet_p4_data_row_t rows[PET_P4_DATA_MAX_ROWS];
} data_snapshot_t;
static EXT_RAM_BSS_ATTR data_snapshot_t g_sources[PET_P4_DATA_MAX_SOURCES];
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static bool snapshot_stale(const data_snapshot_t *s,uint64_t now_ms){
  return now_ms<s->received_ms||now_ms-s->received_ms>s->ttl_ms;
}
#if PET_P4_DATA_ROWS
/* One active filter at a time (the visible component); values follow its int vars. */
static struct {bool used;char source[48];pet_data_rows_config_t config;int32_t values[PET_DATA_FILTERS_MAX];} g_filter;
/* Caller holds g_lock. Visible indexes after the active filter, source order. */
static unsigned visible_rows(const data_snapshot_t *s,uint8_t out[PET_P4_DATA_MAX_ROWS]){
  const pet_data_rows_config_t *c=g_filter.used&&!strcmp(g_filter.source,s->source)?&g_filter.config:NULL;
  return pet_data_rows_filter(s->rows,s->count,c,c?g_filter.values:NULL,out);
}
void pet_p4_widget_data_set_filter(const char *source,const pet_data_rows_config_t *config,const int32_t *values){
  portENTER_CRITICAL(&g_lock);
  if(!source||!source[0]||!config||!config->enabled){g_filter.used=false;}
  else{g_filter.used=true;strncpy(g_filter.source,source,sizeof(g_filter.source)-1);g_filter.source[sizeof(g_filter.source)-1]=0;g_filter.config=*config;
    for(unsigned i=0;i<PET_DATA_FILTERS_MAX;i++)g_filter.values[i]=values?values[i]:0;}
  portEXIT_CRITICAL(&g_lock);
}
bool pet_p4_widget_data_select(const char *source,int32_t select,uint64_t now_ms,pet_p4_data_row_t *out){
  bool ok=false;if(!source||!source[0]||!out)return false;
  portENTER_CRITICAL(&g_lock);
  for(int i=0;i<PET_P4_DATA_MAX_SOURCES;i++)if(g_sources[i].used&&!strcmp(g_sources[i].source,source)){
    if(g_sources[i].clear_on_expire&&snapshot_stale(&g_sources[i],now_ms))break;
    uint8_t visible[PET_P4_DATA_MAX_ROWS];unsigned n=visible_rows(&g_sources[i],visible);int at=pet_data_rows_select(n,select);
    if(at>=0){*out=g_sources[i].rows[visible[at]];ok=true;}
    break;
  }
  portEXIT_CRITICAL(&g_lock);return ok;
}
#endif
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
  const cJSON *clear_on_expire = cJSON_GetObjectItemCaseSensitive(payload, "clearOnExpire");
  if (clear_on_expire && !cJSON_IsBool(clear_on_expire)) return false;
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
  next->clear_on_expire = cJSON_IsTrue(clear_on_expire);
  portENTER_CRITICAL(&g_lock);
  int slot = -1;
  for (int i = 0; i < PET_P4_DATA_MAX_SOURCES; i++) if (g_sources[i].used && !strcmp(g_sources[i].source, next->source)) { slot = i; break; }
  if (slot < 0) for (int i = 0; i < PET_P4_DATA_MAX_SOURCES; i++) if (!g_sources[i].used) { slot = i; break; }
  if (slot >= 0) { g_sources[slot] = *next; ok = true; }
  portEXIT_CRITICAL(&g_lock);
done:
  free(next); return ok;
}
/* Caller holds g_lock. Preserve signed modulo for negative/int32-edge pages. */
static pet_p4_data_paging_t snapshot_paging(const data_snapshot_t *s,int32_t page){
  int count=s->count;
#if PET_P4_DATA_ROWS
  uint8_t visible[PET_P4_DATA_MAX_ROWS];count=(int)visible_rows(s,visible);
#endif
  int pages=count?(count+PET_P4_DATA_PAGE_ROWS-1)/PET_P4_DATA_PAGE_ROWS:1;
  return (pet_p4_data_paging_t){.pages=(uint8_t)pages,.page=(uint8_t)(((page%pages)+pages)%pages)};
}
pet_p4_data_paging_t pet_p4_widget_data_paging(const char *source,int32_t page){
  pet_p4_data_paging_t out={.pages=1};
  if(!source||!source[0])return out;
  portENTER_CRITICAL(&g_lock);
  for(int i=0;i<PET_P4_DATA_MAX_SOURCES;i++)if(g_sources[i].used&&!strcmp(g_sources[i].source,source)){
    out=snapshot_paging(&g_sources[i],page);break;
  }
  portEXIT_CRITICAL(&g_lock);return out;
}
void pet_p4_widget_data_view(const char *source, int32_t page, uint64_t now_ms, pet_p4_data_view_t *out) {
  memset(out, 0, sizeof(*out));
  out->enabled = source && source[0]; out->pages = 1;
  if (!out->enabled) return;
  portENTER_CRITICAL(&g_lock);
  for (int i = 0; i < PET_P4_DATA_MAX_SOURCES; i++) {
    const data_snapshot_t *s = &g_sources[i];
    if (!s->used || strcmp(s->source, source)) continue;
    out->received = true;
    out->stale = snapshot_stale(s,now_ms);
    if (s->clear_on_expire && out->stale) {
      strcpy(out->status, "empty");
      strcpy(out->message, "数据已过期，请刷新");
      break;
    }
    pet_p4_data_paging_t paging=snapshot_paging(s,page);
    out->pages=paging.pages;out->page=paging.page;
    strcpy(out->status, s->status); strcpy(out->message, s->message);
    strcpy(out->date, s->date);
    int start = out->page * PET_P4_DATA_PAGE_ROWS;
#if PET_P4_DATA_ROWS
    uint8_t visible[PET_P4_DATA_MAX_ROWS];int total=(int)visible_rows(s,visible);
    out->count = total > start ? total - start : 0;
    if (out->count > PET_P4_DATA_PAGE_ROWS) out->count = PET_P4_DATA_PAGE_ROWS;
    for (int k = 0; k < out->count; k++) out->rows[k] = s->rows[visible[start + k]];
#else
    out->count = s->count > start ? s->count - start : 0;
    if (out->count > PET_P4_DATA_PAGE_ROWS) out->count = PET_P4_DATA_PAGE_ROWS;
    memcpy(out->rows, s->rows + start, out->count * sizeof(out->rows[0]));
#endif
    break;
  }
  portEXIT_CRITICAL(&g_lock);
}
