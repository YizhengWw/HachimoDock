"""Run the real app transport with scheduler adapters; no device or microphone."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_serial_actions_share_the_device_monotonic_clock():
    protocol = (ROOT / 'main/pet_p4_protocol.c').read_text()
    action = protocol.split('strcmp(topic, "miniapp/event") == 0', 1)[1].split(
        'strcmp(topic, "miniapp/query") == 0', 1)[0]
    assert 'pet_p4_miniapp_dispatch_action(action, esp_timer_get_time() / 1000ULL)' in action
    assert 'json_u64(payload, "tsMs"' not in action

def test_host_identity_actions_and_stale_receipts(tmp_path):
    (tmp_path/'freertos').mkdir()
    (tmp_path/'freertos/FreeRTOS.h').write_text('typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n')
    (tmp_path/'esp_random.h').write_text('#include <stdint.h>\nstatic uint32_t esp_random(void){return 1234;}\n')
    header='''#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#define PET_P4_DEVICE_ID_MAX 64
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char widget_id[48],eyebrow[96],headline[160],metric_value[64],metric_label[96],metric_unit[32],badge[16],note[160],footer[160];uint32_t revision;} pet_p4_miniapp_view_t;
bool pet_app_host_enabled(const char*,const char*,uint64_t);
'''
    code=(ROOT/'main/pet_p4_app_host.c').read_text().replace('#include "pet_p4_app_host.h"',header)
    code+='''
#include <assert.h>
static char sent[2048];static int count;
static void send(const char *line,void *ctx){(void)ctx;snprintf(sent,sizeof(sent),"%s",line);count++;}
int main(void){
 pet_app_host_transport(send,NULL,"test-board");
 pet_app_host_tick("test-app",1,0,true,2100);assert(count==1);
 cJSON *j=cJSON_Parse(sent),*p=cJSON_GetObjectItemCaseSensitive(j,"payload");
 assert(!strcmp(str(p,"boardDeviceId"),"test-board"));assert(!strcmp(str(p,"componentId"),"test-app"));
 char token[32];snprintf(token,sizeof(token),"%s",str(p,"instance"));cJSON_Delete(j);
 assert(pet_app_host_action("test-app",1,0,"host.next",2200));assert(count==2);
 assert(pet_app_host_action("test-app",1,0,"host.next",2210));assert(count==2);
 j=cJSON_CreateObject();cJSON_AddStringToObject(j,"componentId","test-app");cJSON_AddStringToObject(j,"instance",token);cJSON_AddNumberToObject(j,"sequence",1);cJSON_AddObjectToObject(j,"values");
 assert(!pet_app_host_apply(j,2300)); // old status cannot overwrite a newer action
 cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j,"sequence"),2);assert(pet_app_host_apply(j,2300));
 cJSON *ui=cJSON_AddObjectToObject(j,"ui");cJSON_AddNumberToObject(ui,"mode",5);
 cJSON *a=cJSON_AddArrayToObject(ui,"actions");cJSON_AddItemToArray(a,cJSON_CreateString("host.delete"));
 cJSON_AddStringToObject(ui,"context","abc123");assert(pet_app_host_apply(j,2400));
 assert(pet_app_host_mode("test-app",2401)==5);
 assert(!pet_app_host_enabled("test-app","host.next",2401));
 assert(pet_app_host_enabled("test-app","host.delete",2401));
 char context[65];pet_app_host_context("test-app",context);assert(!strcmp(context,"abc123"));
 assert(pet_app_host_confirmed_action("test-app",1,0,"host.delete",context,2402));
 cJSON *event=cJSON_Parse(sent),*ep=cJSON_GetObjectItemCaseSensitive(event,"payload");
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ep,"confirmed")));assert(!strcmp(str(ep,"context"),"abc123"));cJSON_Delete(event);
 assert(pet_app_host_mode("test-app",2403)==6);assert(!pet_app_host_enabled("test-app","host.delete",2403));
 cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j,"sequence"),3);assert(pet_app_host_apply(j,2405));
 cJSON_AddItemToArray(a,cJSON_CreateString("host.delete"));assert(!pet_app_host_apply(j,2406));
 cJSON_DeleteItemFromArray(a,1);cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(ui,"mode"),8);assert(!pet_app_host_apply(j,2407));
 assert(pet_app_host_mode("test-app",13000)==7);assert(!pet_app_host_enabled("test-app","host.delete",13000));
 pet_app_host_tick("other-app",2,0,true,2500);assert(!pet_app_host_apply(j,2501));cJSON_Delete(j);
 return 0;
}
'''
    source=tmp_path/'host.c';source.write_text(code);exe=tmp_path/'host'
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(tmp_path),'-I',str(vendor),str(source),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
