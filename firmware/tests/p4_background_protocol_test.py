"""Actual background wire parser/serializer; native control is a counted stub.
Real admission, lifecycle and persistence have separate production-core tests.
No serial, filesystem service, cloud, device, permissions or acoustic test.
"""
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def test_catalog_inspection_uses_actual_verified_ram_only_and_leaves_output_on_refusal(tmp_path):
    from p4_render_trace_test import function
    safe=function((ROOT/'main/pet_p4_miniapp.c').read_text(),'safe_widget_id')
    inspect=function((ROOT/'main/pet_p4_miniapp_background.inc').read_text(),'pet_p4_miniapp_background_content')
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
typedef struct {char id[48];uint32_t widget_checksum,buttons_checksum,sprites_checksum;unsigned resource_state;} item_t;
static item_t g_catalog[1]={{.id="focus",.widget_checksum=UINT32_MAX,.buttons_checksum=1,.resource_state=1}};
static size_t g_catalog_count=1;static bool g_catalog_recovery_blocked,available=true;
static bool pet_p4_miniapp_background_available(void){return available;}
static int catalog_find(const item_t *v,size_t n,const char *id){for(size_t i=0;i<n;i++)if(!strcmp(v[i].id,id))return (int)i;return -1;}
'''+safe+'\n'+inspect+r'''
int main(void){
 uint32_t out[3]={42,43,44};assert(pet_p4_miniapp_background_content("focus",out));assert(out[0]==UINT32_MAX&&out[1]==1&&out[2]==0);
 out[0]=42;assert(!pet_p4_miniapp_background_content("missing",out)&&out[0]==42);
 for(unsigned mode=0;mode<3;mode++){
  g_catalog_recovery_blocked=mode==0;available=mode!=1;g_catalog[0].resource_state=mode==2?0:1;
  assert(!pet_p4_miniapp_background_content("focus",out)&&out[0]==42);
 }
 available=true;g_catalog_recovery_blocked=false;g_catalog[0].resource_state=1;
 assert(!pet_p4_miniapp_background_content("Upper",out));assert(!pet_p4_miniapp_background_content("focus",NULL));
 char longest[49];memset(longest,'a',48);longest[48]=0;assert(!safe_widget_id(longest));longest[47]=0;assert(safe_widget_id(longest));return 0;
}
'''
    source=tmp_path/'catalog-inspect.c';source.write_text(code);binary=tmp_path/'catalog-inspect'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)

def test_actual_protocol_strict_fields_checksums_serials_and_redacted_status(tmp_path):
    public=(ROOT/'main/pet_p4_miniapp.h').read_text()
    status=re.search(r'typedef struct \{\n  char widget_id\[48\];uint64_t registration;.*?\} pet_p4_miniapp_background_status_t;',public,re.S).group(0)
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#define PET_P4_MINIAPP_BACKGROUND_MAX 4
#define PET_P4_MINIAPP_BACKGROUND_ENGINE "p4-program-background-v2"
typedef struct {char board_device_id[48];} pet_p4_runtime_state_t;
typedef void (*pet_p4_send_line_fn)(const char*,void*);
'''+status+r'''
static unsigned starts,stops,inspects;static bool available=true,force_busy;
static char boot[17]="0123456789abcdef";
static const char *pet_p4_boot_identity(void){return boot;}
static cJSON *response;
static unsigned allocations_left;
static void *fault_malloc(size_t size){if(!allocations_left)return NULL;allocations_left--;return malloc(size);}
static const char *json_string(const cJSON *p,const char *key){const cJSON *v=cJSON_GetObjectItemCaseSensitive(p,key);return cJSON_IsString(v)?v->valuestring:"";}
static void copy_text(char *out,size_t n,const char *value){snprintf(out,n,"%s",value);}
static bool pet_p4_miniapp_background_available(void){return available;}
static bool pet_p4_miniapp_background_content(const char *id,uint32_t content[3]){
 inspects++;if(strcmp(id,"example"))return false;content[0]=UINT32_MAX;content[1]=1;content[2]=0;return true;
}
static bool pet_p4_miniapp_background_start(const char *id,const uint32_t sum[3],uint64_t *out,char *error,size_t size){
 assert(!strcmp(id,"example")&&sum[0]==UINT32_MAX&&sum[1]==1&&sum[2]==0);
 if(force_busy){snprintf(error,size,"PROGRAM_BUSY");return false;}starts++;*out=7;return true;
}
static bool pet_p4_miniapp_background_stop(const char *id,uint64_t serial,char *error,size_t size){(void)error;(void)size;assert(!strcmp(id,"example")&&serial);stops++;return true;}
static size_t pet_p4_miniapp_background_list(pet_p4_miniapp_background_status_t out[4]){
 if(!available)return 0;out[0]=(pet_p4_miniapp_background_status_t){.registration=7,.revision=3,.live=true,.phase=1,.max_step_us=211};strcpy(out[0].widget_id,"example");return 1;
}
static void send_topic_payload(pet_p4_send_line_fn send,void *ctx,const char *topic,cJSON *p){
 (void)send;(void)ctx;assert(!strcmp(topic,"widget/program-status"));cJSON_Delete(response);response=p;
}
#include "pet_p4_background_protocol.inc"
static bool request_raw(const char *topic,const char *text){
 cJSON *p=cJSON_Parse(text);assert(p);pet_p4_runtime_state_t state={.board_device_id="test-board"};
 bool ok=handle_background_program_topic(&state,topic,p,NULL,NULL);cJSON_Delete(p);assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(response,"ok")));
 assert(!strcmp(json_string(response,"bootId"),boot));assert(!strcmp(json_string(response,"engine"),PET_P4_MINIAPP_BACKGROUND_ENGINE));
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response,"ok"))==ok);return ok;
}
static bool request(const char *topic,const char *text){
 cJSON *p=cJSON_Parse(text);assert(p);
 if((!strcmp(topic,"widget/program-start")||!strcmp(topic,"widget/program-stop"))&&!cJSON_HasObjectItem(p,"bootId"))cJSON_AddStringToObject(p,"bootId",boot);
 char *encoded=cJSON_PrintUnformatted(p);assert(encoded);bool ok=request_raw(topic,encoded);cJSON_free(encoded);cJSON_Delete(p);return ok;
}
int main(void){
 assert(request("widget/program-query","{\"requestId\":\"test-1\"}"));assert(!starts&&!stops);
 assert(request("widget/program-inspect","{\"requestId\":\"inspect\",\"widgetId\":\"example\"}"));assert(inspects==1&&!starts&&!stops);
 const cJSON *identity=cJSON_GetObjectItemCaseSensitive(response,"installedContent");
 assert(!strcmp(json_string(identity,"widgetChecksum"),"ffffffff")&&!strcmp(json_string(identity,"buttonsChecksum"),"00000001")&&!strcmp(json_string(identity,"spritesChecksum"),"00000000"));
 assert(!request("widget/program-inspect","{\"requestId\":\"inspect\",\"widgetId\":\"example\",\"code\":1}"));assert(inspects==1);
 assert(!request("widget/program-inspect","{\"requestId\":\"inspect\",\"widgetId\":\"example\",\"widgetId\":\"another\"}"));assert(inspects==1);
 assert(request("widget/program-start","{\"requestId\":\"test-2\",\"widgetId\":\"example\",\"widgetChecksum\":\"ffffffff\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\"}"));assert(starts==1);
 force_busy=true;
 assert(!request("widget/program-start","{\"requestId\":\"busy-negative\",\"widgetId\":\"example\",\"widgetChecksum\":\"ffffffff\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\"}"));
 assert(!strcmp(json_string(response,"requestId"),"busy-negative")&&!strcmp(json_string(response,"error"),"PROGRAM_BUSY")&&starts==1);
 const cJSON *busy_items=cJSON_GetObjectItemCaseSensitive(response,"items");assert(cJSON_GetArraySize(busy_items)==1&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(busy_items,0),"live")));
 force_busy=false;
 const char *bad[]={
 "{\"requestId\":\"x\",\"widgetId\":\"example\",\"widgetChecksum\":\"ffffffff\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\",\"code\":\"privatePayload\"}",
 "{\"requestId\":\"x\",\"widgetId\":\"example\",\"widgetChecksum\":\"ffffffff\",\"buttonsChecksum\":\"00000001\",\"buttonsChecksum\":\"00000000\"}",
 "{\"requestId\":\"x\",\"widgetId\":\"example\",\"widgetChecksum\":4294967295,\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\"}",
 "{\"requestId\":\"x\",\"widgetId\":\"example\",\"widgetChecksum\":\"fffffffG\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\"}",
 "{\"requestId\":\"x\",\"widgetId\":\"example\",\"widgetChecksum\":\"fffffffff\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\"}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!request("widget/program-start",bad[i]));assert(starts==1);
 const char *invalid[]={"0","00","01","-1","+1","1 ","18446744073709551616","999999999999999999999"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
  char input[256];snprintf(input,sizeof(input),"{\"requestId\":\"stop\",\"widgetId\":\"example\",\"registration\":\"%s\"}",invalid[i]);assert(!request("widget/program-stop",input));
 }assert(!stops);
 assert(request("widget/program-stop","{\"requestId\":\"stop\",\"widgetId\":\"example\",\"registration\":\"18446744073709551615\"}"));assert(stops==1);
 // A serial may be reused on a new boot; its old wire ticket cannot stop or
 // start that new owner even though the same board/id/serial still exist.
 strcpy(boot,"1111111111111111");
 assert(!request("widget/program-stop","{\"requestId\":\"old\",\"widgetId\":\"example\",\"registration\":\"7\",\"bootId\":\"0123456789abcdef\"}"));assert(stops==1);
 assert(!request_raw("widget/program-stop","{\"requestId\":\"missing\",\"widgetId\":\"example\",\"registration\":\"7\"}"));assert(stops==1);
 assert(!request("widget/program-start","{\"requestId\":\"old\",\"widgetId\":\"example\",\"widgetChecksum\":\"ffffffff\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\",\"bootId\":\"0123456789abcdef\"}"));assert(starts==1);
 assert(!request("widget/program-stop","{\"requestId\":\"bad\",\"widgetId\":\"example\",\"registration\":\"7\",\"bootId\":\"1111111111111111\",\"bootId\":\"1111111111111111\"}"));assert(stops==1);
 assert(request("widget/program-stop","{\"requestId\":\"current\",\"widgetId\":\"example\",\"registration\":\"7\"}"));assert(stops==2);
 assert(!request("widget/program-query","{\"requestId\":\"privatePayload with spaces\"}"));
 char *text=cJSON_PrintUnformatted(response);assert(text&&!strstr(text,"privatePayload")&&!strstr(text,"cells")&&!strstr(text,"code"));cJSON_free(text);
 // Every allocation refusal must retain an unknown receipt, not serialize a
 // partial status or turn a live owner into an apparently empty success.
 cJSON_Delete(response);response=NULL;
 for(unsigned mode=0;mode<2;mode++){
  cJSON *p=cJSON_Parse(mode?"{\"requestId\":\"oom\",\"widgetId\":\"example\"}":"{\"requestId\":\"oom\"}");assert(p);
  pet_p4_runtime_state_t state={.board_device_id="test-board"};unsigned successful=0;
  for(unsigned budget=0;budget<128;budget++){
   allocations_left=budget;cJSON_Hooks hooks={fault_malloc,free};cJSON_InitHooks(&hooks);
   bool ok=handle_background_program_topic(&state,mode?"widget/program-inspect":"widget/program-query",p,NULL,NULL);
   cJSON_InitHooks(NULL);
   if(response){
    assert(ok);successful++;assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response,"ok")));
    const cJSON *items=cJSON_GetObjectItemCaseSensitive(response,"items");assert(cJSON_IsArray(items)&&cJSON_GetArraySize(items)==1);
    const cJSON *item=cJSON_GetArrayItem(items,0);assert(cJSON_GetArraySize(item)==9);
    assert(!strcmp(json_string(item,"widgetId"),"example")&&!strcmp(json_string(item,"registration"),"7"));
    if(mode)assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(response,"installedContent"))==4);
    cJSON_Delete(response);response=NULL;
   }else assert(!ok);
  }
  assert(successful>0);cJSON_Delete(p);
 }
 available=false;assert(!request("widget/program-start","{\"requestId\":\"test-2\",\"widgetId\":\"example\",\"widgetChecksum\":\"ffffffff\",\"buttonsChecksum\":\"00000001\",\"spritesChecksum\":\"00000000\"}"));assert(starts==1);
 cJSON_Delete(response);return 0;
}
'''
    source=tmp_path/'background-wire.c';source.write_text(code);binary=tmp_path/'background-wire';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',
      '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(vendor),str(source),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
