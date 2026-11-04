/* p4-app-shell-v1: bounded tab bar + reserved tab actions. Pure functions over
 * the parsed declaration; the surface parser expands the bar into standard
 * button nodes so the renderer needs no new drawing branch. No IO, no heap. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_navigation_core.h"
#define PET_SHELL_PAGES 12
#define PET_SHELL_TABS_MIN 2
#define PET_SHELL_TABS_MAX 5
#define PET_SHELL_PAGE_SHARED 12 /* internal sentinel: node visible on every page */
typedef struct {char label[25];int8_t icon;uint8_t page;} pet_shell_tab_t;
typedef struct {
  bool enabled;
  uint8_t count;
  int16_t x,y;
  uint16_t w,h;
  uint16_t colors[4]; /* bar, active tab, label, active label */
  char var[32];
  pet_shell_tab_t items[PET_SHELL_TABS_MAX];
} pet_shell_tabs_t;
enum {PET_SHELL_TAB_NONE=0,PET_SHELL_TAB_NEXT=1,PET_SHELL_TAB_PREV=2};
static inline int pet_shell_tab_operation(const char *action){
  if(!action)return PET_SHELL_TAB_NONE;
  if(!strcmp(action,"tab.next"))return PET_SHELL_TAB_NEXT;
  if(!strcmp(action,"tab.prev"))return PET_SHELL_TAB_PREV;
  return PET_SHELL_TAB_NONE;
}
static inline bool pet_shell_tabs_valid(const pet_shell_tabs_t *t){
  if(!t||!t->enabled||t->count<PET_SHELL_TABS_MIN||t->count>PET_SHELL_TABS_MAX||t->x<0||t->y<0||t->w<64*t->count||t->h<32||t->h>96
     ||t->x+t->w>640||t->y+t->h>480||!t->var[0])return false;
  for(unsigned i=0;i<t->count;i++){
    if(t->items[i].page>=PET_SHELL_PAGES||!t->items[i].label[0])return false;
    for(unsigned j=0;j<i;j++)if(t->items[j].page==t->items[i].page)return false;
  }
  return true;
}
/* Clamp a tab variable to a tab index (negative/out of range -> wrapped). */
static inline unsigned pet_shell_tab_index(const pet_shell_tabs_t *t,int32_t value){
  if(!t||!t->count)return 0;int64_t v=value%(int64_t)t->count;if(v<0)v+=t->count;return (unsigned)v;
}
/* Geometry of tab i inside the bar: equal integer partition without gaps. */
static inline void pet_shell_tab_box(const pet_shell_tabs_t *t,unsigned i,int16_t *x,uint16_t *w){
  unsigned left=(unsigned)t->w*i/t->count,right=(unsigned)t->w*(i+1)/t->count;
  *x=(int16_t)(t->x+(int)left);*w=(uint16_t)(right-left);
}
/* Apply tab.next / tab.prev: wraps, sets the tab var, jumps to the tab's page
 * and clears the navigation history (a tab is a root, like reset). Returns
 * false when nothing changed or the operation is not a tab action. */
static inline bool pet_shell_tab_apply(const pet_shell_tabs_t *t,int op,int32_t *var,int8_t *page,pet_nav_history_t *history){
  if(!pet_shell_tabs_valid(t)||!var||!page||op==PET_SHELL_TAB_NONE)return false;
  unsigned current=pet_shell_tab_index(t,*var);
  unsigned next=op==PET_SHELL_TAB_NEXT?(current+1)%t->count:(current+t->count-1)%t->count;
  *var=(int32_t)next;*page=(int8_t)t->items[next].page;if(history)history->depth=0;
  return true;
}
/* Shared-page test for shell packages: PET_SHELL_PAGE_SHARED or the legacy
 * value 4 are never valid page indexes once a shell is declared; nodes that
 * omit page are visible everywhere. */
static inline bool pet_shell_node_visible(uint8_t node_page,int current_page){
  return node_page==PET_SHELL_PAGE_SHARED||node_page==current_page;
}
