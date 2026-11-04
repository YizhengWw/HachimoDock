"""Execute the production scene function; stubs observe draws and clip state only."""
from pathlib import Path
import subprocess
import pytest
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module", params=[0, 1], ids=["big-off", "big-on"])
def scene_binary(request, tmp_path_factory):
    tmp = tmp_path_factory.mktemp("scene")
    production = function((ROOT / "main/pet_p4_renderer.c").read_text(), "draw_app_scene")
    code = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_surface.h"
#include "pet_p4_game.h"
typedef struct {uint16_t background,surface,ink,primary,secondary,light,shadow;} miniapp_pixel_palette_t;
typedef struct {bool active;int x,y,right,bottom;} clip_t;
static clip_t g_app_clip,observed;
static unsigned sprite_calls,primitive_calls,capsule_calls;
static pet_surface_node_t image_node;
static int observed_x,observed_y;
static void draw_surface_image(const pet_surface_node_t*n){sprite_calls++;image_node=*n;observed=g_app_clip;observed_x=n->x;observed_y=n->y;}
static uint16_t miniapp_game_tone_color(unsigned t,const miniapp_pixel_palette_t*p){(void)p;return (uint16_t)t;}
static void draw_scene_capsule(int x,int y,int w,int h,uint16_t c,const pet_surface_scene_view_t*v){(void)w;(void)h;(void)c;(void)v;capsule_calls++;observed=g_app_clip;observed_x=x;observed_y=y;}
static void draw_miniapp_game_entity(const pet_p4_game_entity_t*e,const void*unused,int x,int y,int cell,bool moving,const miniapp_pixel_palette_t*p){
 (void)unused;(void)cell;(void)moving;(void)p;assert(e->x==0&&e->y==0);primitive_calls++;observed=g_app_clip;observed_x=x;observed_y=y;
}
''' + production + r'''
static void same_clip(clip_t a,clip_t b){assert(a.active==b.active&&a.x==b.x&&a.y==b.y&&a.right==b.right&&a.bottom==b.bottom);}
int main(int argc,char**argv){
 assert(argc==5);int kind=atoi(argv[1]),edge=atoi(argv[2]),outside=atoi(argv[3]),caller=atoi(argv[4]);
 pet_surface_t s={0};s.scene_view.enabled=true;s.scene_view.x=100;s.scene_view.y=80;s.scene_view.w=160;s.scene_view.h=80;
 pet_p4_game_frame_t g={0};g.kind=PET_P4_GAME_BOUNDED;g.width=16;g.height=8;g.motion=true;g.entity_count=1;
 pet_p4_game_entity_t *e=&g.entities[0];strcpy(e->id,"puppy");e->active=true;e->width=e->height=2;e->x=2;e->y=2;e->tone=1;
 int x=2,y=2;
 if(edge==0)x=outside?-2:-1;if(edge==1)x=outside?16:15;if(edge==2)y=outside?-2:-1;if(edge==3)y=outside?8:7;
 e->x_q8=x*256;e->y_q8=y*256;
 if(kind==0){s.scene_view.count=1;strcpy(s.scene_view.sprites[0].entity,"puppy");s.scene_view.sprites[0].image=2;s.scene_view.sprites[0].columns=4;s.scene_view.sprites[0].fps=3;}
 if(kind==2)e->shape=PET_P4_GAME_SHAPE_CAPSULE;
 clip_t saved=caller?(clip_t){true,108,70,252,153}:(clip_t){false,3,4,5,6};g_app_clip=saved;
 draw_app_scene(&s,&g,2000);
 same_clip(g_app_clip,saved);
 unsigned calls=sprite_calls+primitive_calls+capsule_calls;
 /* Capsules own their own raster clipping; sprites/primitives must not be
  * discarded unless their rectangle is fully outside the scene. */
 if(outside&&kind!=2){assert(calls==0);return 0;}
 assert(calls==1);
 same_clip(observed,caller?(clip_t){true,108,80,252,153}:(clip_t){true,100,80,260,160});
 assert(observed_x==100+x*10&&observed_y==80+y*10);
 if(kind==0){assert(sprite_calls==1&&image_node.image==2&&image_node.tile_cols==4&&image_node.tile_rows==1&&image_node.tile_index==2);
#if PET_P4_BIG_IMAGE
 assert(image_node.big==-1);
#endif
 }else if(kind==1)assert(primitive_calls==1);else assert(capsule_calls==1);
 return 0;
}
'''
    src, binary = tmp / "scene.c", tmp / "scene"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", "-fsanitize=address,undefined",
                    f"-DPET_P4_BIG_IMAGE={request.param}", "-DPET_P4_BODIES=0",
                    "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/vendor/cjson"),
                    str(src), "-o", str(binary)], check=True)
    return binary


@pytest.mark.parametrize("kind", [0, 1], ids=["sprite", "primitive"])
@pytest.mark.parametrize("edge", range(4), ids=["left", "right", "top", "bottom"])
@pytest.mark.parametrize("outside", [0, 1], ids=["partial", "fully-outside"])
def test_scene_edges_and_sentinel(scene_binary, kind, edge, outside):
    subprocess.run([str(scene_binary), str(kind), str(edge), str(outside), "0"], check=True)


@pytest.mark.parametrize("kind", [0, 1, 2], ids=["sprite", "primitive", "capsule"])
def test_intersect_and_restore_existing_caller_clip(scene_binary, kind):
    subprocess.run([str(scene_binary), str(kind), "0", "0", "1"], check=True)
