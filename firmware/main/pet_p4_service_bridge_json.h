#pragma once
#include "pet_p4_service_bridge.h"
#include "cJSON.h"
#include <math.h>
static inline bool pet_bridge_json_keys(const cJSON*v,const char *const *keys,unsigned count){
  if(!cJSON_IsObject(v))return false;
  const cJSON*x;
  cJSON_ArrayForEach(x,v){bool known=false;
  for(unsigned i=0;i<count;i++)if(x->string&&!strcmp(x->string,keys[i]))known=true;
  if(!known)return false;
  }
  return true;
}
static inline bool pet_bridge_json_name(const char*s,unsigned max){
  if(!s||!s[0]||strlen(s)>max)return false;
  for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='.'||*s=='_'||*s=='-'))return false;
  return true;
}
static inline bool pet_bridge_json_uint(const cJSON*v,const char*k,uint32_t min,uint32_t max,uint32_t*out){
  const cJSON*n=cJSON_GetObjectItemCaseSensitive(v,k);if(!cJSON_IsNumber(n)||!isfinite(n->valuedouble)||n->valuedouble<min||n->valuedouble>max||floor(n->valuedouble)!=n->valuedouble)return false;*out=(uint32_t)n->valuedouble;return true;
}
static inline bool pet_bridge_json_ref(const char*alias,const cJSON*v,pet_bridge_image_ref_t*out){
  const char*const keys[]={"key","revision","width","height"};uint32_t w,h;
  if(!out||!pet_bridge_json_name(alias,15)||!pet_bridge_json_keys(v,keys,4)||cJSON_GetArraySize(v)!=4)return false;
  const cJSON*k=cJSON_GetObjectItemCaseSensitive(v,"key"),*r=cJSON_GetObjectItemCaseSensitive(v,"revision");
  if(!cJSON_IsString(k)||!cJSON_IsString(r)||!pet_bridge_json_name(k->valuestring,47)||!pet_bridge_json_name(r->valuestring,64)
     ||!pet_bridge_json_uint(v,"width",8,192,&w)||!pet_bridge_json_uint(v,"height",8,192,&h))return false;
  memset(out,0,sizeof(*out));strcpy(out->alias,alias);strcpy(out->key,k->valuestring);strcpy(out->revision,r->valuestring);out->width=w;out->height=h;return true;
}
typedef struct {pet_bridge_timelines_t timelines;pet_bridge_image_ref_t images[PET_BRIDGE_IMAGES];uint8_t image_count;} pet_bridge_presentation_t;
static inline bool pet_bridge_json_presentation(const cJSON*v,pet_bridge_presentation_t*out){
  if(!out)return false;
  memset(out,0,sizeof(*out));
  if(!v)return true;
  const char*const keys[]={"timelines","images"};if(!pet_bridge_json_keys(v,keys,2)||cJSON_GetArraySize(v)!=2)return false;
  const cJSON*t=cJSON_GetObjectItemCaseSensitive(v,"timelines"),*images=cJSON_GetObjectItemCaseSensitive(v,"images"),*item;
  if(!cJSON_IsObject(t)||cJSON_GetArraySize(t)>PET_BRIDGE_TIMELINES||!cJSON_IsObject(images)||cJSON_GetArraySize(images)>PET_BRIDGE_IMAGES)return false;
  cJSON_ArrayForEach(item,t){
    const char*const fields[]={"positionMs","durationMs","running","horizonMs","projection"};
    if(!pet_bridge_json_name(item->string,31)||!pet_bridge_json_keys(item,fields,5)||cJSON_GetArraySize(item)!=5)return false;
    pet_bridge_timeline_t*x=&out->timelines.items[out->timelines.count++];strcpy(x->name,item->string);
    if(!pet_bridge_json_uint(item,"positionMs",0,86400000,&x->position_ms)||!pet_bridge_json_uint(item,"durationMs",1,86400000,&x->duration_ms)
       ||!pet_bridge_json_uint(item,"horizonMs",100,10000,&x->horizon_ms))return false;
    const cJSON*running=cJSON_GetObjectItemCaseSensitive(item,"running"),*projection=cJSON_GetObjectItemCaseSensitive(item,"projection");
    if(!cJSON_IsBool(running)||!cJSON_IsString(projection))return false;
    x->running=cJSON_IsTrue(running);
    if(!strcmp(projection->valuestring,"percent"))x->projection=0;else if(!strcmp(projection->valuestring,"position"))x->projection=1;else if(!strcmp(projection->valuestring,"remaining"))x->projection=2;else return false;
    if(!pet_bridge_timeline_valid(x))return false;
    for(unsigned i=0;i+1<out->timelines.count;i++)if(!strcmp(out->timelines.items[i].name,x->name))return false;
  }
  cJSON_ArrayForEach(item,images){
    for(unsigned i=0;i<out->image_count;i++)if(!strcmp(out->images[i].alias,item->string))return false;
    if(!pet_bridge_json_ref(item->string,item,&out->images[out->image_count++]))return false;
  }
  return true;
}
