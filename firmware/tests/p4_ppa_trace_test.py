"""PPA numeric diagnostics: real helper and renderer adapter, no device access."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def compile_run(tmp_path, name, source):
    path = tmp_path / (name + '.c')
    binary = tmp_path / name
    path.write_text(source)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'main'),
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_numeric_callback_order_cookie_and_clock_matrix(tmp_path):
    compile_run(tmp_path, 'ppa_numeric', r'''
#include "pet_p4_ppa_trace.h"
#include <assert.h>
static pet_ppa_trace_state_t state;
static pet_ppa_trace_frame_t frame;
static pet_ppa_trace_sample_t sample(unsigned op){pet_ppa_trace_snapshot(&state,&frame);return frame.op[op];}
static uint32_t start(unsigned op,uint64_t now){return pet_ppa_trace_begin(&state,(pet_ppa_trace_op_t)op,now);}
int main(void){
  pet_ppa_trace_frame_begin(&state);
  assert(!start(PET_PPA_TRACE_CONVERT,100));
  assert(sample(1).flags==(PET_PPA_TRACE_ATTEMPTED|PET_PPA_TRACE_UNAVAILABLE));
  assert(!pet_ppa_trace_callback(&state,0,150)&&!pet_ppa_trace_return(&state,0,true,200));
  pet_ppa_trace_set_available(&state,true);pet_ppa_trace_frame_begin(&state);
  uint32_t first=start(1,100);assert(first);
  assert(sample(1).flags==PET_PPA_TRACE_ATTEMPTED); /* No fabricated missing while in-flight. */
  assert(pet_ppa_trace_callback(&state,first,150));
  assert(!pet_ppa_trace_callback(&state,first,175)); /* First observation wins. */
  assert(pet_ppa_trace_return(&state,first,true,200));
  assert(!pet_ppa_trace_return(&state,first,false,300));
  pet_ppa_trace_sample_t s=sample(1);
  assert(s.api_us==100&&s.call_to_callback_us==50&&s.callback_to_return_us==50);
  assert(s.flags==(PET_PPA_TRACE_ATTEMPTED|PET_PPA_TRACE_RETURNED|PET_PPA_TRACE_CALLBACK_SEEN|PET_PPA_TRACE_VALID_SPLIT));
  assert(!sample(0).flags&&!sample(2).flags);

  pet_ppa_trace_frame_begin(&state);uint32_t next=start(2,100);assert(next>first);
  assert(!pet_ppa_trace_callback(&state,first,150));
  assert(!pet_ppa_trace_return(&state,first,true,160));
  assert(pet_ppa_trace_return(&state,next,true,200));s=sample(2);
  assert(s.api_us==100&&(s.flags&PET_PPA_TRACE_MISSING)&&!(s.flags&PET_PPA_TRACE_VALID_SPLIT));
  assert(pet_ppa_trace_callback(&state,next,250));s=sample(2);
  assert(s.api_us==100&&(s.flags&PET_PPA_TRACE_LATE)&&!(s.flags&PET_PPA_TRACE_MISSING));
  assert(!s.call_to_callback_us&&!s.callback_to_return_us&&!(s.flags&PET_PPA_TRACE_VALID_SPLIT));

  /* An ISR may capture time before return but publish after another core's return. */
  pet_ppa_trace_frame_begin(&state);next=start(1,100);
  assert(pet_ppa_trace_return(&state,next,true,200));
  assert(pet_ppa_trace_callback(&state,next,150));s=sample(1);
  assert((s.flags&PET_PPA_TRACE_VALID_SPLIT)&&s.call_to_callback_us==50&&s.callback_to_return_us==50);
  pet_ppa_trace_frame_begin(&state);next=start(0,100);
  pet_ppa_trace_callback(&state,next,200);pet_ppa_trace_return(&state,next,true,200);
  assert((sample(0).flags&PET_PPA_TRACE_VALID_SPLIT)&&sample(0).callback_to_return_us==0);

  pet_ppa_trace_frame_begin(&state);next=start(1,100);
  pet_ppa_trace_return(&state,next,false,200);s=sample(1);
  assert((s.flags&PET_PPA_TRACE_ERROR)&&(s.flags&PET_PPA_TRACE_MISSING)&&s.api_us==100);
  pet_ppa_trace_callback(&state,next,150);s=sample(1);
  assert((s.flags&PET_PPA_TRACE_ERROR)&&!(s.flags&PET_PPA_TRACE_VALID_SPLIT));
  assert(!s.call_to_callback_us&&!s.callback_to_return_us);

  /* Each operation's cookie is independent, including a previous late callback. */
  pet_ppa_trace_frame_begin(&state);first=start(0,100);next=start(2,200);
  pet_ppa_trace_return(&state,first,true,150);pet_ppa_trace_callback(&state,next,230);
  pet_ppa_trace_callback(&state,first,160);pet_ppa_trace_return(&state,next,true,260);
  assert((sample(0).flags&PET_PPA_TRACE_LATE)&&(sample(2).flags&PET_PPA_TRACE_VALID_SPLIT));
  assert(sample(2).call_to_callback_us==30&&sample(2).callback_to_return_us==30);
  assert(!start(PET_PPA_TRACE_OP_COUNT,1));

  /* Unexpected multiple calls of one kind fail closed instead of mixing times. */
  pet_ppa_trace_frame_begin(&state);first=start(1,100);assert(!start(1,200));
  assert(!pet_ppa_trace_callback(&state,first,150)&&!pet_ppa_trace_return(&state,first,true,160));
  s=sample(1);assert(s.flags==(PET_PPA_TRACE_ATTEMPTED|PET_PPA_TRACE_REPEATED)&&!s.api_us);

  pet_ppa_trace_frame_begin(&state);next=start(1,200);
  pet_ppa_trace_callback(&state,next,190);pet_ppa_trace_return(&state,next,true,300);
  s=sample(1);assert((s.flags&PET_PPA_TRACE_CLOCK_INVALID)&&!s.api_us&&!(s.flags&PET_PPA_TRACE_VALID_SPLIT));
  pet_ppa_trace_frame_begin(&state);next=start(1,200);pet_ppa_trace_return(&state,next,true,199);
  assert(sample(1).flags&PET_PPA_TRACE_CLOCK_INVALID);
  pet_ppa_trace_frame_begin(&state);next=start(1,0);
  pet_ppa_trace_callback(&state,next,(uint64_t)UINT32_MAX+5);
  pet_ppa_trace_return(&state,next,true,2ULL*UINT32_MAX+10);
  s=sample(1);assert((s.flags&PET_PPA_TRACE_VALID_SPLIT)&&s.api_us==UINT32_MAX&&s.call_to_callback_us==UINT32_MAX&&s.callback_to_return_us==UINT32_MAX);

  /* Cookie zero is reserved. UINT32_MAX can be used once, never wrap/recycle. */
  state.last_cookie=UINT32_MAX-1;pet_ppa_trace_frame_begin(&state);next=start(2,100);assert(next==UINT32_MAX);
  pet_ppa_trace_callback(&state,next,150);pet_ppa_trace_return(&state,next,true,200);
  assert(sample(2).flags&PET_PPA_TRACE_VALID_SPLIT);
  pet_ppa_trace_frame_begin(&state);assert(!start(2,300));
  assert(!pet_ppa_trace_callback(&state,next,350));
  assert(sample(2).flags==(PET_PPA_TRACE_ATTEMPTED|PET_PPA_TRACE_EXHAUSTED));
  pet_ppa_trace_frame_begin(&state);assert(!start(0,400)&&state.last_cookie==UINT32_MAX);
  assert(sample(0).flags&PET_PPA_TRACE_EXHAUSTED);
  return 0;
}
''')


def test_production_adapter_preserves_blocking_buffers_errors_and_optional_registration(tmp_path):
    text = (ROOT / 'main/pet_p4_renderer.c').read_text()
    adapter = text[text.index('static ppa_client_handle_t g_ppa_srm_client;'):
                   text.index('static size_t g_jpeg_input_capacity;')]
    start = text.index('  if (!g_ppa_init_attempted) {')
    begin = text.index('{', start)
    depth, end = 1, begin + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    registration = text[start:end]
    for operation, config in [('SCALE', 'scaling'), ('CONVERT', 'scaling'), ('ROTATE', 'rotation')]:
        assert f'ppa_trace_srm(PET_PPA_TRACE_{operation}, &{config})' in text
    assert adapter.count('ppa_do_scale_rotate_mirror(') == 1
    assert 'portEXIT_CRITICAL(&g_ppa_trace_mux);\n  config->user_data=' in adapter
    source = r'''
#include "pet_p4_ppa_trace.h"
#include <assert.h>
#define DRAM_ATTR
#define IRAM_ATTR
#define ESP_LOGW(...) ((void)0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned held;
#define portENTER_CRITICAL(m) do{(void)(m);assert(!held);held++;}while(0)
#define portEXIT_CRITICAL(m) do{(void)(m);assert(held==1);held--;}while(0)
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)
typedef int esp_err_t;
enum {ESP_OK=0,ERROR=17,PPA_TRANS_MODE_BLOCKING=0,PPA_OPERATION_SRM=1,PPA_DATA_BURST_LENGTH_128=128};
typedef void *ppa_client_handle_t;
typedef struct {unsigned unused;} ppa_event_data_t;
typedef bool (*ppa_event_callback_t)(ppa_client_handle_t,ppa_event_data_t*,void*);
typedef struct {ppa_event_callback_t on_trans_done;} ppa_event_callbacks_t;
typedef struct {int oper_type,max_pending_trans_num,data_burst_length;} ppa_client_config_t;
typedef struct {int mode,rotation;void *in,*out,*user_data;} ppa_srm_oper_config_t;
static uint64_t clock_us;
static int64_t esp_timer_get_time(void){return (int64_t)clock_us;}
static bool g_ppa_init_attempted,g_native_framebuffer_ppa_compatible=true;
static int client_result,registration_result,transaction_result,behavior;
static unsigned calls,registrations;
static ppa_event_callback_t callback;
static void *late_cookie;
static unsigned input[8],output[8];
static esp_err_t ppa_register_client(const ppa_client_config_t *config,ppa_client_handle_t *out){
  assert(!held&&config->oper_type==PPA_OPERATION_SRM&&config->max_pending_trans_num==1&&config->data_burst_length==128);
  if(client_result)return client_result;
  *out=(void*)(uintptr_t)1;
  return ESP_OK;
}
static esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t client,const ppa_event_callbacks_t *cbs){
  assert(!held&&client==(void*)(uintptr_t)1);registrations++;
  if(registration_result)return registration_result;
  callback=cbs->on_trans_done;
  return ESP_OK;
}
static esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client,const ppa_srm_oper_config_t *config){
  assert(!held&&client==(void*)(uintptr_t)1);
  assert(config->mode==PPA_TRANS_MODE_BLOCKING&&config->rotation==270&&config->in==input&&config->out==output);
  calls++;late_cookie=config->user_data;
  if(callback&&behavior==0){clock_us=150;assert(!callback(client,NULL,config->user_data));}
  clock_us=200;return transaction_result;
}
''' + adapter + '\nstatic void initialize(void){\n' + registration + '\n}\n' + r'''
static void reset(void){
  memset(&g_ppa_trace,0,sizeof(g_ppa_trace));g_ppa_init_attempted=false;
  g_ppa_srm_client=NULL;callback=NULL;clock_us=100;
  client_result=registration_result=transaction_result=behavior=0;
  calls=registrations=0;assert(!held);
}
int main(void){
  ppa_srm_oper_config_t config={.mode=PPA_TRANS_MODE_BLOCKING,.rotation=270,.in=input,.out=output};
  pet_ppa_trace_frame_t frame;
  reset();initialize();initialize();assert(registrations==1&&g_ppa_srm_client);
  ppa_trace_frame_begin();assert(ppa_trace_srm(PET_PPA_TRACE_CONVERT,&config)==ESP_OK);
  ppa_trace_frame_snapshot(&frame);assert(calls==1&&frame.op[1].api_us==100&&frame.op[1].call_to_callback_us==50&&frame.op[1].callback_to_return_us==50);
  assert(frame.op[1].flags&PET_PPA_TRACE_VALID_SPLIT);

  reset();registration_result=ERROR;initialize();assert(g_ppa_srm_client&&!callback);
  ppa_trace_frame_begin();assert(ppa_trace_srm(PET_PPA_TRACE_ROTATE,&config)==ESP_OK);
  ppa_trace_frame_snapshot(&frame);assert(calls==1&&(frame.op[2].flags&PET_PPA_TRACE_UNAVAILABLE));
  assert(!frame.op[2].api_us&&!frame.op[2].call_to_callback_us&&!frame.op[2].callback_to_return_us);

  reset();initialize();behavior=1;ppa_trace_frame_begin();
  assert(ppa_trace_srm(PET_PPA_TRACE_SCALE,&config)==ESP_OK);
  ppa_trace_frame_snapshot(&frame);assert(frame.op[0].flags&PET_PPA_TRACE_MISSING);
  clock_us=250;assert(!callback(g_ppa_srm_client,NULL,late_cookie));
  ppa_trace_frame_snapshot(&frame);assert((frame.op[0].flags&PET_PPA_TRACE_LATE)&&!(frame.op[0].flags&PET_PPA_TRACE_VALID_SPLIT));
  ppa_trace_frame_begin();clock_us=300;assert(!callback(g_ppa_srm_client,NULL,late_cookie));
  ppa_trace_frame_snapshot(&frame);assert(!frame.op[0].flags); /* Old frame cannot publish. */

  reset();initialize();behavior=1;transaction_result=ERROR;ppa_trace_frame_begin();
  assert(ppa_trace_srm(PET_PPA_TRACE_CONVERT,&config)==ERROR);
  ppa_trace_frame_snapshot(&frame);assert((frame.op[1].flags&PET_PPA_TRACE_ERROR)&&calls==1);
  reset();client_result=ERROR;initialize();assert(!g_ppa_srm_client&&!registrations);
  assert(!held);return 0;
}
'''
    compile_run(tmp_path, 'ppa_adapter', source)
