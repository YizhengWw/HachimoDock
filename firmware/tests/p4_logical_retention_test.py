"""Exact final-frame ownership; actual app raster, common plans and failures."""
from pathlib import Path
import subprocess
import p4_app_dirty_cache_test as dirty
ROOT=Path(__file__).resolve().parents[1]

def particle_active_macro():
    """Use both production branches without importing renderer/RTOS state."""
    source=(ROOT/'main/pet_p4_capability_draw.inc').read_text()
    branches=[line for line in source.splitlines() if line.startswith('#define PET_PARTICLES_ACTIVE(')]
    assert len(branches)==2
    return '#if PET_P4_PARTICLES\n'+branches[0]+'\n#else\n'+branches[1]+'\n#endif\n'


def test_common_plan_and_successful_owner_lifecycle(tmp_path):
    source=tmp_path/'plan.c';binary=tmp_path/'plan'
    source.write_text(r'''
#include "pet_p4_logical_retention.h"
#include <assert.h>
int main(void){
 for(unsigned bits=0;bits<256;bits++){
  bool state=bits&1,touch=bits&2,connected=bits&4,assets=bits&8,id=bits&16,mounted=bits&32,seen=bits&64,boot=bits&128;
  pet_logical_common_plan_t p=pet_logical_common_plan(state,touch,-123,901,connected,assets,id,mounted,seen,boot);
  unsigned banner=0;if(state){if(connected){if(!assets)banner=1;}else if(id)banner=2;else if(mounted||seen)banner=3;else banner=4;}
  assert(p.banner==banner&&p.touch==(state&&touch)&&p.boot==boot);
  assert(p.touch?(p.touch_x==-123&&p.touch_y==901):(!p.touch_x&&!p.touch_y));
  assert(pet_logical_common_empty(p)==(!p.banner&&!p.touch&&!p.boot));
  pet_logical_retention_t s={0};assert(!pet_logical_retention_begin(&s,p,true,true,true));
  pet_logical_retention_finish(&s,p,true);assert(pet_logical_retention_begin(&s,p,true,true,true));
  assert(!pet_logical_retention_begin(&s,p,true,true,true)); /* prior owner consumed */
  for(unsigned bad=0;bad<3;bad++){
   pet_logical_retention_finish(&s,p,true);
   assert(!pet_logical_retention_begin(&s,p,bad!=0,bad!=1,bad!=2));
  }
  pet_logical_retention_finish(&s,p,false);assert(!pet_logical_retention_begin(&s,p,true,true,true));
  pet_logical_common_plan_t q=p;q.banner=(p.banner+1)%5;
  pet_logical_retention_finish(&s,p,true);assert(!pet_logical_retention_begin(&s,q,true,true,true));
  q=p;q.boot=!q.boot;assert(!pet_logical_common_equal(p,q));
  q=p;q.touch=!q.touch;assert(!pet_logical_common_equal(p,q));
  q=p;q.touch_x++;assert(pet_logical_common_equal(p,q)==!p.touch);
 }
 return 0;
}
''')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_retained_app_full_pixels_overlay_changes_modal_and_resource_invalidation(tmp_path,monkeypatch):
    original=dirty.run
    def retained(path,code):
        code=particle_active_macro()+code
        code='#define PET_P4_PARTICLES 0\n#define PET_P4_APP_RETAIN_LOGICAL 1\n#define PET_P4_LOGICAL_COMPARE 1\n#define PET_P4_APP_CACHE_PROGRESSIVE 1\n#include "pet_p4_logical_retention.h"\n'+code
        marker='static bool g_surface_background_valid,reference_valid;'
        assert code.count(marker)==1
        code=code.replace(marker,marker+r'''
static pet_logical_retention_t retained_owner;
static pet_logical_common_plan_t g_app_common_plan;
static bool g_app_may_retain,g_app_final_reused,g_app_final_eligible,g_app_logical_same,previous_valid;
static struct {bool logical_retained;}g_render_trace;
static uint16_t previous_pixels[640*480];
static unsigned retained_frames;
''')
        # The baseline MUST stay a real full raster, not retain from our buffer.
        a=code.index('static bool reference_render(');b=code.index('static bool render_package_app(',a)
        reference=code[a:b].replace('PET_P4_APP_RETAIN_LOGICAL','0').replace('PET_P4_LOGICAL_COMPARE','0')
        code=code[:a]+reference+code[b:]
        # Keep the complete unbounded reference for every-pixel correctness,
        # AND a separate real R125 progressive control for the work assertion.
        # Comparing work to a one-frame full cache would compare two budgets.
        a=code.index('static bool render_package_app(');b=code.index('static uint32_t random_state',a)
        control=code[a:b].replace('render_package_app(','progressive_control_render(').replace('PET_P4_APP_RETAIN_LOGICAL','0')
        for old,new in [('g_surface_background_valid','control_valid'),('g_surface_background_page','control_page'),
                        ('g_surface_background','control_background'),('g_app_frame_key','control_key'),
                        ('g_app_background_revision','control_revision'),('g_app_cache_progressive','control_progressive')]:
            control=control.replace(old,new)
        declarations='''static uint16_t control_background[640*480];
static bool control_valid;static uint8_t control_page;static uint32_t control_revision;
static pet_app_frame_key_t control_key;static pet_app_cache_progressive_t control_progressive;
'''
        code=code[:a]+declarations+control+code[a:]
        a=code.index('static void compare(');b=code.index('\nint main(void)',a)
        code=code[:a]+r'''
/* Alpha overlay deliberately exposes any repeated blending/darkening. Real
 * glyph/rounded-panel raster and the actual app/modal loop remain unmodified. */
static void common(pet_logical_common_plan_t p){
 if(p.banner)blend_rect(398,18,222,32,p.banner*31,77,128,115);
 if(p.touch)fill_ellipse(p.touch_x,p.touch_y,13,13,0xfedc);
 if(p.boot)fill_rect(0,0,640,6,0xffff);
}
static void compare(pet_p4_miniapp_view_t *app,unsigned frame){
 unsigned group=frame/8;
 g_app_common_plan=pet_logical_common_plan(true,group%3==0,40+group%500,60+group%350,
  group%5==0,group%2==0,group%5==1,group%5==2,group%5==3,group%7==0);
 for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
 labels=0;assert(reference_render(frame*37,app));common(g_app_common_plan);memcpy(expected,pixels,sizeof(pixels));
 if(frame%41==40){ /* same actual resource invalidation for BOTH budgets */
  g_surface_revision++;g_surface_background_valid=g_app_frame_key.valid=false;
  control_valid=control_key.valid=false;
  pet_app_cache_progressive_cancel(&g_app_cache_progressive);pet_app_cache_progressive_cancel(&control_progressive);
 }
 for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
 labels=0;assert(progressive_control_render(frame*37,app));full_labels+=labels;common(g_app_common_plan);
 assert(!memcmp(expected,pixels,sizeof(pixels))); /* old budget is correct too */
 if(previous_valid)memcpy(pixels,previous_pixels,sizeof(pixels));
 else for(unsigned p=0;p<640*480;p++)pixels[p]=(uint16_t)(p*977U+frame*131U);
 g_app_may_retain=pet_logical_retention_begin(&retained_owner,g_app_common_plan,previous_valid,true,true);
 g_app_final_reused=g_app_final_eligible=false;g_render_trace.logical_retained=false;
 labels=0;assert(render_package_app(frame*37,app));partial_labels+=labels;
 assert(g_app_final_reused==g_render_trace.logical_retained);
 if(g_app_final_reused){retained_frames++;assert(!labels&&!g_miniapp_render_snapshot.confirmation_active);}
 else common(g_app_common_plan);
 if(memcmp(expected,pixels,sizeof(pixels))){fprintf(stderr,"retained/full mismatch frame=%u\n",frame);abort();}
 /* An actual failed rotation/LCD must discard authority even if pixels match. */
 previous_valid=frame%47!=46;
 pet_logical_retention_finish(&retained_owner,g_app_common_plan,previous_valid&&g_app_final_eligible);
 memcpy(previous_pixels,pixels,sizeof(pixels));
}
'''+code[b:]
        marker='strcpy(app.metric_value,"123");compare(&app,0);'
        assert code.count(marker)==1
        code=code.replace(marker,marker+'\n for(unsigned n=1;n<32;n++)compare(&app,n);\n assert(retained_frames>0);')
        original(path,code)
    monkeypatch.setattr(dirty,'run',retained)
    dirty.test_actual_dirty_renderer_matches_full_pixels_with_order_clips_forms_and_overlays(tmp_path)

