"""Run the real rounded-alpha rasterizer; no overlapping blend of one pixel."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def test_rounded_alpha_is_single_pass_symmetric_and_clipped():
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    function = "static void blend_round_rect(" + source.split(
        "static void blend_round_rect(", 1)[1].split("static void draw_home_white_panel", 1)[0]
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static unsigned char visits[480][640], alphas[480][640];
static void blend_px(int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
 (void)r;(void)g;(void)b;
 assert(x>=0&&x<640&&y>=0&&y<480);assert(++visits[y][x]==1);alphas[y][x]=a;
}
static void blend_rect(int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
 for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)
  if(xx>=0&&xx<640&&yy>=0&&yy<480)blend_px(xx,yy,r,g,b,a);
}
FUNCTION
static void clear(void){memset(visits,0,sizeof(visits));memset(alphas,0,sizeof(alphas));}
int main(void){
 for(int radius=0;radius<=40;radius++){
  clear();blend_round_rect(28,394,118,44,radius,128,128,128,110);
  for(int y=0;y<44;y++)for(int x=0;x<118;x++){
   assert(alphas[y+394][x+28]==alphas[394+43-y][x+28]);
   assert(alphas[y+394][x+28]==alphas[y+394][28+117-x]);
   assert(alphas[y+394][x+28]<=110);
  }
  for(int y=408;y<424;y++)for(int x=42;x<132;x++)assert(alphas[y][x]==110);
 }
 clear();blend_round_rect(-18,-12,118,44,14,0,0,0,255);
 clear();blend_round_rect(620,465,118,44,14,0,0,0,255);
 clear();blend_round_rect(0,0,0,44,14,0,0,0,255);
 blend_round_rect(0,0,118,-1,14,0,0,0,255);
 blend_round_rect(0,0,118,44,14,0,0,0,0);
 for(int y=0;y<480;y++)for(int x=0;x<640;x++)assert(visits[y][x]==0);
 clear();blend_round_rect(2,2,1,1,99,0,0,0,110);assert(alphas[2][2]==110);
 return 0;
}
'''.replace("FUNCTION", function)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        (path / "test.c").write_text(harness)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(path / "test.c"), "-lm", "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
