#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "pet_p4_widget_data.h"
#if PET_P4_DATA_ROWS
#include "pet_p4_data_rows.h"
#endif
static void test_clear_on_expire(void) {
  cJSON *p=cJSON_Parse("{\"schema\":1,\"source\":\"test.expiry\",\"ttlMs\":5000,\"clearOnExpire\":true,\"status\":\"ok\",\"message\":\"old private value\",\"date\":\"2026-11-03\",\"rows\":[]}");
  assert(p);
  cJSON *rows=cJSON_GetObjectItemCaseSensitive(p,"rows");
  for(int i=0;i<6;i++){
    cJSON *r=cJSON_Parse("{\"id\":\"row\",\"label\":\"old city\",\"value\":\"old value\",\"detail\":\"old detail\",\"meta\":\"old meta\",\"tone\":0}");
    char id[12];snprintf(id,sizeof(id),"row%d",i);
    cJSON_ReplaceItemInObject(r,"id",cJSON_CreateString(id));cJSON_AddItemToArray(rows,r);
  }
  assert(pet_p4_widget_data_apply(p,1000));
  pet_p4_data_view_t view;
  pet_p4_widget_data_view("test.expiry",-1,6000,&view);
  assert(view.enabled&&view.received&&!view.stale&&view.count==1&&view.pages==2&&view.page==1);
  assert(!strcmp(view.status,"ok")&&!strcmp(view.message,"old private value")&&!strcmp(view.date,"2026-11-03"));
  assert(!strcmp(view.rows[0].id,"row5"));
#if PET_P4_DATA_ROWS
  pet_p4_data_row_t selected;
  /* Selection must expire without a preceding stale view or periodic task. */
  assert(!pet_p4_widget_data_select("test.expiry",0,6001,&selected));
  assert(!pet_p4_widget_data_select("test.expiry",0,999,&selected));
  assert(pet_p4_widget_data_select("test.expiry",INT_MAX,6000,&selected)&&!strcmp(selected.id,"row5"));
  assert(pet_p4_widget_data_select("test.expiry",INT_MIN,1000,&selected)&&!strcmp(selected.id,"row0"));
  pet_data_rows_config_t filter={.enabled=true,.filter_count=1};
  filter.filters[0].field=PET_DATA_FIELD_LABEL;filter.filters[0].count=1;
  strcpy(filter.filters[0].values[0],"old city");
  int32_t values[PET_DATA_FILTERS_MAX]={1};
  pet_p4_widget_data_set_filter("test.expiry",&filter,values);
  assert(pet_p4_widget_data_select("test.expiry",5,6000,&selected)&&!strcmp(selected.id,"row5"));
  assert(!pet_p4_widget_data_select("test.expiry",5,6001,&selected));
  values[0]=2;pet_p4_widget_data_set_filter("test.expiry",&filter,values);
  assert(!pet_p4_widget_data_select("test.expiry",0,1000,&selected));
  pet_p4_widget_data_view("test.expiry",0,1000,&view);
  assert(!view.stale&&!view.count&&view.pages==1);
  pet_p4_widget_data_set_filter(NULL,NULL,NULL);
#endif
  /* Invalid optional values reject atomically and cannot renew the TTL. */
  const char *invalid[]={"null","0","1","\"true\"","[]","{}"};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
    cJSON_ReplaceItemInObject(p,"clearOnExpire",cJSON_Parse(invalid[i]));
    assert(!pet_p4_widget_data_apply(p,6001));
  }
  const uint64_t expired[]={6001,999,UINT64_MAX};
  for(unsigned i=0;i<sizeof(expired)/sizeof(*expired);i++){
    memset(&view,0x7f,sizeof(view));
    pet_p4_widget_data_view("test.expiry",INT_MIN,expired[i],&view);
    assert(view.enabled&&view.received&&view.stale&&!view.count&&view.page==0&&view.pages==1);
    assert(!strcmp(view.status,"empty")&&view.message[0]&&!strstr(view.message,"old"));
    assert(!view.date[0]);
    pet_p4_data_row_t zero={0};
    for(unsigned k=0;k<PET_P4_DATA_PAGE_ROWS;k++)assert(!memcmp(&view.rows[k],&zero,sizeof(zero)));
  }
  /* Explicit false and an omitted flag both restore legacy stale projection. */
  for(unsigned mode=0;mode<2;mode++){
    if(!mode)cJSON_ReplaceItemInObject(p,"clearOnExpire",cJSON_CreateBool(false));
    else cJSON_DeleteItemFromObjectCaseSensitive(p,"clearOnExpire");
    assert(pet_p4_widget_data_apply(p,10000));
    pet_p4_widget_data_view("test.expiry",-1,15001,&view);
    assert(view.stale&&view.count==1&&view.pages==2&&view.page==1);
    assert(!strcmp(view.rows[0].id,"row5")&&!strcmp(view.message,"old private value")&&!strcmp(view.date,"2026-11-03"));
    pet_p4_widget_data_view("test.expiry",0,9999,&view);
    assert(view.stale&&view.count==5);
#if PET_P4_DATA_ROWS
    assert(pet_p4_widget_data_select("test.expiry",0,15001,&selected)&&!strcmp(selected.id,"row0"));
    assert(pet_p4_widget_data_select("test.expiry",0,9999,&selected));
#endif
  }
  /* A new empty snapshot remains a normal received, fresh empty state. */
  cJSON_AddBoolToObject(p,"clearOnExpire",true);
  cJSON_ReplaceItemInObject(p,"rows",cJSON_CreateArray());
  cJSON_ReplaceItemInObject(p,"status",cJSON_CreateString("empty"));
  cJSON_ReplaceItemInObject(p,"message",cJSON_CreateString("No data available"));
  assert(pet_p4_widget_data_apply(p,20000));
  pet_p4_widget_data_view("test.expiry",INT_MAX,20001,&view);
  assert(view.enabled&&view.received&&!view.stale&&!view.count&&view.pages==1&&view.page==0);
  assert(!strcmp(view.status,"empty")&&!strcmp(view.message,"No data available"));
