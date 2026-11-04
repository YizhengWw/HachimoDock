"""R130 private exact frame paths: same pixels as the unchanged renderer paths."""
from pathlib import Path
import configparser
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]

GLYPH_HARNESS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "pet_p4_rgb565_blend.h"
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t a[640*480+2],b[640*480+2],*g_framebuffer;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];
static float g_glyph_rows[2][66];
static int32_t g_glyph_sample_y;
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
static float glyph_alpha_at(const uint8_t*p,const lv_font_glyph_dsc_t*g,int x,int y){(void)p;(void)g;return (x+y)%7<5?0:(x*53+y*29)%256;}
static const pet_surface_font_t *app_font_glyph(uint32_t cp,int*index){
 static const pet_surface_font_t f={32,24};*index=3;return cp==1?&f:NULL;
}
static float app_font_sample(const pet_surface_font_t*f,int index,int x,int y){(void)index;return x<0||y<0||x>=f->cell_w||y>=f->cell_h?0:((x+y)%7<5?0:(x*43+y*71)%256);}
static int pet_app_symbol_advance(uint32_t cp){return cp==2?13:cp==0x2103?16:0;}
static float pet_app_symbol_coverage(uint32_t cp,float x,float y,float scale){(void)cp;(void)scale;return x>=1&&y>=2&&x<10&&y<13?.67f:0;}
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
typedef struct {int x0,x1,y0,y1;} glyph_pixel_bounds_t;
'''

GLYPH_MAIN=r'''
static uint32_t state=7919;
static unsigned random_word(void){state=state*1664525U+1013904223U;return state;}
int main(void){
 const uint32_t cps[]={0,1,2,23,0x2103,0x4e2d,0xfe0f,0x200d,1,1};
 unsigned stores=0;
 for(unsigned n=0;n<600;n++){
  for(unsigned i=0;i<640*480+2;i++)a[i]=b[i]=(uint16_t)(i*977+n*317);
  /* Node clips partly outside the framebuffer, viewport clips of every shape. */
  pet_surface_node_t clip={.x=(int)(random_word()%760)-60,.y=(int)(random_word()%560)-40,.w=(int)(random_word()%300)+1,.h=(int)(random_word()%260)+1,.weight=n%3==0};
  g_app_clip.active=n%4!=0;g_app_clip.x=(int)(random_word()%700)-30;g_app_clip.y=(int)(random_word()%520)-20;
  g_app_clip.right=g_app_clip.x+(int)(random_word()%700);g_app_clip.bottom=g_app_clip.y+(int)(random_word()%520);
  float x=(int)(random_word()%760)-80+(random_word()%8)*.125f,y=(int)(random_word()%560)-60+(random_word()%6)*.1666667f;
  int size=8+(int)(random_word()%89);uint32_t cp=cps[n%10];uint16_t color=(uint16_t)random_word();
  releases=0;g_framebuffer=a+1;reference_glyph(cp,65,x,y,size,color,&clip);unsigned before=releases;
  releases=0;g_framebuffer=b+1;draw_app_glyph(cp,65,x,y,size,color,&clip);
  assert(releases==before);assert(!memcmp(a,b,sizeof(a)));
  for(unsigned i=0;i<640*480+2;i++)if(b[i]!=(uint16_t)(i*977+n*317))stores++;
 }
 assert(stores>1000);
 g_framebuffer=NULL;draw_app_glyph(1,65,10,10,30,0xffff,&(pet_surface_node_t){0,0,640,480,0});
 draw_app_glyph(0x4e2d,65,10,10,30,0xffff,&(pet_surface_node_t){0,0,640,480,0});
 return 0;
}
'''


def _glyph_code(wide):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=GLYPH_HARNESS
    if wide:
        code=code.replace('.box_w=7+cp%17','.box_w=7+cp%97').replace('f={32,24}','f={32,96}')
    for name in ('blend_rgb565','app_clip_contains','blend_px','app_glyph_sample','app_prepare_glyph_rows',
                 'app_glyph_row_alpha','glyph_pixel_bounds','glyph_pixel_store'):
        code+=function(text,name)+'\n'
    for fast in (0,1):
        code+=f'\n#undef PET_P4_GLYPH_PIXEL_FAST\n#define PET_P4_GLYPH_PIXEL_FAST {fast}\n'
        glyph=function(text,'draw_app_glyph')
        if not fast:glyph=glyph.replace('draw_app_glyph(','reference_glyph(')
        code+=glyph+'\n'
    return code+GLYPH_MAIN


def test_glyph_pixel_store_path_is_bit_exact_for_fonts_symbols_lvgl_clips_and_contraction(tmp_path):
    for contract,sanitized in (('off',True),('fast',False),('fast',True)):
        for wide in (False,True):
            src=tmp_path/f'glyph-{contract}-{sanitized}-{wide}.c';exe=src.with_suffix('')
            src.write_text(_glyph_code(wide))
            subprocess.run(['cc','-O2',f'-ffp-contract={contract}','-std=c11','-Wall','-Wextra','-Werror',
              *(['-fsanitize=address,undefined'] if sanitized else []),
              '-DPET_P4_RASTER_EXACT=1','-DPET_P4_ALPHA_FAST=1','-DPET_P4_GLYPH_ROWS=1','-DPET_P4_GLYPH_AXIS_INTERNAL=1',
              '-I',str(ROOT/'main'),str(src),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=120)


CLEAR_CODE=r'''
#include "pet_p4_app_dirty.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t pixels[640*480],expected[640*480],*g_framebuffer=pixels;
static struct {bool active;int x,y,right,bottom;}g_app_clip;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static uint32_t g_clear_center_frames,g_clear_full_frames;
static void pet_render_counter_increment(uint32_t *value){if(*value!=UINT32_MAX)(*value)++;}
'''

CLEAR_MAIN=r'''
static uint32_t random_state=0x5eed1230;
static uint32_t random_word(void){random_state=random_state*1664525U+1013904223U;return random_state;}
/* The renderer's own order: first visible projected layer, panels drawn as the real
 * node loop draws them. Text/image/icon nodes are drawn AFTER these in both paths,
 * so they cannot change the equivalence of the states compared here. */
