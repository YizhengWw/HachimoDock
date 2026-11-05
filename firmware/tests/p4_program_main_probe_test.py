"""Actual incremental main-owned native job. Host clocks/audio are adapters,
not P4 timing, acoustic evidence or permission to install arbitrary programs.
"""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_main_owner_one_quantum_retirement_clock_audio_and_no_replay(tmp_path):
    code=r'''
#include "pet_p4_component_program_session.h"
#include "pet_p4_resources_device.h"
#include <assert.h>
#include <string.h>
static int64_t clock_us=1;
static bool audio,clock_failed;
static unsigned scheduler_calls,replies;
static void *task=(void*)1;
int64_t esp_timer_get_time(void){return clock_failed?-1:clock_us++;}
void *xTaskGetCurrentTaskHandle(void){return task;}
void vTaskDelay(uint32_t ticks){(void)ticks;assert(!"main job must not sleep");}
unsigned uxTaskGetStackHighWaterMark(void *value){assert(!value);return 2048;}
bool pet_p4_audio_active(void){return audio;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
static pet_program_schedule_result_t watched_step(pet_program_scheduler_t*s,uint64_t now,pet_program_schedule_diagnostic_t*d){
 scheduler_calls++;return pet_program_scheduler_step(s,now,d);
}
#define pet_program_scheduler_step watched_step
#include "pet_p4_program_main_probe.c"
#undef pet_program_scheduler_step
static char reply[1024];
static void receive(const char*line,void*context){
 assert(context==reply&&strlen(line)<sizeof(reply));strcpy(reply,line);replies++;
 assert(!main_inside);
 unsigned before=scheduler_calls;pet_p4_program_main_probe_reply();assert(scheduler_calls==before);
}
static void turn(void){
 unsigned before=scheduler_calls;pet_p4_program_main_probe_process();assert(scheduler_calls<=before+1);clock_us+=10000;
 /* Main cannot deliver even after completion; the protocol worker collects. */
 unsigned prior=replies;pet_p4_program_main_probe_reply();if(task==(void*)1)assert(replies==prior);
 if(task==(void*)1){task=(void*)2;pet_p4_program_main_probe_reply();task=(void*)1;}
}
static void drain(unsigned before){for(unsigned n=0;n<200&&replies==before;n++)turn();assert(replies==before+1);}
static void clean(void){pet_resource_usage_t u;pet_resources_device_usage(&u);assert(!u.live_owners&&!u.revoked_owners&&!u.leases[PET_RESOURCE_NATIVE_PROBE]);assert(!pet_resources_limit(PET_RESOURCE_PROGRAM).leases);}
int main(void){
 pet_p4_program_main_probe(receive,reply);assert(replies==1&&strstr(reply,"unavailable"));clean();
 assert(pet_p4_program_main_probe_bind_owner());assert(!pet_p4_program_main_probe_bind_owner());
 unsigned before=replies;pet_p4_program_main_probe(receive,reply);
 assert(replies==before&&!main_session.host.instance); /* Request cannot run a VM. */
 task=(void*)2;turn();assert(!main_session.host.instance&&replies==before);
 assert(!pet_p4_program_main_probe_bind_owner());task=(void*)1;
 pet_p4_program_main_probe(receive,reply);assert(replies==before+1&&strstr(reply,"busy"));
 before=replies;drain(before);
 assert(strstr(reply,"\"ok\":true")&&strstr(reply,"\"samples\":8")&&strstr(reply,"\"timerEvents\":1"));
 assert(strstr(reply,"\"executionTask\":\"main\"")&&strstr(reply,"\"sharedDispatcher\":true")&&strstr(reply,"\"guestAdmission\":false"));
 assert(main_job.pumps==95&&main_job.turns>main_job.pumps&&scheduler_calls==95);
 assert(main_job.max_images==2&&main_job.fuel_cases==1&&strstr(reply,"\"maxImageCases\":2"));clean();
 before=replies;turn();assert(replies==before&&scheduler_calls==95);
 pet_p4_program_main_probe(receive,reply);assert(strstr(reply,"rate_limited"));
 clock_us+=6000000;before=replies;pet_p4_program_main_probe(receive,reply);
 for(unsigned n=0;n<3;n++)turn();
 audio=true;
 drain(before);
 audio=false;
 assert(strstr(reply,"audio_busy")&&!atomic_load(&main_active));clean();
 clock_us+=6000000;before=replies;pet_p4_program_main_probe(receive,reply);
 for(unsigned n=0;n<3;n++)turn();
 assert(main_session.host.bound);
 main_session.host.inside=true;audio=true;turn();
 assert(replies==before&&atomic_load(&main_active)&&main_session.host.bound);
 pet_p4_program_main_probe(receive,reply);assert(strstr(reply,"busy"));before=replies;
 main_session.host.inside=false;drain(before);audio=false;assert(strstr(reply,"audio_busy"));clean();
 clock_us+=6000000;before=replies;pet_p4_program_main_probe(receive,reply);
 for(unsigned n=0;n<3;n++)turn();
 clock_failed=true;
 drain(before);
 clock_failed=false;
 assert(strstr(reply,"clock_error"));clean();
 clock_us+=6000000;before=replies;pet_p4_program_main_probe(receive,reply);drain(before);
 assert(strstr(reply,"\"ok\":true"));clean();
 clock_us+=6000000;before=replies;pet_p4_program_main_probe(receive,reply);
 for(unsigned n=0;n<200&&!(main_job.kind==2&&main_session.host.bound);n++)turn();
 assert(main_job.kind==2&&main_session.host.bound&&main_job.max_images==1);
 main_session.host.runtime.state.cells[63]=7; /* Fault, not a user-data reset. */
 drain(before);assert(strstr(reply,"case_result_mismatch"));
 assert(main_session.host.runtime.state.cells[63]==7);clean();return 0;
}
'''
    path=tmp_path/'main-probe.c';path.write_text(code);binary=tmp_path/'main-probe'
    names=('resources','resources_device','component_program_session','program_core','program_image','program_runtime','program_host','program_host_device','program_group','program_scheduler','program_timers','event_bus')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DPET_COMPONENT_PROGRAM_ENABLED=1',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),str(path),
        *[str(ROOT/'main'/f'pet_p4_{name}.c') for name in names],'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_main_binding_and_process_run_outside_ui_lock(tmp_path):
    source=(ROOT/'main/pet_p4_main.c').read_text()
    app=source.split('void app_main(void) {',1)[1]
    assert app.index('pet_p4_program_main_probe_bind_owner()')<app.index('protocol_worker_task,')
    assert source.count('pet_p4_program_main_probe_process();')==1
    # The default-off passive scope cleanup sits AFTER the real unlock. Keep
    # the original adjacency assertion after removing ONLY that exact known
    # hook, and compile the unmodified real boundary under all four gates.
    hook='#if PET_P4_RESET_PHASE_TRACE\n    pet_reset_phase_leave(&main_state_phase);\n#endif\n'
    assert app.count(hook)==1
    assert 'xSemaphoreGive(g_state_mutex);\n#if PET_COMPONENT_PROGRAM_ENABLED\n    pet_p4_program_main_probe_process();' in app.replace(hook,'')
    call=app.index('pet_p4_program_main_probe_process();')
    begin=app.rindex('xSemaphoreGive(g_state_mutex);',0,call)
    end=app.index('\n#endif',call)+len('\n#endif')
    boundary=app[begin:end]
    code=r'''
#include <assert.h>
static int g_state_mutex=1,held=1,processed,cleaned;
static void xSemaphoreGive(int lock){assert(lock==1&&held);held=0;}
#if PET_P4_RESET_PHASE_TRACE
static int main_state_phase;
static void pet_reset_phase_leave(int *scope){assert(scope==&main_state_phase&&!held);cleaned++;}
#endif
#if PET_COMPONENT_PROGRAM_ENABLED
static void pet_p4_program_main_probe_process(void){assert(!held&&cleaned==PET_P4_RESET_PHASE_TRACE);processed++;}
#endif
static void dispatch(void){
'''+boundary+r'''
}
int main(void){dispatch();assert(!held&&processed==PET_COMPONENT_PROGRAM_ENABLED&&cleaned==PET_P4_RESET_PHASE_TRACE);return 0;}
'''
    path=tmp_path/'actual-main-unlock.c';path.write_text(code)
    for trace in (0,1):
        for program in (0,1):
            binary=tmp_path/f'unlock-{trace}-{program}'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                f'-DPET_P4_RESET_PHASE_TRACE={trace}',f'-DPET_COMPONENT_PROGRAM_ENABLED={program}',
                str(path),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
    job=(ROOT/'main/pet_p4_program_main_probe.c').read_text()
    for forbidden in ('malloc(', 'fopen(', 'vTaskDelay(', 'gpio_', 'i2c_', 'music_begin(', 'capture_start('):
        assert forbidden not in job
    assert 'xTaskGetCurrentTaskHandle()!=main_owner' in job
    assert 'PET_RESOURCE_PROGRAM' not in job
    process=job.split('void pet_p4_program_main_probe_process(void){',1)[1].split('bool pet_p4_program_main_probe_pending',1)[0]
    assert 'main_send(' not in process and 'snprintf(' not in process
    assert 'pet_p4_program_main_probe_reply();' in source.split('static void protocol_worker_task',1)[1].split('static bool consume_byte',1)[0]

