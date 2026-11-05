"""Actual renderer scope/peak helpers, bounded numeric-only diagnostics."""
from pathlib import Path
import configparser
import subprocess
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]


def test_actual_scopes_cover_continue_return_and_same_frame_peak(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    begin=text.index('#if PET_P4_APP_NODE_TRACE\n_Static_assert(PET_SURFACE_NODES')
    end=text.index('static void record_render_performance(',begin)
    helpers=text[begin:end]
    code=r'''
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#define PET_P4_APP_PHASE_TRACE 1
#define PET_P4_APP_NODE_TRACE 1
#define PET_SURFACE_NODES 48 /* p4-quota-v2 ceiling; the trace asserts it covers every node */
#include "pet_p4_render_trace.h"
static int64_t clock_now;
static int64_t esp_timer_get_time(void){return clock_now;}
static pet_p4_render_trace_t g_render_trace;
static char logged[1024];
static void log_line(const char *fmt,...){va_list ap;va_start(ap,fmt);vsnprintf(logged,sizeof(logged),fmt,ap);va_end(ap);}
#define TAG 0
#define ESP_LOGI(tag,...) log_line(__VA_ARGS__)
#define ESP_LOGD(tag,...) log_line(__VA_ARGS__)
#define ESP_LOGW(tag,...) log_line(__VA_ARGS__)
'''+helpers+r'''
static void early(void){APP_NODE_SCOPE(return_scope,images_us);clock_now+=5;return;}
int main(void){
 pet_p4_render_trace_begin(&g_render_trace,72,"app");g_render_trace.app_draw_started_us=1;
 clock_now=10;{APP_NODE_SCOPE(clear_scope,clear_us);clock_now=20;}
 assert(g_render_trace.app_nodes.clear_us==10);early();assert(g_render_trace.app_nodes.images_us==5);
 for(unsigned i=0;i<32;i++){
  APP_NODE_MARK(i);APP_NODE_SCOPE(node_scope,nodes_us[i]);clock_now++;
  if(i&1)continue;
  clock_now+=2;
 }
 APP_NODE_HIT();assert(g_render_trace.app_nodes.nodes_mask==UINT32_MAX);
 for(unsigned i=0;i<32;i++)assert(g_render_trace.app_nodes.nodes_us[i]==(i&1?1:3));
 clock_now=100;{APP_NODE_SCOPE(reverse_scope,key_us);clock_now=99;}
 assert(!g_render_trace.app_nodes.key_us&&g_render_trace.app_nodes.invalid_clock==1);
 g_render_trace.app_nodes.remember_us=UINT32_MAX-1;clock_now=200;
 {APP_NODE_SCOPE(large_scope,remember_us);clock_now=300;}
 assert(g_render_trace.app_nodes.remember_us==UINT32_MAX);
 pet_p4_render_peak_t peak={0};pet_p4_render_peak_record(&peak,&g_render_trace,400,0,0,0,0,0,0,0);
 pet_p4_render_trace_begin(&g_render_trace,73,"app");
 assert(!g_render_trace.app_nodes.nodes_mask&&!g_render_trace.app_nodes.clear_us);
 app_node_trace_log(&peak.trace);assert(strstr(logged,"P4 app nodes frame=72")&&strstr(logged,"n31_us=1"));
 assert(strstr(logged,"clear_us=10")&&strstr(logged,"hit=1")&&strstr(logged,"invalid_clock=1"));
 char prior[1024];memcpy(prior,logged,sizeof(prior));app_node_trace_log(&g_render_trace);assert(!memcmp(prior,logged,sizeof(prior)));
 return 0;
}
'''
    src=tmp_path/'scopes.c';src.write_text(code);exe=tmp_path/'scopes'
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def test_maximum_numeric_format_and_truncated_guard_bytes(tmp_path):
    code=r'''
#include <assert.h>
#include <string.h>
#define PET_P4_APP_PHASE_TRACE 1
#define PET_P4_APP_NODE_TRACE 1
#include "pet_p4_app_draw_trace.h"
int main(void){
 pet_app_draw_trace_t t;memset(&t,0xff,sizeof(t));char output[1152];
 assert(pet_app_draw_trace_format(&t,UINT32_MAX,output,sizeof(output))&&strlen(output)<sizeof(output));
 assert(strstr(output,"n0_us=4294967295")&&strstr(output,"n31_us=4294967295")&&strstr(output,"n47_us=4294967295"));
 assert(strstr(output,"mask=18446744073709551615"));
 unsigned char guarded[1300];memset(guarded,0x7b,sizeof(guarded));
 for(unsigned n=1;n<sizeof(output);n++){
  (void)pet_app_draw_trace_format(&t,UINT32_MAX,(char*)guarded+1,n);
  assert(guarded[0]==0x7b&&guarded[n+1]==0x7b);memset(guarded,0x7b,sizeof(guarded));
 }
 assert(!pet_app_draw_trace_format(NULL,1,output,sizeof(output))&&!pet_app_draw_trace_format(&t,1,NULL,2));
 assert(!pet_app_draw_trace_format(&t,1,output,0));uint32_t value=17;
 assert(!pet_app_draw_trace_add(&value,-1,20)&&value==17&&!pet_app_draw_trace_add(&value,21,20)&&value==17);
 assert(!pet_app_draw_trace_add(NULL,1,2));return 0;
}
'''
    src=tmp_path/'format.c';src.write_text(code);exe=tmp_path/'format'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def test_trace_parent_and_renderer_scopes_do_not_change_original_gates(tmp_path):
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        assert ('PET_P4_APP_NODE_TRACE=1' in flags)==(section=='env:esp32_p4_span_target_parent')
    text=(ROOT/'main/pet_p4_renderer.c').read_text();render=function(text,'render_package_app')
    for name in ('images_us','key_us','cache_copy_us','prefix_us','clear_us','remember_us','nodes_us[i]'):
        assert name in render
    assert 'APP_NODE_MARK(i);APP_NODE_SCOPE(node_scope,nodes_us[i]);' in render
    perf=function(text,'record_render_performance')
    assert perf.count('app_node_trace_log(')==3 and 'PET_P4_RENDER_FRAME_BUDGET_US,100000U' in perf
    header=(ROOT/'main/pet_p4_app_draw_trace.h').read_text()
    for forbidden in ('malloc(', 'fopen(', 'xTaskCreate', 'audio_', 'componentId', 'widget_id'):
        assert forbidden not in header
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_app_draw_trace.h"\nint main(void){return 0;}\n')
    failed=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_APP_NODE_TRACE=1',
      '-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert failed.returncode!=0 and 'App node trace requires the original whole-app phase trace' in failed.stderr

def test_boot_winning_frame_retains_exact_nodes_and_lcd_not_later_window_values(tmp_path):
    text=(ROOT/'main/pet_p4_renderer.c').read_text()
    perf=function(text,'record_render_performance')
    marker='  /* Execution time alone hides time spent waiting between frames'
    assert perf.count(marker)==1
    prefix=perf.split(marker,1)[0]+'}\n'
    code=r'''
#include <assert.h>
#include <string.h>
#define PET_P4_APP_PHASE_TRACE 1
#define PET_P4_APP_NODE_TRACE 1
#define PET_P4_RETAIN_PEAK_PHASES 1
#define PET_P4_RENDER_LCD_SPLIT_PROBE 1
#define PET_P4_RENDER_FRAME_BUDGET_US 66667U
#include "pet_p4_render_trace.h"
#include "pet_p4_render_lifetime.h"
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
static struct {uint32_t last_us,last_decode_us,submitted_frames,unchanged_frames;pet_render_lifetime_t lifetime;} g_metrics;
static pet_p4_render_trace_t g_render_trace;
static uint32_t g_last_decode_us,g_last_h264_pack_us,g_last_h264_convert_us,g_lcd_submitted,g_lcd_skipped,g_last_compare_us;
static bool g_last_compare_valid;
static struct {uint32_t submit_us,wait_us;bool valid,submitted,refresh_waited;} g_last_lcd_observation;
'''+function(text,'elapsed_us_clamped')+prefix+r'''
int main(void){
 pet_p4_render_trace_begin(&g_render_trace,900,"app");
 g_render_trace.app_draw_us=40000;g_render_trace.app_nodes.nodes_mask=UINT32_MAX;
 g_render_trace.app_nodes.nodes_us[31]=17000;g_render_trace.app_nodes.cache_deferred=1;
 g_last_compare_us=3400;g_last_compare_valid=true;
 g_last_lcd_observation.submit_us=2100;g_last_lcd_observation.wait_us=14000;
 g_last_lcd_observation.valid=g_last_lcd_observation.submitted=g_last_lcd_observation.refresh_waited=true;
 record_render_performance(100,83141,0,47000,13000,23041);
 pet_render_lifetime_t first=g_metrics.lifetime;
 assert(first.max_us==83041&&first.peak_frame==900&&first.app_nodes.nodes_us[31]==17000);
 assert(first.app_nodes.cache_deferred==1&&first.compare_us==3400&&first.compare_valid);
 assert(first.lcd_submit_us==2100&&first.lcd_wait_us==14000&&first.lcd_valid&&first.lcd_submitted&&first.lcd_waited);
 pet_p4_render_trace_begin(&g_render_trace,901,"app");g_last_compare_us=1;g_last_compare_valid=false;
 memset(&g_last_lcd_observation,0,sizeof(g_last_lcd_observation));
 record_render_performance(90000,140000,0,30000,13000,7000);
 assert(g_metrics.lifetime.samples==2&&g_metrics.lifetime.over_budget==1);
 assert(g_metrics.lifetime.max_us==83041&&g_metrics.lifetime.app_nodes.nodes_us[31]==17000);
 assert(g_metrics.lifetime.lcd_wait_us==14000&&g_metrics.lifetime.compare_us==3400);
 pet_p4_render_trace_begin(&g_render_trace,902,"main");
 record_render_performance(150000,240000,20000,30000,20000,20000);
 assert(g_metrics.lifetime.peak_frame==902&&g_metrics.lifetime.max_us==90000);
 assert(!g_metrics.lifetime.app_nodes.nodes_mask&&!g_metrics.lifetime.app_nodes.cache_deferred);
 assert(!g_metrics.lifetime.lcd_wait_us&&!g_metrics.lifetime.compare_valid);return 0;
}
'''
    src=tmp_path/'lifetime-nodes.c';src.write_text(code);exe=tmp_path/'lifetime-nodes'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    diagnostics=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    part=diagnostics.split('cJSON *nodes=cJSON_AddObjectToObject(p,"appNodes");',1)[1].split('#endif',1)[0]
    assert 'i<PET_APP_TRACE_NODES' in part and 'cacheDeferred' in part and 'nodeUs' in part
    assert 'maskLow' in part and 'maskHigh' in part
    for private in ('widget_id','path','nonce','owner','payload','title'):assert private not in part
