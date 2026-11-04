/* Exact constant-alpha RGB565 lookup; fixed renderer-owned scratch only. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#ifndef PET_P4_RASTER_EXACT
#define PET_P4_RASTER_EXACT 0
#endif
#ifndef PET_P4_ALPHA_FAST
#define PET_P4_ALPHA_FAST 0
#endif
#ifndef PET_P4_GLYPH_ROWS
#define PET_P4_GLYPH_ROWS 0
#endif
#ifndef PET_P4_GLYPH_AXIS_INTERNAL
#define PET_P4_GLYPH_AXIS_INTERNAL 0
#endif
#if PET_P4_GLYPH_AXIS_INTERNAL && !PET_P4_RASTER_EXACT
#error "Glyph axis placement requires the exact-raster parent"
#endif
#ifndef PET_P4_GLYPH_PIXEL_FAST
#define PET_P4_GLYPH_PIXEL_FAST 0
#endif
#if PET_P4_GLYPH_PIXEL_FAST && (!PET_P4_RASTER_EXACT || !PET_P4_ALPHA_FAST || !PET_P4_GLYPH_ROWS)
#error "Glyph pixel store path requires the exact-raster, alpha-fast and glyph-rows parent"
#endif
#if PET_P4_GLYPH_ROWS && !PET_P4_RASTER_EXACT
#error "Glyph source rows require the exact-raster parent"
#endif
#if PET_P4_ALPHA_FAST && !PET_P4_RASTER_EXACT
#error "Alpha burst comparison requires the exact-raster parent"
#endif
typedef struct { uint8_t red[32],green[64],blue[32]; } pet_rgb565_blend_t;
_Static_assert(sizeof(pet_rgb565_blend_t)==128,"bounded renderer lookup");
static inline void pet_rgb565_blend_prepare(pet_rgb565_blend_t *table,
    uint8_t r,uint8_t g,uint8_t b,uint8_t alpha){
  for(unsigned i=0;i<64;i++){
    unsigned dst=i*255/63;
    table->green[i]=(uint8_t)(((g*alpha+dst*(255-alpha))/255)>>2);
    if(i<32){
      dst=i*255/31;
      table->red[i]=(uint8_t)(((r*alpha+dst*(255-alpha))/255)>>3);
      table->blue[i]=(uint8_t)(((b*alpha+dst*(255-alpha))/255)>>3);
    }
  }
}
static inline uint16_t pet_rgb565_blend_pixel(const pet_rgb565_blend_t *table,uint16_t pixel){
  return (uint16_t)((table->red[pixel>>11]<<11)|(table->green[(pixel>>5)&63]<<5)|table->blue[pixel&31]);
}
static inline void pet_rgb565_blend_rows(uint16_t *row,size_t stride,
    size_t columns,size_t rows,const pet_rgb565_blend_t *table){
  if(!row||!table||!columns||!rows||columns>stride)return;
  for(size_t y=0;y<rows;y++){
#if PET_P4_ALPHA_FAST
    /* Bounded internal scratch lets libc burst the PSRAM transfers. No
     * aliasing word casts or extra pixels at a clipped row's end. */
    uint16_t pixels[16];
    for(size_t x=0;x<columns;){
      size_t count=columns-x;if(count>16)count=16;
      memcpy(pixels,row+x,count*sizeof(*pixels));
      for(size_t i=0;i<count;i++)pixels[i]=pet_rgb565_blend_pixel(table,pixels[i]);
      memcpy(row+x,pixels,count*sizeof(*pixels));x+=count;
    }
#else
    for(size_t x=0;x<columns;x++)row[x]=pet_rgb565_blend_pixel(table,row[x]);
#endif
    if(y+1<rows)row+=stride;
  }
}
enum {PET_RASTER_AXIS_TILE=64};
typedef struct {int index;float fraction;} pet_raster_axis_t;
_Static_assert(sizeof(pet_raster_axis_t)*PET_RASTER_AXIS_TILE==512,"bounded renderer axis scratch");
/* Identical original float expression/floor, once per column, fixed tiles. */
static inline void pet_raster_axis_prepare(pet_raster_axis_t *axis,
    int begin,unsigned count,float origin,float scale){
  for(unsigned n=0;n<count;n++){
    float at=(begin+(int)n+.5f-origin)/scale-.5f;int index=(int)floorf(at);
    axis[n]=(pet_raster_axis_t){index,at-index};
  }
}
