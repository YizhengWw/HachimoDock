"""Private exact dirty cache: real renderer versus its unchanged full path."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]


def run(tmp_path,code):
    for copy_words in (False,True):_run_copy_mode(tmp_path,code,copy_words)

def _run_copy_mode(tmp_path,code,copy_words):
    src=tmp_path/'dirty.c';binary=tmp_path/'dirty';src.write_text(code)
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
      '-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined',
      '-DPET_P4_APP_DIRTY_CACHE=1','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),
      f'-DPET_P4_APP_COPY_WORDS={int(copy_words)}',
      str(src),str(ROOT/'main/pet_p4_surface.c'),str(ROOT/'tests/vendor/cjson/cJSON.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)


def test_actual_dirty_renderer_matches_full_pixels_with_order_clips_forms_and_overlays(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    start=text.index('static bool render_package_app(')
    render=text[start:text.index('static bool render_package_media(')]
    helpers=text[text.index('#if PET_P4_APP_DIRTY_CACHE',text.index('static EXT_RAM_BSS_ATTR pet_app_frame_key_t g_app_frame_key;')):start]
    trace=text[text.index('#if PET_P4_APP_NODE_TRACE\n_Static_assert(PET_SURFACE_NODES'):text.index('static void record_render_performance(')]
    code=r'''
#include "pet_p4_app_dirty.h"
#include "pet_p4_render_trace.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define PET_P4_GAME_NONE 0
typedef struct {int kind;} game_t;
typedef struct {
 char title[64],eyebrow[96],headline[160],metric_label[96],metric_value[64],metric_unit[32],badge[16],note[160],footer[160],data_source[48];
 uint8_t surface_page;int progress_percent;bool slicing,media,instrument;game_t game;struct{bool enabled;}data;
} pet_p4_miniapp_view_t;
static struct {pet_surface_t surface;bool surface_active,side_active,confirmation_active;char confirmation_prompt[121];}g_miniapp_render_snapshot;
static uint16_t pixels[640*480],expected[640*480],*g_framebuffer=pixels;
static uint16_t g_surface_background[640*480],reference_background[640*480];
static bool g_surface_background_valid,reference_valid;
static uint8_t g_surface_background_page,reference_page;
static uint32_t g_surface_revision;
static pet_app_frame_key_t g_app_frame_key,reference_key;
static const pet_surface_t *g_app_text_surface;
static struct {bool active;int x,y,right,bottom;}g_app_clip;
static unsigned labels,full_labels,partial_labels;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void load_surface_images(unsigned long long now){}
static void draw_app_game(const pet_surface_t*s,const game_t*g){}
static void draw_app_scene(const pet_surface_t*s,const game_t*g,unsigned long long now){}
static void draw_app_data(const pet_surface_t*s,const pet_p4_miniapp_view_t*a){}
static void draw_app_side(void){}
#define draw_app_canvas(surface,page) ((void)(surface),(void)(page))
static void draw_surface_image(const pet_surface_node_t*n){}
static void draw_app_icon(int icon,int x,int y,int size,uint16_t color);
static const char*draw_text_line(const char*t,int x,int y,int w,uint16_t c,int scale,bool last){return t+strlen(t);}
static int app_glyph_width(uint32_t cp,uint32_t next){return cp>127?32:12+cp%9;}
static void app_text_bounds(const char*text,int*top,int*bottom){*top=1;*bottom=29;}
'''
    for n in ('blend_rgb565','app_clip_contains','put_px','fill_rect','fill_ellipse','blend_px','blend_rect','blend_round_rect','fill_round_rect','draw_app_panel'):
        code+=function(text,n)+'\n'
    code+=r'''
/* Deterministic glyph coverage goes through the REAL clip/alpha store. Native
 * and atlas bitmap sampling have their separate exhaustive production tests. */
static void draw_app_glyph(uint32_t cp,uint32_t next,float xx,float yy,int px,uint16_t color,const pet_surface_node_t*n){
 for(int y=(int)floorf(yy);y<(int)ceilf(yy+px);y++)for(int x=(int)floorf(xx);x<(int)ceilf(xx+px*.6f);x++){
  if(x<n->x||y<n->y||x>=n->x+n->w||y>=n->y+n->h)continue;
  unsigned a=(cp*17U+(unsigned)x*31U+(unsigned)y*13U)&255U;
  blend_px(x,y,((color>>11)&31)*255/31,((color>>5)&63)*255/63,(color&31)*255/31,a);
 }
}
static void draw_app_icon(int icon,int x,int y,int size,uint16_t color){fill_rect(x,y,size,size,color);}
'''
    for n in ('utf8_next','utf8_peek','app_text_width'):
        code+=function(text,n)+'\n'
    code+=function(text,'draw_app_label').replace('int count=0,px=', 'labels++;int count=0,px=')+'\n'
    code+=trace+helpers
    reference=render.replace('PET_P4_APP_DIRTY_CACHE','0').replace('PET_P4_APP_CACHE_POPULATION_BUDGET','0').replace('PET_P4_APP_CACHE_PROGRESSIVE','0').replace('render_package_app(','reference_render(')
    for a,b in [('g_surface_background_page','reference_page'),('g_surface_background_valid','reference_valid'),('g_surface_background','reference_background'),('g_app_frame_key','reference_key')]:reference=reference.replace(a,b)
    code+=reference+render+r'''
static uint32_t random_state=0x119ca543;
static uint32_t random_word(void){random_state=random_state*1664525U+1013904223U;return random_state;}
static void compare(pet_p4_miniapp_view_t *app,unsigned frame){
 for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
 labels=0;assert(reference_render(frame*37,app));full_labels+=labels;memcpy(expected,pixels,sizeof(pixels));
 for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
 labels=0;assert(render_package_app(frame*37,app));partial_labels+=labels;
 if(memcmp(expected,pixels,sizeof(pixels))){fprintf(stderr,"dirty/full mismatch frame=%u\n",frame);abort();}
 /* Simulate a moving connection/touch overlay AFTER the clean frame cache. */
 fill_rect((frame*17)%600,(frame*11)%440,32,32,0xaaaa);
}
int main(void){
 pet_surface_t *s=&g_miniapp_render_snapshot.surface;g_miniapp_render_snapshot.surface_active=true;
 s->app=s->graphics=true;s->background=0x1203;s->count=6;
 for(unsigned i=0;i<s->count;i++)s->nodes[i]=(pet_surface_node_t){.type=1,.page=4,.x=40,.y=40+(int)i*48,.w=220,.h=40,.bind=255,.icon=-1,.font_size=22,.line_height=28,.color=0xffff};
 s->nodes[0]=(pet_surface_node_t){.type=0,.page=4,.x=20,.y=20,.w=600,.h=400,.radius=24,.opacity=255,.opacity_end=255,.color=0x1458,.icon=-1};
 strcpy(s->nodes[1].text,"Fixed heading");s->nodes[2].bind=4;
 s->nodes[3].type=8;s->nodes[3].opacity=s->nodes[3].opacity_end=255;s->nodes[3].radius=12;s->nodes[3].foreground=0xffe0;
 strcpy(s->nodes[3].text,"Accept");s->nodes[4].type=2;s->nodes[4].h=9;s->nodes[4].color=0x07e0;
 /* An overlapping translucent panel MUST replay above changed text. */
 s->nodes[5]=(pet_surface_node_t){.type=0,.page=4,.x=150,.y=105,.w=180,.h=100,.radius=18,.opacity=63,.opacity_end=191,.color=0x891f,.icon=-1};
 pet_p4_miniapp_view_t app={0};strcpy(app.metric_value,"123");compare(&app,0);
 for(unsigned frame=1;frame<=240;frame++){
  switch(frame%16){
   case 0:snprintf(app.metric_value,sizeof(app.metric_value),"%u",random_word()%100000);break;
   case 1:s->nodes[3].pressed=!s->nodes[3].pressed;break;
   case 2:s->nodes[3].disabled=!s->nodes[3].disabled;break;
   case 3:s->nodes[2].hidden=!s->nodes[2].hidden;break;
   case 4:s->nodes[2].x=30+random_word()%300;break;
   case 5:s->nodes[2].w=40+random_word()%250;break;
   case 6:s->nodes[2].color=(uint16_t)random_word();break;
   case 7:app.progress_percent=random_word()%101;break;
   case 8:s->viewport=(pet_surface_viewport_t){.enabled=true,.x=24,.y=64,.w=420,.h=250,.content_height=800,.offset=(int)(random_word()%200)};s->nodes[2].in_view=true;break;
   case 9:s->viewport.offset=random_word()%400;break;
   case 10:s->nodes[3].hidden=!s->nodes[3].hidden;break;
   case 11:s->nodes[5].opacity=(uint8_t)random_word();break;
   case 12:g_miniapp_render_snapshot.confirmation_active=!g_miniapp_render_snapshot.confirmation_active;strcpy(g_miniapp_render_snapshot.confirmation_prompt,"Confirm now?");break;
   case 13:app.surface_page^=1;break;
   case 14:s->background=(uint16_t)random_word();break;
   case 15:break; /* exact cache hit with native overlay residue */
  }
  compare(&app,frame);
 }
 assert(partial_labels<full_labels);
 /* Unknown/unsupported modes must keep full repaint, not silently omit UI. */
 s->nodes[3].icon=1;compare(&app,241);s->nodes[3].icon=-1;
 s->graphics=false;compare(&app,242);s->graphics=true;
 s->image_count=1;compare(&app,243);s->image_count=0;
 s->font_count=1;compare(&app,244);s->font_count=0;
 return 0;
}
'''
    run(tmp_path,code)


def test_optional_cache_deferral_preserves_actual_pixels_and_later_population(tmp_path,monkeypatch):
    original=run
    def budgeted(path,code):
        marker='#include "pet_p4_app_dirty.h"'
        assert code.count(marker)==2 # harness + real renderer's guarded include
        code=code.replace(marker,'#define PET_P4_APP_CACHE_POPULATION_BUDGET 1\n'+marker+r'''
static int64_t budget_start,budget_end;static unsigned budget_calls;
static int64_t esp_timer_get_time(void){return budget_calls++?budget_end:budget_start;}
''',1)
        marker=' labels=0;assert(render_package_app(frame*37,app));'
        assert code.count(marker)==1
        code=code.replace(marker,r'''
 budget_calls=0;budget_start=100;
 budget_end=frame%4==0?28101:frame%4==1?100:frame%4==2?-1:99;
'''+marker)
        marker='strcpy(app.metric_value,"123");compare(&app,0);'
        assert code.count(marker)==1
        code=code.replace(marker,marker+r'''
 assert(!g_app_frame_key.valid&&reference_key.valid);
 compare(&app,1);assert(g_app_frame_key.valid&&g_surface_background_valid);
''')
        original(path,code)
    monkeypatch.setattr(__import__(__name__,fromlist=['run']),'run',budgeted)
    test_actual_dirty_renderer_matches_full_pixels_with_order_clips_forms_and_overlays(tmp_path)


def test_empty_resource_revisions_cannot_reuse_foreign_dynamic_prefix(tmp_path,monkeypatch):
    original=run
    def revised(path,code):
        marker=' assert(partial_labels<full_labels);'
        assert code.count(marker)==1
        vector=r'''
 reference_key.valid=g_app_frame_key.valid=false;
 reference_valid=g_surface_background_valid=false;
 s->nodes[1].floating=true;s->nodes[0].color=0x001f;
 compare(&app,245);
 s->nodes[0].color=0xf800;g_surface_revision++;reference_valid=false;
 compare(&app,246);
 /* Same revision may reuse this prefix; another component/revision cannot. */
 compare(&app,247);
 s->nodes[0].color=0x07e0;g_surface_revision++;reference_valid=false;
 compare(&app,248);
'''
        original(path,code.replace(marker,marker+vector))
    monkeypatch.setattr(__import__(__name__,fromlist=['run']),'run',revised)
    test_actual_dirty_renderer_matches_full_pixels_with_order_clips_forms_and_overlays(tmp_path)


def test_first_opaque_center_preserves_all_pixels_and_falls_back_for_unknowns(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include "pet_p4_app_dirty.h"
#include <assert.h>
#include <math.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t pixels[640*480],expected[640*480],*g_framebuffer=pixels;
static struct {bool active;int x,y,right,bottom;}g_app_clip;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
'''
    for n in ('blend_rgb565','app_clip_contains','put_px','fill_rect','blend_px','draw_app_panel','app_clear_uncovered'):
        code+=function(text,n)+'\n'
    code+=r'''
static void panel(const pet_surface_t*s,uint8_t page){
 const pet_surface_node_t *source=&s->nodes[0];pet_surface_node_t n;
 const pet_surface_viewport_t*v=pet_surface_node_viewport(s,source);
 if(source->hidden||(source->page!=4&&source->page!=page)||!pet_surface_project(v,source,v?v->offset:0,&n))return;
 if(n.type!=0&&(n.type!=8||!s->graphics))return;
 if(v){g_app_clip.active=true;g_app_clip.x=v->x;g_app_clip.y=v->y;g_app_clip.right=v->x+v->w;g_app_clip.bottom=v->y+v->h;}
 draw_app_panel(&n);g_app_clip.active=false;
}
int main(void){
 for(unsigned i=0;i<360;i++){
  pet_surface_t s={.app=true,.graphics=true,.background=(uint16_t)(i*1777),.count=1};
  pet_surface_node_t*n=&s.nodes[0];
  *n=(pet_surface_node_t){.x=(int)(i*37%800)-100,.y=(int)(i*23%640)-80,.w=1+i*29%640,.h=1+i*11%480,.type=i%2?0:8,.page=4,.radius=i%25,.opacity=255,.opacity_end=255,.color=(uint16_t)(i*31337),.icon=-1};
  switch(i%9){case 1:n->opacity=127;break;case 2:n->opacity_end=31;break;case 3:n->stroke_width=2;n->stroke=0xf800;break;case 4:n->hidden=true;break;case 5:n->page=1;break;case 6:n->type=1;break;case 7:s.graphics=false;break;default:break;}
  if(i%3){s.viewport=(pet_surface_viewport_t){.enabled=true,.x=40,.y=64,.w=500,.h=300,.content_height=800,.offset=(int)i%230};n->in_view=true;}
  for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*31337+i);
  fill_rect(0,0,640,480,s.background);panel(&s,0);memcpy(expected,pixels,sizeof(pixels));
  for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*31337+i);
  app_clear_uncovered(&s,0,true);panel(&s,0);assert(!memcmp(expected,pixels,sizeof(pixels)));
 }
 return 0;
}
'''
    run(tmp_path,code)


def test_empty_images_revision_never_invalidates_pixels_but_nonempty_still_does(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp.c').read_text()
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct{unsigned count,data_length;unsigned items[4];uint8_t pixels[32];}pet_p4_miniapp_sprite_pack_t;
static uint32_t g_image_revision;
static struct{pet_p4_miniapp_sprite_pack_t sprites;}g_runtime;
'''+function(text,'surface_images_locked')+r'''
int main(void){
 pet_p4_miniapp_sprite_pack_t out={0};uint32_t revision=0;out.items[0]=17;
 g_image_revision=1;assert(!surface_images_locked(&out,&revision));assert(revision==1&&out.items[0]==0);
 g_runtime.sprites.count=1;g_runtime.sprites.data_length=3;g_runtime.sprites.pixels[0]=99;g_image_revision++;
 assert(surface_images_locked(&out,&revision)&&out.count==1&&out.pixels[0]==99);
 out.pixels[0]=13;assert(!surface_images_locked(&out,&revision)&&out.pixels[0]==13);
 g_runtime.sprites.count=0;g_runtime.sprites.data_length=0;g_image_revision++;
 assert(surface_images_locked(&out,&revision)&&!out.count&&!out.data_length);
 g_image_revision++;assert(!surface_images_locked(&out,&revision)&&revision==4);
 return 0;
}
'''
    run(tmp_path,code)


def test_dirty_candidate_has_no_public_execution_or_new_buffers():
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    for section in c.sections():
        flags=c[section].get('build_flags','')
        if section=='env:esp32_p4_dirty_parent':
            assert c[section]['extends']=='env:esp32_p4_span_target_parent'
            assert 'PET_P4_APP_DIRTY_CACHE=1' in flags and 'PET_P4_SPARE_UPDATE=1' in flags
            assert not any(x in flags for x in ('TASK_PRIORITY','F_CPU','PROGRAM_EXECUTION','PERIPHERAL'))
        else:assert 'PET_P4_APP_DIRTY_CACHE=1' not in flags and 'PET_P4_SPARE_UPDATE=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert text.count('uint16_t g_surface_background[640*480]')==1
    media=function(text,'render_package_media');assert 'g_app_frame_key.valid=false;' in media
    header=(ROOT/'main/pet_p4_app_cache.h').read_text();assert '#define PET_P4_APP_DIRTY_CACHE 0' in header
