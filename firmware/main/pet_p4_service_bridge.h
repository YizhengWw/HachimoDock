#pragma once
/* Pure bounded presentation core. Callers own locking, native identity and
 * memory. It never performs IO, allocates, plays audio or authorizes a command. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#ifndef PET_SERVICE_BRIDGE_ENABLED
#define PET_SERVICE_BRIDGE_ENABLED 0
#endif
#define PET_BRIDGE_IMAGES 2
#define PET_BRIDGE_IMAGE_SIDE 192
#define PET_BRIDGE_IMAGE_PIXELS (192U*192U)
#define PET_BRIDGE_TIMELINES 2
typedef struct {char name[32];uint32_t position_ms,duration_ms,horizon_ms;bool running;uint8_t projection;} pet_bridge_timeline_t;
typedef struct {uint8_t count;pet_bridge_timeline_t items[PET_BRIDGE_TIMELINES];uint64_t received_ms;} pet_bridge_timelines_t;
static inline bool pet_bridge_timeline_valid(const pet_bridge_timeline_t *t){
  return t&&t->name[0]&&t->duration_ms&&t->duration_ms<=86400000U&&t->position_ms<=t->duration_ms&&t->horizon_ms>=100&&t->horizon_ms<=10000&&t->projection<=2;
}
static inline bool pet_bridge_timeline_project(const pet_bridge_timelines_t *s,const char *name,uint64_t now,int32_t *out){
  if(!s||!name||!out||now<s->received_ms||now-s->received_ms>=10000)return false;
  for(unsigned i=0;i<s->count&&i<PET_BRIDGE_TIMELINES;i++){
    const pet_bridge_timeline_t *t=&s->items[i];if(strcmp(t->name,name)||!pet_bridge_timeline_valid(t))continue;
    uint64_t age=now-s->received_ms;if(age>t->horizon_ms)age=t->horizon_ms;
    uint64_t p=t->position_ms+(t->running?age:0);if(p>t->duration_ms)p=t->duration_ms;
    *out=t->projection==0?(int32_t)(p*100/t->duration_ms):t->projection==1?(int32_t)p:(int32_t)(t->duration_ms-p);return true;
  }
  return false;
}
typedef struct {char alias[16],key[48],revision[65];uint16_t width,height;} pet_bridge_image_ref_t;
typedef struct {pet_bridge_image_ref_t ref;uint32_t received;uint16_t readers;uint32_t tokens[8];bool ready,writing;} pet_bridge_image_bank_t;
typedef struct {
  pet_bridge_image_ref_t expected[PET_BRIDGE_IMAGES];
  pet_bridge_image_bank_t banks[PET_BRIDGE_IMAGES][2];
  uint8_t count,front[PET_BRIDGE_IMAGES];uint64_t epoch;uint32_t next_token;
} pet_bridge_images_t;
typedef struct {uint8_t slot,bank;uint64_t epoch;uint32_t token;uint16_t width,height;const uint16_t *pixels;bool valid;} pet_bridge_image_lease_t;
static inline bool pet_bridge_image_same(const pet_bridge_image_ref_t*a,const pet_bridge_image_ref_t*b){
  return a&&b&&a->width==b->width&&a->height==b->height&&!strcmp(a->alias,b->alias)&&!strcmp(a->key,b->key)&&!strcmp(a->revision,b->revision);
}
static inline bool pet_bridge_image_valid(const pet_bridge_image_ref_t *r){
  return r&&r->alias[0]&&r->key[0]&&r->revision[0]&&r->width>=8&&r->width<=PET_BRIDGE_IMAGE_SIDE&&r->height>=8&&r->height<=PET_BRIDGE_IMAGE_SIDE;
}
static inline void pet_bridge_images_clear(pet_bridge_images_t*s){
  if(!s)return;
  s->count=0;
  memset(s->expected,0,sizeof(s->expected));
  s->epoch++;
  for(unsigned i=0;i<PET_BRIDGE_IMAGES;i++)for(unsigned b=0;b<2;b++)s->banks[i][b].writing=false;
}
static inline bool pet_bridge_images_expect(pet_bridge_images_t*s,const pet_bridge_image_ref_t *refs,unsigned count){
  if(!s||count>PET_BRIDGE_IMAGES||(!refs&&count))return false;
  for(unsigned i=0;i<count;i++){if(!pet_bridge_image_valid(&refs[i]))return false;for(unsigned j=0;j<i;j++)if(!strcmp(refs[i].alias,refs[j].alias))return false;}
  bool changed=count!=s->count;
  for(unsigned i=0;i<count;i++)changed|=!pet_bridge_image_same(&s->expected[i],&refs[i]);
  if(changed){s->epoch++;for(unsigned i=0;i<PET_BRIDGE_IMAGES;i++)for(unsigned b=0;b<2;b++)s->banks[i][b].writing=false;}
  memset(s->expected,0,sizeof(s->expected));if(count)memcpy(s->expected,refs,count*sizeof(*refs));s->count=count;return true;
}
/* Bytes are RGB565 little endian. A new transfer writes only the unborrowed
 * back bank; the old visible bank remains immutable until a full commit. */
