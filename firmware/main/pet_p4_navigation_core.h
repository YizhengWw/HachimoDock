#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Fixed-memory local navigation. Callers own validated declared page IDs and
 * serialize state/input. No files, providers, hardware or persistence. */
enum {PET_NAV_NONE,PET_NAV_PUSH,PET_NAV_REPLACE,PET_NAV_RESET,PET_NAV_BACK};
typedef struct {uint8_t op;int8_t target,from_page;} pet_nav_rule_t;
#define PET_NAV_MAX_DEPTH 16
typedef struct {uint8_t depth,pages[PET_NAV_MAX_DEPTH];} pet_nav_history_t;
/* Page index ceiling: 4 for v1 packages, 6 under quota v2, 12 under quota v3 /
 * the app shell. The caller validates the declared page count; the core only
 * bounds the index space (history depth is 16). */
#define PET_NAV_MAX_PAGES 12
static inline bool pet_nav_allowed(const pet_nav_history_t*h,int page,const pet_nav_rule_t*r){
  if(!h||!r||page<0||page>=PET_NAV_MAX_PAGES||h->depth>PET_NAV_MAX_DEPTH||r->op>PET_NAV_BACK||r->from_page< -1||r->from_page>=PET_NAV_MAX_PAGES)return false;
  for(unsigned i=0;i<h->depth;i++)if(h->pages[i]>=PET_NAV_MAX_PAGES)return false;
  if(r->from_page>=0&&r->from_page!=page)return false;
  if(r->op==PET_NAV_BACK)return h->depth>0;
  if(r->op!=PET_NAV_NONE&&(r->target<0||r->target>=PET_NAV_MAX_PAGES))return false;
  if(r->op==PET_NAV_PUSH)return h->depth<PET_NAV_MAX_DEPTH;
  return true;
}
static inline bool pet_nav_apply(pet_nav_history_t*h,int8_t*page,const pet_nav_rule_t*r){
  if(!page||!pet_nav_allowed(h,*page,r))return false;
  if(r->op==PET_NAV_BACK)*page=(int8_t)h->pages[--h->depth];
  else if(r->op==PET_NAV_PUSH){h->pages[h->depth++]=(uint8_t)*page;*page=r->target;}
  else if(r->op==PET_NAV_REPLACE)*page=r->target;
  else if(r->op==PET_NAV_RESET){h->depth=0;*page=r->target;}
  return true;
}
