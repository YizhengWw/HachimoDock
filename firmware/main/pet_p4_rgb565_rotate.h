/* Bit-exact clockwise quarter-turn used by the existing panel mapping.
 * Private CPU/PPA comparison only: no extra image allocation, DMA, task, fence
 * or pixel-format change. Bounded tiles improve locality for BOTH matrices. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef PET_P4_RENDER_ROTATE_TILE
#define PET_P4_RENDER_ROTATE_TILE 0
#endif
static inline bool pet_rgb565_rotate_panel(uint16_t *out,size_t capacity,
  const uint16_t *in,unsigned width,unsigned height){
  if(!out||!in||!width||!height||width>640||height>480)return false;
  size_t count=(size_t)width*height,bytes=count*sizeof(*out);
  if(capacity<count)return false;
  uintptr_t a=(uintptr_t)out,b=(uintptr_t)in;
  if(a<=b?b-a<bytes:a-b<bytes)return false;
  for(unsigned x0=0;x0<width;x0+=32){
    unsigned xe=width-x0<32?width:x0+32;
    for(unsigned y0=0;y0<height;y0+=32){
      unsigned ye=height-y0<32?height:y0+32;
      for(unsigned x=x0;x<xe;x++){
        uint16_t *target=out+(size_t)x*height+(height-ye);
        /* Integer index remains in bounds even for the final y==1. No pointer
         * is decremented before the source allocation after its last read. */
        for(unsigned y=ye;y>y0;y--)*target++=in[(size_t)(y-1)*width+x];
      }
    }
  }
  return true;
}
