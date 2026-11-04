/* Bounded opaque RGB565 spans. Same pixels/order; no DMA or shared scratch. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef PET_P4_RENDER_SPAN_PIXELS
#define PET_P4_RENDER_SPAN_PIXELS 16
#endif
#if PET_P4_RENDER_SPAN_PIXELS != 16 && PET_P4_RENDER_SPAN_PIXELS != 32 && PET_P4_RENDER_SPAN_PIXELS != 64 && PET_P4_RENDER_SPAN_PIXELS != 128
#error "RGB565 span pattern must be a reviewed bounded size"
#endif
#ifndef PET_P4_RENDER_SPAN_FILL
#define PET_P4_RENDER_SPAN_FILL 0
#endif
static inline void pet_rgb565_fill_rows(uint16_t *row,size_t stride,size_t columns,size_t rows,uint16_t color){
  if(!row||!columns||!rows||columns>stride)return;
  /* A tiny private pattern, not a full row/frame or aliasing uint32_t cast.
   * libc can burst-copy internal stack to PSRAM instead of halfword stores.
   * Default scratch is 32 bytes; private experiments are capped at 256 bytes.
   * Actual target stack and full-frame gates still apply. */
  uint16_t pattern[PET_P4_RENDER_SPAN_PIXELS];
  for(unsigned i=0;i<PET_P4_RENDER_SPAN_PIXELS;i++)pattern[i]=color;
  for(size_t y=0;y<rows;y++){
    size_t x=0;
    for(;columns-x>=PET_P4_RENDER_SPAN_PIXELS;x+=PET_P4_RENDER_SPAN_PIXELS)memcpy(row+x,pattern,sizeof(pattern));
    if(x<columns)memcpy(row+x,pattern,(columns-x)*sizeof(*row));
    if(y+1<rows)row+=stride;
  }
}
