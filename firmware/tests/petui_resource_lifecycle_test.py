"""Compile the production host transport; no device, audio or user storage."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_host_lifecycle_revokes_reply_and_action_authority(tmp_path):
    (tmp_path / 'freertos').mkdir()
    (tmp_path / 'freertos/FreeRTOS.h').write_text(
        'typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n'
        '#define portENTER_CRITICAL(x) ((void)(x))\n'
        '#define portEXIT_CRITICAL(x) ((void)(x))\n')
    # Even a repeated RNG value must not reuse an instance in the current boot.
    (tmp_path / 'esp_random.h').write_text(
        '#include <stdint.h>\nstatic uint32_t esp_random(void){return 1234;}\n')
    header = '''#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#define PET_P4_DEVICE_ID_MAX 64
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char widget_id[48],eyebrow[96],headline[160],metric_value[64],metric_label[96],metric_unit[32],badge[16],note[160],footer[160];uint32_t revision;} pet_p4_miniapp_view_t;
bool pet_app_host_enabled(const char*,const char*,uint64_t);
'''
    code = (ROOT / 'main/pet_p4_app_host.c').read_text().replace(
        '#include "pet_p4_app_host.h"', header)
    code += r'''
#include <assert.h>
static char sent[2048];static unsigned sends;
static void send(const char *line,void *ctx){(void)ctx;snprintf(sent,sizeof(sent),"%s",line);sends++;}
static cJSON *reply_from_last_event(void){
  cJSON *event=cJSON_Parse(sent),*p=cJSON_GetObjectItemCaseSensitive(event,"payload");
  cJSON *reply=cJSON_CreateObject();
  cJSON_AddStringToObject(reply,"componentId",str(p,"componentId"));
  cJSON_AddStringToObject(reply,"instance",str(p,"instance"));
  cJSON_AddNumberToObject(reply,"sequence",cJSON_GetObjectItemCaseSensitive(p,"sequence")->valuedouble);
  cJSON *v=cJSON_AddObjectToObject(reply,"values");cJSON_AddStringToObject(v,"headline","current");
  cJSON *vars=cJSON_AddObjectToObject(reply,"variables");cJSON_AddNumberToObject(vars,"value",5);
  cJSON_Delete(event);return reply;
}
int main(void){
  pet_app_host_transport(send,NULL,"board");
  pet_app_host_tick("component",1,0,true,0);assert(sends==1); // boot must not wait 2 s
  pet_resource_usage_t usage;pet_resources_device_usage(&usage);
  assert(usage.leases[PET_RESOURCE_HOST_VIEW]==1&&usage.live_owners==1);
  pet_resource_view_session_t first_view=resource_view;
  cJSON *old=reply_from_last_event();assert(pet_app_host_apply(old,1));
  assert(!pet_app_host_apply(old,9999));assert(last_reply==1); // duplicate status is not a heartbeat
  uint64_t epoch=pet_app_host_variable_epoch("component");assert(epoch>0);
  assert(pet_app_host_variable_epoch("other")==0);
  char value[64];int32_t number;
  assert(pet_app_host_variable("component","value",true,value,&number,2)&&number==5);
  const char integer_names[2][32]={"value","absent"};int32_t captured_values[8]={99,98,97,96,95,94,93,92};
  assert(!pet_app_host_integer_snapshot("component",integer_names,2,captured_values,2)&&captured_values[0]==99&&captured_values[1]==98);
  assert(pet_app_host_integer_snapshot("component",integer_names,1,captured_values,2)&&captured_values[0]==5&&captured_values[1]==0);
  captured_values[0]=99;assert(!pet_app_host_integer_snapshot("component",integer_names,1,captured_values,10002)&&captured_values[0]==99);
  uint32_t old_owner=incarnation;
  pet_app_host_tick("",0,0,false,3);assert(sends==2);
  pet_resources_device_usage(&usage);assert(usage.leases[PET_RESOURCE_HOST_VIEW]==0);
  assert(!pet_resources_device_view_current(&first_view));
  cJSON *exit=cJSON_Parse(sent),*p=cJSON_GetObjectItemCaseSensitive(exit,"payload");
  assert(!strcmp(str(p,"instance"),str(old,"instance"))); // exit addresses its owner
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p,"visible")));cJSON_Delete(exit);
  assert(!pet_app_host_apply(old,4));
  assert(pet_app_host_variable_epoch("component")>epoch); // clearing variables revokes gestures
  assert(!pet_app_host_variable("component","value",true,value,&number,4));
  assert(!pet_app_host_enabled("component","host.increment",4));
  pet_app_host_tick("",0,0,false,5);assert(sends==2); // no hidden poll flood
  pet_app_host_tick("component",1,0,true,6);assert(sends==3);
  assert(resource_view.owner.generation!=first_view.owner.generation);
  pet_resources_device_view_end(&first_view); // late cleanup cannot stop reentry
  assert(pet_resources_device_view_current(&resource_view));
  cJSON *current=reply_from_last_event();assert(strcmp(str(old,"instance"),str(current,"instance")));
  assert(!pet_app_host_apply(old,7));assert(pet_app_host_apply(current,7));
  assert(!pet_app_host_apply(current,100));assert(last_reply==7);
  assert(!request("component",1,0,"status",8,false,NULL,old_owner,NULL)); // delayed exit
  assert(pet_app_host_variable("component","value",true,value,&number,8));
  assert(!request("other",1,0,"host.increment",9,true,NULL,0,NULL)); // action cannot mount
  pet_app_host_tick("other",2,0,true,10);assert(!pet_app_host_apply(current,11));
  cJSON *other=reply_from_last_event();assert(pet_app_host_apply(other,11));
  pet_app_host_transport(send,(void*)1,"board");assert(!pet_app_host_apply(other,12));
  pet_resources_device_usage(&usage);assert(usage.leases[PET_RESOURCE_HOST_VIEW]==0);
  pet_app_host_tick("other",2,0,true,13);cJSON *replacement=reply_from_last_event();
  assert(strcmp(str(other,"instance"),str(replacement,"instance")));assert(pet_app_host_apply(replacement,14));
  sequence=UINT32_MAX;
  pet_app_host_tick("other",2,0,true,15);cJSON *wrapped=reply_from_last_event();
  assert(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence")->valuedouble==1);
  assert(!pet_app_host_apply(replacement,16));assert(pet_app_host_apply(wrapped,16));
  assert(!pet_app_host_apply(wrapped,9999));assert(last_reply==16);
  epoch=pet_app_host_variable_epoch("other");
  // Changed-away-and-back snapshots are both observed even without any render.
  cJSON *vars=cJSON_GetObjectItemCaseSensitive(wrapped,"variables");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(vars,"value"),6);
  assert(pet_app_host_apply(wrapped,17));assert(pet_app_host_variable_epoch("other")==epoch+1);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(vars,"value"),5);
  assert(pet_app_host_apply(wrapped,18));assert(pet_app_host_variable_epoch("other")==epoch+2);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,19));assert(pet_app_host_variable_epoch("other")==epoch+2);
  cJSON_AddStringToObject(vars,"label","stable");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,20));epoch=pet_app_host_variable_epoch("other");
  cJSON_DeleteItemFromObjectCaseSensitive(vars,"value");cJSON_AddNumberToObject(vars,"value",5);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,21));assert(pet_app_host_variable_epoch("other")==epoch); // object order is not a change
  cJSON_DeleteItemFromObjectCaseSensitive(vars,"value");cJSON_AddStringToObject(vars,"value","5");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,22));assert(pet_app_host_variable_epoch("other")==epoch+1); // type is part of state
  // Real host: a row reply changes after UP, before the guarded request's
  // admission. Old observations must be consumed without emitting an event.
  cJSON *ui=cJSON_AddObjectToObject(wrapped,"ui");cJSON_AddNumberToObject(ui,"mode",0);
  cJSON *actions=cJSON_AddArrayToObject(ui,"actions");cJSON_AddItemToArray(actions,cJSON_CreateString("host.row"));
  cJSON_AddStringToObject(ui,"context","aaa");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,23));
  pet_host_observation_t observed;assert(pet_app_host_observe("other",&observed));
  assert(pet_app_host_observation_current("other",&observed));
  cJSON_ReplaceItemInObjectCaseSensitive(ui,"context",cJSON_CreateString("bbb"));
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,24));unsigned before=sends;
  assert(pet_app_host_observed_action("other",2,0,"host.row",&observed,200));assert(sends==before);
  assert(!pet_app_host_observation_current("other",&observed));
  assert(pet_app_host_observe("other",&observed));
  assert(pet_app_host_observed_action("other",2,0,"host.row",&observed,201));assert(sends==before+1);
  cJSON *guarded=cJSON_Parse(sent),*gp=cJSON_GetObjectItemCaseSensitive(guarded,"payload");
  assert(!strcmp(str(gp,"context"),"bbb")&&!cJSON_HasObjectItem(gp,"confirmed"));cJSON_Delete(guarded);
  // Reply action availability is rechecked atomically even if an earlier
  // public enabled() call succeeded. No stale UI action can reserve sequence.
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),sequence);
  assert(pet_app_host_apply(wrapped,202));assert(pet_app_host_enabled("other","host.row",203));
  cJSON_DeleteItemFromArray(actions,0);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(wrapped,"sequence"),++sequence);
  assert(pet_app_host_apply(wrapped,204));before=sends;
  assert(!request("other",2,0,"host.row",400,true,NULL,0,NULL));assert(sends==before);
  // Returning identical text/context after reentry is still a new lifetime.
  pet_app_host_tick("",0,0,false,401);pet_app_host_tick("other",2,0,true,402);
  assert(!pet_app_host_observation_current("other",&observed));before=sends;
  assert(pet_app_host_observed_action("other",2,0,"host.row",&observed,403));assert(sends==before);
  pet_host_observation_t missing=observed;assert(!pet_app_host_observe("absent",&missing)&&!missing.valid);
  pet_app_host_tick("",0,0,false,404);pet_resources_device_usage(&usage);
  assert(usage.live_owners==0&&usage.revoked_owners==0&&usage.leases[PET_RESOURCE_HOST_VIEW]==0);
  cJSON_Delete(old);cJSON_Delete(current);cJSON_Delete(other);cJSON_Delete(replacement);cJSON_Delete(wrapped);
  return 0;
}
'''
    source, executable = tmp_path / 'lifecycle.c', tmp_path / 'lifecycle'
    source.write_text(code)
    vendor = ROOT / 'tests/vendor/cjson'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-error=deprecated-declarations', '-fsanitize=address,undefined',
                    '-I', str(tmp_path), '-I', str(vendor), '-I', str(ROOT / 'main'), str(source),
                    str(ROOT / 'main/pet_p4_resources.c'), str(ROOT / 'main/pet_p4_resources_device.c'),
                    str(vendor / 'cJSON.c'), '-lm', '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
