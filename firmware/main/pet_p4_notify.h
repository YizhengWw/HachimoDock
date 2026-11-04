/* p4-notify-v1: bounded component notifications on the shared widget/notify
 * topic. One payload shape in both directions (PC -> device bubble, device ->
 * PC toast): exact keys componentId, title, optional text, optional badge.
 * At most one notification per component every 30 s; the limiter is tiny,
 * fixed-memory and shared by both ends. No IO, no heap. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define PET_NOTIFY_TOPIC "widget/notify"
#define PET_NOTIFY_COOLDOWN_MS 30000ULL
#define PET_NOTIFY_SLOTS 8
#define PET_NOTIFY_TITLE_BYTES 63
#define PET_NOTIFY_TEXT_BYTES 120
typedef struct {char component[48];char title[PET_NOTIFY_TITLE_BYTES+1];char text[PET_NOTIFY_TEXT_BYTES+1];int32_t badge;bool has_badge;} pet_notify_t;
typedef struct {char component[48];uint64_t last_ms;bool used;} pet_notify_slot_t;
typedef struct {pet_notify_slot_t slots[PET_NOTIFY_SLOTS];} pet_notify_limiter_t;
static inline bool pet_notify_text_ok(const char *s,unsigned max,bool nonempty){
  if(!s)return false;unsigned i=0;
  for(;s[i];i++){unsigned char c=(unsigned char)s[i];if(i>=max||c<32||c==127)return false;}
  return !nonempty||i>0;
}
static inline bool pet_notify_component_ok(const char *s){
  if(!s||!s[0]||strlen(s)>47)return false;
  for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return false;
  return true;
}
/* Strict parse of one widget/notify payload. Unknown keys refuse the whole
 * notification; nothing is repaired or truncated. */
static inline bool pet_notify_parse(const cJSON *payload,pet_notify_t *out){
  if(!cJSON_IsObject(payload)||!out)return false;
  memset(out,0,sizeof(*out));
  const cJSON *v;int seen=0;
  cJSON_ArrayForEach(v,payload){
    if(!v->string)return false;
    if(!strcmp(v->string,"componentId")){if(!cJSON_IsString(v)||!pet_notify_component_ok(v->valuestring))return false;strcpy(out->component,v->valuestring);seen|=1;}
    else if(!strcmp(v->string,"title")){if(!cJSON_IsString(v)||!pet_notify_text_ok(v->valuestring,PET_NOTIFY_TITLE_BYTES,true))return false;strcpy(out->title,v->valuestring);seen|=2;}
    else if(!strcmp(v->string,"text")){if(!cJSON_IsString(v)||!pet_notify_text_ok(v->valuestring,PET_NOTIFY_TEXT_BYTES,false))return false;strcpy(out->text,v->valuestring);}
    else if(!strcmp(v->string,"badge")){if(!cJSON_IsNumber(v)||v->valuedouble<-1000000000||v->valuedouble>1000000000||v->valuedouble!=(double)(int32_t)v->valuedouble)return false;out->badge=(int32_t)v->valuedouble;out->has_badge=true;}
    else return false;
  }
  return seen==3;
}
/* Per-component cooldown. Admits the first notification of a component and
 * then at most one every PET_NOTIFY_COOLDOWN_MS; only expired slots may be reused; a full live table refuses conservatively. */
static inline bool pet_notify_admit(pet_notify_limiter_t *l,const char *component,uint64_t now_ms){
  if(!l||!pet_notify_component_ok(component))return false;
  int free_slot=-1;
  for(int i=0;i<PET_NOTIFY_SLOTS;i++){
    pet_notify_slot_t *s=&l->slots[i];
    if(s->used&&!strcmp(s->component,component)){
      if(now_ms<s->last_ms||now_ms-s->last_ms<PET_NOTIFY_COOLDOWN_MS)return false;
      s->last_ms=now_ms;return true;
    }
    if(free_slot<0&&(!s->used||(now_ms>=s->last_ms&&now_ms-s->last_ms>=PET_NOTIFY_COOLDOWN_MS)))free_slot=i;
  }
  if(free_slot<0)return false;
  pet_notify_slot_t *s=&l->slots[free_slot];
  memset(s,0,sizeof(*s));strcpy(s->component,component);s->last_ms=now_ms;s->used=true;return true;
}
/* Bubble text: "title · badge" when a badge is present, otherwise the title. */
static inline void pet_notify_bubble_title(const pet_notify_t *n,char *out,unsigned capacity){
  if(!out||!capacity)return;
  if(n->has_badge)snprintf(out,capacity,"%s · %ld",n->title,(long)n->badge);
  else snprintf(out,capacity,"%s",n->title);
}
