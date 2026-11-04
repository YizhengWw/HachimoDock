"""Actual native adapter/lifetime/pixel regressions; no RTOS, USB or sound."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def compile_run(tmp_path,code,name):
    source=tmp_path/(name+'.c');source.write_text(code);exe=tmp_path/name
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),
      *([str(ROOT/'main/pet_p4_resources.c')] if name=='adapter' else []),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=35)

def test_exact_prefetch_lifetime_cannot_retire_active_workers_consumers_or_newer_jobs(tmp_path):
    compile_run(tmp_path,r'''
#include "pet_p4_prefetch_lifetime.h"
#include <assert.h>
#include <string.h>
int main(void){
 pet_prefetch_lifetime_t p={0};pet_prefetch_ticket_t t={0};assert(pet_prefetch_init(&p));
 assert(!pet_prefetch_init(&p));assert(!pet_prefetch_offer(&p,0,&t));
 assert(pet_prefetch_offer(&p,1,&t));pet_prefetch_lifetime_t copy=p;
 assert(!pet_prefetch_exact((pet_prefetch_ticket_t){&copy,t.serial}));
 assert(!pet_prefetch_retire(&p));assert(!pet_prefetch_take(t,1));assert(!pet_prefetch_release(t));
 bool execute=false;assert(pet_prefetch_claim(t,&execute)&&execute);assert(!pet_prefetch_retire(&p));
 assert(pet_prefetch_cancel(t));assert(!pet_prefetch_retire(&p));assert(!pet_prefetch_take(t,1));
 assert(pet_prefetch_complete(t,true));assert(p.phase==PET_PREFETCH_DISCARDED);assert(!pet_prefetch_take(t,1));
 assert(pet_prefetch_retire(&p));pet_prefetch_ticket_t next={0};assert(pet_prefetch_offer(&p,2,&next));
 assert(!pet_prefetch_cancel(t)&&!pet_prefetch_complete(t,true));
 assert(pet_prefetch_claim(next,&execute)&&execute);assert(pet_prefetch_complete(next,true));
 assert(!pet_prefetch_take(next,1));assert(pet_prefetch_take(next,2));
 assert(pet_prefetch_cancel(next));assert(!pet_prefetch_retire(&p));assert(pet_prefetch_release(next));
 assert(p.phase==PET_PREFETCH_DISCARDED&&pet_prefetch_retire(&p));
 for(unsigned i=0;i<100000;i++){
   t=(pet_prefetch_ticket_t){0};assert(pet_prefetch_offer(&p,(uint64_t)i+3,&t));
   if(i%3==0)assert(pet_prefetch_cancel(t));
   assert(pet_prefetch_claim(t,&execute));assert(execute==(i%3!=0));
   assert(pet_prefetch_complete(t,execute));
   if(execute){assert(pet_prefetch_take(t,(uint64_t)i+3));assert(!pet_prefetch_retire(&p));assert(pet_prefetch_release(t));}
   assert(pet_prefetch_retire(&p));
 }
 p.serial=UINT64_MAX;t=(pet_prefetch_ticket_t){0};assert(!pet_prefetch_offer(&p,1,&t));
 return 0;
}
''','lifetime')

def test_actual_packed_fallback_matches_original_i420_full_pixels_padding_alignment_and_scaling(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    native=(ROOT/'main/pet_p4_h264_prefetch.inc').read_text()
    code=r'''
#include "pet_p4_h264_packed.h"
#include "pet_p4_h264_pack_word.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
typedef struct {const uint8_t *plane_y,*plane_u,*plane_v;uint8_t *packed;size_t packed_stride;int src_w,src_stride,row_begin,row_end;}pet_p4_h264_pack_job_t;
static uint16_t framebuffer[640*480],reference[640*480];static uint16_t *g_framebuffer=framebuffer;
static int destination_width,destination_height;
static bool asset_fit_layout(int sw,int sh,int *x,int *y,int *w,int *h,float *scale){
 (void)sw;(void)sh;(void)scale;*w=destination_width;*h=destination_height;*x=(640-*w)/2;*y=(480-*h)/2;return true;
}
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r&248U)<<8)|((g&252U)<<3)|(b>>3));}
'''+function(text,'clamp_u8')+function(text,'pack_h264_i420_rows')+function(text,'scale_h264_frame_cpu')+function(native,'scale_h264_packed_cpu')+r'''
int main(void){
 unsigned random=9284127;const int widths[]={2,6,16,30,62,318,640},heights[]={2,6,14,48,240,480};
 for(unsigned i=0;i<7;i++)for(unsigned j=0;j<6;j++)for(unsigned alignment=0;alignment<4;alignment++)for(int padding=0;padding<3;padding++){
  int w=widths[i],h=heights[j],stride=w+padding*2,storage_h=h+padding*2;
  size_t source_size=(size_t)stride*storage_h*3/2,packed_stride=(size_t)w*3/2+padding,packed_size=packed_stride*storage_h;
  uint8_t *source=malloc(source_size+8),*packed=malloc(packed_size+16);assert(source&&packed);
  memset(packed,0xa5,packed_size+16);
  for(size_t n=0;n<source_size+8;n++){random=random*1664525U+1013904223U;source[n]=(uint8_t)(random>>24);}
  uint8_t *original=malloc(source_size+8);assert(original);memcpy(original,source,source_size+8);
  uint8_t *in=source+alignment,*out=packed+8;
  pet_p4_h264_pack_job_t job={in,in+(size_t)stride*storage_h,in+(size_t)stride*storage_h*5/4,out,packed_stride,w,stride,0,storage_h/2};
  pack_h264_i420_rows(&job);job.row_begin=storage_h/2;job.row_end=storage_h;pack_h264_i420_rows(&job);
  pet_h264_packed_view_t view={0};assert(pet_h264_packed_view_init(&view,out,packed_size,packed_stride,w,h,storage_h));
  destination_width=(i%2)?640:317;destination_height=(j%2)?480:239;
  scale_h264_frame_cpu(in,w,h,stride,storage_h);memcpy(reference,framebuffer,sizeof(reference));
  memset(framebuffer,0xff,sizeof(framebuffer));scale_h264_packed_cpu(&view);
  assert(!memcmp(reference,framebuffer,sizeof(reference)));assert(!memcmp(original,source,source_size+8));
  for(unsigned n=0;n<8;n++)assert(packed[n]==0xa5&&packed[packed_size+8+n]==0xa5);
  for(int row=0;row<storage_h;row++)for(size_t n=(size_t)w*3/2;n<packed_stride;n++)assert(out[(size_t)row*packed_stride+n]==0xa5);
  pet_h264_packed_view_t previous=view;
  assert(!pet_h264_packed_view_init(&view,out,packed_size-1,packed_stride,w,h,storage_h));assert(!memcmp(&view,&previous,sizeof(view)));
  assert(!pet_h264_packed_view_init(&view,out,SIZE_MAX,SIZE_MAX,w,h,storage_h));
  free(original);free(source);free(packed);
 }
 return 0;
}
'''
    compile_run(tmp_path,code,'pixels')

def test_actual_adapter_pins_native_readers_cancels_safely_and_never_shows_future_frames(tmp_path):
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    declarations=source[source.index('static esp_h264_dec_handle_t g_h264_decoder;'):source.index('static uint8_t *g_h264_ppa_input;')]
    declarations+='\nstatic uint8_t *g_h264_ppa_input;static size_t g_h264_ppa_input_capacity;static char g_h264_fs_path[80];\n'
    declarations+=source[source.index('typedef struct {\n  uint8_t *bytes;'):source.index('static bool h264_stream_has_single_slice_access_units(')]
    functions='\n'.join(function(source,name) for name in (
        'asset_prepare_audio_busy','asset_prepare_current','asset_cache_release','pet_p4_renderer_cancel_asset_preparation',
        'pet_p4_renderer_set_asset_preparation_suspended','reset_h264_decoder','reset_h264_decoder_traced',
        'start_h264_decoder_role','decode_next_h264_frame_role'))
    code=(ROOT/'tests/p4_h264_prefetch_adapter_test.c').read_text().replace('/* ACTUAL_DECLARATIONS */',declarations).replace('/* ACTUAL_FUNCTIONS */',functions)
    # The role helpers use the original measured-reset entry only in the
    # legacy wrapper, which is covered by the unchanged replay/trace tests.
    compile_run(tmp_path,code,'adapter')
