"""Actual LCD core and wrappers with counted driver/fence/clock adapters only."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_actual_lcd_split_preserves_single_submit_fence_and_errors(tmp_path):
    from p4_render_trace_test import function
    text=(ROOT/'main/pet_p4_lcd.c').read_text()
    core=function(text,'lcd_draw_core')
    ordinary=function(text,'pet_p4_lcd_draw_rgb565')
    observed=function(text,'pet_p4_lcd_draw_rgb565_observed')
    source=r'''
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "pet_p4_lcd_profile.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_TIMEOUT -3
#define pdTRUE 1
#define LCD_REFRESH_WAIT_TICKS 100
#define ESP_LOGW(...) (warns++)
static void *g_panel_handle=(void*)1,*g_refresh_done_sem=(void*)2;
static unsigned draws,waits,clocks,warns,stale;
static bool ready=true,backwards;static int driver_error;
static int64_t now=100;
#if PET_P4_RENDER_LCD_SPLIT_PROBE
static int64_t esp_timer_get_time(void){clocks++;return now;}
#endif
static int xSemaphoreTake(void *s,unsigned timeout){
 assert(s==(void*)2);if(!timeout){if(stale){stale--;return pdTRUE;}return 0;}
 assert(timeout==100);waits++;now+=73;return ready?pdTRUE:0;
}
static int esp_lcd_panel_draw_bitmap(void *p,int x,int y,int ex,int ey,const void *pixels){
 assert(p==(void*)1&&x==3&&y==4&&ex==5&&ey==6&&pixels);draws++;now+=backwards?-37:37;return driver_error;
}
'''+core+'\n'+ordinary+'\n'+('#if PET_P4_RENDER_LCD_SPLIT_PROBE\n'+observed+'\n#endif\n')+r'''
int main(void){
 uint16_t pixels[4]={0};pet_lcd_observation_t out={0};
 assert(pet_p4_lcd_draw_rgb565(3,4,2,2,pixels)==ESP_OK);assert(draws==1&&waits==1&&!clocks);
 g_panel_handle=NULL;assert(lcd_draw_core(3,4,2,2,pixels,&out)==ESP_ERR_INVALID_STATE);assert(draws==1&&!clocks);g_panel_handle=(void*)1;
 assert(lcd_draw_core(3,4,0,2,pixels,&out)==ESP_ERR_INVALID_ARG);assert(draws==1&&!clocks);
#if PET_P4_RENDER_LCD_SPLIT_PROBE
 now=100;stale=2;
 assert(pet_p4_lcd_draw_rgb565_observed(3,4,2,2,pixels,&out)==ESP_OK);
 assert(draws==2&&waits==2&&clocks==3&&!stale&&out.valid&&out.submitted&&out.refresh_waited&&out.submit_us==37&&out.wait_us==73);
 driver_error=-9;unsigned old_waits=waits;
 assert(pet_p4_lcd_draw_rgb565_observed(3,4,2,2,pixels,&out)==-9);assert(out.valid&&!out.submitted&&!out.refresh_waited&&out.submit_us==37&&waits==old_waits);
 driver_error=0;ready=false;
 assert(pet_p4_lcd_draw_rgb565_observed(3,4,2,2,pixels,&out)==ESP_ERR_TIMEOUT);assert(out.valid&&out.submitted&&out.refresh_waited&&out.wait_us==73&&warns==1);
 ready=true;g_refresh_done_sem=NULL;
 assert(pet_p4_lcd_draw_rgb565_observed(3,4,2,2,pixels,&out)==ESP_OK);assert(out.valid&&out.submitted&&!out.refresh_waited&&out.wait_us==0);
 backwards=true;assert(pet_p4_lcd_draw_rgb565_observed(3,4,2,2,pixels,&out)==ESP_OK);assert(!out.valid&&out.submitted&&out.submit_us==0);
 bool valid=true;assert(!pet_lcd_elapsed(-1,10,&valid)&&!valid);valid=true;assert(!pet_lcd_elapsed(9,8,&valid)&&!valid);
 valid=true;assert(!pet_lcd_elapsed(0,INT64_C(4294967296),&valid)&&!valid);
 valid=true;assert(pet_lcd_elapsed(2,2,&valid)==0&&valid);
#else
 ready=false;assert(pet_p4_lcd_draw_rgb565(3,4,2,2,pixels)==ESP_ERR_TIMEOUT);assert(!clocks&&warns==1);
#endif
 return 0;
}
'''
    for enabled in (0,1):
        path=tmp_path/f'lcd-{enabled}.c';path.write_text(source);binary=path.with_suffix('')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DPET_P4_RENDER_LCD_SPLIT_PROBE={enabled}',
          '-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

def test_split_observation_is_private_same_peak_only_and_not_a_timing_policy():
    import configparser
    c=configparser.ConfigParser(interpolation=None);c.read(ROOT/'platformio.ini')
    for name in c.sections():
        flags=c[name].get('build_flags','')
        if name=='env:esp32_p4_render_lcd_probe':
            assert c[name]['extends']=='env:esp32_p4_canvas_candidate' and 'PET_P4_RENDER_LCD_SPLIT_PROBE=1' in flags
        elif name=='env:esp32_p4_progressive_parent':
            assert c[name]['extends']=='env:esp32_p4_negotiated_parent'
            assert 'PET_P4_APP_CACHE_PROGRESSIVE=1' in flags and 'PET_P4_RENDER_LCD_SPLIT_PROBE=1' in flags
        else:assert 'PET_P4_RENDER_LCD_SPLIT_PROBE=1' not in flags
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    assert 'if(!g_render_perf.peak.valid||total_us>g_render_perf.peak.total_us)' in text
    assert 'g_render_perf.peak_compare_us=g_last_compare_us' in text and '(unsigned)peak->trace.frame' in text
    assert text.index('g_render_perf.peak_compare_us=g_last_compare_us')<text.index('pet_p4_render_peak_record(')
    assert 'pet_p4_frame_unchanged(g_native_framebuffer, previous_native,' in text
