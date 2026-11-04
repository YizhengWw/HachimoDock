/* Read-only renderer-owned packed YUV420. No guest pointer or capability. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {const uint8_t *data;size_t stride;int width,height,storage_height;} pet_h264_packed_view_t;
static inline bool pet_h264_packed_view_init(pet_h264_packed_view_t *out,const uint8_t *data,
  size_t bytes,size_t stride,int width,int height,int storage_height){
  if(!out||!data||width<=0||height<=0||storage_height<height||(width&1)||(storage_height&1)
    ||(size_t)width>SIZE_MAX/3||stride<(size_t)width*3/2||!stride
    ||(size_t)storage_height>SIZE_MAX/stride||bytes<stride*(size_t)storage_height)return false;
  *out=(pet_h264_packed_view_t){data,stride,width,height,storage_height};return true;
}
static inline uint8_t pet_h264_packed_clamp(int v){return v<0?0:v>255?255:(uint8_t)v;}
/* Caller validated the view and sx/sy bounds. Same limited BT709/RGB565
 * coefficients and nearest-neighbor samples as the original I420 fallback. */
static inline uint16_t pet_h264_packed_pixel(const pet_h264_packed_view_t *v,int sx,int sy){
  size_t at=(size_t)(sx/2)*3;
  int y=(int)v->data[(size_t)sy*v->stride+at+1+(sx&1)]-16;
  int u=(int)v->data[(size_t)(sy&~1)*v->stride+at]-128;
  int c=(int)v->data[(size_t)(sy|1)*v->stride+at]-128;if(y<0)y=0;
  uint8_t r=pet_h264_packed_clamp((298*y+459*c+128)>>8);
  uint8_t g=pet_h264_packed_clamp((298*y-55*u-136*c+128)>>8);
  uint8_t b=pet_h264_packed_clamp((298*y+541*u+128)>>8);
  return (uint16_t)(((uint16_t)(r&0xf8)<<8)|((uint16_t)(g&0xfc)<<3)|(b>>3));
}
