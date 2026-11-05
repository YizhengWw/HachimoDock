"""R139 cold APP frame fast paths: pixel-exact against the loops they bypass; flag gating and call placement."""
from pathlib import Path
import configparser
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]
FLAGS=['-DPET_P4_APP_COLD_FAST=1','-DPET_P4_APP_CLEAR_OPAQUE_CENTER=1','-DPET_P4_APP_DIRTY_CACHE=1','-DPET_P4_RASTER_EXACT=1','-DPET_P4_ALPHA_FAST=1','-DPET_P4_GLYPH_ROWS=1']


def test_spans_match_the_per_pixel_reference_for_every_alpha(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_rgb565_blend.h"
'''+function(text,'rgb565')+function(text,'blend_rgb565')+function(text,'cold_fast_blend_span')+text[text.index('typedef struct {uint16_t r[32],g[64],b[32];} cold_fast_blend_lut_t;'):text.index('static inline void cold_fast_image_span(')]+function(text,'cold_fast_image_span')+function(text,'cold_fast_palette_opaque')+function(text,'cold_fast_indices_opaque')+r'''
/* Reference: the original draw_surface_image per-pixel body at identity sampling. */
static void reference_image(uint16_t *out,const uint8_t *row,const uint16_t *colors,const uint8_t *alpha,const uint8_t *palette,int count){
  for(int dx=0;dx<count;dx++){
    unsigned index=row[dx],a=alpha[index];
    if(!a)continue;
    if(a==255){out[dx]=colors[index];continue;}
    const uint8_t *p=palette+4*index;uint16_t bg=out[dx];
    unsigned r=((bg>>11)&31)*255/31,g=((bg>>5)&63)*255/63,b=(bg&31)*255/31;
    out[dx]=rgb565((p[0]*a+r*(255-a))/255,(p[1]*a+g*(255-a))/255,(p[2]*a+b*(255-a))/255);
  }
}
static uint32_t seed=12345;static unsigned rnd(void){seed=seed*1103515245u+12345u;return seed>>8;}
int main(void){
  enum{N=4096};
  static uint16_t a[N],b[N];static uint8_t row[N],palette[1024];static uint16_t colors[256];static uint8_t alpha[256];
  for(int c=0;c<256;c++){palette[4*c]=rnd();palette[4*c+1]=rnd();palette[4*c+2]=rnd();palette[4*c+3]=(c%7==0)?0:(c%5==0)?255:rnd();colors[c]=rgb565(palette[4*c],palette[4*c+1],palette[4*c+2]);alpha[c]=palette[4*c+3];}
  for(int t=0;t<64;t++){
    for(int i=0;i<N;i++){a[i]=b[i]=rnd();row[i]=rnd();}
    int count=1+rnd()%N;reference_image(a,row,colors,alpha,palette,count);cold_fast_image_span(b,row,colors,alpha,palette,count);
    assert(!memcmp(a,b,sizeof(a)));
  }
  /* Panel: every alpha, against blend_rgb565 pixel by pixel (what blend_px does for 0<alpha<255). */
  for(int al=1;al<255;al+=1){
    for(int i=0;i<N;i++){a[i]=b[i]=rnd();}
    uint8_t r=rnd(),g=rnd(),bl=rnd();
    for(int i=0;i<N;i++)a[i]=blend_rgb565(a[i],r,g,bl,al);
    cold_fast_blend_span(b,N,r,g,bl,al);assert(!memcmp(a,b,sizeof(a)));
    /* LUT span: exact against the same reference for every alpha and random colours. */
    for(int i=0;i<N;i++)b[i]=rnd();
    memcpy(a,b,sizeof(a));
    for(int i=0;i<N;i++)a[i]=blend_rgb565(a[i],r,g,bl,al);
    cold_fast_blend_lut_t lut;cold_fast_blend_lut(&lut,r,g,bl,al);cold_fast_blend_span_lut(b,N,&lut);assert(!memcmp(a,b,sizeof(a)));
  }
  /* used-index opacity: unused non-opaque palette entries must not matter; a used one must. */
  { static uint8_t idx[300];for(int i=0;i<300;i++)idx[i]=(uint8_t)(i%7==0?5:10);static uint8_t al[256];memset(al,255,256);al[5]=255;al[10]=255;al[200]=0;
    assert(cold_fast_indices_opaque(idx,300,al));al[10]=254;assert(!cold_fast_indices_opaque(idx,300,al)); }
  static uint8_t opaque[1024];memset(opaque,255,sizeof(opaque));assert(cold_fast_palette_opaque(opaque));
  opaque[4*200+3]=254;assert(!cold_fast_palette_opaque(opaque));assert(!cold_fast_palette_opaque(palette));
  return 0;
}
'''
    src=tmp_path/'cold.c';exe=src.with_suffix('');src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',*FLAGS,'-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=120)


def test_fast_paths_are_flag_gated_and_keep_the_original_conditions(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text();diag=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    assert text.count('#if PET_P4_APP_COLD_FAST')==13
    service=text[text.index('static bool draw_service_image('):text.index('static void draw_surface_image(')]
    assert service.count('#if PET_P4_APP_COLD_FAST')==1
    assert service.index('pet_app_host_image_release(&lease);')<service.index('#if PET_P4_APP_COLD_FAST')<service.index('g_cold_fast_jpeg_pending=false;')<service.index('#endif')<service.index('return true;')
    image=text[text.index('static void draw_surface_image('):text.index('static void load_surface_images(')]
    assert image.count('if(!n->rotate_ms&&!n->tile_cols&&n->w==iw&&n->h==ih&&g_framebuffer){')==1
    assert image.index('const uint16_t *colors=g_surface_colors[n->image];')<image.index('#if PET_P4_APP_COLD_FAST\n  if(!n->rotate_ms')<image.index('for(int dy=0;dy<n->h;dy++,row_x+=xy,row_y+=yy){')
    assert "if(py<0||py>=480||(g_app_clip.active&&(py<g_app_clip.y||py>=g_app_clip.bottom)))continue;" in image
    panel=text[text.index('static void draw_app_panel('):text.index('static void draw_app_icon(')]
    assert panel.count('if(!radius&&!n->stroke_width&&n->opacity==n->opacity_end&&n->opacity&&n->opacity!=255&&g_framebuffer&&x1>x0&&y1>y0){')==1
    assert panel.index('if(g_app_clip.active){')<panel.index('#if PET_P4_APP_COLD_FAST')<panel.index('const bool solid=')
    clear=text[text.index('static bool cold_fast_opaque_first_image('):text.index('static void app_clear_uncovered(')]
    assert "n->type!=6||n->in_view||n->floating||n->x||n->y||n->w!=640||n->h!=480||n->rotate_ms||n->tile_cols" in clear and 'return g_surface_opaque[n->image];' in clear and 'g_cold_fast_jpeg_pending=true;return true;' in clear
    jpeg=image[image.index('if(im->data_length>=16&&packed[2]==\'J\'){'):image.index('const uint8_t *palette=')]
    assert jpeg.count('if(g_cold_fast_jpeg_pending){g_cold_fast_jpeg_pending=false;if(!(ok&&w==640&&h==480)&&g_app_text_surface)fill_rect(0,0,640,480,g_app_text_surface->background);}')==1
    assert 'g_surface_opaque[i]=cold_fast_palette_opaque(palette)||cold_fast_indices_opaque(g_surface_indices+g_surface_offsets[i],pixels,g_surface_alpha[i]);' in text
    render=text[text.index('static bool render_package_app('):]
    assert render.count('if(g_cold_fast_jpeg_pending&&n->type!=6){g_cold_fast_jpeg_pending=false;g_app_clip.active=false;fill_rect(0,0,640,480,surface->background);if(viewport){g_app_clip.active=true;}}')==1
    assert render.index('if(cold_fast_opaque_first_image(surface,app->surface_page))pet_render_counter_increment(&g_cold_fast_clear_skips);else')<render.index('app_clear_uncovered(surface,app->surface_page,cache_frame);}')
    for key in ('"coldFast"','"imageNodes"','"panelNodes"','"clearSkips"'):assert key in diag
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_cold_fast_parent':
            assert config[section]['extends']=='env:esp32_p4_large_reply_parent' and config[section]['custom_p4_project_version']=='0.7.103-p4-r141-coldfast'
            assert 'PET_P4_APP_COLD_FAST=1' in flags and not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert 'PET_P4_APP_COLD_FAST=1' not in flags
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_render_lifetime.h"\nint main(void){return PET_P4_APP_COLD_FAST;}\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_APP_COLD_FAST=1','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert result.returncode!=0 and 'Cold APP fast paths require' in result.stderr
    assert subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True).returncode==0
