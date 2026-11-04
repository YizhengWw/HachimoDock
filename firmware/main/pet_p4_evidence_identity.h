/* Read-only evidence, not authentication, a grant, or an ID key. Initialize
 * once before worker tasks, then publish immutable per-boot bytes. Seeds are
 * supplied only by trusted startup code, never decoded from wire input. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef struct{char id[17];bool ready;} pet_boot_identity_t;
static inline bool pet_boot_identity_start(pet_boot_identity_t *boot,uint32_t first,uint32_t second){
  if(!boot||boot->ready)return false;
  /* Nonsecret probabilistic boot distinction, not a persistent tracking ID. */
  snprintf(boot->id,sizeof(boot->id),"%08x%08x",(unsigned)first,(unsigned)second);boot->ready=true;return true;
}
static inline bool pet_evidence_token(const char *text,size_t maximum,bool build){
  if(!text||!*text||strlen(text)>maximum)return false;
  for(const unsigned char*p=(const unsigned char*)text;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'||(build&&*p=='+')))return false;
  return true;
}
static inline bool pet_evidence_hex(const char *text,size_t length){
  if(!text||strlen(text)!=length)return false;
  for(const char*p=text;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')))return false;return true;
}
static inline const char *pet_evidence_request(const cJSON *payload){
  if(!cJSON_IsObject(payload)||cJSON_GetArraySize(payload)!=1)return NULL;
  const cJSON*v=cJSON_GetObjectItemCaseSensitive(payload,"requestId");
  return cJSON_IsString(v)&&pet_evidence_token(v->valuestring,64,false)?v->valuestring:NULL;
}
static inline bool pet_evidence_source_valid(const char *json){
  if(!json)return false;
  for(const char*p=json;*p;p++)if(*p=='\\'){p++;if(!*p)break;if(*p=='u'&&!strncmp(p+1,"0000",4))return false;}
  return true;
}
static inline cJSON *pet_evidence_identity(const pet_boot_identity_t *boot,const char *request,
    const char *board,const char *build,const char *git,const char *elf,bool dirty,uint32_t schema,uint64_t uptime){
  if(!boot||!boot->ready||!pet_evidence_hex(boot->id,16)||!pet_evidence_token(request,64,false)
      ||!pet_evidence_token(board,47,false)||!pet_evidence_token(build,95,true)||!pet_evidence_token(git,40,false)
      ||!pet_evidence_hex(elf,64)||!schema||uptime>UINT64_C(9007199254740991))return NULL;
  cJSON*out=cJSON_CreateObject();if(!out)return NULL;
  if(!cJSON_AddStringToObject(out,"engine","p4-evidence-identity-v1")||!cJSON_AddStringToObject(out,"requestId",request)
      ||!cJSON_AddStringToObject(out,"boardDeviceId",board)||!cJSON_AddStringToObject(out,"bootId",boot->id)
      ||!cJSON_AddStringToObject(out,"buildId",build)||!cJSON_AddStringToObject(out,"gitSha",git)
      ||!cJSON_AddStringToObject(out,"appElfSha256",elf)||!cJSON_AddBoolToObject(out,"buildDirty",dirty)
      ||!cJSON_AddNumberToObject(out,"protocolSchema",schema)||!cJSON_AddNumberToObject(out,"uptimeUs",(double)uptime)){
    cJSON_Delete(out);return NULL;
  }
  return out;
}
