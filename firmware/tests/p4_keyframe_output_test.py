"""A failed worker decode cannot leave an old ready tag on overwritten pixels."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_keyframe_output_is_unpublished_before_decode_and_on_failure(tmp_path):
    source = (ROOT / "main/pet_p4_capability_draw.inc").read_text()
    renderer = (ROOT / "main/pet_p4_renderer.c").read_text()
    code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_prefetch_lifetime.h"
#define PET_P4_H264_PREFETCH 1
#define PET_P4_KEYFRAME_BG 1
static pet_prefetch_lifetime_t g_h264_prefetch_lifetime;
static bool g_h264_program_window;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
typedef int esp_err_t;
typedef int JRESULT;
static int g_asset_prepare_mux;
static uint8_t input[32],work[32];
static uint16_t output[640*480];
static uint8_t *g_keyframe_input=input,*g_keyframe_tjpgd_workbuf=work;
static size_t g_keyframe_input_capacity=sizeof(input),g_keyframe_output_capacity=sizeof(output);
static uint16_t *g_keyframe_output=output;
static bool g_keyframe_output_dma=true;
static void *g_jpeg_decoder=(void*)1;
static uint32_t g_keyframe_worker_decodes;
static struct {int8_t frame,ready;bool working;uint32_t revision,ready_revision;size_t size;} g_keyframe;
static unsigned hardware_calls,fallback_calls;
static bool success;
static void pet_render_counter_increment(uint32_t *v){++*v;}
static bool decode_jpeg_frame_hardware_into(const uint8_t *in,size_t n,uint16_t *out,size_t cap,int *w,int *h,int *stride,esp_err_t *err){
 (void)in;(void)n;(void)cap;(void)err;
 assert(g_keyframe.working&&g_keyframe.ready==-1&&g_keyframe.ready_revision==0);
 hardware_calls++;out[0]=0x2222;*w=640;*h=480;*stride=640;return success;
}
static bool decode_jpeg_frame_tjpgd_into(const uint8_t *in,size_t n,uint16_t *out,size_t pixels,uint8_t *workbuf,int *w,int *h,JRESULT *err){
 (void)in;(void)n;(void)pixels;(void)workbuf;(void)w;(void)h;(void)err;
 assert(g_keyframe.working&&g_keyframe.ready==-1&&g_keyframe.ready_revision==0);
 fallback_calls++;out[1]=0x3333;return false;
}
""" + function(source, "keyframe_working_locked") + function(renderer, "pet_p4_renderer_program_window_begin") + function(source, "keyframe_worker") + r"""
static void prepare(void){
 g_keyframe.frame=2;g_keyframe.ready=1;g_keyframe.working=false;
 g_keyframe.revision=g_keyframe.ready_revision=17;g_keyframe.size=24;
 output[0]=output[1]=0x1111;
}
int main(void){
 prepare();success=false;assert(keyframe_worker());
 assert(hardware_calls==1&&fallback_calls==1&&output[0]==0x2222&&output[1]==0x3333);
 assert(g_keyframe.ready==-1&&g_keyframe.ready_revision==0&&!g_keyframe.working&&g_keyframe.frame==-1);
 assert(!g_keyframe_worker_decodes);
 prepare();success=true;assert(keyframe_worker());
 assert(g_keyframe.ready==2&&g_keyframe.ready_revision==17&&!g_keyframe.working&&g_keyframe.frame==-1);
 assert(g_keyframe_worker_decodes==1);
 /* A busy worker is not a claim and cannot retire a live descriptor. */
 prepare();g_keyframe.working=true;assert(!keyframe_worker());assert(g_keyframe.ready==1&&g_keyframe.ready_revision==17);
 prepare();g_keyframe.size=12;assert(keyframe_worker());assert(g_keyframe.ready==-1);
 /* PROGRAM and keyframe work use the same mux admission in both directions. */
 prepare();assert(pet_p4_renderer_program_window_begin());assert(g_h264_program_window);
 assert(!keyframe_worker());assert(g_keyframe.frame==2&&g_keyframe.ready==1);
 g_h264_program_window=false;assert(keyframe_worker());
 prepare();g_keyframe.working=true;assert(!pet_p4_renderer_program_window_begin());assert(!g_h264_program_window);
 g_keyframe.working=false;g_h264_prefetch_lifetime.phase=PET_PREFETCH_WORKING;
 assert(!pet_p4_renderer_program_window_begin());
 return 0;
}
"""
    src = tmp_path / "keyframe.c"
    exe = tmp_path / "keyframe"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(ROOT / "main"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_keyframe_swap_retires_partial_cache_even_when_surface_key_is_identical(tmp_path):
    source=(ROOT/"main/pet_p4_capability_draw.inc").read_text()
    renderer=(ROOT/"main/pet_p4_renderer.c").read_text()
    code=r"""
#include <assert.h>
#include "pet_p4_app_cache_progressive.h"
#include "pet_p4_image.h"
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static int g_asset_prepare_mux;
static bool g_surface_background_valid;
static uint32_t g_surface_revision=17,g_keyframe_swaps;
static pet_app_frame_key_t g_app_frame_key;
static pet_app_cache_progressive_t g_app_cache_progressive;
static struct{int frame,ready;bool working;uint32_t ready_revision,shown_revision,revision;uint8_t shown;uint64_t swapped_ms;size_t size;}g_keyframe;
typedef struct{unsigned data_length,data_offset;}pet_p4_miniapp_sprite_t;
static struct{uint8_t pixels[32];}g_surface_images;
static uint8_t input[32],*g_keyframe_input=input;
static size_t g_keyframe_input_capacity=32;
static int g_asset_prepare_task;
static bool pet_p4_renderer_keyframe_ready(void){return true;}
static const pet_p4_miniapp_sprite_t *keyframe_item(const pet_surface_t*s,unsigned want){(void)s;(void)want;return NULL;}
static void xTaskNotifyGive(int task){(void)task;}
static void pet_render_counter_increment(uint32_t*v){++*v;}
"""+function(renderer,"keyframe_invalidate_frame_cache")+function(source,"keyframe_advance")+r"""
int main(void){
 pet_surface_t s={.app=true,.graphics=true,.keyframes=2,.keyframe_fps=1};const char *values[9]={0};
 pet_app_frame_stage(&g_app_frame_key,&s,values,0,0);
 g_app_cache_progressive.active=true;g_app_cache_progressive.revision=17;g_app_cache_progressive.count=64;
 g_app_cache_progressive.rows[0]=UINT32_MAX;g_app_cache_progressive.rows[1]=UINT32_MAX;
 assert(pet_app_frame_content_matches(&g_app_frame_key,&s,values,0,0));
 g_keyframe.shown_revision=17;g_keyframe.shown=0;g_keyframe.frame=-1;g_keyframe.ready=1;g_keyframe.ready_revision=17;
 keyframe_advance(&s,1000);
 assert(g_keyframe.shown==1&&g_keyframe_swaps==1);
 assert(!g_surface_background_valid&&!g_app_frame_key.valid&&!g_app_cache_progressive.active&&!g_app_cache_progressive.count);
 /* Surface values still match: only explicit row retirement can prevent reuse. */
 assert(pet_app_frame_content_matches(&g_app_frame_key,&s,values,0,0));
 return 0;
}
"""
    src=tmp_path/"cache.c";exe=tmp_path/"cache";src.write_text(code)
    flags=["-DPET_P4_KEYFRAME_BG=1","-DPET_P4_H264_PREFETCH=1","-DPET_P4_GRAPHICS_V2=1","-DPET_P4_APP_DIRTY_CACHE=1","-DPET_P4_APP_CACHE_PROGRESSIVE=1"]
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-misleading-indentation","-fsanitize=address,undefined",*flags,"-I",str(ROOT/"main"),"-I",str(ROOT/"tests/vendor/cjson"),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
