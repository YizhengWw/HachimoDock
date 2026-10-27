/* Shared drawing/hit-test geometry for the native component launcher. */
#pragma once
#include <stddef.h>
enum { PET_CENTER_COLUMNS=2, PET_CENTER_PAGE_SIZE=4,
  PET_CENTER_LEFT=24, PET_CENTER_TOP=86, PET_CENTER_STEP_X=304,
  PET_CENTER_STEP_Y=160, PET_CENTER_CARD_W=288, PET_CENTER_CARD_H=146 };
static inline int pet_center_hit(int x,int y,size_t selected,size_t count) {
  for(int slot=0;slot<PET_CENTER_PAGE_SIZE;slot++) {
    int left=PET_CENTER_LEFT+(slot%PET_CENTER_COLUMNS)*PET_CENTER_STEP_X;
    int top=PET_CENTER_TOP+(slot/PET_CENTER_COLUMNS)*PET_CENTER_STEP_Y;
    size_t index=(selected/PET_CENTER_PAGE_SIZE)*PET_CENTER_PAGE_SIZE+(size_t)slot;
    if(index<count&&x>=left&&x<left+PET_CENTER_CARD_W&&y>=top&&y<top+PET_CENTER_CARD_H)return (int)index;
  }
  return -1;
}
static inline size_t pet_center_page(size_t selected,size_t count,int direction) {
  if(!count)return 0;
  size_t pages=(count+PET_CENTER_PAGE_SIZE-1)/PET_CENTER_PAGE_SIZE;
  size_t page=selected/PET_CENTER_PAGE_SIZE;
  page=direction>0?(page+1)%pages:(page+pages-1)%pages;
  return page*PET_CENTER_PAGE_SIZE;
}
