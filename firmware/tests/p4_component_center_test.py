"""Compile the actual device card renderer; exercise every catalog size/focus.

Optional --preview PATH emits the same draw calls for a host layout preview.
This is not a hardware display, serial or audio acceptance test.
"""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
typedef struct {char widget_id[64],title[128];bool active;} pet_p4_miniapp_catalog_entry_t;
static size_t count,selected,reads;
static bool preview;
static const char *ids[]={"stock-watchlist","music-player","upcoming-todos","computer-status"};
static const char *titles[]={"自选股行情","随身听","近期待办","电脑状态"};
static size_t pet_p4_miniapp_catalog_count(void){return count;}
static size_t pet_p4_miniapp_catalog_selected(void){return selected;}
static bool pet_p4_miniapp_catalog_get(size_t i,pet_p4_miniapp_catalog_entry_t *out){
 assert(i<count);assert(i/4==selected/4);++reads;
 snprintf(out->widget_id,sizeof(out->widget_id),"%s",i<4?ids[i]:"other");
 snprintf(out->title,sizeof(out->title),"%s",i<4?titles[i]:"名称很长很长很长的组件");out->active=i==1;return true;
}
static uint16_t rgb565(int r,int g,int b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void bounds(int x,int y,int w,int h){assert(x>=0&&y>=0&&w>0&&h>0&&x+w<=640&&y+h<=480);}
static void fill_round_rect(int x,int y,int w,int h,int r,uint16_t c){
 bounds(x,y,w,h);assert(r>=0&&r<=w/2&&r<=h/2);
 if(preview)printf("[\"rect\",%d,%d,%d,%d,%d,%u]\n",x,y,w,h,r,c);
}
static void fill_rect(int x,int y,int w,int h,uint16_t c){fill_round_rect(x,y,w,h,0,c);}
static void fill_round_rect_outline(int x,int y,int w,int h,int r,uint16_t f,uint16_t o){
 fill_round_rect(x,y,w,h,r,o);fill_round_rect(x+2,y+2,w-4,h-4,r-2,f);
}
static void fill_ellipse(int x,int y,int rx,int ry,uint16_t c){
 bounds(x-rx,y-ry,rx*2,ry*2);if(preview)printf("[\"ellipse\",%d,%d,%d,%d,%u]\n",x,y,rx,ry,c);
}
static void draw_line(int x,int y,int x1,int y1,uint16_t c){
 assert(x>=0&&x<640&&x1>=0&&x1<640&&y>=0&&y<480&&y1>=0&&y1<480);
 if(preview)printf("[\"line\",%d,%d,%d,%d,%u]\n",x,y,x1,y1,c);
}
static int text_y_in_box(const char *t,int s,int y,int h){(void)t;return y+(h-16*s)/2;}
static void text(const char *t,int x,int y,int max,uint16_t c,int s,const char *anchor){
 assert(y>=0&&y+16*s<=480&&max>0&&s>0);assert(x>=0&&x<=640);
 if(preview)printf("[\"text\",%d,%d,%d,%d,%u,\"%s\",\"%s\"]\n",x,y,max,s,c,anchor,t);
}
static void draw_text_line(const char*t,int x,int y,int w,uint16_t c,int s,bool e){(void)e;bounds(x,y,w,16*s);text(t,x,y,w,c,s,"left");}
static void draw_text_right(const char*t,int x,int y,int w,uint16_t c,int s){text(t,x,y,w,c,s,"right");}
static void draw_text_center(const char*t,int x,int y,int w,uint16_t c,int s){text(t,x,y,w,c,s,"center");}
'''
MAIN = r'''
int main(int argc,char **argv){
 (void)argv;
 for(count=0;count<=16;++count){
   for(selected=0;selected<(count?count:1);++selected){
     reads=0;render_component_center_page();
     size_t remaining=count-selected/4*4;assert(reads==(remaining<4?remaining:4));
   }
 }
 count=0;selected=99;render_component_center_page();
 if(argc>1){preview=true;count=12;selected=0;render_component_center_page();}
 return 0;
}
'''


def run_renderer(preview=False):
    source = (ROOT / "main/pet_p4_renderer.c").read_text()
    renderer = "static void draw_component_icon" + source.split(
        "static void draw_component_icon", 1)[1].split("static void draw_connection_banner", 1)[0]
    with tempfile.TemporaryDirectory() as tmp:
        code, binary = Path(tmp)/"cards.c", Path(tmp)/"cards"
        code.write_text(HARNESS + renderer + MAIN)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(code), "-o", str(binary)], check=True)
        return subprocess.check_output([str(binary)] + (["preview"] if preview else []), text=True)


def test_card_renderer_bounds_empty_single_partial_full_pages():
    run_renderer()


def preview(path):
    from PIL import Image, ImageDraw, ImageFont
    image = Image.new("RGB", (640, 480))
    draw = ImageDraw.Draw(image)
    font_path = "/System/Library/Fonts/STHeiti Medium.ttc"
    color = lambda n: ((n >> 11)*255//31, ((n >> 5)&63)*255//63, (n&31)*255//31)
    for line in run_renderer(True).splitlines():
        op, *a = json.loads(line)
        if op == "rect":
            x,y,w,h,r,c=a;draw.rounded_rectangle((x,y,x+w-1,y+h-1),r,fill=color(c))
        elif op == "line":
            x,y,x1,y1,c=a;draw.line((x,y,x1,y1),fill=color(c))
        elif op == "ellipse":
            x,y,rx,ry,c=a;draw.ellipse((x-rx,y-ry,x+rx,y+ry),fill=color(c))
        elif op == "text":
            x,y,w,s,c,anchor,t=a; font=ImageFont.truetype(font_path,16*s)
            while font.getlength(t)>w: t=t[:-1]
            width=font.getlength(t)
            if anchor=="right": x-=width
            if anchor=="center": x-=width/2
            draw.text((x,y),t,font=font,fill=color(c),anchor="lt")
    image.save(path)


if __name__ == "__main__":
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument("--preview")
    args=parser.parse_args()
    if args.preview: preview(args.preview)
    else: run_renderer()
