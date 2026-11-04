"""Actual opt-in native optimization paths; no device or user files."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
from p4_render_trace_test import function
import p4_raster_exact_test as raster


def fast_run(tmp_path,code):
    src=tmp_path/'fast.c';exe=tmp_path/'fast';src.write_text(code)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_ALPHA_FAST=1','-DPET_P4_RASTER_EXACT=1','-I',str(ROOT/'main'),str(src),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_actual_fast_alpha_endpoints_preserve_every_destination_and_clip(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "pet_p4_rgb565_blend.h"
#define PET_P4_UI_WIDTH 1
#define PET_P4_UI_HEIGHT 1
static uint16_t pixels[3],*g_framebuffer;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
'''
    for name in ('blend_rgb565','app_clip_contains','blend_px'):code+=function(text,name)
    code+=r'''
int main(void){
 const uint8_t colors[][3]={{0,0,0},{255,255,255},{128,128,128},{7,173,249},{254,1,92}};
 g_framebuffer=pixels+1;
 for(unsigned c=0;c<5;c++)for(unsigned d=0;d<65536;d++)for(unsigned a=0;a<=255;a+=255){
  pixels[0]=0x1234;pixels[1]=(uint16_t)d;pixels[2]=0x4321;
  blend_px(0,0,colors[c][0],colors[c][1],colors[c][2],a);
  assert(pixels[1]==blend_rgb565(d,colors[c][0],colors[c][1],colors[c][2],a));assert(pixels[0]==0x1234&&pixels[2]==0x4321);
 }
 pixels[1]=0x5abc;
 blend_px(-1,0,255,255,255,255);blend_px(1,0,255,255,255,255);
 blend_px(0,-1,255,255,255,255);blend_px(0,1,255,255,255,255);assert(pixels[1]==0x5abc);
 g_app_clip.active=true;g_app_clip.x=1;g_app_clip.right=2;g_app_clip.bottom=1;
 blend_px(0,0,255,255,255,255);assert(pixels[1]==0x5abc);
 g_app_clip.active=false;g_framebuffer=NULL;blend_px(0,0,255,255,255,255);
 return 0;
}
'''
    fast_run(tmp_path,code)


def test_actual_burst_masks_and_glyphs_match_original_pixels(tmp_path,monkeypatch):
    monkeypatch.setattr(raster,'run',fast_run)
    raster.test_all_destination_words_and_alpha_values(tmp_path)
    raster.test_actual_round_rectangle_preserves_clipping_and_each_pixel(tmp_path)
    raster.test_actual_glyph_all_sampling_paths_are_bit_exact(tmp_path)


def test_default_and_invalid_parent_flags_are_compile_time_boundaries(tmp_path):
    src=tmp_path/'default.c';exe=tmp_path/'default'
    includes='#include "pet_p4_rgb565_blend.h"\n#include "pet_p4_storage_io_trace.h"\n'
    src.write_text(includes+'_Static_assert(!PET_P4_ALPHA_FAST&&!PET_P4_SPARE_EXACT_REUSE,"default off");\nint main(void){return 0;}\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    src.write_text(includes+'int main(void){return 0;}\n')
    for flags,message in (
        (['-DPET_P4_ALPHA_FAST=1'],'Alpha burst comparison requires the exact-raster parent'),
        (['-DPET_P4_SPARE_EXACT_REUSE=1'],'Exact spare reuse requires the reviewed storage parent'),
        (['-DPET_P4_SPARE_EXACT_REUSE=1','-DPET_P4_STORAGE_PRESERVE_UNCHANGED=1','-DPET_P4_STORAGE_IO_TRACE_ENABLE=1','-DPET_P4_SPARE_RECREATE=1'],'Exact spare reuse requires the reviewed storage parent'),
    ):
        result=subprocess.run(['cc','-std=c11','-fsyntax-only','-I',str(ROOT/'main'),*flags,str(src)],capture_output=True,text=True)
        assert result.returncode!=0 and message in result.stderr
