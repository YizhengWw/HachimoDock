"""Offscreen track platforms must be clipped, not dropped or drawn over the HUD."""
from pathlib import Path
import subprocess


def test_scrolling_platform_clips_at_both_view_edges(tmp_path):
    source = (Path(__file__).resolve().parents[1] / "main/pet_p4_renderer.c").read_text()
    code = source[source.index("static void draw_scene_capsule("):]
    code = code[:code.index("static void draw_app_scene(")]
    program = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct {int x,y,w,h;} pet_surface_scene_view_t;
static uint16_t pixels[640*480];
static void put_px(int x,int y,uint16_t color){assert(x>=0&&x<640&&y>=0&&y<480);pixels[y*640+x]=color;}
''' + code + r'''
int main(void){
 pet_surface_scene_view_t v={24,138,592,216};
 for(int x=-80;x<700;x+=4){
  memset(pixels,0,sizeof(pixels));draw_scene_capsule(x,290,108,27,7,&v);int painted=0;
  for(int y=0;y<480;y++)for(int xx=0;xx<640;xx++)if(pixels[y*640+xx]){
   assert(xx>=24&&xx<616&&y>=138&&y<354);painted++;
  }
  if(x+108>36&&x<604)assert(painted>0);
 }
}
'''
    c = tmp_path / "clip.c"
    c.write_text(program)
    executable = tmp_path / "clip"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
