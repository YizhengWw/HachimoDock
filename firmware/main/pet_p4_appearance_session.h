/* Pure appearance transaction admission. Serialized by the caller's existing
 * state mutex; route tokens are compared, never dereferenced. Not authentication. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef void (*pet_appearance_sender_t)(const char *,void *);
typedef struct {pet_appearance_sender_t sender;const void *route;char id[64];uint8_t kind;bool active,operation_active,recovery_blocked;uint64_t generation;}pet_appearance_session_t;
static inline bool pet_appearance_id_valid(const char *id){
  if(!id)return false;
  size_t n=0;for(;n<64&&id[n];n++){
    unsigned c=(unsigned char)id[n];
    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':'))return false;
  }
  return n>0&&n<64;
}
static inline bool pet_appearance_matches(const pet_appearance_session_t *s,pet_appearance_sender_t sender,const void *route,const char *id,unsigned kind){
  return s&&s->active&&sender&&s->sender==sender&&s->route==route&&s->kind==kind&&pet_appearance_id_valid(id)&&!strcmp(s->id,id);
}
static inline bool pet_appearance_claim(pet_appearance_session_t *s,pet_appearance_sender_t sender,const void *route,const char *id,unsigned kind){
  if(!s||s->active||s->operation_active||s->recovery_blocked||s->generation==UINT64_MAX||!sender||(kind!=1&&kind!=2)||!pet_appearance_id_valid(id))return false;
  s->generation++;s->sender=sender;s->route=route;memcpy(s->id,id,strlen(id)+1);s->kind=(uint8_t)kind;s->active=true;return true;
}
static inline bool pet_appearance_release(pet_appearance_session_t *s,pet_appearance_sender_t sender,const void *route,const char *id,unsigned kind){
  if(!pet_appearance_matches(s,sender,route,id,kind)||s->operation_active||s->recovery_blocked)return false;
  uint64_t generation=s->generation;memset(s,0,sizeof(*s));s->generation=generation;return true;
}
static inline bool pet_appearance_size(double v,uint32_t *out){
  if(!out||!isfinite(v)||v<0||v>UINT32_MAX)return false;
  uint32_t n=(uint32_t)v;if((double)n!=v)return false;*out=n;return true;
}
static inline bool pet_appearance_checksum(const char *s){
  if(!s)return false;
  for(unsigned i=0;i<16;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
  return s[16]==0;
}
