"""Actual panel rotation, exact pixels and default-off private experiment."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def run(tmp_path,name,source,flags=()):
    path=tmp_path/(name+'.c');path.write_text(source);binary=tmp_path/name
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      *flags,'-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=20)

def test_all_element_values_edges_odd_dimensions_and_no_partial_invalid_output(tmp_path):
    run(tmp_path,'rotate-pixels',r'''
#include "pet_p4_rgb565_rotate.h"
#include <assert.h>
#include <string.h>
static uint16_t src[640*480],dst[640*480+2],reference[640*480+2];
static uint32_t random_state=517;
static uint32_t random_word(void){random_state=random_state*1664525U+1013904223U;return random_state;}
int main(void){
 for(unsigned i=0;i<640*480;i++)src[i]=(uint16_t)random_word();
 for(unsigned pass=0;pass<260;pass++){
   unsigned w=pass==0?640:pass==1?1:1+random_word()%640;
   unsigned h=pass==0?480:pass==1?1:1+random_word()%480;
   memset(dst,0xa5,sizeof(dst));memcpy(reference,dst,sizeof(dst));
   for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)reference[1+x*h+h-1-y]=src[y*w+x];
   assert(pet_rgb565_rotate_panel(dst+1,(size_t)w*h,src,w,h));assert(!memcmp(dst,reference,sizeof(dst)));
 }
 for(unsigned color=0;color<65536;color++){src[0]=(uint16_t)color;assert(pet_rgb565_rotate_panel(dst,1,src,1,1)&&dst[0]==color);}
 memcpy(reference,dst,sizeof(dst));
 assert(!pet_rgb565_rotate_panel(NULL,1,src,1,1));assert(!pet_rgb565_rotate_panel(dst,1,NULL,1,1));
 assert(!pet_rgb565_rotate_panel(dst,0,src,1,1));assert(!pet_rgb565_rotate_panel(dst,640*480,src,641,1));
 assert(!pet_rgb565_rotate_panel(dst,640*480,src,1,481));assert(!pet_rgb565_rotate_panel(dst,1,src,0,1));
 assert(!memcmp(dst,reference,sizeof(dst)));
 uint16_t overlap[16],saved[16];for(unsigned i=0;i<16;i++)overlap[i]=(uint16_t)i;memcpy(saved,overlap,sizeof(saved));
 assert(!pet_rgb565_rotate_panel(overlap,4,overlap,2,2));
 assert(!pet_rgb565_rotate_panel(overlap+1,4,overlap,2,2));assert(!pet_rgb565_rotate_panel(overlap,4,overlap+1,2,2));
 assert(!memcmp(overlap,saved,sizeof(saved)));return 0;
}
''')

def test_actual_rotate_adapter_preserves_default_ppa_fallback_and_private_mapping(tmp_path):
    from p4_render_trace_test import function
    actual=function((ROOT/'main/pet_p4_renderer.c').read_text(),'rotate_landscape_to_panel')
    code=r'''
#include "pet_p4_rgb565_rotate.h"
#include <assert.h>
#include <string.h>
#define PET_P4_UI_WIDTH 64
#define PET_P4_UI_HEIGHT 48
#define BSP_LCD_H_RES 48
#define BSP_LCD_V_RES 64
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
enum {PPA_SRM_COLOR_MODE_RGB565=1,PPA_SRM_ROTATION_ANGLE_270=270,PPA_TRANS_MODE_BLOCKING=2,PET_PPA_TRACE_ROTATE=2};
typedef struct {void *buffer;unsigned pic_w,pic_h,block_w,block_h,block_offset_x,block_offset_y,buffer_size,srm_cm;} image_t;
typedef struct {image_t in,out;int rotation_angle,mode;float scale_x,scale_y;bool rgb_swap,byte_swap;} ppa_srm_oper_config_t;
static uint16_t source[64*48],dest[64*48],expected[64*48];
static uint16_t *g_framebuffer=source,*g_native_framebuffer=dest;
static bool g_framebuffer_ppa_compatible=true,g_native_framebuffer_ppa_compatible=true,g_logged_ppa_rotation,g_logged_ppa_fallback;
static void *g_ppa_srm_client=(void*)1;
static unsigned calls;static int fail;
static esp_err_t ppa_trace_srm(int kind,ppa_srm_oper_config_t *c){
 assert(kind==PET_PPA_TRACE_ROTATE&&c->in.buffer==source&&c->out.buffer==dest&&c->in.pic_w==64&&c->in.pic_h==48);
 assert(c->out.pic_w==48&&c->out.pic_h==64&&c->out.buffer_size==sizeof(dest));
 assert(c->rotation_angle==270&&c->mode==PPA_TRANS_MODE_BLOCKING&&c->scale_x==1&&c->scale_y==1&&!c->byte_swap&&!c->rgb_swap);
 calls++;if(fail)return -2;memcpy(dest,expected,sizeof(dest));return 0;
}
'''+actual+r'''
int main(void){
 for(unsigned n=0;n<64*48;n++)source[n]=(uint16_t)(n*39U+n/13U);
 for(unsigned y=0;y<48;y++)for(unsigned x=0;x<64;x++)expected[x*48+47-y]=source[y*64+x];
 assert(rotate_landscape_to_panel()==0&&!memcmp(dest,expected,sizeof(dest)));
 assert(calls==(PET_P4_RENDER_ROTATE_TILE?0:1));
 memset(dest,0,sizeof(dest));fail=1;calls=0;
 assert(rotate_landscape_to_panel()==0&&!memcmp(dest,expected,sizeof(dest)));
 assert(calls==(PET_P4_RENDER_ROTATE_TILE?0:1));
 g_framebuffer=NULL;assert(rotate_landscape_to_panel()==-1);return 0;
}
'''
    for enabled in (0,1):run(tmp_path,f'adapter-{enabled}',code,[f'-DPET_P4_RENDER_ROTATE_TILE={enabled}'])

def test_rotation_experiment_stays_private_without_panel_publication_changes():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_render_rotate_probe':
            assert config[section]['extends']=='env:esp32_p4_render_queue_probe'
            assert '-DPET_P4_RENDER_ROTATE_TILE=1' in flags
        else:assert 'PET_P4_RENDER_ROTATE_TILE=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'pet_p4_lcd_draw_rgb565_observed(' in text and 'g_last_native_valid = lcd_err == ESP_OK;' in text
    helper=(ROOT/'main/pet_p4_rgb565_rotate.h').read_text()
    for forbidden in ('malloc(', 'ESP_LOG', 'vTaskDelay(', 'ppa_do_', 'portENTER_', 'esp_cache_'):assert forbidden not in helper
