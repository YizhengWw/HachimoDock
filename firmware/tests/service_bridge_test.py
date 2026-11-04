"""Shared bridge admission and real bounded C presentation core; no device/IO."""
from pathlib import Path
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
SCRIPTS=ROOT/'pc/skills/petui/scripts'
CONTRACT=ROOT/'pc/skills/petui/contracts/service-bridge-v1.json'

def test_python_import_uses_native_service_catalog_and_shared_rejections():
    sys.path.insert(0,str(SCRIPTS))
    from app_services import application_errors
    from service_bridge import binding_contract
    from app_surface import graphics_errors
    suite=json.loads(CONTRACT.read_text())
    assert not application_errors(suite['declaration'],suite['runtime'])
    assert not graphics_errors(suite['runtime']['surface'],suite['runtime'])
    for change in suite['reject']:
        candidate=json.loads(json.dumps(suite['declaration']))
        parts=change['pointer'].split('/')[1:];node=candidate
        for part in parts[:-1]:node=node[int(part)] if isinstance(node,list) else node[part]
        node[parts[-1]]=change['value']
        patched_runtime=json.loads(json.dumps(suite['runtime']));patched_runtime['host_contract']=binding_contract(candidate,patched_runtime)
        assert application_errors(candidate,patched_runtime),change
    stale=json.loads(json.dumps(suite['runtime']));stale['surface']['elements'][0]['x']+=1;assert application_errors(suite['declaration'],stale)
    runtime=json.loads(json.dumps(suite['runtime']));runtime['input']['confirm']={'host.level':'Confirm'}
    assert graphics_errors(runtime['surface'],runtime)

def test_real_c_core_timeline_vectors_and_image_bank_lifetimes(tmp_path):
    code=r'''
#include "pet_p4_service_bridge_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint16_t pixels[PET_BRIDGE_IMAGES*2*PET_BRIDGE_IMAGE_PIXELS];
int main(int argc,char**argv){
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);rewind(f);char*text=calloc((size_t)size+1,1);assert(text&&fread(text,1,(size_t)size,f)==(size_t)size);fclose(f);
 cJSON*suite=cJSON_Parse(text);free(text);assert(suite);const cJSON*vectors=cJSON_GetObjectItemCaseSensitive(suite,"timelineVectors"),*v;
 cJSON_ArrayForEach(v,vectors){
   cJSON*p=cJSON_CreateObject(),*times=cJSON_AddObjectToObject(p,"timelines");cJSON_AddItemToObject(times,"progress",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(v,"timeline"),true));cJSON_AddObjectToObject(p,"images");
   pet_bridge_presentation_t parsed;bool valid=pet_bridge_json_presentation(p,&parsed);int32_t projected=-1;
   bool applied=valid&&pet_bridge_timeline_project(&parsed.timelines,"progress",(uint64_t)cJSON_GetObjectItemCaseSensitive(v,"age")->valuedouble,&projected);
   const cJSON*expected=cJSON_GetObjectItemCaseSensitive(v,"value");if(cJSON_IsNull(expected))assert(!applied);else assert(applied&&projected==expected->valueint);cJSON_Delete(p);
 }
 pet_bridge_images_t images={0};pet_bridge_image_ref_t ref={.alias="cover",.key="asset-a",.revision="one",.width=8,.height=8};
 uint8_t data[128];memset(data,17,sizeof(data));pet_bridge_image_lease_t first,second;
 assert(pet_bridge_images_expect(&images,&ref,1));assert(pet_bridge_image_write(&images,&ref,0,data,64,pixels));assert(!pet_bridge_image_borrow(&images,"cover",pixels,&first));
 assert(!pet_bridge_image_write(&images,&ref,62,data,64,pixels));assert(pet_bridge_image_write(&images,&ref,64,data+64,64,pixels));assert(pet_bridge_image_borrow(&images,"cover",pixels,&first));assert(first.pixels[0]==0x1111);
 strcpy(ref.revision,"two");assert(pet_bridge_images_expect(&images,&ref,1));assert(!pet_bridge_image_borrow(&images,"cover",pixels,&second));memset(data,34,sizeof(data));assert(pet_bridge_image_write(&images,&ref,0,data,128,pixels));assert(first.pixels[0]==0x1111);assert(pet_bridge_image_borrow(&images,"cover",pixels,&second));assert(second.pixels[0]==0x2222);
 strcpy(ref.revision,"three");assert(pet_bridge_images_expect(&images,&ref,1));assert(!pet_bridge_image_write(&images,&ref,0,data,128,pixels));
 pet_bridge_image_lease_t stale=first;assert(pet_bridge_image_release(&images,&first));assert(!pet_bridge_image_release(&images,&stale));assert(pet_bridge_image_write(&images,&ref,0,data,128,pixels));assert(!pet_bridge_image_release(&images,&stale));
 pet_bridge_images_clear(&images);assert(!pet_bridge_image_borrow(&images,"cover",pixels,&first));assert(!pet_bridge_image_write(&images,&ref,0,data,128,pixels));assert(pet_bridge_image_release(&images,&second));
 cJSON_Delete(suite);return 0;
}
'''
    source=tmp_path/'bridge.c';source.write_text(code);exe=tmp_path/'bridge'
    vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe),str(CONTRACT)],check=True)

