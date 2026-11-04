/* Native harness for extraction of the ACTUAL renderer row kernel. */
#include "pet_p4_h264_pack_word.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
 const uint8_t *plane_y,*plane_u,*plane_v;uint8_t *packed;
 size_t packed_stride;int src_w,src_stride,row_begin,row_end;
} pet_p4_h264_pack_job_t;
static unsigned attempts,successes;
static bool counted_pack(uint8_t *out,const uint8_t *y,const uint8_t *uv){
 attempts++;bool ok=pet_h264_pack8_word_io(out,y,uv);successes+=ok;return ok;
}
#if PET_P4_H264_PACK_WORD_IO
#define pet_h264_pack8_word_io counted_pack
#endif
/* ACTUAL_ROW_KERNEL */
static uint8_t luma[660*12+32] __attribute__((aligned(4)));
static uint8_t chroma_u[330*6+32] __attribute__((aligned(4)));
static uint8_t chroma_v[330*6+32] __attribute__((aligned(4)));
static uint8_t output[660*12*3/2+32] __attribute__((aligned(4)));
static uint8_t expected[sizeof(output)];
static uint32_t state=0x817451;
static uint32_t next_word(void){state=state*1664525U+1013904223U;return state;}
static uint32_t hash(const uint8_t *p,size_t n){uint32_t h=2166136261U;while(n--)h=(h^*p++)*16777619U;return h;}
static void independent_reference(const pet_p4_h264_pack_job_t *j,uint8_t *dest){
 for(int row=j->row_begin;row<j->row_end;row++){
  const uint8_t *y=j->plane_y+(size_t)row*j->src_stride;
  const uint8_t *uv=((row&1)?j->plane_v:j->plane_u)+(size_t)(row/2)*(j->src_stride/2);
  uint8_t *at=dest+(size_t)row*j->packed_stride;
  for(int x=0;x<j->src_w;x+=2){*at++=uv[x/2];*at++=y[x];*at++=y[x+1];}
 }
}
int main(void){
 for(size_t i=0;i<sizeof(luma);i++)luma[i]=(uint8_t)next_word();
 for(size_t i=0;i<sizeof(chroma_u);i++){chroma_u[i]=(uint8_t)next_word();chroma_v[i]=(uint8_t)next_word();}
 uint32_t hy=hash(luma,sizeof(luma)),hu=hash(chroma_u,sizeof(chroma_u)),hv=hash(chroma_v,sizeof(chroma_v));
 const int widths[]={2,4,6,8,10,14,16,18,30,62,126,318,638,640};
 for(unsigned wi=0;wi<sizeof(widths)/sizeof(widths[0]);wi++)for(unsigned align=0;align<256;align++)for(unsigned pad=0;pad<3;pad++){
  int width=widths[wi],height=2+(int)(wi%5)*2,stride=width+(int)pad*2;
  unsigned ay=align&3U,au=(align>>2)&3U,av=(align>>4)&3U,ao=(align>>6)&3U;
  unsigned offset=8+ao;memset(output,0xa5,sizeof(output));memcpy(expected,output,sizeof(output));
  pet_p4_h264_pack_job_t j={luma+ay,chroma_u+au,chroma_v+av,output+offset,(size_t)width*3/2,width,stride,0,height};
  independent_reference(&j,expected+offset);attempts=successes=0;
  int split=height/2;j.row_end=split;pack_h264_i420_rows(&j);j.row_begin=split;j.row_end=height;pack_h264_i420_rows(&j);
  assert(!memcmp(output,expected,sizeof(output)));
  assert(attempts==(PET_P4_H264_PACK_WORD_IO?(unsigned)height*(width/8):0));
  unsigned aligned=0;
  for(int row=0;row<height;row++){
   const uint8_t *y=j.plane_y+(size_t)row*stride,*uv=((row&1)?j.plane_v:j.plane_u)+(size_t)(row/2)*(stride/2);
   uint8_t *at=j.packed+(size_t)row*j.packed_stride;
   if(!(((uintptr_t)y|(uintptr_t)uv|(uintptr_t)at)&3U))aligned+=(unsigned)width/8;
  }
  assert(successes==(PET_P4_H264_PACK_WORD_IO?aligned:0));
 }
 assert(hy==hash(luma,sizeof(luma))&&hu==hash(chroma_u,sizeof(chroma_u))&&hv==hash(chroma_v,sizeof(chroma_v)));
 /* All alignment rejections leave every target byte unchanged. */
 memset(output,0xa5,sizeof(output));memcpy(expected,output,sizeof(output));
 for(unsigned a=0;a<64;a++)if(a){
  assert(!counted_pack(output+(a&3U),luma+((a>>2)&3U),chroma_u+((a>>4)&3U)));
  assert(!memcmp(output,expected,sizeof(output)));
 }
 assert(!counted_pack(NULL,luma,chroma_u)&&!counted_pack(output,NULL,chroma_u)&&!counted_pack(output,luma,NULL));
 assert(!memcmp(output,expected,sizeof(output)));
 return 0;
}
