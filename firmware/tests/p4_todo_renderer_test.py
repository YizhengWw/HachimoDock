"""Compile the actual list renderer with draw-call spies; no board/flash access."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TodoRendererTest(unittest.TestCase):
    def test_todo_hierarchy_colors_dates_and_unchanged_stock_layout(self):
        source = (ROOT / "main/pet_p4_renderer.c").read_text()
        renderer = "static void render_data_miniapp_page" + source.split(
            "static void render_data_miniapp_page", 1
        )[1].split("// Semantic icons", 1)[0]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_widget_data.h"
typedef struct { char title[61], footer[157], data_source[48]; pet_p4_data_view_t data; } pet_p4_miniapp_view_t;
typedef struct {char text[160]; int x,y,scale; uint16_t color;} draw_t;
static draw_t calls[64]; static int count;
static uint16_t rgb565(int r,int g,int b) {return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
static void fill_rect(int x,int y,int w,int h,uint16_t color) {(void)x;(void)y;(void)w;(void)h;(void)color;}
static void draw_text_line(const char *t,int x,int y,int width,uint16_t color,int scale,bool ellipsis) {
  (void)width;(void)ellipsis; assert(count<64);
  draw_t *d=&calls[count++]; snprintf(d->text,sizeof(d->text),"%s",t);d->x=x;d->y=y;d->scale=scale;d->color=color;
}
static draw_t *find(const char *s) {for(int i=0;i<count;i++)if(!strcmp(calls[i].text,s))return &calls[i];assert(0);return NULL;}
'''
        main = r'''
int main(void) {
  pet_p4_miniapp_view_t app={0};
  strcpy(app.data_source,"todos.upcoming");strcpy(app.title,"近期待办");
  strcpy(app.data.date,"2026-09-23");app.data.received=true;app.data.count=2;app.data.pages=1;
  strcpy(app.data.rows[0].label,"买火车票");strcpy(app.data.rows[0].meta,"和整理行李");
  strcpy(app.data.rows[0].value,"09-24 14:30");strcpy(app.data.rows[0].detail,"待完成");app.data.rows[0].tone=1;
  strcpy(app.data.rows[1].label,"已完成事项");strcpy(app.data.rows[1].value,"未定日期");app.data.rows[1].tone=-1;
  render_data_miniapp_page(&app);
  assert(find("09-23")->scale==1);
  assert(find("买火车票和整理行李")->scale==2);
  assert(find("09-24 14:30")->scale==1);
  assert(find("买火车票和整理行李")->color==rgb565(76,167,250));
  assert(find("已完成事项")->color==rgb565(79,212,149));
  assert(find("09-24 14:30")->y==107);
  count=0;app.data.stale=true;render_data_miniapp_page(&app);
  assert(find("买火车票和整理行李")->color==rgb565(147,165,182));
  count=0;app.data.stale=false;strcpy(app.data_source,"stocks.watchlist");render_data_miniapp_page(&app);
  assert(find("2026-09-23")->scale==1);
  assert(find("买火车票")->scale==1);
  assert(find("09-24 14:30")->scale==2);
  assert(find("09-24 14:30")->color==rgb565(255,100,102));
  count=0;app.data.count=0;render_data_miniapp_page(&app);assert(find("等待数据"));
  puts("PASS todo typography, state colors, dates, stale/empty and stock isolation");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / "renderer.c"
            file.write_text(harness + renderer + main)
            exe = Path(tmp) / "renderer-test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "main"),
                            "-I", str(ROOT / "tests/stubs/widget_data"),
                            "-I", str(ROOT / "tests/vendor/cjson"),
                            str(file), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