def test_closed_background_parent_exports_refusals_without_changing_callers(tmp_path):
    from pathlib import Path
    import subprocess
    root=Path(__file__).resolve().parents[1]
    text=(root/'main/pet_p4_miniapp_background.inc').read_text()
    closed=text.rsplit('\n#else\n',1)[1].rsplit('\n#endif',1)[0]
    source=tmp_path/'closed.c'
    source.write_text('''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define PET_P4_MINIAPP_BACKGROUND_MAX 4
typedef struct {unsigned untouched;} pet_p4_miniapp_background_status_t;
static void set_error(char *out,size_t size,const char *message){if(size){strncpy(out,message,size-1);out[size-1]=0;}}
'''+closed+'''
int main(void){
 uint32_t content[3]={1,2,3};uint64_t serial=19;char error[64]={0};
 pet_p4_miniapp_background_status_t rows[4]={{.untouched=99}};
 assert(!pet_p4_miniapp_background_available());
 assert(!pet_p4_miniapp_background_content("example",content));
 assert(content[0]==1&&content[1]==2&&content[2]==3);
 assert(!pet_p4_miniapp_background_content(NULL,NULL));
 assert(!pet_p4_miniapp_background_list(rows)&&rows[0].untouched==99);
 assert(!pet_p4_miniapp_background_start("example",content,&serial,error,sizeof(error))&&serial==19);
 assert(!pet_p4_miniapp_background_stop("example",serial,error,sizeof(error)));
 assert(!strcmp(error,"background capability unavailable"));return 0;
}
''')
    binary=tmp_path/'closed'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
