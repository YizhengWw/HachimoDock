"""Actual two-source-row sampler: same pixels, fewer bitmap reads, bounded RAM."""
from pathlib import Path
import subprocess
import p4_raster_exact_test as raster
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]


def _run_rows(tmp_path,code,*,measure):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    extra='\nstatic float g_glyph_rows[2][66];\nstatic int32_t g_glyph_sample_y;\n'
    extra+=function(text,'app_prepare_glyph_rows')+function(text,'app_glyph_row_alpha')
    marker='\n#undef PET_P4_RASTER_EXACT\n#define PET_P4_RASTER_EXACT 0\n'
    assert code.count(marker)==1
    code=code.replace(marker,extra+marker,1)
    if measure:
        code=code.replace('static float glyph_alpha_at(', 'static unsigned source_samples;\nstatic float glyph_alpha_at(',1)
        code=code.replace('return (x*53+y*29)%256;', 'source_samples++;return (x*53+y*29)%256;',1)
        code=code.replace('(void)index;return x<0', '(void)index;source_samples++;return x<0',1)
        code=code.replace('int main(void){','int main(void){\n uint64_t old_total=0,new_total=0;',1)
        code=code.replace('releases=0;g_framebuffer=a+1;', 'source_samples=0;releases=0;g_framebuffer=a+1;',1)
        code=code.replace('unsigned before=releases;', 'unsigned before=releases;old_total+=source_samples;source_samples=0;',1)
        code=code.replace('assert(releases==before);', 'new_total+=source_samples;assert(releases==before);',1)
        code=code.replace(' return 0;',' assert(old_total>0&&new_total*2<old_total);\n return 0;',1)
    else:
        # Wider future/native cells must take the unchanged sampling path,
        # never index the fixed row scratch using an unbounded width.
        code=code.replace('.box_w=7+cp%17', '.box_w=7+cp%97',1)
        code=code.replace('f={32,24}', 'f={32,96}',1)
    src=tmp_path/'rows.c';exe=tmp_path/'rows';src.write_text(code)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_RASTER_EXACT=1','-DPET_P4_ALPHA_FAST=1','-DPET_P4_GLYPH_ROWS=1',
      '-I',str(ROOT/'main'),str(src),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_actual_native_and_atlas_rows_preserve_pixels_and_halve_source_sampling(tmp_path,monkeypatch):
    monkeypatch.setattr(raster,'run',lambda path,code:_run_rows(path,code,measure=True))
    raster.test_actual_glyph_all_sampling_paths_are_bit_exact(tmp_path)


def test_wide_glyphs_fall_back_without_clipping_or_scratch_overflow(tmp_path,monkeypatch):
    monkeypatch.setattr(raster,'run',lambda path,code:_run_rows(path,code,measure=False))
    raster.test_actual_glyph_all_sampling_paths_are_bit_exact(tmp_path)


def test_row_scratch_is_private_and_requires_the_exact_parent(tmp_path):
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_glyph_rows_parent':
            assert config[section]['extends']=='env:esp32_p4_delta_parent'
            assert 'PET_P4_GLYPH_ROWS=1' in flags
            assert not any(v in flags for v in ('TASK_PRIORITY','PROGRAM_EXECUTION','F_CPU','PERIPHERAL'))
        else:assert 'PET_P4_GLYPH_ROWS=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'static DRAM_ATTR float g_glyph_rows[2][66];' in text
    assert 'static DRAM_ATTR int32_t g_glyph_sample_y;' in text
    src=tmp_path/'boundary.c';src.write_text('#include "pet_p4_rgb565_blend.h"\nint main(void){return 0;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_GLYPH_ROWS=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Glyph source rows require the exact-raster parent' in result.stderr


def test_renderer_owned_layout_rows_preserve_wrapping_and_clear_between_labels(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_surface.h"
static char g_app_label_rows[8][192];
typedef struct{uint32_t cp;float x,y;int size;uint16_t color;}command_t;
static command_t commands[2][2048];static unsigned counts[2],stream;
static void draw_app_glyph(uint32_t cp,uint32_t next,float x,float y,int size,uint16_t color,const pet_surface_node_t*n){
 (void)next;(void)n;assert(counts[stream]<2048);command_t*c=&commands[stream][counts[stream]++];c->cp=cp;c->x=x;c->y=y;c->size=size;c->color=color;
}
static void draw_app_icon(int icon,int x,int y,int size,uint16_t color){draw_app_glyph(UINT32_MAX,0,x,y,size,color,NULL);(void)icon;}
static int app_glyph_width(uint32_t cp,uint32_t next){(void)next;return cp>127?32:12+cp%9;}
static void app_text_bounds(const char*text,int*top,int*bottom){(void)text;*top=1;*bottom=29;}
'''
    for name in ('utf8_next','utf8_peek','app_text_width'):code+=function(text,name)
    label=function(text,'draw_app_label')
    code+='\n#undef PET_P4_GLYPH_ROWS\n#define PET_P4_GLYPH_ROWS 0\n'+label.replace('draw_app_label(', 'reference_label(')
    code+='\n#undef PET_P4_GLYPH_ROWS\n#define PET_P4_GLYPH_ROWS 1\n'+label
    code+=r'''
int main(void){
 for(unsigned n=0;n<500;n++){
  char text[256];unsigned at=0;if(n%3==0){memcpy(text,"中文",6);at=6;}
  while(at<6+n%190){text[at]=(char)('a'+(at+n)%26);at++;}text[at]=0;
  pet_surface_node_t node={.x=(int)(n%47)-10,.y=(int)(n%31)-8,.w=n%140,.h=12+n%210,.font_size=12+n%37,.line_height=20+n%25,.padding=n%9,.align=n%3,.valign=(n/3)%3,.icon=n%5? -1:2,.icon_size=12,.gap=4,.type=n%2?1:8};
  counts[0]=counts[1]=0;stream=0;reference_label(&node,text,0x738e);stream=1;draw_app_label(&node,text,0x738e);
  assert(counts[0]==counts[1]);
  for(unsigned i=0;i<counts[0];i++){
   command_t*a=&commands[0][i],*b=&commands[1][i];assert(a->cp==b->cp&&a->x==b->x&&a->y==b->y&&a->size==b->size&&a->color==b->color);
  }
 }
 return 0;
}
'''
    src=tmp_path/'layout.c';exe=tmp_path/'layout';src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),
      '-I',str(ROOT/'tests/vendor/cjson'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
