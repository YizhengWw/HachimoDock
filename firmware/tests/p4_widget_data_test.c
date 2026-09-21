#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_widget_data.h"
int main(void) {
  const char *json = "{\"schema\":1,\"source\":\"test.quotes\",\"ttlMs\":30000,\"status\":\"ok\",\"message\":\"test\",\"rows\":[]}";
  cJSON *p = cJSON_Parse(json), *rows = cJSON_GetObjectItem(p, "rows");
  pet_p4_data_view_t view;
  pet_p4_widget_data_view("test.quotes", 0, 100, &view);
  assert(view.enabled && !view.received && view.count == 0);
  for (int i = 0; i < 12; i++) {
    cJSON *r = cJSON_CreateObject(); char id[10]; snprintf(id, sizeof(id), "test%d", i);
    cJSON_AddStringToObject(r, "id", id); cJSON_AddStringToObject(r, "label", "测试");
    cJSON_AddStringToObject(r, "value", "1.23"); cJSON_AddStringToObject(r, "detail", "+1%");
    cJSON_AddStringToObject(r, "meta", "test timestamp"); cJSON_AddNumberToObject(r, "tone", 1); cJSON_AddItemToArray(rows, r);
  }
  assert(pet_p4_widget_data_apply(p, 1000));
  cJSON_AddStringToObject(p, "date", "2026-09-21");
  assert(pet_p4_widget_data_apply(p, 1000));
  pet_p4_widget_data_view("test.quotes", 0, 1001, &view);
  assert(view.count == 5 && view.pages == 3 && view.received && !view.stale);
  assert(!strcmp(view.date, "2026-09-21"));
  cJSON_ReplaceItemInObject(p, "date", cJSON_CreateString("2026-09-21 12:00"));
  assert(!pet_p4_widget_data_apply(p, 2000));
  cJSON_ReplaceItemInObject(p, "date", cJSON_CreateString("2026-09-21"));
  pet_p4_widget_data_view("test.quotes", -1, 1001, &view);
  assert(view.page == 2 && view.count == 2 && !strcmp(view.rows[0].id, "test10"));
  pet_p4_widget_data_view("test.quotes", 3, 31001, &view);
  assert(view.page == 0 && view.stale && !strcmp(view.rows[0].value, "1.23"));
  cJSON_ReplaceItemInObject(cJSON_GetArrayItem(rows, 1), "id", cJSON_CreateString("test0"));
  assert(!pet_p4_widget_data_apply(p, 32000));
  pet_p4_widget_data_view("test.quotes", 0, 32001, &view);
  assert(view.stale); // Rejected update cannot extend the freshness lease.
  cJSON_ReplaceItemInObject(cJSON_GetArrayItem(rows, 1), "id", cJSON_CreateString("test1"));
  cJSON_ReplaceItemInObject(p, "ttlMs", cJSON_CreateNumber(3000000));
  assert(!pet_p4_widget_data_apply(p, 33000));
  cJSON_ReplaceItemInObject(p, "ttlMs", cJSON_CreateNumber(30000));
  cJSON_ReplaceItemInObject(p, "rows", cJSON_CreateArray());
  cJSON_ReplaceItemInObject(p, "status", cJSON_CreateString("empty"));
  assert(pet_p4_widget_data_apply(p, 34000));
  pet_p4_widget_data_view("test.quotes", 99, 34001, &view);
  assert(!view.count && view.page == 0 && view.pages == 1 && !view.stale);
  pet_p4_widget_data_view("other.source", 0, 34001, &view);
  assert(!view.received); // Source isolation.
  cJSON_Delete(p); puts("widget data: atomic validation, paging, expiry, source isolation PASS");
}