def test_real_host_entry_scopes_images_and_parameter_commands_to_one_observation(tmp_path):
    (tmp_path/'freertos').mkdir()
    (tmp_path/'mbedtls').mkdir()
    (tmp_path/'freertos/FreeRTOS.h').write_text('typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n')
    (tmp_path/'esp_random.h').write_text('#include <stdint.h>\nstatic uint32_t esp_random(void){return 1234;}\n')
    (tmp_path/'esp_attr.h').write_text('#define EXT_RAM_BSS_ATTR\n')
    # Inject fixed decoded bytes at the SDK decoder boundary. Base64 itself is
    # the platform implementation, not replaced in the production code.
    (tmp_path/'mbedtls/base64.h').write_text('''#include <stddef.h>
#include <string.h>
static int mbedtls_base64_decode(unsigned char*out,size_t cap,size_t*written,const unsigned char*data,size_t n){if(cap<128||n!=7||memcmp(data,"fixture",7))return -1;memset(out,17,128);*written=128;return 0;}
''')
    header='''#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#define PET_P4_DEVICE_ID_MAX 64
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char widget_id[48],eyebrow[96],headline[160],metric_value[64],metric_label[96],metric_unit[32],badge[16],note[160],footer[160];uint32_t revision;} pet_p4_miniapp_view_t;
bool pet_app_host_enabled(const char*,const char*,uint64_t);
'''
    code=(ROOT/'firmware/main/pet_p4_app_host.c').read_text().replace('#include "pet_p4_app_host.h"',header)
    code+=r'''
#include <assert.h>
static char emitted[2048];
static void sink(const char*line,void*ctx){(void)ctx;snprintf(emitted,sizeof(emitted),"%s",line);}
int main(void){
 pet_app_host_transport(sink,NULL,"board");pet_app_host_tick_bound("bridge-app",1,0,true,2100,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
 cJSON*event=cJSON_Parse(emitted);const cJSON*ep=cJSON_GetObjectItemCaseSensitive(event,"payload");char token[32];snprintf(token,sizeof(token),"%s",str(ep,"instance"));cJSON_Delete(event);
 cJSON*snapshot=cJSON_Parse("{\"componentId\":\"bridge-app\",\"sequence\":1,\"values\":{},\"variables\":{\"progress\":10},\"ui\":{\"mode\":0,\"actions\":[\"host.level\"],\"context\":\"abc123\"},\"presentation\":{\"timelines\":{\"progress\":{\"positionMs\":10000,\"durationMs\":100000,\"running\":true,\"horizonMs\":2000,\"projection\":\"percent\"}},\"images\":{\"cover\":{\"key\":\"asset-a\",\"revision\":\"one\",\"width\":8,\"height\":8}}}}");
 cJSON_AddStringToObject(snapshot,"instance",token);cJSON_AddStringToObject(snapshot,"contract","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");assert(pet_app_host_apply(snapshot,2200));assert(pet_app_host_instance_matches("bridge-app",token));assert(!pet_app_host_instance_matches("other-app",token));
 pet_host_render_t rendered;pet_app_host_render_capture("bridge-app",&rendered,NULL,3700);assert(rendered.variables[0].number==11);
 char text[64];int32_t actual;assert(pet_app_host_variable("bridge-app","progress",true,text,&actual,3700)&&actual==10);
 cJSON*packet=cJSON_Parse("{\"componentId\":\"bridge-app\",\"context\":\"abc123\",\"alias\":\"cover\",\"key\":\"asset-a\",\"revision\":\"one\",\"width\":8,\"height\":8,\"offset\":0,\"data\":\"fixture\"}");cJSON_AddStringToObject(packet,"instance",token);
 assert(!pet_app_host_image_ready(packet,4000));assert(pet_app_host_image_apply(packet,4000));assert(pet_app_host_image_ready(packet,4000));
 pet_bridge_image_lease_t lease;assert(pet_app_host_image_borrow("bridge-app","cover",4001,&lease));assert(lease.pixels[0]==0x1111);
 cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(packet,"context"),"wrong");assert(!pet_app_host_image_apply(packet,4002));cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(packet,"context"),"abc123");
 pet_host_observation_t observed;assert(pet_app_host_observe("bridge-app",&observed));assert(pet_app_host_observed_value_action("bridge-app",1,0,"host.level","amount",50,&observed,4100));
 event=cJSON_Parse(emitted);ep=cJSON_GetObjectItemCaseSensitive(event,"payload");assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ep,"data"),"amount")->valueint==50);assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ep,"confirmed")));cJSON_Delete(event);
 pet_app_host_tick("other-app",2,0,true,4200);assert(!pet_app_host_image_apply(packet,4201));assert(!pet_app_host_instance_matches("bridge-app",token));assert(!pet_app_host_observed_value_action("bridge-app",1,0,"host.level","amount",60,&observed,4201));pet_app_host_image_release(&lease);
 cJSON_Delete(packet);cJSON_Delete(snapshot);return 0;
}
'''
    source=tmp_path/'host.c';source.write_text(code);exe=tmp_path/'host'
    vendor=ROOT/'firmware/tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-DPET_SERVICE_BRIDGE_ENABLED=1','-fsanitize=address,undefined','-I',str(tmp_path),'-I',str(vendor),'-I',str(ROOT/'firmware/main'),str(source),str(ROOT/'firmware/main/pet_p4_resources.c'),str(ROOT/'firmware/main/pet_p4_resources_device.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