#if PET_P4_DATA_ROWS
  assert(!pet_p4_widget_data_select("test.expiry",0,20001,&selected));
#endif
  cJSON_Delete(p);
}
static void test_source_capacity(void) {
  _Static_assert(PET_P4_DATA_MAX_SOURCES==8,"bounded eight-source store");
  _Static_assert(PET_P4_DATA_MAX_ROWS==20,"row bound is unchanged");
  const char *sources[]={"test.quotes","test.expiry","test.capacity2","test.capacity3",
                         "test.capacity4","test.capacity5","test.capacity6","test.capacity7"};
  cJSON *p=cJSON_Parse("{\"schema\":1,\"source\":\"test.quotes\",\"ttlMs\":5000,\"status\":\"ok\",\"message\":\"initial\",\"rows\":[{\"id\":\"row\",\"label\":\"Row\",\"value\":\"1\",\"detail\":\"\",\"meta\":\"\",\"tone\":0}]}");
  assert(p);
  for(unsigned i=0;i<PET_P4_DATA_MAX_SOURCES;i++){
    cJSON_ReplaceItemInObject(p,"source",cJSON_CreateString(sources[i]));
    assert(pet_p4_widget_data_apply(p,100000));
  }
  cJSON_ReplaceItemInObject(p,"source",cJSON_CreateString("test.ninth"));
  assert(!pet_p4_widget_data_apply(p,100001));
  pet_p4_data_view_t view;
  pet_p4_widget_data_view("test.ninth",0,100001,&view);assert(!view.received);
  for(unsigned i=0;i<PET_P4_DATA_MAX_SOURCES;i++){
    pet_p4_widget_data_view(sources[i],0,100001,&view);
    assert(view.received&&!view.stale&&view.count==1&&!strcmp(view.message,"initial"));
    cJSON_ReplaceItemInObject(p,"source",cJSON_CreateString(sources[i]));
    cJSON_ReplaceItemInObject(p,"message",cJSON_CreateString("updated"));
    assert(pet_p4_widget_data_apply(p,101000));
    pet_p4_widget_data_view(sources[i],0,101001,&view);
    assert(view.received&&!view.stale&&view.count==1&&!strcmp(view.message,"updated"));
#if PET_P4_DATA_ROWS
    pet_p4_data_row_t selected;
    assert(pet_p4_widget_data_select(sources[i],0,101001,&selected)&&!strcmp(selected.id,"row"));
#endif
  }
  cJSON_ReplaceItemInObject(p,"source",cJSON_CreateString("test.ninth"));
  assert(!pet_p4_widget_data_apply(p,102000));
  cJSON_Delete(p);
}
int main(void) {
#if PET_P4_DATA_ROWS
  /* Every signed-byte tone must fit with its terminator and preserve neighbours. */
  for(int tone=INT8_MIN;tone<=INT8_MAX;tone++){
    struct {unsigned char before;char text[5];unsigned char after;} output={.before=0xa5,.after=0x5a};
    pet_p4_data_row_t row={.tone=(int8_t)tone};char expected[16];
    snprintf(expected,sizeof(expected),"%d",tone);
    assert(!strcmp(pet_data_row_field(&row,PET_DATA_FIELD_TONE,output.text),expected));
    assert(output.before==0xa5&&output.after==0x5a);
    pet_data_filter_t filter={.field=PET_DATA_FIELD_TONE,.count=1};
    strcpy(filter.values[0],expected);assert(pet_data_row_matches(&row,&filter,1));
  }
#endif
  _Static_assert(sizeof(pet_p4_data_paging_t)==2,"eligibility never copies a full row view");
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
  assert(pet_p4_widget_data_paging(NULL,INT_MIN).pages==1);
  assert(pet_p4_widget_data_paging("other.source",INT_MAX).page==0);
  const int counts[]={0,1,5,6,11,16,20},pages[]={INT_MIN,-11,-1,0,1,8,INT_MAX};
  for(unsigned n=0;n<sizeof(counts)/sizeof(counts[0]);n++){
    rows=cJSON_GetObjectItem(p,"rows");
    while(cJSON_GetArraySize(rows))cJSON_DeleteItemFromArray(rows,0);
    for(int i=0;i<counts[n];i++){
      cJSON *r=cJSON_CreateObject();char id[16];snprintf(id,sizeof(id),"row%d",i);
      cJSON_AddStringToObject(r,"id",id);cJSON_AddStringToObject(r,"label","Row");
      cJSON_AddStringToObject(r,"value","1");cJSON_AddStringToObject(r,"detail","");
      cJSON_AddStringToObject(r,"meta","");cJSON_AddNumberToObject(r,"tone",0);cJSON_AddItemToArray(rows,r);
    }
    assert(pet_p4_widget_data_apply(p,35000));
    for(unsigned j=0;j<sizeof(pages)/sizeof(pages[0]);j++){
      pet_p4_data_paging_t paging=pet_p4_widget_data_paging("test.quotes",pages[j]);
      pet_p4_widget_data_view("test.quotes",pages[j],UINT64_MAX,&view);
      assert(view.stale&&paging.pages==view.pages&&paging.page==view.page);
      pet_p4_widget_data_view("test.quotes",pages[j],35001,&view);
      assert(!view.stale&&paging.pages==view.pages&&paging.page==view.page);
    }
  }
  cJSON_Delete(p);test_clear_on_expire();test_source_capacity();
  puts("widget data: atomic validation, paging, expiry, source isolation PASS");
}