def test_retention_is_private_and_all_new_work_stays_inside_original_frame_clock():
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    render=text[text.index('esp_err_t pet_p4_renderer_render('):text.index('esp_err_t pet_p4_renderer_render_transfer_status(')]
    assert render.index('int64_t render_started_us = esp_timer_get_time();')<render.index('g_app_common_plan=pet_logical_common_plan(')
    assert render.index('g_app_common_plan=pet_logical_common_plan(')<render.index('render_miniapp_page(')
    assert render.index('pet_logical_retention_finish(')<render.index('int64_t render_ended_us = esp_timer_get_time();')
    assert 'pet_p4_logical_frame_unchanged(g_framebuffer,previous_native' in render
    assert 'if(!g_app_final_reused)' in render and 'g_app_may_retain=false' in render
    assert 'g_app_final_reused=true;g_render_trace.logical_retained=true' in text
    assert 'pet_logical_common_empty(g_app_common_plan)' in text

def test_actual_common_overlay_commands_match_old_path_and_use_captured_state(tmp_path):
    from p4_render_trace_test import function
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    code=r'''
#include "pet_p4_logical_retention.h"
#include <assert.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
#define P4_BOOT_DIAGNOSTIC_MS 10000
typedef struct {int local_touch_x,local_touch_y;unsigned asset_family_count;char desktop_device_id[8];bool native_usb_mounted,touch,connected;unsigned long long host_last_seen_ms;}pet_p4_runtime_state_t;
static bool pet_p4_state_touch_feedback_active(const pet_p4_runtime_state_t*s,unsigned long long t){(void)t;return s&&s->touch;}
static bool pet_p4_state_host_connected(const pet_p4_runtime_state_t*s,unsigned long long t){(void)t;return s&&s->connected;}
typedef struct {int op,x,y,w,h,extra;uint16_t color;}command_t;
static command_t commands[64],expected[64];static unsigned count;
static uint16_t rgb565(unsigned r,unsigned g,unsigned b){return((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void add(int op,int x,int y,int w,int h,int extra,uint16_t c){assert(count<64);commands[count++]=(command_t){.op=op,.x=x,.y=y,.w=w,.h=h,.extra=extra,.color=c};}
static void fill_rect(int x,int y,int w,int h,uint16_t c){add(1,x,y,w,h,0,c);}
static void fill_round_rect(int x,int y,int w,int h,int r,uint16_t c){add(2,x,y,w,h,r,c);}
static void fill_ellipse(int x,int y,int w,int h,uint16_t c){add(3,x,y,w,h,0,c);}
static int utf8_text_width(const char*t,int max,int scale){(void)max;return(int)strlen(t)*scale*7;}
static void draw_text_line_vcenter(const char*t,int x,int y,int h,int w,uint16_t c,int scale,bool last){
 unsigned hash=0;for(const unsigned char*p=(const unsigned char*)t;*p;p++)hash=hash*31+*p;
 add(4,x,y,w,h,(int)((hash&0xffff)^scale^(last<<20)),c);
}
'''
    for name in ('draw_touch_feedback_pixels','draw_touch_feedback','draw_connection_banner_pixels','draw_connection_banner','draw_common_connection','draw_boot_diagnostic'):
        code+=function(text,name)+'\n'
    code+=r'''
int main(void){
 for(unsigned bits=0;bits<256;bits++)for(unsigned present=0;present<2;present++){
  pet_p4_runtime_state_t s={.local_touch_x=-123,.local_touch_y=901,.touch=!!(bits&1),.connected=!!(bits&2),.asset_family_count=!!(bits&4),.native_usb_mounted=!!(bits&16),.host_last_seen_ms=!!(bits&32)};
  if(bits&8)strcpy(s.desktop_device_id,"owned");unsigned long long now=bits&64?10001:10000;
  pet_p4_runtime_state_t *state=present?&s:NULL;
  pet_logical_common_plan_t p=pet_logical_common_plan(state!=NULL,
   pet_p4_state_touch_feedback_active(state,now),state?state->local_touch_x:0,state?state->local_touch_y:0,
   state&&pet_p4_state_host_connected(state,now),state&&state->asset_family_count>0,state&&state->desktop_device_id[0],
   state&&state->native_usb_mounted,state&&state->host_last_seen_ms>0,now<=P4_BOOT_DIAGNOSTIC_MS);
  memset(commands,0,sizeof(commands));count=0;
  draw_touch_feedback(state,now);draw_connection_banner(state,now);draw_boot_diagnostic(now);
  unsigned wanted=count;memcpy(expected,commands,sizeof(commands));
  /* Mutating the ambient state cannot change the already captured plan. */
  memset(&s,0,sizeof(s));s.connected=true;s.asset_family_count=77;
  memset(commands,0,sizeof(commands));count=0;
  if(p.touch)draw_touch_feedback_pixels(p.touch_x,p.touch_y);
  draw_common_connection(p.banner);if(p.boot)draw_boot_diagnostic(now);
  assert(count==wanted);
  for(unsigned i=0;i<count;i++){
   command_t a=expected[i],b=commands[i];
   assert(a.op==b.op&&a.x==b.x&&a.y==b.y&&a.w==b.w&&a.h==b.h&&a.extra==b.extra&&a.color==b.color);
  }
 }
 return 0;
}
'''
    source=tmp_path/'actual-common.c';binary=tmp_path/'actual-common';source.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_production_particle_overlay_disables_final_frame_retention(tmp_path):
    # Execute the actual eligibility expression against real surface layouts in
    # both builds; background caching is allowed, final overlay retention is not.
    renderer=(ROOT/'main/pet_p4_renderer.c').read_text()
    assignments=[line.strip() for line in renderer.splitlines()
                 if line.strip().startswith('g_app_final_eligible=cache_frame')]
    assert len(assignments)==1
    for enabled in (0,1):
        source=tmp_path/f'particle-retention-{enabled}.c'
        binary=source.with_suffix('')
        source.write_text('#include "pet_p4_surface.h"\n#include <assert.h>\n'+particle_active_macro()+r'''
int main(void){
 pet_surface_t storage={0};const pet_surface_t *surface=&storage;
 struct {bool confirmation_active;}g_miniapp_render_snapshot={0};
 bool g_app_final_eligible;
 for(unsigned cache=0;cache<2;cache++)for(unsigned modal=0;modal<2;modal++)for(unsigned dots=0;dots<3;dots++){
  bool cache_frame=cache;g_miniapp_render_snapshot.confirmation_active=modal;
#if PET_P4_PARTICLES
  storage.particle_count=dots;
#endif
'''+assignments[0]+r'''
  assert(g_app_final_eligible==(cache&&!modal&&(!PET_P4_PARTICLES||!dots)));
 }
 return 0;
}
''')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        f'-DPET_P4_PARTICLES={enabled}',f'-DPET_P4_GRAPHICS_V2={enabled}','-I',str(ROOT/'main'),
                        '-I',str(ROOT/'tests/vendor/cjson'),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)
