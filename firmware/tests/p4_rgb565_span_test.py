"""Bit-exact actual renderer rectangle with/without bounded burst stores."""
from pathlib import Path
import subprocess
import configparser
import pytest
ROOT=Path(__file__).resolve().parents[1]

def test_actual_rectangle_clipping_and_all_colors_remain_identical(tmp_path):
    check_actual_rectangle(tmp_path,16)

@pytest.mark.parametrize('pixels',[32,64,128])
def test_bounded_patterns_preserve_every_original_rectangle_and_color(tmp_path,pixels):
    check_actual_rectangle(tmp_path,pixels)

def check_actual_rectangle(tmp_path,pixels):
    from p4_render_trace_test import function
    rect=function((ROOT/'main/pet_p4_renderer.c').read_text(),'fill_rect')
    source=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_rgb565_span.h"
#define PET_P4_UI_WIDTH 640
#define PET_P4_UI_HEIGHT 480
static uint16_t a[640*480],b[640*480];
static uint16_t *g_framebuffer;
static struct {bool active;int x,y,right,bottom;} g_app_clip;
'''.replace('#include <assert.h>','#include <assert.h>\n#include <stdbool.h>')
    source+='#undef PET_P4_RENDER_SPAN_FILL\n#define PET_P4_RENDER_SPAN_FILL 0\n'+rect.replace('fill_rect(', 'reference(')
    source+='\n#undef PET_P4_RENDER_SPAN_FILL\n#define PET_P4_RENDER_SPAN_FILL 1\n'+rect+r'''
static uint32_t state=127;
static unsigned random_word(void){state=state*1664525U+1013904223U;return state;}
int main(void){
 for(unsigned i=0;i<640*480;i++)a[i]=b[i]=(uint16_t)random_word();
 for(unsigned n=0;n<900;n++){
   int x=(int)(random_word()%850)-100,y=(int)(random_word()%650)-100;
   int w=(int)(random_word()%850)-50,h=(int)(random_word()%650)-50;
   g_app_clip.active=n%3;g_app_clip.x=(int)(random_word()%640);g_app_clip.y=(int)(random_word()%480);
   g_app_clip.right=g_app_clip.x+(int)(random_word()%650);g_app_clip.bottom=g_app_clip.y+(int)(random_word()%490);
   uint16_t color=(uint16_t)random_word();
   g_framebuffer=a;reference(x,y,w,h,color);g_framebuffer=b;fill_rect(x,y,w,h,color);
   assert(!memcmp(a,b,sizeof(a)));
 }
 uint16_t guard[100];for(unsigned i=0;i<100;i++)guard[i]=0xabcd;
 for(unsigned color=0;color<65536;color++){
   pet_rgb565_fill_rows(guard+1,19,17,4,(uint16_t)color);
   assert(guard[0]==0xabcd&&guard[18]==0xabcd&&guard[19]==0xabcd&&guard[77]==0xabcd);
   for(unsigned y=0;y<4;y++)for(unsigned x=0;x<17;x++)assert(guard[1+y*19+x]==color);
 }
 uint16_t retained[100];memcpy(retained,guard,sizeof(guard));
 pet_rgb565_fill_rows(NULL,19,17,4,1);pet_rgb565_fill_rows(guard,19,20,4,1);
 pet_rgb565_fill_rows(guard,19,0,4,1);pet_rgb565_fill_rows(guard,19,17,0,1);
 assert(!memcmp(guard,retained,sizeof(guard)));return 0;
}
'''
    path=tmp_path/'spans.c';path.write_text(source);binary=tmp_path/'spans'
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DPET_P4_RENDER_SPAN_PIXELS={pixels}',
      '-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15)

def test_span_throughput_experiment_is_private_and_not_a_frame_gate_change():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_render_span_probe':
            assert config[section]['extends']=='env:esp32_p4_render_lcd_probe' and 'PET_P4_RENDER_SPAN_FILL=1' in flags
        elif section=='env:esp32_p4_span_target_parent':
            assert config[section]['extends']=='env:esp32_p4_glyph_rows_parent' and 'PET_P4_RENDER_SPAN_FILL=1' in flags
            assert 'PET_P4_APP_NODE_TRACE=1' in flags
        else:assert 'PET_P4_RENDER_SPAN_FILL=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'pet_p4_frame_unchanged(g_native_framebuffer, previous_native,' in text
    assert 'g_last_native_valid = lcd_err == ESP_OK;' in text
