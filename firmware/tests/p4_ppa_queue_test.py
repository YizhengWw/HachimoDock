"""Private same-transaction SDK boundary, counted original forward, no device."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_queue_probe_default_object_contains_no_wrapper_or_runtime_state(tmp_path):
    source=tmp_path/'closed.c';obj=tmp_path/'closed.o'
    source.write_text('#include "pet_p4_ppa_trace.h"\n#include "pet_p4_ppa_queue_probe.inc"\n')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),'-c',str(source),'-o',str(obj)],check=True)
    symbols=subprocess.run(['nm','-a',str(obj)],check=True,capture_output=True,text=True).stdout
    assert '__wrap_ppa' not in symbols and 'g_ppa_queue' not in symbols and 'esp_timer_get_time' not in symbols

def test_actual_private_queue_wrapper_owner_clock_error_and_forwarding(tmp_path):
    source=r'''
#define PET_P4_PPA_QUEUE_PROBE 1
#define PET_PPA_QUEUE_HOST_TEST 1
#include "pet_p4_ppa_trace.h"
#include <assert.h>
#include <errno.h>
#define DRAM_ATTR
typedef void *TaskHandle_t;
typedef void *ppa_client_handle_t;
typedef int ppa_trans_mode_t;
typedef int esp_err_t;
static int g_ppa_trace_mux;
static unsigned held,calls,clocks;
static void *task=(void*)1;
static bool isr;
static uint64_t now=120;
static pet_ppa_trace_state_t g_ppa_trace;
#define portENTER_CRITICAL(m) do{(void)(m);assert(!held);held++;}while(0)
#define portEXIT_CRITICAL(m) do{(void)(m);assert(held==1);held--;}while(0)
static TaskHandle_t xTaskGetCurrentTaskHandle(void){assert(!held&&!isr);errno=EIO;return task;}
static bool xPortInIsrContext(void){return isr;}
static int64_t esp_timer_get_time(void){assert(!held);clocks++;errno=ERANGE;return (int64_t)now;}
#include "pet_p4_ppa_queue_probe.inc"
static void *client=(void*)2;
static ppa_engine_t *engine=(ppa_engine_t*)3;
static ppa_trans_t *transaction=(ppa_trans_t*)4;
esp_err_t __real_ppa_do_operation(ppa_client_handle_t c,ppa_engine_t *e,ppa_trans_t *t,ppa_trans_mode_t mode){
  assert(!held&&c==client&&e==engine&&t==transaction&&mode==9&&errno==EACCES);
  calls++;errno=ENOSPC;return -17;
}
static void forward(void){
  errno=EACCES;unsigned before=calls;
  assert(__wrap_ppa_do_operation(client,engine,transaction,9)==-17&&errno==ENOSPC&&calls==before+1);
}
static pet_ppa_trace_sample_t sample(void){pet_ppa_trace_frame_t f;pet_ppa_trace_snapshot(&g_ppa_trace,&f);return f.op[1];}
static uint32_t begin(uint64_t at){pet_ppa_trace_frame_begin(&g_ppa_trace);return pet_ppa_trace_begin(&g_ppa_trace,PET_PPA_TRACE_CONVERT,at);}
static void finish(uint32_t cookie,uint64_t cb,uint64_t end){assert(pet_ppa_trace_callback(&g_ppa_trace,cookie,cb));assert(pet_ppa_trace_return(&g_ppa_trace,cookie,true,end));}
int main(void){
  pet_ppa_trace_set_available(&g_ppa_trace,true);
  forward();assert(!clocks);
  uint32_t cookie=begin(100);errno=EACCES;ppa_queue_scope_begin(client,cookie);assert(errno==EACCES);
  task=(void*)5;forward();assert(!clocks);task=(void*)1;
  client=(void*)6;forward();assert(!clocks);client=(void*)2;
  isr=true;forward();assert(!clocks);isr=false;
  forward();assert(clocks==1);finish(cookie,150,170);
  pet_ppa_trace_sample_t s=sample();assert((s.flags&PET_PPA_TRACE_QUEUE_VALID)&&s.prepare_us==20&&s.queue_to_callback_us==30&&s.callback_to_return_us==20);
  errno=EACCES;ppa_queue_scope_end(cookie);assert(errno==EACCES&&!g_ppa_queue_owner);
  forward();assert(clocks==1);
  cookie=begin(100);ppa_queue_scope_begin(client,cookie);now=130;forward();forward();finish(cookie,150,170);
  s=sample();assert((s.flags&PET_PPA_TRACE_QUEUE_INVALID)&&!s.prepare_us&&!s.queue_to_callback_us);ppa_queue_scope_end(cookie);
  cookie=begin(100);ppa_queue_scope_begin(client,cookie);now=90;forward();finish(cookie,150,170);
  assert(sample().flags&PET_PPA_TRACE_QUEUE_INVALID);ppa_queue_scope_end(cookie);
  cookie=begin(100);ppa_queue_scope_begin(client,cookie);now=160;forward();finish(cookie,150,170);
  assert(sample().flags&PET_PPA_TRACE_QUEUE_INVALID);ppa_queue_scope_end(cookie);
  cookie=begin(0);ppa_queue_scope_begin(client,cookie);now=120;forward();finish(cookie,150,(uint64_t)UINT32_MAX+1);
  assert(sample().flags&PET_PPA_TRACE_QUEUE_INVALID);ppa_queue_scope_end(cookie);
  cookie=begin(100);ppa_queue_scope_begin(client,cookie);
  ppa_queue_scope_begin((void*)7,cookie+1);ppa_queue_scope_end(cookie+1);assert(g_ppa_queue_cookie==cookie);
  now=120;forward();assert(pet_ppa_trace_return(&g_ppa_trace,cookie,false,170));
  assert(!(sample().flags&PET_PPA_TRACE_QUEUE_VALID));ppa_queue_scope_end(cookie);
  cookie=begin(100);assert(!pet_ppa_trace_queue(&g_ppa_trace,cookie-1,120));finish(cookie,150,170);
  assert(!(sample().flags&PET_PPA_TRACE_QUEUE_SEEN));assert(!held);
  return 0;
}
'''
    path=tmp_path/'queue.c';path.write_text(source);binary=tmp_path/'queue'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-I',str(ROOT/'main'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_queue_wrapper_matches_pinned_sdk_tags_signature_and_boundary():
    sdk=Path.home()/'.platformio/packages/framework-espidf/components/esp_driver_ppa/src'
    private=(sdk/'ppa_priv.h').read_text();srm=(sdk/'ppa_srm.c').read_text()
    assert 'typedef struct ppa_engine_t ppa_engine_t;' in private
    assert 'typedef struct ppa_trans_s {' in private
    assert 'esp_err_t ppa_do_operation(ppa_client_handle_t ppa_client, ppa_engine_t *ppa_engine_base, ppa_trans_t *trans_elm, ppa_trans_mode_t mode);' in private
    assert srm.index('esp_cache_msync(')<srm.index('ret = ppa_do_operation(')
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for name in config.sections():
        flags=config[name].get('build_flags','')
        if name=='env:esp32_p4_render_queue_probe':
            assert config[name]['extends']=='env:esp32_p4_render_lcd_probe'
            assert '-DPET_P4_PPA_QUEUE_PROBE=1' in flags and '-Wl,--wrap=ppa_do_operation' in flags
        else:assert 'PET_P4_PPA_QUEUE_PROBE=1' not in flags and '--wrap=ppa_do_operation' not in flags
    source=(ROOT/'main/pet_p4_renderer.c').read_text()
    adapter=source.split('static esp_err_t ppa_trace_srm(',1)[1].split('static size_t g_jpeg_input_capacity;',1)[0]
    assert adapter.index('ppa_queue_scope_begin(')<adapter.index('ppa_do_scale_rotate_mirror(')<adapter.index('ppa_queue_scope_end(')
    wrapper=(ROOT/'main/pet_p4_ppa_queue_probe.inc').read_text()
    assert wrapper.count('return __real_ppa_do_operation(client,engine,transaction,mode);')==1
    for forbidden in ('malloc(', 'printf(', 'ESP_LOG', 'vTaskDelay(', 'config->mode', 'esp_cache_msync('):assert forbidden not in wrapper
