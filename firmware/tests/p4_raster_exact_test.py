"""Bit-exact production alpha/glyph raster paths with bounded scratch."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
from p4_render_trace_test import function


def run(tmp_path, code):
    source=tmp_path/'raster.c';binary=tmp_path/'raster'
    source.write_text(code)
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
      '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),
      '-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)


def test_all_destination_words_and_alpha_values(tmp_path):
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    run(tmp_path,r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_rgb565_blend.h"
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
''' + function(source,'blend_rgb565') + r'''
int main(void){
 pet_rgb565_blend_t table;
 const uint8_t colors[][3]={{0,0,0},{255,255,255},{128,128,128},{7,173,249},{254,1,92}};
 for(unsigned c=0;c<sizeof(colors)/sizeof(colors[0]);c++)for(unsigned a=0;a<256;a++){
  pet_rgb565_blend_prepare(&table,colors[c][0],colors[c][1],colors[c][2],a);
  for(unsigned d=0;d<65536;d++)assert(pet_rgb565_blend_pixel(&table,d)==blend_rgb565(d,colors[c][0],colors[c][1],colors[c][2],a));
 }
 uint16_t pixels[100],before[100];for(unsigned i=0;i<100;i++)pixels[i]=before[i]=(uint16_t)(i*977);
 pet_rgb565_blend_prepare(&table,19,231,99,110);
 pet_rgb565_blend_rows(pixels+1,19,17,4,&table);
 for(unsigned i=0;i<100;i++){
  bool changed=false;for(unsigned y=0;y<4;y++)if(i>=1+y*19&&i<18+y*19)changed=true;
  assert(pixels[i]==(changed?blend_rgb565(before[i],19,231,99,110):before[i]));
 }
 memcpy(before,pixels,sizeof(pixels));
 pet_rgb565_blend_rows(NULL,19,17,4,&table);pet_rgb565_blend_rows(pixels,19,20,4,&table);
 pet_rgb565_blend_rows(pixels,19,0,4,&table);pet_rgb565_blend_rows(pixels,19,17,0,&table);
 pet_rgb565_blend_rows(pixels,19,17,4,NULL);assert(!memcmp(before,pixels,sizeof(pixels)));
 return 0;
}
'''.replace('#include <stdbool.h>','#include <stdbool.h>\n#include <string.h>'))


def test_actual_round_rectangle_preserves_clipping_and_each_pixel(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    names=['blend_rgb565','app_clip_contains','blend_px','blend_rect','blend_round_rect']
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "pet_p4_rgb565_blend.h"
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t a[640*480+2],b[640*480+2],*g_framebuffer;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static pet_rgb565_blend_t g_blend_lookup;
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
'''
    code+=function(text,'blend_rgb565')+function(text,'app_clip_contains')+function(text,'blend_px')
    for enabled in (0,1):
        code+=f'\n#undef PET_P4_RASTER_EXACT\n#define PET_P4_RASTER_EXACT {enabled}\n'
        rect=function(text,'blend_rect');rounded=function(text,'blend_round_rect')
        if not enabled:
            rect=rect.replace('blend_rect(', 'reference_rect(')
            rounded=rounded.replace('blend_round_rect(', 'reference_round(').replace('blend_rect(', 'reference_rect(')
        code+=rect+rounded
    code+=r'''
static uint32_t state=519;
static unsigned random_word(void){state=state*1664525U+1013904223U;return state;}
int main(void){
 for(unsigned i=0;i<640*480+2;i++)a[i]=b[i]=(uint16_t)random_word();
 for(unsigned n=0;n<1200;n++){
  int x=(int)(random_word()%850)-100,y=(int)(random_word()%650)-100;
  int w=(int)(random_word()%680)-20,h=(int)(random_word()%500)-20,r=(int)(random_word()%160)-20;
  g_app_clip.active=n%3;g_app_clip.x=(int)(random_word()%640);g_app_clip.y=(int)(random_word()%480);
  g_app_clip.right=g_app_clip.x+(int)(random_word()%650);g_app_clip.bottom=g_app_clip.y+(int)(random_word()%490);
  uint8_t red=random_word(),green=random_word(),blue=random_word(),alpha=n%256;
  g_framebuffer=a+1;reference_round(x,y,w,h,r,red,green,blue,alpha);
  g_framebuffer=b+1;blend_round_rect(x,y,w,h,r,red,green,blue,alpha);
  assert(!memcmp(a,b,sizeof(a)));
 }
 g_framebuffer=NULL;blend_round_rect(1,1,30,30,10,128,128,128,110);
 return 0;
}
'''
    run(tmp_path,code)


def test_actual_glyph_all_sampling_paths_are_bit_exact(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "pet_p4_rgb565_blend.h"
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t a[640*480+2],b[640*480+2],*g_framebuffer;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];
typedef struct{int x,y,w,h,weight;}pet_surface_node_t;
typedef struct{int cell_h,cell_w;}pet_surface_font_t;
typedef struct lv_font_glyph_dsc lv_font_glyph_dsc_t;
typedef struct{int line_height,base_line;const uint8_t*(*get_glyph_bitmap)(lv_font_glyph_dsc_t*,void*);}font_t;
struct lv_font_glyph_dsc{int box_w,box_h,ofs_x,ofs_y,req_raw_bitmap;font_t *resolved_font;};
static unsigned releases;
static uint8_t bitmap[100];
static const uint8_t *get_bitmap(lv_font_glyph_dsc_t*g,void*v){(void)v;return g->box_w==19?NULL:bitmap;}
static font_t pet_p4_font_cn_16={24,5,get_bitmap};
static bool lv_font_get_glyph_dsc(font_t*f,lv_font_glyph_dsc_t*g,uint32_t cp,uint32_t next){
 (void)next;if(!cp)return false;
 *g=(lv_font_glyph_dsc_t){.box_w=7+cp%17,.box_h=9+cp%15,.ofs_x=(int)(cp%5)-2,.ofs_y=(int)(cp%7)-3,.resolved_font=f};return true;
}
static void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t*g){(void)g;releases++;}
static float glyph_alpha_at(const uint8_t*p,const lv_font_glyph_dsc_t*g,int x,int y){(void)p;(void)g;return (x*53+y*29)%256;}
static const pet_surface_font_t *app_font_glyph(uint32_t cp,int*index){
 static const pet_surface_font_t f={32,24};*index=3;return cp==1?&f:NULL;
}
static float app_font_sample(const pet_surface_font_t*f,int index,int x,int y){(void)index;return x<0||y<0||x>=f->cell_w||y>=f->cell_h?0:(x*43+y*71)%256;}
static int pet_app_symbol_advance(uint32_t cp){return cp==2?13:cp==0x2103?16:0;}
static float pet_app_symbol_coverage(uint32_t cp,float x,float y,float scale){(void)cp;(void)scale;return x>=1&&y>=2&&x<10&&y<13?.67f:0;}
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
'''
    for name in ('blend_rgb565','app_clip_contains','blend_px','app_glyph_sample'):
        code+=function(text,name)
    for enabled in (0,1):
        code+=f'\n#undef PET_P4_RASTER_EXACT\n#define PET_P4_RASTER_EXACT {enabled}\n'
        glyph=function(text,'draw_app_glyph')
        if not enabled:glyph=glyph.replace('draw_app_glyph(', 'reference_glyph(')
        code+=glyph
    code+=r'''
int main(void){
 const uint32_t cps[]={0,1,2,23,0x2103,0x4e2d,0xfe0f,0x200d};
 for(unsigned n=0;n<240;n++){
  for(unsigned i=0;i<640*480+2;i++)a[i]=b[i]=(uint16_t)(i*977+n*317);
  pet_surface_node_t clip={.x=(int)(n%53)-10,.y=(int)(n%41)-9,.w=80+n%175,.h=60+n%160,.weight=n%2};
  g_app_clip.active=n%3;g_app_clip.x=9;g_app_clip.y=13;g_app_clip.right=600;g_app_clip.bottom=430;
  float x=(int)(n%101)-23+(n%4)*.25f,y=(int)(n%81)-17+(n%3)*.333333f;
  int size=12+n%85;uint32_t cp=cps[n%8];uint16_t color=(uint16_t)(n*5913);
  releases=0;g_framebuffer=a+1;reference_glyph(cp,65,x,y,size,color,&clip);unsigned before=releases;
  releases=0;g_framebuffer=b+1;draw_app_glyph(cp,65,x,y,size,color,&clip);
  assert(releases==before);assert(!memcmp(a,b,sizeof(a)));
 }
 return 0;
}
'''
    run(tmp_path,code)


def test_raster_candidate_keeps_default_policy_and_fixed_scratch():
    import configparser
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_raster_parent':
            assert config[section]['extends']=='env:esp32_p4_prefetch_parent'
            assert 'PET_P4_RASTER_EXACT=1' in flags
            assert 'PET_P4_HELLO_ASYNC=1' in flags and 'PET_P4_APP_PHASE_TRACE=1' in flags
            assert 'PET_P4_SPARE_RECREATE' not in flags
        else:assert 'PET_P4_RASTER_EXACT=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'static DRAM_ATTR pet_rgb565_blend_t g_blend_lookup;' in text
    assert 'static EXT_RAM_BSS_ATTR pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];' in text
    assert '#define PET_P4_RENDER_FRAME_BUDGET_US 66667U' in text
    assert 'pet_p4_frame_unchanged(g_native_framebuffer, previous_native,' in text
