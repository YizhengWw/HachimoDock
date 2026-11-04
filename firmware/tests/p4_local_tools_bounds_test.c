#include "pet_p4_local_tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static cJSON *configuration(unsigned count){
 cJSON *c=cJSON_CreateObject();cJSON_AddStringToObject(c,"engine",PET_LOCAL_ENGINE);cJSON_AddStringToObject(c,"kind","deck");
 cJSON_AddNumberToObject(c,"reveal_ms",300);cJSON_AddStringToObject(c,"idle","idle");cJSON_AddStringToObject(c,"busy","busy");cJSON_AddStringToObject(c,"hint","hint");
 cJSON *groups=cJSON_AddArrayToObject(c,"groups");
 for(unsigned g=0;g<3;g++){cJSON*q=cJSON_CreateObject();cJSON_AddItemToArray(groups,q);cJSON_AddStringToObject(q,"title","group");cJSON*a=cJSON_AddArrayToObject(q,"items");
  for(unsigned i=0;i<count;i++){char label[40];snprintf(label,sizeof(label),"item-%u-%u",g,i);cJSON*v=cJSON_CreateObject();cJSON_AddStringToObject(v,"title",label);cJSON_AddStringToObject(v,"note",label);cJSON_AddItemToArray(a,v);}}
 cJSON *a=cJSON_AddObjectToObject(c,"actions");const char*ops[]={"draw","exclude","reset","previous","next"};
 for(unsigned i=0;i<5;i++){char key[40];snprintf(key,sizeof(key),"local.%s",ops[i]);cJSON_AddStringToObject(a,key,ops[i]);}return c;
}
static void saved_round_trip(int32_t *v,unsigned words){
 cJSON *o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"version",words==13?2:1);cJSON*q=cJSON_AddObjectToObject(o,"vars");
 for(unsigned i=0;i<words;i++){char key[16];snprintf(key,sizeof(key),"local%u",i);assert(v[i]>=-1&&v[i]<=16777215);cJSON_AddNumberToObject(q,key,v[i]);}
 char*text=cJSON_PrintUnformatted(o);assert(text);cJSON*restored=cJSON_Parse(text);assert(restored);assert(cJSON_GetObjectItem(restored,"version")->valueint==(words==13?2:1));
 int32_t original[13];memcpy(original,v,sizeof(original));memset(v,0,13*sizeof(*v));
 for(unsigned i=0;i<words;i++){char key[16];snprintf(key,sizeof(key),"local%u",i);cJSON*n=cJSON_GetObjectItem(cJSON_GetObjectItem(restored,"vars"),key);assert(cJSON_IsNumber(n)&&n->valuedouble==original[i]);v[i]=n->valueint;}
 assert(!memcmp(original,v,words*sizeof(*v)));cJSON_Delete(restored);cJSON_free(text);cJSON_Delete(o);
}
int main(int argc,char**argv){assert(argc==2);unsigned count=(unsigned)atoi(argv[1]);assert(count>=2&&count<=64);
 cJSON*source=configuration(count);pet_local_config_t*c=calloc(1,sizeof(*c));assert(c&&pet_local_parse(source,c));
 #if FIX_CHECK
 assert(c->wide_masks==(count>24));
 #endif
 int32_t v[13]={0,0,-1,0,-1,0,-1};pet_local_state_t s;pet_local_init(&s);uint64_t ms=0;pet_local_view_t view;
 for(unsigned g=0;g<3;g++){
  v[0]=(int32_t)g;pet_local_init(&s);assert(pet_local_action(c,&s,v,"local.draw",ms,0,0));
  for(unsigned i=0;i<count;i++){
   assert(s.phase==1);ms+=300;assert(pet_local_tick(c,&s,v,ms,0));assert(s.phase==2);assert(v[2+2*g]>=0&&(unsigned)v[2+2*g]<count);
   pet_local_view(c,&s,v,ms,0,&view);char label[40];snprintf(label,sizeof(label),"item-%u-%d",g,v[2+2*g]);assert(!strcmp(view.note,label));
   assert(pet_local_action(c,&s,v,"local.exclude",ms,0,0));
  }
  assert(s.phase==4);pet_local_view(c,&s,v,ms,0,&view);assert(!strcmp(view.metric,"0"));
  saved_round_trip(v,count>24?13:7);pet_local_init(&s);assert(pet_local_action(c,&s,v,"local.draw",ms,0,0));assert(s.phase==4);
  assert(pet_local_action(c,&s,v,"local.reset",ms,0,0));pet_local_view(c,&s,v,ms,0,&view);assert((unsigned)atoi(view.metric)==count);
 }
 /* Reject an actually empty pool: no modulo by zero or invented choice. */
 cJSON*empty=cJSON_Duplicate(source,1);cJSON*g=cJSON_GetArrayItem(cJSON_GetObjectItem(empty,"groups"),0);cJSON_ReplaceItemInObject(g,"items",cJSON_CreateArray());assert(!pet_local_parse(empty,c));cJSON_Delete(empty);
 printf("BOUNDARY_OK count=%u all3groups exclude/exhaust/restore/reset empty_rejected\n",count);cJSON_Delete(source);free(c);return 0;
}