def test_actual_wire_entry_retains_only_static_routes_and_rejects_extra_inputs(tmp_path):
    source=(ROOT/'main/pet_p4_main.c').read_text()
    handler='static bool maybe_handle_program_main_probe_line('+source.split('static bool maybe_handle_program_main_probe_line(',1)[1].split('static bool maybe_handle_program_session_probe_line(',1)[0]
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_program_probe.h"
typedef pet_program_probe_send_fn pet_p4_send_line_fn;
#define portMAX_DELAY 1
static int g_state_mutex=1,locked,called,replies,g_uart_route,g_usb_serial_jtag_route,temporary;
static struct {bool asset_transfer_active;unsigned sentinel;} g_state={false,123};
static bool ota,reboot;
static const char *expected;
static void*expected_context;
static int xSemaphoreTake(int lock,int wait){assert(lock==1&&wait==1&&!locked);locked=1;return 1;}
static void xSemaphoreGive(int lock){assert(lock==1&&locked);locked=0;}
static bool pet_p4_ota_transfer_active(void){assert(locked);return ota;}
static bool pet_p4_diagnostics_reboot_pending(void){assert(locked);return reboot;}
static void receive(const char*line,void*context){
 assert(!locked&&context==expected_context);cJSON*r=cJSON_Parse(line);assert(r);
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(r,"topic")->valuestring,"debug/program_main_result"));
 const cJSON*p=cJSON_GetObjectItemCaseSensitive(r,"payload");
 if(expected)assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"error")->valuestring,expected));
 cJSON_Delete(r);replies++;
}
static void transport_send_line(const char*line,void*context){receive(line,context);}
static void pet_p4_native_usb_send_json_line(const char*line,void*context){receive(line,context);}
void pet_p4_program_main_probe(pet_program_probe_send_fn send,void*context){
 assert(!locked&&!g_state.asset_transfer_active&&!ota&&!reboot);
 assert((send==transport_send_line&&(!context||context==&g_uart_route||context==&g_usb_serial_jtag_route))
  ||(send==pet_p4_native_usb_send_json_line&&!context));called++;
 /* Request only retains an accepted static route; no result is sent here. */
}
'''+handler+r'''
int main(void){
 const char*valid="{\"topic\":\"debug/program_main_probe\",\"payload\":{}}";
 assert(!maybe_handle_program_main_probe_line(NULL,transport_send_line,NULL));
 assert(!maybe_handle_program_main_probe_line(valid,NULL,NULL));
 assert(!maybe_handle_program_main_probe_line("{\"topic\":\"debug/program_main_probe_more\",\"payload\":{}}",transport_send_line,NULL));
 void*routes[]={NULL,&g_uart_route,&g_usb_serial_jtag_route};
 for(unsigned i=0;i<3;i++)assert(maybe_handle_program_main_probe_line(valid,transport_send_line,routes[i]));
 assert(maybe_handle_program_main_probe_line(valid,pet_p4_native_usb_send_json_line,NULL));
 assert(called==4&&!replies);
 expected="unsupported_reply";expected_context=&temporary;
 assert(maybe_handle_program_main_probe_line(valid,transport_send_line,&temporary));
 assert(maybe_handle_program_main_probe_line(valid,pet_p4_native_usb_send_json_line,&temporary));
 expected_context=NULL;assert(maybe_handle_program_main_probe_line(valid,receive,NULL));
 expected="invalid_request";
 const char*bad[]={"{\"topic\":\"debug/program_main_probe\"}",
 "{\"topic\":\"debug/program_main_probe\",\"payload\":[]}",
 "{\"topic\":\"debug/program_main_probe\",\"payload\":{\"code\":[]}}",
 "{\"topic\":\"debug/program_main_probe\",\"payload\":{\"grant\":1}}",
 "{\"topic\":\"debug/program_main_probe\",\"payload\":{},\"payload\":{}}",
 "{\"topic\":\"debug/program_main_probe\",\"payload\":{},\"extra\":1}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(maybe_handle_program_main_probe_line(bad[i],transport_send_line,NULL));
 assert(!maybe_handle_program_main_probe_line("{\"topic\":\"debug/program_main_probe\",\"payload\":{}}junk",transport_send_line,NULL));
 expected="transfer_busy";
 for(unsigned i=0;i<3;i++){
  g_state.asset_transfer_active=i==0;ota=i==1;reboot=i==2;
  assert(maybe_handle_program_main_probe_line(valid,transport_send_line,NULL));
 }
 assert(called==4&&replies==12&&!locked&&g_state.sentinel==123);return 0;
}
'''
    path=tmp_path/'entry.c';path.write_text(code);binary=tmp_path/'entry';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(vendor),str(path),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_normal_build_stub_has_no_background_work(tmp_path):
    code=r'''
#include "pet_p4_program_probe.h"
#include <assert.h>
#include <string.h>
static unsigned replies;
static void receive(const char*line,void*context){assert(!context&&strstr(line,"unavailable"));replies++;}
int main(void){assert(!pet_p4_program_main_probe_bind_owner());
 pet_p4_program_main_probe(receive,0);pet_p4_program_main_probe_process();
 pet_p4_program_main_probe_reply();assert(replies==1&&!pet_p4_program_main_probe_pending());return 0;}
'''
    path=tmp_path/'stub.c';path.write_text(code);binary=tmp_path/'stub'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(path),
        str(ROOT/'main/pet_p4_program_main_probe.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