static inline bool pet_bridge_image_write(pet_bridge_images_t*s,const pet_bridge_image_ref_t*r,uint32_t offset,const uint8_t *bytes,unsigned length,uint16_t *storage){
  if(!s||!r||!bytes||!storage||!length||length>2048||offset%2||length%2)return false;
  unsigned i=0;while(i<s->count&&!pet_bridge_image_same(r,&s->expected[i]))i++;if(i==s->count)return false;
  unsigned back=s->front[i]^1U;pet_bridge_image_bank_t *b=&s->banks[i][back];
  if(!offset){if(b->readers)return false;memset(b,0,sizeof(*b));b->ref=*r;b->writing=true;}
  uint32_t total=(uint32_t)r->width*r->height*2U;
  if(!b->writing||b->readers||!pet_bridge_image_same(&b->ref,r)||offset!=b->received||offset>total||length>total-offset)return false;
  uint16_t *dest=storage+((i*2U+back)*PET_BRIDGE_IMAGE_PIXELS)+offset/2;
  for(unsigned k=0;k<length;k+=2)dest[k/2]=(uint16_t)(bytes[k]|((uint16_t)bytes[k+1]<<8));
  b->received+=length;
  if(b->received==total){b->writing=false;b->ready=true;s->front[i]=(uint8_t)back;}
  return true;
}
static inline bool pet_bridge_image_borrow(pet_bridge_images_t*s,const char *alias,const uint16_t *storage,pet_bridge_image_lease_t*out){
  if(!out)return false;
  memset(out,0,sizeof(*out));
  if(!s||!alias||!storage)return false;
  for(unsigned i=0;i<s->count;i++)if(!strcmp(alias,s->expected[i].alias)){
    unsigned b=s->front[i];pet_bridge_image_bank_t *bank=&s->banks[i][b];
    if(!bank->ready||bank->writing||bank->readers>=8||s->next_token==UINT32_MAX||!pet_bridge_image_same(&bank->ref,&s->expected[i]))return false;
    unsigned t=0;while(t<8&&bank->tokens[t])t++;if(t==8)return false;bank->tokens[t]=++s->next_token;
    bank->readers++;*out=(pet_bridge_image_lease_t){.slot=i,.bank=b,.epoch=s->epoch,.token=bank->tokens[t],.width=bank->ref.width,.height=bank->ref.height,.pixels=storage+(i*2U+b)*PET_BRIDGE_IMAGE_PIXELS,.valid=true};return true;
  }
  return false;
}
static inline bool pet_bridge_image_release(pet_bridge_images_t*s,pet_bridge_image_lease_t*lease){
  if(!s||!lease||!lease->valid||lease->slot>=PET_BRIDGE_IMAGES||lease->bank>1)return false;
  pet_bridge_image_bank_t*b=&s->banks[lease->slot][lease->bank];if(!b->readers)return false;
  unsigned t=0;while(t<8&&b->tokens[t]!=lease->token)t++;if(t==8||!lease->token)return false;b->tokens[t]=0;
  b->readers--;lease->valid=false;lease->pixels=NULL;return true;
}
