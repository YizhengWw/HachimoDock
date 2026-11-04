/* Bounded inverse sampler for body-only sprite/shape rotation. No framebuffer. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
static inline int body_sin_q8(int angle){
  int d=angle%360;if(d<0)d+=360;bool neg=d>=180;if(neg)d-=180;
  int p=d*(180-d),v=4*p*256/(40500-p);return neg?-v:v;
}
static inline bool body_source_pixel(int px,int py,int cx,int cy,int w,int h,int angle,int *sx,int *sy){
  int co=body_sin_q8(angle+90),si=body_sin_q8(angle);
  int dx=(px-cx)*256+128,dy=(py-cy)*256+128;
  int x=(co*dx+si*dy)/256+w*128,y=(-si*dx+co*dy)/256+h*128;
  if(x<0||y<0||x>=w*256||y>=h*256)return false;
  *sx=x/256;*sy=y/256;return true;
}
