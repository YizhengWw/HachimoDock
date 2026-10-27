#include "pet_p4_local_tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char source[20000];
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);size_t n=fread(source,1,sizeof(source)-1,f);assert(feof(f));fclose(f);source[n]=0;
 cJSON *root=cJSON_Parse(source);assert(root);pet_local_config_t c;assert(pet_local_parse(cJSON_GetObjectItem(root,"local"),&c));
 pet_local_state_t s;pet_local_init(&s);int32_t v[7];for(int i=0;i<7;i++){char k[8];snprintf(k,8,"local%d",i);v[i]=cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"vars"),k),"init")->valueint;}
 uint64_t ms=0;int64_t now=(int64_t)pet_local_day(2026,10,23)*86400+17*3600;pet_local_view_t view;
 assert(pet_local_tick(&c,&s,v,ms,now));
 if(!c.countdown){
  for(int group=0;group<c.count;group++){
   v[0]=group;int last=-1;unsigned reached=0,seed=0x12345678;
   for(int i=0;i<512;i++){
    seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
    assert(pet_local_action(&c,&s,v,"local.draw",ms,now,seed));assert(s.phase==1);
    assert(!pet_local_action(&c,&s,v,"local.draw",ms,now,seed));ms+=c.reveal_ms;assert(pet_local_tick(&c,&s,v,ms,now));
    int selected=v[2+group*2];assert(s.phase==2&&selected!=last);last=selected;reached|=1u<<selected;
    pet_local_view(&c,&s,v,ms,now,&view);assert(!strcmp(view.note,c.groups[group].items[selected].note));
   }assert(reached==(1u<<c.groups[group].count)-1);
   if(pet_local_enabled(&c,&s,"local.exclude")){
    for(int i=0;i<c.groups[group].count;i++){assert(pet_local_action(&c,&s,v,"local.exclude",ms,now,seed));if(i<c.groups[group].count-1){ms+=c.reveal_ms;pet_local_tick(&c,&s,v,ms,now);}}
    assert(s.phase==4);assert(pet_local_action(&c,&s,v,"local.reset",ms,now,seed));assert(v[1+group*2]==0);
   }
  }
 }else{
  pet_local_view(&c,&s,v,ms,-1,&view);assert(view.phase==5);assert(pet_local_action(&c,&s,v,"local.edit",ms,-1,0));assert(s.phase==5);
  pet_local_tick(&c,&s,v,ms,now);assert(s.phase==0);pet_local_view(&c,&s,v,ms,now,&view);assert(!strcmp(view.metric,"01:00:00"));
  pet_local_view(&c,&s,v,ms,now+3600,&view);assert(!strcmp(view.metric,"已到站"));
  assert(pet_local_action(&c,&s,v,"local.edit",ms,now,0));assert(pet_local_action(&c,&s,v,"local.up",ms,now,0));assert(pet_local_action(&c,&s,v,"local.cancel",ms,now,0));assert(v[2]==1080);
  assert(pet_local_action(&c,&s,v,"local.edit",ms,now,0));assert(pet_local_action(&c,&s,v,"local.up",ms,now,0));assert(pet_local_action(&c,&s,v,"local.edit",ms,now,0));assert(v[2]==1140);
  v[0]=2;v[5]=pet_local_day(2024,2,29);assert(pet_local_action(&c,&s,v,"local.edit",ms,now,0));assert(pet_local_action(&c,&s,v,"local.up",ms,now,0));assert(s.draft_day==pet_local_day(2025,2,28));
  for(int y=2020;y<=2099;y++)for(int m=1;m<=12;m++){int yy,mm,dd;pet_local_date(pet_local_day(y,m,1),&yy,&mm,&dd);assert(yy==y&&mm==m&&dd==1);}
 }
 cJSON *local=cJSON_GetObjectItem(root,"local");cJSON_AddStringToObject(local,"script","not allowed");assert(!pet_local_parse(local,&c));cJSON_Delete(root);
 puts("local-tools native tests passed");return 0;
}
