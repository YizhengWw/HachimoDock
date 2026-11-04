/* Exact frame deduplication: caller retains two distinct, complete buffers.
 * Only a successful LCD submission makes a reference valid. No hash collisions,
 * extra framebuffer, or changes to input/game/animation ticking are involved. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include "pet_p4_render_policy.h"

/* Exact READ-ONLY comparison with the already displayed clockwise-rotated
 * RGB565 buffer. Never a hash/sample or authority from an application key.
 * Two-by-two tiles retain both buffers' locality. Unsupported layouts simply
 * require the original rotate/compare/submit path. Native-owned disjoint data
 * only, no allocation, DMA, clocks, or ownership changes in this helper. */
static inline bool pet_p4_logical_frame_unchanged(const uint16_t *logical,
    const uint16_t *previous,unsigned width,unsigned height){
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if(!logical||!previous||!width||!height||width>640||height>480||(width&1u)||(height&1u)
      ||(((uintptr_t)logical|(uintptr_t)previous)&3u))return false;
  size_t bytes=(size_t)width*height*sizeof(*logical);
  uintptr_t a=(uintptr_t)logical,b=(uintptr_t)previous;
  if(a<=b?b-a<bytes:a-b<bytes)return false;
  typedef uint32_t word_t __attribute__((__may_alias__));
  for(unsigned top=0;top<height;top+=16)for(unsigned left=0;left<width;left+=16){
    unsigned bottom=height-top<16?height:top+16,right=width-left<16?width:left+16;
    for(unsigned y=top;y<bottom;y+=2)for(unsigned x=left;x<right;x+=2){
      uint32_t first=*(const word_t*)(logical+(size_t)y*width+x);
      uint32_t second=*(const word_t*)(logical+(size_t)(y+1)*width+x);
      size_t at=(size_t)x*height+height-y-2;
      if(*(const word_t*)(previous+at)!=((second&0xffffu)|(first<<16))
          ||*(const word_t*)(previous+at+height)!=((second>>16)|(first&0xffff0000u)))return false;
    }
  }
  return true;
#else
  (void)logical;(void)previous;(void)width;(void)height;return false;
#endif
}

static inline bool pet_p4_frame_unchanged(const void *candidate,
    const void *previous, size_t bytes, bool valid) {
  return valid && candidate && previous && candidate != previous && bytes > 0
    && memcmp(candidate, previous, bytes) == 0;
}
