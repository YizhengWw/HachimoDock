/* Renderer-owned bookkeeping for the EXISTING clean-frame buffer. No task,
 * allocation, clock, resource grant or second surface/key/framebuffer. */
#pragma once
#include "pet_p4_app_dirty.h"
enum { PET_APP_CACHE_ROWS=480, PET_APP_CACHE_ROW_QUANTUM=64 };
typedef struct {
  uint32_t rows[15];
  uint32_t revision;
  uint16_t count,cursor;
  bool active;
} pet_app_cache_progressive_t;
static inline void pet_app_cache_progressive_cancel(pet_app_cache_progressive_t *p){
  memset(p,0,sizeof(*p));
}
static inline void pet_app_cache_progressive_prepare(pet_app_cache_progressive_t *p,
    pet_app_frame_key_t *key,const pet_surface_t *surface,const char *const values[9],
    uint8_t page,int progress,uint32_t revision,bool full_valid){
  pet_app_dirty_rect_t dirty={0};
  bool previous=(p->active&&p->revision==revision)||full_valid;
  bool same=previous&&pet_app_frame_content_matches(key,surface,values,page,progress);
  bool bounded=previous&&pet_app_dirty_content_plan(key,surface,values,page,progress,&dirty);
  if(!previous||(!same&&!bounded))pet_app_cache_progressive_cancel(p);
  else if(!p->active){
    memset(p->rows,255,sizeof(p->rows));p->count=PET_APP_CACHE_ROWS;p->cursor=0;
  }
  if(!same&&bounded){
    for(int y=dirty.y;y<dirty.bottom;y++){
      uint32_t bit=UINT32_C(1)<<(y%32);
      if(p->rows[y/32]&bit){p->rows[y/32]&=~bit;p->count--;}
    }
  }
  p->active=true;p->revision=revision;
  pet_app_frame_stage(key,surface,values,page,progress);
}
/* Only invalid FULL rows are copied, before all transient overlays. A row is
 * published valid only after memcpy completes. Never expose a partial image.
 * The cursor is retained across exact-key changes, preventing a frequently
 * changing field from starving unrelated rows at the bottom of the screen. */
static inline unsigned pet_app_cache_progressive_copy(pet_app_cache_progressive_t *p,
    pet_app_frame_key_t *key,uint16_t *to,const uint16_t *from){
  if(!p->active)return 0;
  unsigned copied=0,visited=0;
  while(visited<PET_APP_CACHE_ROWS&&copied<PET_APP_CACHE_ROW_QUANTUM){
    unsigned y=p->cursor;p->cursor=(y+1)%PET_APP_CACHE_ROWS;visited++;
    uint32_t bit=UINT32_C(1)<<(y%32);
    if(p->rows[y/32]&bit)continue;
    memcpy(to+y*640,from+y*640,640*sizeof(*to));
    p->rows[y/32]|=bit;p->count++;copied++;
  }
  if(p->count==PET_APP_CACHE_ROWS){key->valid=true;p->active=false;}
  return copied;
}
