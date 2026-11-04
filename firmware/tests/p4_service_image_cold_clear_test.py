"""Real service raster/JPEG fallback dispatch must consume cold pending clears.

Only host image-lease entry points and JPEG codec results are doubled. The
parser, service pixel loop, complete fallback image function and cold helpers
come from production. No board, driver or font renderer is required.
"""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from p4_render_trace_test import function


ROOT = Path(__file__).resolve().parents[1]


def host_code(renderer):
    render = function(renderer, "render_package_app")
    start = render.index("    if(n->type==6){")
    end = render.index("\n#if PET_P4_CLIP", start)
    image_dispatch = render[start:end]
    guard = next(line.strip() for line in render.splitlines()
                 if line.strip().startswith("if(g_cold_fast_jpeg_pending&&n->type!=6)"))
    fixture = {
        "engine": "p4-app-surface-v1", "graphics": "p4-app-graphics-v1",
        "bindings": "p4-variable-bindings-v1", "services": "p4-service-bridge-v1",
        "background": 123,
        "images": [{"id": "background", "asset": "assets/background.png",
                    "frame_width": 640, "frame_height": 480,
                    "frames": 1, "fps": 1, "encoding": "jpeg"}],
        "elements": [
            {"type": "image", "x": 0, "y": 0, "w": 640, "h": 480,
             "color": 0, "image": "background", "image_bind": "cover"},
            {"type": "text", "x": 25, "y": 25, "w": 80, "h": 30,
             "color": 0, "text": "Label"},
        ],
    }
    code = r'''
#include "pet_p4_surface.h"
#include "pet_p4_image.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
typedef int esp_err_t;
typedef int JRESULT;
static uint16_t guarded[640*480+2],*g_framebuffer=guarded+1;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static bool g_cold_fast_jpeg_pending,g_surface_opaque[1];
static uint32_t g_cold_fast_image_nodes,g_surface_offsets[1];
static uint64_t g_surface_play_ms;
typedef struct{uint8_t fps;uint16_t frame_width,frame_height;uint32_t data_offset,data_length;}pet_p4_miniapp_sprite_t;
static struct{uint8_t count;pet_p4_miniapp_sprite_t items[1];uint8_t pixels[39];}g_surface_images;
static uint16_t g_surface_colors[1][256];static uint8_t g_surface_alpha[1][256],g_surface_indices[640*480];
static const pet_surface_t *g_app_text_surface;
static uint8_t input[64],*g_jpeg_input=input;static size_t g_jpeg_input_capacity=sizeof(input);
static uint16_t jpeg_output[656*480],*g_jpeg_output=jpeg_output;
static uint16_t service_pixels[7*5];
static unsigned mode,frame,borrows,releases,hardware_calls,software_calls;
static bool borrowed;
static uint16_t expected_pixel(unsigned x,unsigned y){
 if(mode==0)return (uint16_t)(0x1234U+frame*79U+(y*5U/480U)*7U+(x*7U/640U)*37U);
 return (uint16_t)(0x3467U+frame*83U+y*13U+x*31U);
}
static void assert_complete_pixels(void){
 assert(guarded[0]==0xa55a&&guarded[640*480+1]==0x5aa5);
 for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++)
   assert(g_framebuffer[y*640+x]==(mode==3?g_app_text_surface->background:expected_pixel(x,y)));
}
static void pet_render_counter_increment(uint32_t *v){++*v;}
/* External host boundary: return a small nonuniform image to exercise the
 * production nearest-neighbour scaling, rather than painting the frame here. */
static bool pet_app_host_image_borrow(const char *id,const char *alias,uint64_t now,pet_bridge_image_lease_t *out){
 assert(!strcmp(id,"widget")&&!strcmp(alias,"cover")&&now==1000U+frame);
 assert(!borrowed&&g_cold_fast_jpeg_pending);borrows++;
 if(mode!=0)return false;
 for(unsigned y=0;y<5;y++)for(unsigned x=0;x<7;x++)
   service_pixels[y*7+x]=(uint16_t)(0x1234U+frame*79U+y*7U+x*37U);
 *out=(pet_bridge_image_lease_t){.valid=true,.token=borrows,.width=7,.height=5,.pixels=service_pixels};
 borrowed=true;return true;
}
static void pet_app_host_image_release(pet_bridge_image_lease_t *lease){
 assert(borrowed&&lease->valid&&lease->token==borrows&&lease->pixels==service_pixels);
 /* The source must stay leased through the LAST pixel. Pending consumption
  * belongs after this release, not before borrow or halfway through raster. */
 assert(g_cold_fast_jpeg_pending);assert_complete_pixels();
 releases++;borrowed=false;lease->valid=false;lease->pixels=NULL;
}
/* Codec boundaries only. Failed hardware writes partial output to ensure the
 * real software/error fallback, not old decoder pixels, decides the frame. */
static bool decode_jpeg_frame_hardware(const uint8_t *bytes,size_t size,int *w,int *h,int *stride,esp_err_t *error){
 assert(mode!=0&&bytes==g_jpeg_input&&size==27&&g_cold_fast_jpeg_pending);
 hardware_calls++;*error=0;*w=640;*h=480;*stride=656;
 jpeg_output[0]=0xdead;
 if(mode!=1)return false;
 for(unsigned y=0;y<480;y++)for(unsigned x=0;x<656;x++)
   jpeg_output[y*656+x]=x<640?expected_pixel(x,y):0xbeef;
 return true;
}
static bool decode_jpeg_frame_tjpgd(const uint8_t *bytes,size_t size,int *w,int *h,JRESULT *error){
 assert(mode>=2&&bytes==g_jpeg_input&&size==27&&g_cold_fast_jpeg_pending);
 software_calls++;*error=0;*w=640;*h=480;jpeg_output[1]=0xf00d;
 if(mode!=2)return false;
 for(unsigned y=0;y<480;y++)for(unsigned x=0;x<640;x++)jpeg_output[y*640+x]=expected_pixel(x,y);
 return true;
}
'''
    for name in ("rgb565", "app_clip_contains", "fill_rect", "cold_fast_image_span",
                 "draw_service_image", "draw_surface_image", "cold_fast_opaque_first_image"):
        code += function(renderer, name) + "\n"
    code += r'''
static void draw_first(const pet_surface_t *surface){
 const pet_surface_node_t *n=&surface->nodes[0];
 struct{char widget_id[8];}app_value={"widget"},*app=&app_value;
 unsigned long long now_ms=1000U+frame;
''' + image_dispatch + r'''
}
static void before_next_label(const pet_surface_t *surface){
 const pet_surface_node_t *n=&surface->nodes[1];const pet_surface_viewport_t *viewport=NULL;
 assert(n->type==1&&!n->hidden&&pet_surface_node_on_page(n,0));
''' + guard + r'''
}
int main(int argc,char **argv){
 assert(argc==2);mode=(unsigned)atoi(argv[1]);assert(mode<4);
 pet_surface_t s;
 cJSON *fixture=cJSON_Parse(''' + json.dumps(json.dumps(fixture)) + r''');
 assert(fixture&&pet_surface_parse(fixture,&s));cJSON_Delete(fixture);
 assert(s.count==2&&s.nodes[0].image==0&&s.nodes[0].w==640&&s.nodes[0].h==480);
 assert(!s.nodes[0].in_view&&!strcmp(s.nodes[0].image_bind,"cover"));g_app_text_surface=&s;
 /* Header accepted by the real P4J predicate. Codec bytes are intentionally a
  * boundary double: this host test never claims to decode a physical JPEG. */
 const uint8_t jpeg[39]={'P','4','J','1',128,2,224,1,39,0,0,0,255,216,255,192,
   0,17,8,1,224,2,128,3,0,0,0,0,0,0,0,0,0,255,218,0,2,255,217};
 assert(pet_image_jpeg(jpeg,sizeof(jpeg)));
 g_surface_images.count=1;g_surface_images.items[0]=(pet_p4_miniapp_sprite_t){.frame_width=640,.frame_height=480,.data_length=39};
 memcpy(g_surface_images.pixels,jpeg,sizeof(jpeg));
 for(frame=0;frame<2;frame++){
   guarded[0]=0xa55a;guarded[640*480+1]=0x5aa5;
   for(unsigned i=0;i<640*480;i++)g_framebuffer[i]=0x7777;
   assert(!g_cold_fast_jpeg_pending&&!borrowed&&!g_app_clip.active);
   assert(cold_fast_opaque_first_image(&s,0)&&g_cold_fast_jpeg_pending);
   draw_first(&s);
   assert(!borrowed&&!g_cold_fast_jpeg_pending);assert_complete_pixels();
   before_next_label(&s);assert_complete_pixels();assert(!g_cold_fast_jpeg_pending);
 }
 assert(borrows==2&&releases==(mode==0?2U:0U));
 assert(hardware_calls==(mode==0?0U:2U)&&software_calls==(mode>=2?2U:0U));
 return 0;
}
'''
    return code


class ServiceImageColdClearTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="service-cold-clear-")
        cls.addClassCleanup(cls.tmp.cleanup)
        src = Path(cls.tmp.name) / "service.c"
        cls.binary = src.with_suffix("")
        src.write_text(host_code((ROOT / "main/pet_p4_renderer.c").read_text()))
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-misleading-indentation", "-Wno-deprecated-declarations",
            "-fsanitize=address,undefined", "-DPET_SERVICE_BRIDGE_ENABLED=1",
            "-DPET_P4_APP_COLD_FAST=1", "-DPET_P4_APP_CLEAR_OPAQUE_CENTER=1",
            "-DPET_P4_APP_DIRTY_CACHE=1",
            "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/vendor/cjson"),
            str(src), str(ROOT / "main/pet_p4_surface.c"),
            str(ROOT / "tests/vendor/cjson/cJSON.c"), "-lm", "-o", str(cls.binary),
        ], check=True)

    def run_mode(self, mode):
        result = subprocess.run([str(self.binary), str(mode)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_service_success_releases_then_consumes_pending_before_label(self):
        self.run_mode(0)

    def test_unavailable_service_uses_original_padded_hardware_jpeg_fallback(self):
        self.run_mode(1)

    def test_unavailable_service_uses_original_software_jpeg_fallback(self):
        self.run_mode(2)

    def test_unavailable_service_and_failed_codecs_clear_original_background(self):
        self.run_mode(3)


if __name__ == "__main__":
    unittest.main()
