"""Real widget handler/JSON guard/ACK/inventory and native catalog cursor.
No device or service: unrelated install/activate/delete paths are forbidden stubs.
"""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
ROOT = Path(__file__).resolve().parents[1]


def test_real_selection_parser_preserves_active_and_restores_visible_id(tmp_path):
    protocol = (ROOT/'main/pet_p4_protocol.c').read_text()
    miniapp = (ROOT/'main/pet_p4_miniapp.c').read_text()
    native = (ROOT/'main/pet_p4_usb_native.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_json_source.h"
#include "pet_p4_storage_lane.h"
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_P4_MINIAPP_CATALOG_MAX 30
#define PET_WIDGET_TRACE_SCOPE(name,kind) ((void)0)
#define PET_WIDGET_TRACE_OPERATION(name,kind) int name=0
#define PET_WTRACE_COMMIT 1
#define PET_WTRACE_DELETE 2
#define PET_WTRACE_ACTIVATE 3
#define PET_WTRACE_NONE 0
#define PET_WTRACE_ACK 4
typedef int pet_widget_trace_kind_t;
static void pet_widget_trace_end(int*p,bool ok){(void)p;(void)ok;}
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char board_device_id[48],screen_page[24];unsigned stats,last_update_ms;}pet_p4_runtime_state_t;
typedef struct {char *line;}pet_p4_query_reply_t;
typedef struct {char widget_id[48],title[64];bool active;unsigned resource_state,image_count;}pet_p4_miniapp_catalog_entry_t;
typedef struct {char widget_id[48],title[64];unsigned resource_state,image_count;}miniapp_catalog_item_t;
static miniapp_catalog_item_t g_catalog[4]={
 {.widget_id="music-player"},{.widget_id="user-z",.resource_state=1},
 {.widget_id="user-a",.resource_state=1},{.widget_id="fixture",.resource_state=1}};