static void draw_panels(const pet_surface_t *s,uint8_t page){
 for(unsigned i=0;i<s->count;i++){
  const pet_surface_node_t *source=&s->nodes[i];
  if(source->hidden||(source->page!=4&&source->page!=page))continue;
  const pet_surface_viewport_t *v=pet_surface_node_viewport(s,source);
  pet_surface_node_t n;
  if(!pet_surface_project(v,source,v?v->offset:0,&n))continue;
  g_app_clip.active=false;
  if(v){g_app_clip.active=true;g_app_clip.x=v->x;g_app_clip.y=v->y;g_app_clip.right=v->x+v->w;g_app_clip.bottom=v->y+v->h;}
  if(n.type==0||n.type==8)draw_app_panel(&n);
  g_app_clip.active=false;
 }
}
int main(void){
 unsigned skipped=0,full=0,expected_center=0,expected_full=0;
 for(unsigned frame=0;frame<400;frame++){
  pet_surface_t s;memset(&s,0,sizeof(s));
  s.app=true;s.graphics=frame%23!=0;s.background=(uint16_t)random_word();s.count=1+random_word()%8;
  s.font_count=frame%3?1+random_word()%2:0;s.image_count=frame%5?random_word()%3:0;
  uint8_t page=random_word()%2;
  for(unsigned i=0;i<s.count;i++){
   pet_surface_node_t *n=&s.nodes[i];memset(n,0,sizeof(*n));
   n->type=i?(uint8_t)(random_word()%9):(uint8_t)((random_word()%4)?0:8);
   if(n->type==3||n->type==4||n->type==5)n->type=1;
   n->page=random_word()%3?4:(uint8_t)(random_word()%2);
   n->x=(int16_t)((int)(random_word()%760)-60);n->y=(int16_t)((int)(random_word()%560)-40);
   n->w=(uint16_t)(random_word()%660);n->h=(uint16_t)(random_word()%500);
   n->radius=(uint8_t)(random_word()%48);n->color=(uint16_t)random_word();n->icon=-1;
   n->opacity=n->opacity_end=frame%7?255:(uint8_t)random_word();
   if(frame%11==0)n->stroke_width=(uint8_t)(random_word()%4);
   n->hidden=random_word()%9==0;n->floating=frame%29==0&&i==1;n->vertical=random_word()%2;
   n->font_size=22;n->line_height=28;n->bind=255;
  }
  if(frame%6==0){
   s.viewport=(pet_surface_viewport_t){.enabled=true,.x=(uint16_t)(random_word()%500),.y=(uint16_t)(random_word()%400),.w=(uint16_t)(40+random_word()%400),.h=(uint16_t)(40+random_word()%300),.content_height=900,.offset=(int32_t)(random_word()%300)};
   s.nodes[0].in_view=random_word()%2;
  }
  bool cache_frame=frame%13!=0;
  {pet_app_dirty_rect_t probe;if(cache_frame&&pet_app_clear_center_supported(&s)&&pet_app_opaque_center(&s,page,&probe))expected_center++;else expected_full++;}
  for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
  reference_clear(&s,page,cache_frame);memcpy(expected,pixels,sizeof(pixels));
  for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
  app_clear_uncovered(&s,page,cache_frame);
  bool differs=memcmp(expected,pixels,sizeof(pixels))!=0;if(differs)skipped++;else full++;
  draw_panels(&s,page);
  uint16_t after_candidate[640*480];memcpy(after_candidate,pixels,sizeof(pixels));
  memcpy(pixels,expected,sizeof(pixels));draw_panels(&s,page);
  if(memcmp(after_candidate,pixels,sizeof(pixels))){fprintf(stderr,"clear mismatch frame=%u\n",frame);abort();}
 }
 assert(skipped>40&&full>40);
 /* The candidate's own counters match the header predicate on every frame (the
  * reference also skips the center for font-free surfaces, so pixel difference
  * alone undercounts). */
 assert(g_clear_center_frames==expected_center&&g_clear_full_frames==expected_full&&expected_center>=skipped);
 return 0;
}
'''


def test_opaque_center_clear_with_fonts_images_matches_full_clear_after_first_layer(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=CLEAR_CODE
    for n in ('blend_rgb565','app_clip_contains','put_px','fill_rect','blend_px','draw_app_panel'):
        code+=function(text,n)+'\n'
    clear=function(text,'app_clear_uncovered')
    code+='\n#undef PET_P4_APP_CLEAR_OPAQUE_CENTER\n#define PET_P4_APP_CLEAR_OPAQUE_CENTER 0\n'+clear.replace('app_clear_uncovered(','reference_clear(')
    code+='\n#undef PET_P4_APP_CLEAR_OPAQUE_CENTER\n#define PET_P4_APP_CLEAR_OPAQUE_CENTER 1\n'+clear+CLEAR_MAIN
    for mirror in (0,1):
        src=tmp_path/f'clear-{mirror}.c';exe=src.with_suffix('');src.write_text(code)
        subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
          '-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined',
          '-DPET_P4_APP_DIRTY_CACHE=1',f'-DPET_P4_APP_PANEL_MIRROR={mirror}','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),
          str(src),str(ROOT/'main/pet_p4_surface.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-lm','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=120)


def test_frame_io_overlap_evidence_splits_quiet_and_overlapped_peaks(tmp_path):
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_render_lifetime.h"
#include "pet_p4_render_trace.h"
int main(void){
 pet_render_lifetime_t life;memset(&life,0,sizeof(life));
 /* Quiet main-page boot frame: counts for quietPeak but never for quietAppPeak. */
 pet_render_overlap_record(&life,false,false,61000,1,0,13000,12000,0,0,4700,61000>life.max_us);pet_render_lifetime_record(&life,61000,0,1,1,100,61100,66667,100000);
 pet_render_overlap_record(&life,false,true,10000,2,5000,2000,1000,500,4000,7000,10000>life.max_us);pet_render_lifetime_record(&life,10000,0,2,2,200,10200,66667,100000);
 pet_render_overlap_record(&life,true,true,50000,3,30000,12000,6000,6000,25000,40000,50000>life.max_us);pet_render_lifetime_record(&life,50000,0,3,2,300,50300,66667,100000);
 assert(life.overlap_frames==1&&!life.peak_overlapped&&life.max_us==61000);
 assert(life.quiet_peak.max_us==61000&&life.quiet_peak.frame==1);
 assert(life.quiet_app_peak.max_us==10000&&life.quiet_app_peak.frame==2&&life.quiet_app_peak.app_draw_us==5000);
 pet_render_overlap_record(&life,false,true,70000,4,60000,12000,6000,6000,54000,62000,70000>life.max_us);pet_render_lifetime_record(&life,70000,0,4,2,400,70400,66667,100000);
 assert(life.quiet_peak.max_us==70000&&life.quiet_app_peak.max_us==70000&&life.quiet_app_peak.nodes_us==54000);
 assert(life.overlap_peak.max_us==50000&&life.overlap_peak.frame==3&&life.overlap_peak.clear_us==6000);
 pet_render_overlap_record(&life,true,true,90000,5,70000,12000,6000,6000,60000,80000,90000>life.max_us);pet_render_lifetime_record(&life,90000,0,5,2,500,90500,66667,100000);
 assert(life.overlap_frames==2&&life.peak_overlapped&&life.overlap_peak.max_us==90000&&life.quiet_peak.max_us==70000&&life.quiet_app_peak.max_us==70000);
 /* A lower overlapped frame never replaces any peak. */
 pet_render_overlap_record(&life,true,true,20000,6,1,1,1,1,1,1,false);
 assert(life.overlap_peak.max_us==90000&&life.overlap_frames==3&&life.quiet_app_peak.max_us==70000);
 pet_p4_render_trace_t trace;pet_p4_render_trace_begin(&trace,9,"app");
 pet_widget_trace_snapshot_t snap;memset(&snap,0,sizeof(snap));
 snap.lanes[0].last_completed_seq=4;snap.lanes[1].last_completed_seq=7;snap.lanes[0].seq=5;
 pet_p4_render_trace_observe(&trace,&snap,false);
 snap.lanes[0].last_completed_seq=5;snap.lanes[0].active=1;
 pet_p4_render_trace_observe(&trace,&snap,true);
 assert(trace.completed_before[0]==4&&trace.completed_before[1]==7&&trace.completed_after[0]==5&&trace.completed_after[1]==7);
 assert(((trace.operation_after[0].phase>>16)&1U)==1U&&((trace.operation_before[0].phase>>16)&1U)==0U);
 return 0;
}
'''
    src=tmp_path/'overlap.c';exe=src.with_suffix('');src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_FRAME_IO_OVERLAP=1','-DPET_P4_RETAIN_PEAK_PHASES=1','-DPET_P4_APP_PHASE_TRACE=1','-DPET_P4_APP_NODE_TRACE=1',
      '-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=60)


