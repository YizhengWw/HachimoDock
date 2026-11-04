"""Exact logical/reference pixels and ACTUAL renderer-tail buffer ownership."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]

def run(tmp_path,code):
    src=tmp_path/'logical.c';binary=tmp_path/'logical';src.write_text(code)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)

def test_logical_comparison_exact_rotation_all_small_pixels_and_bounded_large_samples(tmp_path):
    run(tmp_path,r'''
#include "pet_p4_frame_gate.h"
#include <assert.h>
static uint16_t a[640*480+8],b[640*480+8];
static void check(unsigned w,unsigned h){
 uint16_t *logical=a+4,*panel=b+4;
 for(unsigned i=0;i<640*480+8;i++)a[i]=b[i]=(uint16_t)(i*31337U+197U);
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)panel[x*h+h-1-y]=logical[y*w+x];
 assert(pet_p4_logical_frame_unchanged(logical,panel,w,h));
 unsigned step=w*h>2000?997:1;
 for(unsigned i=0;i<w*h;i+=step){panel[i]^=0x5a5a;assert(!pet_p4_logical_frame_unchanged(logical,panel,w,h));panel[i]^=0x5a5a;}
 panel[w*h-1]^=0xffff;assert(!pet_p4_logical_frame_unchanged(logical,panel,w,h));panel[w*h-1]^=0xffff;
 assert(pet_p4_logical_frame_unchanged(logical,panel,w,h));
 for(unsigned i=0;i<4;i++){assert(a[i]==(uint16_t)(i*31337U+197U));assert(b[i]==a[i]);}
}
int main(void){
 for(unsigned w=2;w<=36;w+=2)for(unsigned h=2;h<=30;h+=2)check(w,h);
 check(640,480);check(638,478);
 assert(!pet_p4_logical_frame_unchanged(NULL,b,640,480));
 assert(!pet_p4_logical_frame_unchanged(a,NULL,640,480));
 assert(!pet_p4_logical_frame_unchanged(a,a,640,480));
 assert(!pet_p4_logical_frame_unchanged(a,a+2,640,480));
 assert(!pet_p4_logical_frame_unchanged(a+1,b,640,480));
 assert(!pet_p4_logical_frame_unchanged(a,b,639,480));
 assert(!pet_p4_logical_frame_unchanged(a,b,640,479));
 assert(!pet_p4_logical_frame_unchanged(a,b,642,480));
 assert(!pet_p4_logical_frame_unchanged(a,b,640,482));
 assert(!pet_p4_logical_frame_unchanged(a,b,0,480));return 0;
}
''')

def test_actual_renderer_tail_retains_index_pointer_reference_failure_and_transfer_invalidation(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text();render=function(text,'pet_p4_renderer_render')
    prelude=render[render.index('  const uint16_t *previous_native'):render.index('  int64_t render_started_us')]
    tail=render[render.index('  int64_t rotate_started_us'):render.index('  int64_t render_ended_us')]
    code=r'''
#define PET_P4_LOGICAL_COMPARE 1
#define PET_P4_APP_CACHE_PROGRESSIVE 1
#define PET_P4_RENDER_LCD_SPLIT_PROBE 1
#include "pet_p4_frame_gate.h"
#include "pet_p4_lcd_profile.h"
#include <assert.h>
#define PET_P4_UI_WIDTH 4
#define PET_P4_UI_HEIGHT 4
#define BSP_LCD_H_RES 4
#define BSP_LCD_V_RES 4
#define PET_P4_NATIVE_OUTPUT_BUFFER_COUNT 2
#define ESP_OK 0
#define ESP_RETURN_ON_ERROR(value,tag,message) do{int error=(value);if(error)return error;}while(0)
typedef int esp_err_t;
static uint16_t logical[16],one[16],two[16],*g_framebuffer=logical,*g_native_framebuffers[2]={one,two},*g_native_framebuffer=one;
static unsigned g_native_framebuffer_index,g_lcd_skipped,g_lcd_submitted,rotations,lcd_calls;
static bool g_last_native_valid,g_app_logical_same,g_last_compare_valid;
static uint32_t g_last_compare_us;
static pet_lcd_observation_t g_last_lcd_observation;
static struct {uint32_t logical_compare_us,common_overlay_us;}g_render_trace;
static int64_t now;static int rotate_error,lcd_error;
static int64_t esp_timer_get_time(void){return ++now;}
static uint32_t elapsed_us_clamped(int64_t a,int64_t b){assert(b>=a);return (uint32_t)(b-a);}
static esp_err_t rotate_landscape_to_panel(void){
 rotations++;if(rotate_error)return rotate_error;
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)g_native_framebuffer[x*4+3-y]=logical[y*4+x];
 return 0;
}
static esp_err_t pet_p4_lcd_draw_rgb565_observed(int x,int y,int w,int h,const void *p,pet_lcd_observation_t *out){
 assert(!x&&!y&&w==4&&h==4&&p==g_native_framebuffer);lcd_calls++;
 *out=(pet_lcd_observation_t){.valid=true,.submitted=!lcd_error,.refresh_waited=!lcd_error};return lcd_error;
}
static esp_err_t draw(bool hint){
'''+prelude+r'''
 g_app_logical_same=hint;int64_t common_overlay_started_us=esp_timer_get_time(),overlay_started_us=common_overlay_started_us;
'''+tail+r'''
 (void)overlay_us;(void)rotate_us;return lcd_err;
}
int main(void){
 for(unsigned i=0;i<16;i++)logical[i]=(uint16_t)(i*197+17);
 assert(draw(true)==0&&rotations==1&&lcd_calls==1&&g_last_native_valid);
 unsigned ref=g_native_framebuffer_index;uint16_t *pixels=g_native_framebuffer;
 for(unsigned n=0;n<8;n++){assert(draw(true)==0);assert(rotations==1&&lcd_calls==1&&g_native_framebuffer_index==ref&&g_native_framebuffer==pixels);}
 for(unsigned i=0;i<16;i++){logical[i]^=0x55aa;unsigned r=rotations,l=lcd_calls;assert(draw(true)==0&&rotations==r+1&&lcd_calls==l+1);}
 lcd_error=-3;logical[15]^=123;assert(draw(true)==-3&&!g_last_native_valid);lcd_error=0;
 unsigned r=rotations,l=lcd_calls;assert(draw(true)==0&&rotations==r+1&&lcd_calls==l+1&&g_last_native_valid);
 rotate_error=-7;logical[0]^=1;assert(draw(true)==-7&&!g_last_native_valid);rotate_error=0;
 r=rotations;l=lcd_calls;assert(draw(true)==0&&rotations==r+1&&lcd_calls==l+1);
 /* Transfer renders invalidate the normal frame and choose another output. */
 g_last_native_valid=false;g_native_framebuffer_index=(g_native_framebuffer_index+1)%2;g_native_framebuffer=g_native_framebuffers[g_native_framebuffer_index];
 memset(g_native_framebuffer,0x7f,32);r=rotations;l=lcd_calls;
 assert(draw(true)==0&&rotations==r+1&&lcd_calls==l+1);assert(g_lcd_skipped==8&&g_lcd_submitted==lcd_calls-1);
 return 0;
}
'''
    run(tmp_path,code)

def test_private_policy_keeps_defaults_closed_and_requires_staged_key_parent(tmp_path):
    src=tmp_path/'policy.c';binary=tmp_path/'policy.o'
    src.write_text('#include "pet_p4_frame_gate.h"\n#include "pet_p4_save_policy.h"\n'
        '_Static_assert(!PET_P4_LOGICAL_COMPARE&&!PET_P4_APP_PANEL_MIRROR&&!PET_P4_SAVE_TRACE_DETACHED&&!PET_P4_APP_RETAIN_LOGICAL,"default off");\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(src),'-o',str(binary)],check=True)
    src.write_text('#include "pet_p4_frame_gate.h"\n#include "pet_p4_save_policy.h"\n')
    for flags,message in [(['-DPET_P4_LOGICAL_COMPARE=1'],'Logical comparison requires'),
                          (['-DPET_P4_APP_RETAIN_LOGICAL=1'],'Final logical retention requires'),
                          (['-DPET_P4_SAVE_TRACE_DETACHED=1'],'Detached SAVE trace requires')]:
        r=subprocess.run(['cc','-std=c11','-fsyntax-only','-I',str(ROOT/'main'),*flags,str(src)],text=True,capture_output=True)
        assert r.returncode!=0 and message in r.stderr
