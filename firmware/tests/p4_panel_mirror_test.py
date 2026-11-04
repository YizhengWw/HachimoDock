"""Actual original/mirrored SDF pixels; no generated art or approximate AA."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def test_actual_panel_mirror_preserves_full_clipped_gradient_and_stroke_pixels(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include "pet_p4_surface.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t a[640*480+2],b[640*480+2],*g_framebuffer;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return(uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
'''
    for name in ('blend_rgb565','app_clip_contains','put_px','fill_rect','blend_px'):code+=function(text,name)+'\n'
    panel=function(text,'draw_app_panel')
    code+='\n#define PET_P4_APP_PANEL_MIRROR 0\n'+panel.replace('draw_app_panel(', 'reference_panel(')
    code+='\n#undef PET_P4_APP_PANEL_MIRROR\n#define PET_P4_APP_PANEL_MIRROR 1\n'+panel
    code+=r'''
static uint32_t state=0x125a1953;
static unsigned random_word(void){state=state*1664525U+1013904223U;return state;}
static void check(pet_surface_node_t n){
 for(unsigned i=0;i<640*480+2;i++)a[i]=b[i]=(uint16_t)(i*31337U+state);
 g_framebuffer=a+1;reference_panel(&n);g_framebuffer=b+1;draw_app_panel(&n);
 assert(!memcmp(a,b,sizeof(a)));
}
int main(void){
 for(unsigned n=0;n<600;n++){
  pet_surface_node_t p={.x=(int)(random_word()%850)-100,.y=(int)(random_word()%650)-90,
   .w=1+random_word()%680,.h=1+random_word()%500,.radius=random_word()%255,
   .opacity=255,.opacity_end=255,.color=(uint16_t)random_word(),.stroke=(uint16_t)random_word(),.vertical=n%2};
  if(n%7==1)p.opacity=127;if(n%7==2)p.opacity_end=17;if(n%7==3)p.stroke_width=3;
  g_app_clip.active=n%3;g_app_clip.x=17;g_app_clip.y=19;g_app_clip.right=529;g_app_clip.bottom=438;check(p);
 }
 g_app_clip.active=false;
 for(unsigned r=1;r<=200;r++)for(unsigned odd=0;odd<2;odd++){
  pet_surface_node_t p={.x=8,.y=6,.w=2*r+17+odd,.h=2*r+19+odd,.radius=r,
   .opacity=255,.opacity_end=255,.color=(uint16_t)(r*31337U)};check(p);
 }
 return 0;
}
'''
    src=tmp_path/'panel.c';binary=tmp_path/'panel';src.write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),
      str(src),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)
