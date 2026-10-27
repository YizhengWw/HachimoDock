#include "pet_p4_local_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);char buf[16384];size_t n=fread(buf,1,sizeof(buf)-1,f);buf[n]=0;fclose(f);
 cJSON *r=cJSON_Parse(buf);pet_local_config_t c;assert(pet_local_parse(cJSON_GetObjectItem(r,"local"),&c));pet_local_state_t s;pet_local_init(&s);int32_t v[7];
 for(int i=0;i<7;i++){char name[8];snprintf(name,8,"local%d",i);v[i]=cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"vars"),name),"init")->valueint;}
 unsigned seed=0x12983746;uint64_t ms=0;int64_t base=(int64_t)pet_local_day(2026,10,23)*86400+17*3600;
 for(int i=0;i<1200;i++){
  seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;ms+=seed%1200;int op=seed%c.action_count;
  int64_t now=i<8?-1:base+(int64_t)(ms/1000);
  pet_local_tick(&c,&s,v,ms,now);pet_local_action(&c,&s,v,c.actions[op],ms,now,seed);
  pet_local_view_t view;pet_local_view(&c,&s,v,ms,now,&view);
  cJSON *row=cJSON_CreateObject();cJSON_AddNumberToObject(row,"phase",view.phase);cJSON_AddNumberToObject(row,"field",s.field);cJSON_AddNumberToObject(row,"draftDay",s.draft_day);cJSON_AddNumberToObject(row,"draftMinute",s.draft_minute);cJSON_AddNumberToObject(row,"pending",s.pending);cJSON_AddStringToObject(row,"headline",view.headline);cJSON_AddStringToObject(row,"note",view.note);cJSON_AddStringToObject(row,"metricValue",view.metric);cJSON_AddNumberToObject(row,"progress",view.progress);
  cJSON *vars=cJSON_AddArrayToObject(row,"vars");for(int j=0;j<7;j++)cJSON_AddItemToArray(vars,cJSON_CreateNumber(v[j]));char *out=cJSON_PrintUnformatted(row);puts(out);free(out);cJSON_Delete(row);
 }cJSON_Delete(r);return 0;
}
