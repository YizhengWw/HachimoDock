"""Actual bounded native hello work; no device, network, audio or SDK edits."""
from pathlib import Path
import configparser
import subprocess
from p4_control_integrity_test import _c_function as function

ROOT=Path(__file__).resolve().parents[1]

def test_actual_hello_queue_coalesces_backpressure_and_never_sends_under_state_lock(tmp_path):
    text=(ROOT/'main/pet_p4_main.c').read_text()
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define pdTRUE 1
typedef void(*send_fn)(const char*,void*);
typedef struct{char *line;send_fn send_line;void *reply_ctx;uint64_t activation_token;bool unsolicited_hello;}pet_p4_protocol_request_t;
static int g_protocol_queue,g_rx_health_mutex;
static bool g_hello_pending,fail_queue,state_held,inject;
static unsigned mux,queued,sent;static char g_hello_board_id[64]="native-board";
static void enter(int *m){assert(m==&g_rx_health_mutex&&!mux);mux++;}
static void leave(int *m){assert(m==&g_rx_health_mutex&&mux==1);mux--;}
#define portENTER_CRITICAL(m) enter(m)
#define portEXIT_CRITICAL(m) leave(m)
static int xQueueSend(int q,const pet_p4_protocol_request_t *r,unsigned wait){
 assert(q==1&&!wait&&!mux&&state_held&&g_hello_pending);
 assert(r->unsolicited_hello&&!r->line&&!r->send_line&&!r->reply_ctx&&!r->activation_token);
 queued++;return fail_queue?0:pdTRUE;
}
static void transport_send_line(const char *s,void *ctx){assert(s&&!ctx&&!state_held&&!mux);sent++;}
static bool enqueue_unsolicited_hello(void);
static void pet_p4_send_hello_for_board(const char *id,send_fn send,void *ctx){
 assert(!state_held&&!mux&&g_hello_pending&&!strcmp(id,"native-board"));
 if(inject){state_held=true;assert(enqueue_unsolicited_hello());state_held=false;}
 send("full-immutable-hello",ctx);
}
'''+function(text,'enqueue_unsolicited_hello')+'\n'+function(text,'run_unsolicited_hello')+r'''
int main(void){
 state_held=true;assert(!enqueue_unsolicited_hello()&&!g_hello_pending&&!queued&&!sent);
 g_protocol_queue=1;fail_queue=true;
 assert(!enqueue_unsolicited_hello()&&!g_hello_pending&&queued==1&&!sent);
 fail_queue=false;assert(enqueue_unsolicited_hello()&&g_hello_pending&&queued==2&&!sent);
 for(unsigned i=0;i<100;i++){assert(enqueue_unsolicited_hello());}assert(queued==2&&!sent);
 state_held=false;inject=true;run_unsolicited_hello();assert(sent==1&&queued==2&&!g_hello_pending&&!mux);
 state_held=true;assert(enqueue_unsolicited_hello()&&queued==3&&sent==1);
 state_held=false;inject=false;run_unsolicited_hello();assert(sent==2&&!g_hello_pending&&!mux);
 return 0;
}
'''
    src,exe=tmp_path/'hello.c',tmp_path/'hello'
    src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

def test_async_hello_retains_full_payload_identity_readiness_and_existing_tasks():
    source=(ROOT/'main/pet_p4_protocol.c').read_text()
    builder=function(source,'pet_p4_send_hello_for_board')
    wrapper=function(source,'pet_p4_send_hello')
    assert 'state->' not in builder and '"boardDeviceId", board_id ? board_id : "p4-unknown"' in builder
    assert 'state ? state->board_device_id : NULL' in wrapper
    assert 'send_topic_payload(send_line, ctx, "hello", payload)' in builder
    for field in ('"audioAec"','"audioVad"','"audioFullDuplex"','"widgetVariableOwners"'):
        assert field in builder
    main=(ROOT/'main/pet_p4_main.c').read_text()
    worker=function(main,'protocol_worker_task');run=function(main,'run_unsolicited_hello');enqueue=function(main,'enqueue_unsolicited_hello')
    assert worker.index('if(request.unsolicited_hello)')<worker.index('protocol_debug_source_bounded(request.line)')
    assert all(s not in run for s in ('xSemaphoreTake','g_state','malloc','xTaskCreate','vTaskPrioritySet'))
    assert 'xQueueSend(g_protocol_queue,&request,0)' in enqueue and 'transport_send_line(' not in enqueue
    boot=function(main,'app_main')
    assert boot.index('snprintf(g_hello_board_id')<boot.index('protocol_worker_task,')
    assert boot.index('if (pet_p4_audio_ready() && pet_p4_audio_playback_ready()) (void)pet_p4_afe_init();')<boot.index('if(enqueue_unsolicited_hello())')
    assert boot.index('pet_p4_lcd_scan_i2c(i2c_devices,sizeof(i2c_devices))')<boot.index('PET_BOOT_HARDWARE_DONE')<boot.index('PET_BOOT_FIRST_FRAME')<boot.index('PET_BOOT_TRANSPORT_STARTED')
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    p=config['env:esp32_p4_startup_parent']
    assert p['extends']=='env:esp32_p4_prefetch_parent'
    assert p['custom_p4_project_version']=='0.7.103-p4-r114-startup'
    assert set(p['build_flags'].splitlines()[2:])=={'-DPET_P4_HELLO_ASYNC=1','-DPET_P4_SPARE_RECREATE=1','-DPET_P4_APP_PHASE_TRACE=1'}
    for flag in ('PET_P4_HELLO_ASYNC','PET_P4_SPARE_RECREATE','PET_P4_APP_PHASE_TRACE'):
        headers=''.join((ROOT/'main'/name).read_text() for name in ('pet_p4_query_reply.h','pet_p4_storage_io_trace.h','pet_p4_miniapp.h'))
        assert f'#define {flag} 0' in headers
