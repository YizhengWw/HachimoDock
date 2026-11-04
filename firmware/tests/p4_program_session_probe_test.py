"""Real native diagnostic authority, not arbitrary guest admission or P4 timing."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_real_broker_factory_session_group_scheduler_and_realtime_cancellation(tmp_path):
    names=('resources','resources_device','component_program_session','program_core','program_image',
           'program_runtime','program_host','program_host_device','program_group','program_scheduler','program_timers','event_bus','program_session_probe')
    binary=tmp_path/'session-device'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DPET_COMPONENT_PROGRAM_ENABLED=1',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/program_probe_stubs'),
        *[str(ROOT/'main'/f'pet_p4_{name}.c') for name in names],
        str(ROOT/'tests/p4_program_session_device_test.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_empty_request_entry_has_no_code_grant_state_or_effect_inputs(tmp_path):
    main=(ROOT/'main/pet_p4_main.c').read_text()
    handler='static bool maybe_handle_program_session_probe_line('+main.split('static bool maybe_handle_program_session_probe_line(',1)[1].split('static bool maybe_handle_program_probe_line(',1)[0]
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_program_probe.h"
typedef pet_program_probe_send_fn pet_p4_send_line_fn;
#define portMAX_DELAY 1
static int g_state_mutex=1,locked,called,replies;
static struct {bool asset_transfer_active;unsigned sentinel;} g_state={false,123};
static bool ota,reboot;
static const char *expected;
static int xSemaphoreTake(int lock,int wait){assert(lock==1&&wait==1&&!locked);locked=1;return 1;}
static void xSemaphoreGive(int lock){assert(lock==1&&locked);locked=0;}
static bool pet_p4_ota_transfer_active(void){assert(locked);return ota;}
static bool pet_p4_diagnostics_reboot_pending(void){assert(locked);return reboot;}
static void receive(const char*line,void*context){
  assert(!locked&&context==&called);cJSON*r=cJSON_Parse(line);assert(r);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(r,"topic")->valuestring,"debug/program_session_result"));
  const cJSON*p=cJSON_GetObjectItemCaseSensitive(r,"payload");
  if(expected)assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"error")->valuestring,expected));
  else assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok")));
  cJSON_Delete(r);replies++;
}
void pet_p4_program_session_probe(pet_program_probe_send_fn send,void*context){
  assert(!locked&&!g_state.asset_transfer_active&&!ota&&!reboot);called++;
  send("{\"topic\":\"debug/program_session_result\",\"payload\":{\"ok\":true}}",context);
}
'''+handler+r'''
int main(void){
  const char*valid="{\"topic\":\"debug/program_session_probe\",\"payload\":{}}";
  assert(!maybe_handle_program_session_probe_line(NULL,receive,&called));
  assert(!maybe_handle_program_session_probe_line("{\"topic\":\"debug/program_session_probe_more\",\"payload\":{}}",receive,&called));
  assert(maybe_handle_program_session_probe_line(valid,receive,&called)&&called==1&&replies==1);
  const char*invalid[]={
    "{\"topic\":\"debug/program_session_probe\"}",
    "{\"topic\":\"debug/program_session_probe\",\"payload\":[]}",
    "{\"topic\":\"debug/program_session_probe\",\"payload\":{\"code\":[]}}",
    "{\"topic\":\"debug/program_session_probe\",\"payload\":{\"grant\":1}}",
    "{\"topic\":\"debug/program_session_probe\",\"payload\":{\"state\":{}}}",
    "{\"topic\":\"debug/program_session_probe\",\"payload\":{},\"payload\":{}}",
    "{\"topic\":\"debug/program_session_probe\",\"payload\":{},\"extra\":1}"};
  expected="invalid_request";
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)assert(maybe_handle_program_session_probe_line(invalid[i],receive,&called));
  assert(called==1);
  assert(!maybe_handle_program_session_probe_line("{\"topic\":\"debug/program_session_probe\",\"payload\":{}}junk",receive,&called));
  expected="transfer_busy";
  for(unsigned i=0;i<3;i++){
    g_state.asset_transfer_active=i==0;ota=i==1;reboot=i==2;
    assert(maybe_handle_program_session_probe_line(valid,receive,&called));
  }
  assert(called==1&&!locked&&g_state.sentinel==123);return 0;
}
'''
    path=tmp_path/'entry.c';path.write_text(code);binary=tmp_path/'entry'
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
        '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(vendor),str(path),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
