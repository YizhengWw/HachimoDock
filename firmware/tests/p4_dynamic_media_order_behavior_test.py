"""Run the complete production APP draw dispatcher with observable leaf draws."""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def order_binary(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("order")
    production = function((ROOT / "main/pet_p4_renderer.c").read_text(), "render_package_app")
    code = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_surface.h"
/* Only the dispatcher is compiled here: these enable its branches, without
 * importing native decoder/PPA owners. No SDK/driver implementation is run. */
#undef PET_P4_CLIP
#define PET_P4_CLIP 1
#define PET_SURFACE_NODE_CLIP 20
#define PET_P4_GAME_NONE 0
#define APP_NODE_SCOPE(...) ((void)0)
#define APP_NODE_HIT() ((void)0)
#define APP_NODE_MARK(...) ((void)0)
typedef struct {
 const char *title,*eyebrow,*headline,*metric_label,*metric_value,*metric_unit,*badge,*note,*footer;
 bool slicing,media,instrument;struct{int kind;}game;struct{bool enabled;}data;char data_source[8];unsigned surface_page;int progress_percent;
}pet_p4_miniapp_view_t;
static struct {bool surface_active,side_active,confirmation_active;const char *confirmation_prompt;pet_surface_t surface;}g_miniapp_render_snapshot;
static const pet_surface_t *g_app_text_surface;
static bool g_surface_background_valid;static unsigned g_surface_background_page;
static uint16_t framebuffer[2],background[2],*g_framebuffer=framebuffer;
#define g_surface_background background
static struct{bool valid;}g_app_frame_key;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static unsigned copies,eligible_calls,match_calls;static bool canvas_enabled;
static char draws[64];static unsigned draw_count;
static void mark(char ch){assert(draw_count+1<sizeof(draws));draws[draw_count++]=ch;draws[draw_count]=0;}
static void load_surface_images(uint64_t n){(void)n;}
static bool pet_app_frame_eligible_at(const pet_surface_t*s,unsigned p){(void)s;(void)p;eligible_calls++;return true;}
static bool pet_app_frame_matches(const void*k,const pet_surface_t*s,const char**v,unsigned p,int n){(void)k;(void)s;(void)v;(void)p;(void)n;match_calls++;return true;}
static void pet_app_frame_remember(void*k,const pet_surface_t*s,const char**v,unsigned p,int n){(void)k;(void)s;(void)v;(void)p;(void)n;}
static void pet_app_frame_copy(void*d,const void*s,size_t n){copies++;memcpy(d,s,n);}
static void fill_rect(int x,int y,int w,int h,uint16_t c){(void)x;(void)y;(void)w;(void)h;framebuffer[0]=framebuffer[1]=c;}
static void draw_surface_image(const pet_surface_node_t*n){if(n->big>=0){mark('I');framebuffer[1]=3;}else{mark('B');framebuffer[0]=framebuffer[1]=1;}}
static void renderer_ext_draw_clip(const pet_surface_node_t*n,uint64_t t){(void)n;(void)t;mark('C');framebuffer[1]=2;}
static void draw_app_canvas(const pet_surface_t*s,unsigned p){(void)s;(void)p;if(canvas_enabled){mark('V');framebuffer[0]=4;}}
#define draw_app_game(...) ((void)0)
#define draw_app_scene(...) ((void)0)
#define draw_app_data(...) ((void)0)
#define draw_app_side(...) ((void)0)
const pet_surface_viewport_t*pet_surface_node_viewport(const pet_surface_t*s,const pet_surface_node_t*n){(void)s;(void)n;return NULL;}
bool pet_surface_project(const pet_surface_viewport_t*v,const pet_surface_node_t*n,int32_t o,pet_surface_node_t*out){(void)v;(void)o;*out=*n;return true;}
#define draw_app_panel(...) ((void)0)
#define draw_app_icon(...) ((void)0)
#define draw_app_label(n,t,c) ((void)(t))
#define blend_round_rect(...) ((void)0)
#define blend_rect(...) ((void)0)
#define fill_round_rect(...) ((void)0)
#define rgb565(...) 0
static const char*draw_text_line(const char*t,int x,int y,int w,uint16_t c,int s,bool last){(void)t;(void)x;(void)y;(void)w;(void)c;(void)s;(void)last;return NULL;}
''' + production + r'''
int main(int argc,char**argv){
 assert(argc==3);bool big=atoi(argv[1]);canvas_enabled=atoi(argv[2]);
 pet_surface_t*s=&g_miniapp_render_snapshot.surface;s->app=true;s->graphics=true;s->count=2;
 s->nodes[0]=(pet_surface_node_t){.type=6,.image=0,.big=-1,.w=640,.h=480};
 s->nodes[1]=(pet_surface_node_t){.type=big?6:PET_SURFACE_NODE_CLIP,.image=-1,.big=big?0:-1,.x=20,.y=20,.w=160,.h=120};
 g_miniapp_render_snapshot.surface_active=true;
 g_miniapp_render_snapshot.confirmation_prompt="";
 pet_p4_miniapp_view_t app={.title="",.eyebrow="",.headline="",.metric_label="",.metric_value="",.metric_unit="",.badge="",.note="",.footer=""};
 /* A stale prefix exists, so removing the dynamic-media reuse guard really
  * changes observed pixels/copies rather than merely a source assertion. */
 g_surface_background_valid=true;background[0]=background[1]=9;g_app_frame_key.valid=true;
 for(int frame=0;frame<2;frame++){
  draw_count=0;draws[0]=0;framebuffer[0]=framebuffer[1]=0;
  assert(render_package_app(1000+frame*100,&app));
  assert(!strcmp(draws,big?(canvas_enabled?"BVI":"BI"):(canvas_enabled?"BVC":"BC")));
  assert(framebuffer[0]==(canvas_enabled?4:1));assert(framebuffer[1]==(big?3:2));
  assert(!copies&&!eligible_calls&&!match_calls&&!g_app_clip.active);
 }
 return 0;
}
'''
    src, binary = tmp / "order.c", tmp / "order"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                    "-Wno-unused-but-set-variable", "-fsanitize=address,undefined", "-DPET_P4_BIG_IMAGE=1",
                    "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/vendor/cjson"),
                    str(src), "-lm", "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("big", [0, 1], ids=["clip", "big"])
@pytest.mark.parametrize("canvas", [0, 1], ids=["without-canvas", "with-canvas"])
def test_background_then_canvas_then_dynamic_media_without_cache_reuse(order_binary, big, canvas):
    subprocess.run([str(order_binary), str(big), str(canvas)], check=True)
