"""Execute the real panel rasterizer against the unchanged per-pixel SDF path."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_gradient_panel_interiors_preserve_clips_corners_and_every_alpha(tmp_path):
    text = (ROOT / "main/pet_p4_renderer.c").read_text()
    code = r"""
#include "pet_p4_surface.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t a[640*480+2],b[640*480+2],*g_framebuffer;
static struct{bool active;int x,y,right,bottom;}g_app_clip;
static uint32_t g_cold_fast_panel_nodes;
static void pet_render_counter_increment(uint32_t *v){++*v;}
static uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){return(uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
"""
    for name in ("blend_rgb565", "app_clip_contains", "put_px", "fill_rect", "blend_px"):
        code += function(text, name) + "\n"
    panel = function(text, "draw_app_panel")
    code += "\n#define PET_P4_APP_COLD_FAST 0\n" + panel.replace("draw_app_panel(", "reference_panel(")
    code += text[text.index("typedef struct {uint16_t r[32],g[64],b[32];} cold_fast_blend_lut_t;"):text.index("static inline void cold_fast_image_span(")]
    code += function(text, "cold_fast_panel_interior")
    code += "\n#undef PET_P4_APP_COLD_FAST\n#define PET_P4_APP_COLD_FAST 1\n" + panel
    code += r"""
static uint32_t state=0x5a183d12;
static unsigned rnd(void){state=state*1664525U+1013904223U;return state;}
static void check(pet_surface_node_t n){
 for(unsigned i=0;i<640*480+2;i++)a[i]=b[i]=(uint16_t)(i*31337U+state);
 g_framebuffer=a+1;reference_panel(&n);g_framebuffer=b+1;draw_app_panel(&n);
 assert(!memcmp(a,b,sizeof(a)));
}
int main(void){
 /* The measured cold-frame panel, both gradient directions and reversed alpha. */
 for(unsigned vertical=0;vertical<2;vertical++)for(unsigned reverse=0;reverse<2;reverse++){
  pet_surface_node_t n={.x=12,.y=132,.w=392,.h=196,.radius=20,.color=65501,
    .opacity=reverse?100:225,.opacity_end=reverse?225:100,.vertical=vertical};check(n);
 }
 for(unsigned alpha=0;alpha<256;alpha++)for(unsigned vertical=0;vertical<2;vertical++){
  pet_surface_node_t n={.x=17,.y=29,.w=37,.h=27,.radius=alpha%14,.color=(uint16_t)rnd(),
   .opacity=alpha,.opacity_end=255-alpha,.vertical=vertical};check(n);
  n.opacity_end=alpha;check(n);
 }
 /* Full clipping, viewport clipping, oversized radii, strokes, odd/even shapes,
  * and gradients with 0/255 endpoints retain exactly the original pixels. */
 for(unsigned i=0;i<300;i++){
  pet_surface_node_t n={.x=(int)(rnd()%850)-100,.y=(int)(rnd()%650)-90,
   .w=1+rnd()%680,.h=1+rnd()%500,.radius=rnd()%100,.color=(uint16_t)rnd(),
   .opacity=rnd(),.opacity_end=rnd(),.vertical=i%2,.stroke=(uint16_t)rnd(),.stroke_width=i%5==0?3:0};
  g_app_clip.active=i%3;g_app_clip.x=17;g_app_clip.y=19;g_app_clip.right=529;g_app_clip.bottom=438;check(n);
 }
 assert(g_cold_fast_panel_nodes>500);
 return 0;
}
"""
    src = tmp_path / "panel.c"
    binary = tmp_path / "panel"
    src.write_text(code)
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"), "-I", str(ROOT / "tests/vendor/cjson"),
                    str(src), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
