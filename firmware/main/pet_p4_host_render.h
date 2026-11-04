#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_host_collection.h"
/* Native-owned, ephemeral rendering data, NOT an input observation or grant.
 * One capture copies a whole reply and availability decision. Pure consumers
 * never consult live host state while resolving a frame. No tokens/context. */
typedef struct {char name[32],text[64];int32_t number;bool integer;} pet_host_render_variable_t;
typedef struct {
  pet_host_render_variable_t variables[8];
  char actions[16][48];
  uint8_t count,action_count,mode;
  bool current,fresh,legacy,pending;
#if PET_SERVICE_BRIDGE_ENABLED
  uint64_t image_epoch;
#endif
#if PET_HOST_COLLECTION_ENABLED
  pet_host_collection_snapshot_t collection;
  uint64_t collection_epoch;
#endif
} pet_host_render_t;
static inline bool pet_host_render_variable(const pet_host_render_t *s,const char *name,
    bool integer,char text[64],int32_t *number){
  if(text)text[0]=0;
  if(number)*number=0;
  if(!s||!s->fresh||!name)return false;
  for(unsigned i=0;i<s->count&&i<8;i++)if(s->variables[i].integer==integer&&!strcmp(name,s->variables[i].name)){
    if(text)memcpy(text,s->variables[i].text,64);
    if(number)*number=s->variables[i].number;
    return true;
  }
  return false;
}
static inline bool pet_host_render_enabled(const pet_host_render_t *s,const char *action){
  if(!s||!s->current||!action)return false;
  if(s->legacy)return true; /* Match legacy native action availability. */
  if(!s->fresh||s->pending)return false;
  for(unsigned i=0;i<s->action_count&&i<16;i++)if(!strcmp(action,s->actions[i]))return true;
  return false;
}