def test_frame_exact_parent_keeps_gates_defaults_and_dependency_errors(tmp_path):
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    private=('PET_P4_APP_CLEAR_OPAQUE_CENTER=1','PET_P4_GLYPH_PIXEL_FAST=1','PET_P4_FRAME_IO_OVERLAP=1')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_frame_exact_parent':
            assert config[section]['extends']=='env:esp32_p4_glyph_axis_parent'
            assert config[section]['custom_p4_project_version']=='0.7.103-p4-r130-exact'
            assert all(v in flags for v in private)
            assert not any(v in flags for v in ('TASK_PRIORITY','PROGRAM_EXECUTION','F_CPU','PERIPHERAL','BUDGET_US','WATCHDOG','AFFINITY'))
        else:assert not any(v in flags for v in private)
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert '#define PET_P4_RENDER_FRAME_BUDGET_US 66667U' in text
    assert 'static DRAM_ATTR pet_raster_axis_t g_glyph_axis[PET_RASTER_AXIS_TILE];' in text
    assert text.count('static EXT_RAM_BSS_ATTR PET_SURFACE_BACKGROUND_ALIGN uint16_t g_surface_background[640*480];')==1  # R143: alignment macro (empty unless PET_P4_APP_PPA_COPY)
    assert 'glyph_atlas' not in text and 'g_glyph_cache' not in text
    for define,header,message in (
        ('-DPET_P4_GLYPH_PIXEL_FAST=1','pet_p4_rgb565_blend.h','Glyph pixel store path requires'),
        ('-DPET_P4_APP_CLEAR_OPAQUE_CENTER=1','pet_p4_render_policy.h','Opaque-center clear requires'),
        ('-DPET_P4_FRAME_IO_OVERLAP=1','pet_p4_render_lifetime.h','Frame/IO overlap evidence requires')):
        src=tmp_path/'guard.c';src.write_text(f'#include "{header}"\nint main(void){{return 0;}}\n')
        result=subprocess.run(['cc','-std=c11','-fsyntax-only',define,'-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
        assert result.returncode!=0 and message in result.stderr,(define,result.stderr)
        result=subprocess.run(['cc','-std=c11','-fsyntax-only','-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
        assert result.returncode==0,result.stderr