static size_t g_catalog_count=4,g_catalog_selected=2;
static bool g_catalog_recovery_blocked;
static struct {bool active;char widget_id[48];unsigned runtime_cookie;}g_runtime={true,"user-z",12345};
static unsigned lock_depth,forbidden_calls;
static void miniapp_runtime_lock(void){assert(!lock_depth);lock_depth++;}
static void miniapp_runtime_unlock(void){assert(lock_depth==1);lock_depth--;}
static void copy_utf8(char*out,size_t n,const char*v){snprintf(out,n,"%s",v);}
static void copy_text(char*out,size_t n,const char*v){snprintf(out,n,"%s",v);}
static int64_t esp_timer_get_time(void){return 123000;}
static const char*json_string(const cJSON*p,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(p,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool pet_p4_miniapp_active_id(char*out,size_t n){if(g_runtime.active)copy_text(out,n,g_runtime.widget_id);return g_runtime.active;}
static bool forbidden(void){forbidden_calls++;assert(!"selection invoked unrelated side effect");return false;}
static bool refused_install(const cJSON*p,char*e,size_t n){(void)p;(void)e;(void)n;return forbidden();}
static bool refused_remove(const char*p,char*e,size_t n){(void)p;(void)e;(void)n;return forbidden();}
#define pet_p4_miniapp_install_begin refused_install
#define pet_p4_miniapp_install_chunk refused_install
#define pet_p4_miniapp_install_commit refused_install
#define pet_p4_miniapp_remove refused_remove
#define pet_p4_miniapp_remove_preserving_save refused_remove
static bool pet_p4_miniapp_resource_repair_active(void){return forbidden();}
static void pet_bubble_pixels_desire(const cJSON*p,void*c,const char*t,uint64_t n){(void)p;(void)c;(void)t;(void)n;forbidden();}
static bool pet_p4_miniapp_catalog_activate_id(const char*id){(void)id;return forbidden();}
static void pet_p4_miniapp_sync_stats(unsigned*s){(void)s;forbidden();}
static bool pet_p4_audio_active(void){return forbidden();}
#define pet_p4_audio_playback_active pet_p4_audio_active
#define pet_p4_audio_conversation_active pet_p4_audio_active
#define pet_p4_audio_music_active pet_p4_audio_active
static bool handle_background_program_topic(pet_p4_runtime_state_t*s,const char*t,const cJSON*p,pet_p4_send_line_fn f,void*c){(void)s;(void)t;(void)p;(void)f;(void)c;return forbidden();}
static void pet_app_host_transport(pet_p4_send_line_fn f,void*c,const char*b){(void)f;(void)c;(void)b;}
static void pet_p4_media_transport(pet_p4_send_line_fn f,void*c){(void)f;(void)c;}
static void pet_p4_state_note_host_activity(pet_p4_runtime_state_t*s,unsigned long long t){(void)s;(void)t;}
/* Real storage-lane occupancy and lock handoff; only IO/admission are fakes. */
static pet_storage_lane_t selection_lane;
static bool state_unlocked,fail_cursor,g_catalog_cursor_dirty;
static uint64_t g_catalog_cursor_serial;
static uint32_t g_catalog_sequence=17;
static uintptr_t storage_owner(void*ctx){(void)ctx;return 1;}
static void storage_unlock(void*ctx){(void)ctx;assert(!lock_depth&&!state_unlocked);state_unlocked=true;}
static void storage_lock(void*ctx){(void)ctx;assert(!lock_depth&&state_unlocked);state_unlocked=false;}
static bool storage_scope_begin(pet_storage_scope_t*s,bool activation){assert(!activation);return pet_storage_scope_begin(&selection_lane,s);}
static void storage_scope_finish(pet_storage_scope_t*s){assert(pet_storage_scope_finish(s));}
static bool storage_audio_busy(void){return false;}
static bool storage_work_current(void){return true;}
static bool safe_widget_id(const char*id){return id&&id[0]&&strlen(id)<48;}
static int catalog_find(const miniapp_catalog_item_t*items,size_t n,const char*id){for(size_t i=0;i<n;i++)if(!strcmp(items[i].widget_id,id))return (int)i;return -1;}
static bool catalog_cursor_write(uint32_t sequence,const char*id){assert(!lock_depth&&state_unlocked&&sequence==17&&id[0]);return !fail_cursor;}
static bool catalog_select_id_queued(const char*id){(void)id;return forbidden();}
static bool send_catalog_admin(pet_p4_runtime_state_t*s,const char*t,const cJSON*p,pet_p4_send_line_fn f,void*c){(void)s;(void)t;(void)p;(void)f;(void)c;return forbidden();}
static cJSON *response;
static void capture(const char*line,void*ctx){(void)ctx;assert(!response);response=cJSON_ParseWithOpts(line,NULL,true);assert(response);}
'''
    for name in ('catalog_item_visible','catalog_entry_visible','catalog_visible_index',
                 'pet_p4_miniapp_catalog_recovery_blocked','pet_p4_miniapp_catalog_count',
                 'pet_p4_miniapp_catalog_capacity','pet_p4_miniapp_catalog_selected',
                 'pet_p4_miniapp_catalog_get','catalog_select_id_durable',
                 'pet_p4_miniapp_catalog_select_durable','pet_p4_miniapp_catalog_select'):
        code += function(miniapp,name)+'\n'
    # Preserve the actual route preflight dependency; no unconditional-true stub.
    code += r'''
#include <stdatomic.h>
#include <stdint.h>
static _Atomic uint32_t g_connection_generation=7;
static void pet_p4_native_usb_send_json_line(const char*line,void*ctx){(void)line;(void)ctx;abort();}
'''
    for name in ('native_route_context','pet_p4_native_usb_route_current'):
        code += function(native,name)+'\n'
    code += function(protocol,'asset_phase_send_current')+'\n'
    for name in ('send_topic_payload','send_protocol_ack','send_widget_ack',
                 'send_widget_inventory','handle_widget_topic','pet_p4_protocol_source_valid'):
        code += function(protocol,name)+'\n'
    # The real outer representation/envelope parser prefix, then only its real
    # widget delegate; unrelated hardware dispatch branches are not compiled.
    outer = function(protocol,'pet_p4_handle_line_with_reply')
    prefix = outer[:outer.index('  if (strcmp(topic, "ack") == 0')]
    assert 'if (!handle_widget_topic(state, topic, payload, send_line, ctx))' in outer
    code += prefix + r'''
  bool handled=handle_widget_topic(state,topic,payload,send_line,ctx);
  cJSON_Delete(root);return handled;
}
static pet_p4_runtime_state_t state={.board_device_id="board",.screen_page="app",.last_update_ms=7};
static bool wire(const char*line){cJSON_Delete(response);response=NULL;return pet_p4_handle_line_with_reply(&state,line,capture,NULL,NULL);}
static cJSON*payload(void){assert(response);return cJSON_GetObjectItemCaseSensitive(response,"payload");}
static void inventory(const char*id,int index){
 assert(wire("{\"topic\":\"widget/list\",\"payload\":{\"requestId\":\"inventory-request\"}}"));
 assert(!strcmp(json_string(response,"topic"),"widget/inventory"));cJSON*p=payload();
 assert(!strcmp(json_string(p,"requestId"),"inventory-request"));
 if(id){assert(!strcmp(json_string(p,"selectedWidgetId"),id));assert(cJSON_GetObjectItemCaseSensitive(p,"selectedIndex")->valueint==index);}
 else assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(p,"selectedWidgetId"))&&cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(p,"selectedIndex")));
 assert(!strcmp(json_string(p,"activeWidgetId"),"user-z"));
 assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p,"items"))==(int)pet_p4_miniapp_catalog_count());
}
static void ack(bool ok,const char*request,const char*code){
 assert(!strcmp(json_string(response,"topic"),"protocol/ack"));cJSON*p=payload();
 assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"))==ok);
 assert(!strcmp(json_string(p,"requestTopic"),"widget/select_by_id"));
 if(request)assert(!strcmp(json_string(p,"requestId"),request));
 if(code)assert(!strcmp(json_string(p,"code"),code));
}
int main(void){
 assert(pet_storage_lane_configure(&selection_lane,storage_unlock,storage_lock,storage_owner,NULL));
 assert(pet_storage_lane_enter(&selection_lane));
 inventory("user-a",1);size_t original=g_catalog_selected;
 assert(wire("{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"select-1\",\"widgetId\":\"fixture\"}}"));ack(true,"select-1",NULL);
 assert(g_catalog_selected==3&&state.last_update_ms==8);inventory("fixture",2);
 assert(wire("{\"topic\":\"widget/select_by_id\",\"payload\":{\"widgetId\":\"user-a\",\"requestId\":\"restore\"}}"));ack(true,"restore",NULL);
 assert(g_catalog_selected==original);inventory("user-a",1);
 const char*bad[]={
 "{}","{\"requestId\":\"bad\"}","{\"requestId\":\"bad\",\"widgetId\":1}",
 "{\"requestId\":7,\"widgetId\":\"fixture\"}","{\"requestId\":\"\",\"widgetId\":\"fixture\"}",
 "{\"requestId\":\"bad\",\"widgetId\":\"fixture\",\"show\":true}",
 "{\"requestId\":\"bad\",\"requestId\":\"dup\",\"widgetId\":\"fixture\"}",
 "{\"requestId\":\"bad\",\"widgetId\":\"fixture\",\"widgetId\":\"user-z\"}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
  char line[512];snprintf(line,sizeof(line),"{\"topic\":\"widget/select_by_id\",\"payload\":%s}",bad[i]);
  assert(!wire(line));ack(false,NULL,"invalid_selection");assert(g_catalog_selected==original&&state.last_update_ms==9);
 }
 for(unsigned mode=0;mode<3;mode++){
  g_catalog_recovery_blocked=mode==2;
  const char*id=mode==0?"missing":mode==1?"music-player":"fixture";
  char line[256];snprintf(line,sizeof(line),"{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"refused\",\"widgetId\":\"%s\"}}",id);
  assert(!wire(line));ack(false,"refused",mode==2?"catalog_unavailable":"selection_failed");assert(g_catalog_selected==original);
 }
 g_catalog_recovery_blocked=false;
 for(unsigned mode=0;mode<2;mode++){
  char large[100],line[512];memset(large,'x',sizeof(large)-1);large[sizeof(large)-1]=0;
  snprintf(line,sizeof(line),"{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"%s\",\"widgetId\":\"%s\"}}",mode?"bounded":large,mode?large:"fixture");
  assert(!wire(line));ack(false,NULL,"invalid_selection");assert(g_catalog_selected==original);
 }
 assert(!wire("{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"nul\",\"widgetId\":\"fixture\\u0000suffix\"}}"));assert(g_catalog_selected==original);
 assert(!wire("{\"topic\":\"widget/select_by_id\",\"payload\":[]}"));assert(g_catalog_selected==original);
 assert(!pet_p4_miniapp_catalog_select(99)&&g_catalog_selected==original);
 /* Restore by ID after visible order changed: index is observational, not authority. */
 miniapp_catalog_item_t swap=g_catalog[1];g_catalog[1]=g_catalog[2];g_catalog[2]=swap;
 assert(wire("{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"reorder\",\"widgetId\":\"user-a\"}}"));ack(true,"reorder",NULL);inventory("user-a",0);
 /* Failed durable cursor IO must not publish a new selection or activate. */
 size_t before=g_catalog_selected;fail_cursor=true;
 assert(!wire("{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"write-fault\",\"widgetId\":\"fixture\"}}"));
 ack(false,"write-fault","selection_failed");assert(g_catalog_selected==before&&!state_unlocked&&!selection_lane.occupied);
 fail_cursor=false;
 g_catalog_count=0;inventory(NULL,0);
 assert(!wire("{\"topic\":\"widget/select_by_id\",\"payload\":{\"requestId\":\"empty\",\"widgetId\":\"user-a\"}}"));ack(false,"empty","selection_failed");
 assert(g_runtime.active&&!strcmp(g_runtime.widget_id,"user-z")&&g_runtime.runtime_cookie==12345);
 assert(!strcmp(state.screen_page,"app")&&!forbidden_calls&&!lock_depth);
 assert(pet_storage_lane_leave(&selection_lane));
 cJSON_Delete(response);return 0;
}
'''
    source=tmp_path/'selection-protocol.c';source.write_text(code)
    binary=tmp_path/'selection-protocol'
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
                    '-Wno-unused-parameter','-Wno-unused-variable','-Wno-misleading-indentation','-Wno-deprecated-declarations',
                    '-fsanitize=address,undefined','-I',str(ROOT/'main'),
                    '-I',str(ROOT/'tests/vendor/cjson'),str(source),
                    str(ROOT/'tests/vendor/cjson/cJSON.c'),str(ROOT/'main/pet_p4_storage_lane.c'),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
