#pragma once
#include "pet_p4_surface.h"
#include <stdio.h>
#include <string.h>
#include "pet_p4_render_policy.h"
#ifndef PET_P4_APP_DIRTY_CACHE
#define PET_P4_APP_DIRTY_CACHE 0
#endif
#ifndef PET_P4_APP_COPY_WORDS
#define PET_P4_APP_COPY_WORDS 0
#endif
#ifndef PET_P4_APP_CACHE_POPULATION_BUDGET
#define PET_P4_APP_CACHE_POPULATION_BUDGET 0
#endif
#ifndef PET_P4_APP_CACHE_PROGRESSIVE
#define PET_P4_APP_CACHE_PROGRESSIVE 0
#endif
#if PET_P4_APP_CACHE_PROGRESSIVE && !PET_P4_APP_DIRTY_CACHE
#error Progressive cache requires exact dirty-key support
#endif
/* Optional cache work has a smaller allocation than the unchanged whole-frame
 * gate. Deferral changes neither current pixels nor key/byte consistency. */
static inline bool pet_app_cache_population_admitted(int64_t started,int64_t now){
  return started>=0&&now>=started&&(uint64_t)(now-started)<=UINT64_C(28000);
}
/* Native disjoint-buffer copy only; no new framebuffer or DMA ownership.
 * GNU may_alias explicitly permits RGB565 object-representation access.
 * Unaligned/tail cases retain libc memcpy. Default builds remain unchanged. */
static inline void pet_app_frame_copy(void *to,const void *from,size_t bytes){
#if PET_P4_APP_COPY_WORDS
  if(bytes&&(((uintptr_t)to|(uintptr_t)from|bytes)&3u)==0){
    typedef uint32_t word_t __attribute__((__may_alias__));
    word_t *out=(word_t*)to;const word_t *in=(const word_t*)from;size_t count=bytes/4;
    while(count>=8){
      out[0]=in[0];out[1]=in[1];out[2]=in[2];out[3]=in[3];
      out[4]=in[4];out[5]=in[5];out[6]=in[6];out[7]=in[7];
      out+=8;in+=8;count-=8;
    }
    while(count--)*out++=*in++;
    return;
  }
#endif
  memcpy(to,from,bytes);
}
/* Cache keys only: the renderer reuses its existing background buffer. Resolved
 * text (including field_bind), values and interaction flags are part of the
 * exact key. A static application does not need a viewport to be cacheable.
 * The caller also excludes dynamic runtime layers outside this surface. Never
 * cache gameplay/animation or global connection/confirmation overlays. */
typedef struct {bool valid;uint8_t page;int progress;pet_surface_t surface;char values[9][160];} pet_app_frame_key_t;
/* page<0 is the conservative legacy query; renderer/matches know the page.
 * External pixels matter only when the resolved node can actually draw. */
static inline bool pet_app_frame_eligible_at(const pet_surface_t*s,int page){
  if(!s||!s->app||s->game_view||s->scene_view.enabled||s->data_view.enabled)return false;
  for(int i=0;i<s->count;i++){
    const pet_surface_node_t *n=&s->nodes[i];
    if(n->rotate_ms||n->floating)return false;
#if PET_P4_CELLS_V1
    /* The grid hash is a change hint, never exact pixel authority. */
    if(n->type!=PET_SURFACE_NODE_CELLS)continue;
    if(page<0)return false;
    if(n->hidden||!pet_surface_node_on_page(n,page))continue;
    pet_surface_node_t projected;
    const pet_surface_viewport_t *viewport=pet_surface_node_viewport(s,n);
    if(pet_surface_project(viewport,n,viewport?viewport->offset:0,&projected))return false;
#else
    (void)page;
#endif
  }
  return true;
}
static inline bool pet_app_frame_eligible(const pet_surface_t*s){return pet_app_frame_eligible_at(s,-1);}
static inline bool pet_app_frame_content_matches(const pet_app_frame_key_t*k,const pet_surface_t*s,const char*const values[9],uint8_t page,int progress){
  if(!pet_app_frame_eligible_at(s,page)||k->page!=page||k->progress!=progress||memcmp(&k->surface,s,sizeof(*s)))return false;
  for(int i=0;i<9;i++)if(strcmp(k->values[i],values[i]?values[i]:""))return false;
  return true;
}
static inline bool pet_app_frame_matches(const pet_app_frame_key_t*k,const pet_surface_t*s,const char*const values[9],uint8_t page,int progress){
  return k->valid&&pet_app_frame_content_matches(k,s,values,page,progress);
}
static inline void pet_app_frame_stage(pet_app_frame_key_t*k,const pet_surface_t*s,const char*const values[9],uint8_t page,int progress){
  k->valid=false;
  k->surface=*s;k->page=page;k->progress=progress;
  for(int i=0;i<9;i++)snprintf(k->values[i],sizeof(k->values[i]),"%s",values[i]?values[i]:"");
}
static inline void pet_app_frame_remember(pet_app_frame_key_t*k,const pet_surface_t*s,const char*const values[9],uint8_t page,int progress){
  pet_app_frame_stage(k,s,values,page,progress);
  k->valid=true;
}
