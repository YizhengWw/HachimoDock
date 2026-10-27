#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* P4I1: magic, u8 width/height, reserved=1, encoding=0, u32 LE total bytes;
 * 256 RGBA palette entries, then PackBits-like runs of palette indices.
 * Header/palette are retained in flash; indices are expanded only in RAM. */
#define PET_IMAGE_HEADER 1036U
#define PET_IMAGE_MAX_BYTES 100000U
/* P4J1 is a single opaque, full-resolution static background. The package
 * retains PNG; the trusted host emits bounded baseline JPEG, never scripts. */
static inline bool pet_image_jpeg(const uint8_t *p,size_t n) {
  if(!p||n<16||n>PET_IMAGE_MAX_BYTES||p[0]!='P'||p[1]!='4'||p[2]!='J'||p[3]!='1')return false;
  unsigned w=p[4]|(unsigned)p[5]<<8,h=p[6]|(unsigned)p[7]<<8;
  uint32_t size=(uint32_t)p[8]|(uint32_t)p[9]<<8|(uint32_t)p[10]<<16|(uint32_t)p[11]<<24;
  if(w!=640||h!=480||size!=n||p[12]!=255||p[13]!=216||p[n-2]!=255||p[n-1]!=217)return false;
  bool frame=false;
  for(size_t at=14;at+4<=n;){
    if(p[at++]!=255)return false;while(at<n&&p[at]==255)at++;
    if(at>=n)return false;unsigned marker=p[at++];
    if(marker==0xda)return frame; /* entropy data is checked by bounded decoder */
    if(marker==0||marker==0xd8||marker==0xd9||(marker>=0xd0&&marker<=0xd7)||at+2>n)return false;
    size_t len=((size_t)p[at]<<8)|p[at+1];if(len<2||len>n-at)return false;
    if(marker>=0xc0&&marker<=0xcf&&marker!=0xc4&&marker!=0xc8&&marker!=0xcc){
      if(marker!=0xc0||frame||len!=17||p[at+2]!=8||(((unsigned)p[at+3]<<8)|p[at+4])!=h||(((unsigned)p[at+5]<<8)|p[at+6])!=w||p[at+7]!=3)return false;
      frame=true;
    }
    at+=len;
  }return false;
}
static inline bool pet_image_decode(const uint8_t *p,size_t n,uint8_t *indices,size_t capacity) {
  if(pet_image_jpeg(p,n))return indices==NULL;
  if(!p||n<PET_IMAGE_HEADER||n>PET_IMAGE_MAX_BYTES||p[0]!='P'||p[1]!='4'||p[2]!='I'||(p[3]!='1'&&p[3]!='2'))return false;
  bool wide=p[3]=='2';
  unsigned w=wide?(p[4]|(unsigned)p[5]<<8):p[4],h=wide?(p[6]|(unsigned)p[7]<<8):p[5];
  if(w<8||w>(wide?512:240)||h<8||h>240||w*h>98304||(!wide&&(p[6]!=1||p[7]!=0)))return false;
  uint32_t size=(uint32_t)p[8]|(uint32_t)p[9]<<8|(uint32_t)p[10]<<16|(uint32_t)p[11]<<24;
  if(size!=n)return false;
  size_t count=(size_t)w*h,out=0,pos=PET_IMAGE_HEADER;
  if(indices&&capacity<count)return false;
  while(pos<n){
    unsigned tag=p[pos++],len=(tag&127)+1;
    if(out+len>count)return false;
    if(tag&128){if(pos>=n)return false;for(unsigned j=0;j<len;j++)if(indices)indices[out+j]=p[pos];pos++;}
    else {if(pos+len>n)return false;for(unsigned j=0;j<len;j++)if(indices)indices[out+j]=p[pos+j];pos+=len;}
    out+=len;
  }
  return out==count;
}
