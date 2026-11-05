"""Real production C entry points under synthetic filesystem/SDK interleavings.
No device, network, credentials, SDK build or generated source assertions.
"""
from pathlib import Path
import os, subprocess
import pytest
from p4_render_trace_test import function
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT.parents[1]
def production(name):
    if os.environ.get('SOURCE_VARIANT')=='before':
        return (OUT/'preimages/firmware/main'/name).read_text()
    return (ROOT/'main'/name).read_text()
def compile_c(tmp, text, name, assets=False, resources=False):
    tmp.mkdir(parents=True,exist_ok=True)
    src=tmp/(name+'.c');src.write_text(text);exe=tmp/name
    vendor=ROOT/'tests/vendor/cjson'
    command=['/usr/bin/cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
      '-Wno-unused-function','-Wno-unused-parameter','-Wno-misleading-indentation',
      '-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-omit-frame-pointer',
      '-I',str(tmp),'-I',str(ROOT/'main'),'-I',str(vendor),str(src),str(vendor/'cJSON.c')]
    if assets:
        (tmp/'esp_heap_caps.h').write_text('#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nvoid *test_heap_caps_calloc(size_t,size_t,unsigned);\n#define heap_caps_calloc test_heap_caps_calloc\n#define heap_caps_free free\n')
        command.append(str(ROOT/'main/pet_p4_assets.c'))
    if resources:command.append(str(ROOT/'main/pet_p4_resources.c'))
    subprocess.run([*command,'-o',str(exe)],check=True,timeout=60)
    return exe
def execute(exe,case):
    subprocess.run([str(exe),str(case)],cwd=exe.parent,check=True,timeout=15)
COMMON=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <math.h>
#include "cJSON.h"
static void pet_p4_audio_status_cues_defer(bool busy){(void)busy;}
static bool audio_patch_idle_due(uint64_t now){(void)now;return false;}
"""
@pytest.fixture(scope='module',params=[0,1])
def begin_binary(tmp_path_factory,request):
    code=production('pet_p4_miniapp.c')
    fixture=COMMON+r"""
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_P4_STORAGE_PRESERVE_UNCHANGED 1
#ifndef PET_P4_CAPABILITY_EXT
#define PET_P4_CAPABILITY_EXT EXT_VALUE
#endif
static struct {bool active,repair_resources,preserve_unchanged;char transfer_id[64],widget_id[48],widget_json[200];unsigned char sprites[128];}g_staging;
static unsigned g_image_used,g_image_next;static char g_image_id[32];
static bool g_builtin_sync_in_progress,g_install_unchanged,busy,g_catalog_recovery_blocked;static unsigned audio,media_resets;
static unsigned char media[256];
static bool pet_p4_miniapp_storage_busy(void){return busy;}
static bool pet_p4_audio_active(void){return audio==1;}
static bool pet_p4_audio_playback_active(void){return audio==2;}
static bool pet_p4_audio_conversation_active(void){return audio==3;}
static bool pet_p4_audio_music_active(void){return audio==4;}
static void pet_cmedia_stage_reset(void){memset(media,0,sizeof(media));media_resets++;}
"""
    fixture=fixture.replace('EXT_VALUE',str(request.param))
    fixture+='\n'.join(function(code,n) for n in ['json_string','set_error','copy_utf8','safe_id','safe_widget_id','transfer_matches','pet_p4_miniapp_install_begin'])
    fixture+=r"""
int main(int argc,char **argv){
 assert(argc==2);int mode=atoi(argv[1]);char error[200]={0};
 memset(&g_staging,0xa5,sizeof(g_staging));g_staging.active=true;
 strcpy(g_staging.transfer_id,"accepted");strcpy(g_staging.widget_id,"garden");
 strcpy(g_staging.widget_json,"accepted-json");memset(media,0x6b,sizeof(media));
 g_image_used=91;g_image_next=3;strcpy(g_image_id,"accepted-image");
 unsigned char old[sizeof(g_staging)],old_media[sizeof(media)];memcpy(old,&g_staging,sizeof(old));memcpy(old_media,media,sizeof(media));
 const char *json="{\"transferId\":\"new\",\"widgetId\":\"book\",\"preserveUnchanged\":true}";
 if(mode==1)json="{\"transferId\":\"\",\"widgetId\":\"book\"}";
 if(mode==2)json="{\"transferId\":\"new\",\"widgetId\":\"INVALID\"}";
 if(mode==3)json="{\"transferId\":\"new\",\"widgetId\":\"book\",\"preserveUnchanged\":3}";
 if(mode>=4&&mode<=7){audio=(unsigned)(mode-3);json="{\"transferId\":\"new\",\"widgetId\":\"book\",\"repairResources\":true}";}
 if(mode==8)busy=true;
 if(mode==9)json="{\"transferId\":\"new\",\"widgetId\":\"music-player\"}";
 if(mode==10)g_catalog_recovery_blocked=true;
 cJSON *p=cJSON_Parse(json);assert(p);
 bool ok=pet_p4_miniapp_install_begin(p,error,sizeof(error));
 if(mode){assert(!ok&&error[0]);assert(!memcmp(old,&g_staging,sizeof(old)));
  assert(!memcmp(media,old_media,sizeof(media))&&media_resets==0);
  assert(g_image_used==91&&g_image_next==3&&!strcmp(g_image_id,"accepted-image"));
  cJSON *old_request=cJSON_Parse("{\"transferId\":\"accepted\"}");assert(transfer_matches(old_request));cJSON_Delete(old_request);
 }else{assert(ok&&g_staging.active&&!strcmp(g_staging.transfer_id,"new")&&!strcmp(g_staging.widget_id,"book"));
  assert(!g_staging.widget_json[0]&&g_image_used==0&&g_image_next==0&&!g_image_id[0]);
  assert(g_staging.preserve_unchanged&&media_resets==PET_P4_CAPABILITY_EXT);}
 cJSON_Delete(p);return 0;
}
"""
    return compile_c(tmp_path_factory.mktemp('begin'),fixture,'begin')
@pytest.mark.parametrize('case',range(11))
def test_real_begin_reject_preserves_accepted_staging(begin_binary,case):execute(begin_binary,case)
@pytest.fixture(scope='module')
def close_binary(tmp_path_factory):
    code=production('pet_p4_protocol.c')
    fixture=COMMON+r"""
#define PET_P4_FS_PATH_MAX 64
#define PET_P4_JSON_SAFE_INTEGER_MAX 9007199254740991ULL
#define PET_P4_RAW_APPEARANCE_SLOT 1
#define PET_P4_LOGICAL_PATH_MAX 128
typedef void (*pet_p4_send_line_fn)(const char*,void*);
static FILE *g_asset_chunk_file;static char g_asset_chunk_logical_path[128],g_asset_chunk_error[128];
static unsigned long long g_asset_chunk_size,g_asset_chunk_checksum=0xcbf29ce484222325ULL;
static bool g_asset_chunk_stat_valid,fail_close,raw,g_asset_phase_write_attempted;static int g_asset_transfer_slot=1;
static unsigned closes,acks;static bool ack_ok;
static int fault_close(FILE*f){closes++;int rc=fclose(f);if(fail_close){errno=EIO;return EOF;}return rc;}
#define fclose fault_close
static bool ensure_asset_transfer_slot(const char*i){return !strcmp(i,"tx");}
static bool pet_p4_asset_fs_path_for_slot(int slot,const char*p,bool tmp,char*out,size_t n){snprintf(out,n,"asset.bin");return slot==1;}
static bool pet_p4_raw_assets_available(void){return raw;}
static bool pet_p4_raw_assets_supports_path(const char*p){return raw;}
static bool pet_p4_raw_assets_write_chunk(const char*p,const char*i,const unsigned char*d,size_t n,char*e,size_t c){assert(raw);return true;}
static bool pet_p4_raw_assets_finish_file(const char*p,unsigned long long n,const char*h,char*e,size_t c){assert(raw);return true;}
static void send_asset_ack(pet_p4_send_line_fn f,void*ctx,const char*i,const char*phase,const char*p,bool ok,const char*e){assert(!strcmp(phase,"file"));acks++;ack_ok=ok;if(!ok)assert(e&&e[0]);}
"""
    fixture+='\n'.join(function(code,n) for n in ['json_string','json_u64','copy_text','fnv1a64_update','fnv1a64_file','path_is_p4_asset','close_asset_chunk_file','reset_asset_chunk_tracking','read_file_stat','write_asset_chunk_bytes','handle_asset_file'])
    fixture+=r"""
int main(int argc,char **argv){
 assert(argc==2);int mode=atoi(argv[1]);raw=mode==3;
 const unsigned char bytes[]="real buffered bytes";
 assert(write_asset_chunk_bytes("tx","p4/test.bin","0",bytes,sizeof(bytes)-1));
 char json[256],hash[17];snprintf(hash,sizeof(hash),"%016llx",fnv1a64_update(0xcbf29ce484222325ULL,bytes,sizeof(bytes)-1));
 snprintf(json,sizeof(json),"{\"transferId\":\"tx\",\"path\":\"p4/test.bin\",\"size\":%u,\"checksum\":\"%s\"}",(unsigned)(sizeof(bytes)-1+(mode==2)),hash);
 cJSON*p=cJSON_Parse(json);assert(p);fail_close=mode==1;
 handle_asset_file(p,NULL,NULL);
 assert(acks==1&&ack_ok==(mode==0||mode==3)&&!g_asset_chunk_file);
 assert(closes==(raw?0:1));assert(raw||!g_asset_chunk_stat_valid);
 cJSON_Delete(p);return 0;
}
"""
    return compile_c(tmp_path_factory.mktemp('close'),fixture,'close')
@pytest.mark.parametrize('case',range(4))
def test_real_file_close_error_never_acknowledges_success(close_binary,case):execute(close_binary,case)
@pytest.fixture(scope='module')
def activation_binary(tmp_path_factory):
    code=production('pet_p4_protocol.c');main=production('pet_p4_main.c')
    fixture=COMMON+r"""
#include "pet_p4_appearance_session.h"
#include "pet_p4_resources.h"
#include "pet_p4_assets.h"
#include "pet_p4_json_source.h"
#define PET_P4_FS_PATH_MAX 64
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_ASSET_MANIFEST_MAX 65536
#define PET_P4_ASSET_SLOT_COUNT 2
#define PET_P4_ASSET_SLOT_MARKER "marker.txt"
#define PET_APPEARANCE_SERIAL 1
#define PET_APPEARANCE_NATIVE 2
#define PET_P4_STORAGE_IO_TASK_PRIORITY 1
#define PET_P4_PROTOCOL_TASK_PRIORITY 3
#define configASSERT assert
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
typedef int BaseType_t;typedef void*TaskHandle_t;
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char asset_manifest_json[65536];pet_p4_asset_catalog_t asset_catalog;unsigned asset_family_count,asset_revision;bool asset_transfer_active;}pet_p4_runtime_state_t;
static pet_p4_runtime_state_t state;
static _Atomic int g_asset_active_slot;static pet_appearance_session_t g_appearance_session;
static pet_resource_broker_t broker;
static bool held=true,suspended,g_protocol_storage_background,g_protocol_storage_command_entered=true;
static _Atomic bool g_storage_query_window;
static void *g_state_mutex=(void*)1,*g_protocol_task=(void*)2;
static int mode;static unsigned borrows,acks,marker_writes,raw_reads,checks;static bool ack_ok;
static FILE *marker_file;
static pet_resource_realtime_t realtime;
static const char PACK[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void *xTaskGetCurrentTaskHandle(void){return g_protocol_task;}
static void *xSemaphoreGetMutexHolder(void *m){return held?g_protocol_task:NULL;}
static int xSemaphoreGive(void*m){assert(held);held=false;return pdTRUE;}
static int xSemaphoreTake(void*m,unsigned t){assert(!held);held=true;return pdTRUE;}
static void vTaskPrioritySet(void*t,unsigned p){assert(t==g_protocol_task&&(p==1||p==3));}
static void pet_p4_renderer_set_asset_preparation_suspended(bool b){suspended=b;}
static void interleave(void);
static void pet_storage_io_protocol_window(bool enabled){assert(!held);if(enabled){borrows++;interleave();}}
static pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t*out){return pet_resources_owner_open(&broker,m,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t u,pet_resource_lease_t*out){return pet_resources_try_acquire(&broker,o,k,u,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
static void pet_resources_device_revoke(pet_resource_owner_t o){(void)pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){return pet_resources_finish(&broker,l);}
static bool gbegin(void*);static void gend(void*);
static bool (*g_asset_query_io_begin)(void*)=gbegin;static void (*g_asset_query_io_end)(void*)=gend;
static void *g_asset_query_io_context;
static bool slot_is_valid(int n){return n>=0&&n<2;}
static bool p4_asset_manifest_path_for_candidate(int n,char*out,size_t size){snprintf(out,size,"slot%d.json",n);return slot_is_valid(n);}
static bool p4_asset_ready_marker_path(int n,char*out,size_t size){snprintf(out,size,"slot%d.ready",n);return slot_is_valid(n);}
static bool pet_p4_raw_assets_slot_available(int n){assert(!held);return true;}
static bool pet_p4_raw_assets_committed_pack_id_for_slot(int n,char*out,size_t size){assert(!held);raw_reads++;snprintf(out,size,"%s",mode==8?"wrong":PACK);return true;}
typedef struct {bool asset_transfer,raw_receiving,appearance_owner,file_open,slot_pending,native_transfer,native_receiving,native_pending,worker_pending,storage_pending,firmware_ota,reboot_pending;uint32_t native_epoch;}appearance_io_observation_t;
static appearance_io_observation_t appearance_io_observe(const pet_p4_runtime_state_t*s){assert(held);return (appearance_io_observation_t){.appearance_owner=g_appearance_session.active,.worker_pending=true,.storage_pending=mode==9};}
static bool appearance_io_busy(appearance_io_observation_t s){return s.appearance_owner||s.storage_pending||s.worker_pending;}
static FILE*fault_open(const char*p,const char*m){assert(!held);if(!strcmp(p,"marker.txt")&&!strcmp(m,"wb")){marker_writes++;if(mode==10){errno=ENOSPC;return NULL;}}FILE*f=fopen(p,m);if(f&&!strcmp(p,"marker.txt")&&!strcmp(m,"wb"))marker_file=f;return f;}
static int fault_close(FILE*f){assert(!held);bool marker=f==marker_file;if(marker)marker_file=NULL;int rc=fclose(f);if(marker&&mode==4){errno=EIO;return EOF;}return rc;}
static void*workspace_calloc(size_t n,size_t z){assert(!held);if(mode==1)return NULL;return calloc(n,z);}
void *test_heap_caps_calloc(size_t n,size_t z,unsigned caps){assert(!held);if(mode==11)return NULL;return calloc(n,z);}
static void send_asset_ack(pet_p4_send_line_fn f,void*c,const char*i,const char*p,const char*path,bool ok,const char*e){assert(!held&&!strcmp(p,"activate"));acks++;ack_ok=ok;if(!ok)assert(e&&e[0]);}
static void sender(const char*l,void*c){}
static _Atomic uint32_t g_connection_generation=1;
static void pet_p4_native_usb_send_json_line(const char*l,void*c){}
#define fopen fault_open
#define fclose fault_close
#define calloc workspace_calloc
"""
    fixture+='\n'.join(function(code,n) for n in ['json_string','copy_text','pet_p4_protocol_source_valid','read_slot_pack_id','read_ready_slot_pack_id','pet_p4_appearance_claim','pet_p4_appearance_owned','pet_p4_appearance_end'])
    fixture+='\n'.join(function(main,n) for n in ['storage_command_unlock','storage_command_lock'])
    fixture+=r"""
static bool gbegin(void*c){assert(held&&g_protocol_storage_command_entered);storage_command_unlock(c);return true;}
static void gend(void*c){storage_command_lock(c);}
"""
    fixture+='\n'.join(function(production('pet_p4_usb_native.c'),n) for n in ['native_route_context','pet_p4_native_usb_route_current'])
    fixture+=function(code,'asset_phase_send_current')
    start=code.index('/* One protocol command owns each activation')
    fixture+=code[start:code.index('static void handle_asset_file(',start)]
    fixture+=r"""
#undef fopen
#undef fclose
#undef calloc
static void put(const char*p,const char*t){FILE*f=fopen(p,"wb");assert(f);assert(fwrite(t,1,strlen(t),f)==strlen(t));assert(!fclose(f));}
static void interleave(void){
 if(!g_appearance_session.active)return;
 assert(!held&&suspended);
 int route;
 assert(!pet_p4_appearance_claim(sender,&route,"other",PET_APPEARANCE_NATIVE));
 pet_resource_owner_t o={0};pet_resource_lease_t l={0};
 assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&o)==PET_RESOURCE_OK);
 pet_resource_result_t admitted=pet_resources_device_acquire(o,PET_RESOURCE_STORAGE_WORK,1,&l);
 if(borrows<=2)assert(admitted==PET_RESOURCE_BUSY);
 else if(admitted==PET_RESOURCE_OK)(void)pet_resources_device_finish(l);
 pet_resources_device_revoke(o);checks++;
 if(borrows==1&&mode==2)state.asset_revision++;
 if(borrows==1&&mode==14)g_asset_activation_generation++;
 if(borrows==1&&mode==3)assert(pet_resources_realtime_begin(&broker,&realtime)==PET_RESOURCE_OK);
 if(borrows==2&&mode==5)assert(pet_resources_realtime_begin(&broker,&realtime)==PET_RESOURCE_OK);
}
int main(int argc,char**argv){
 assert(argc==2);mode=atoi(argv[1]);g_asset_active_slot=0;state.asset_revision=7;
 strcpy(state.asset_manifest_json,"old");state.asset_family_count=1;
 char manifest[1000];snprintf(manifest,sizeof(manifest),"{\"packId\":\"%s\",\"format\":\"p4-h264-v1\",\"width\":640,\"height\":480,\"fps\":10,\"families\":[{\"family\":\"idle\",\"path\":\"p4/a.h264\",\"frames\":1,\"streamBytes\":42}]}",PACK);
 if(mode==13){char *end=manifest+strlen(manifest)-1;snprintf(end,sizeof(manifest)-(size_t)(end-manifest),",\"packId\":\"duplicate\"}");}
 if(mode==6)strcpy(manifest,"{\"packId\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"format\":\"bad\",\"families\":[]}");
 put("slot1.json",manifest);put("slot1.ready",PACK);put("marker.txt","0\n");
 if(mode==15){
   const unsigned char tail[]={0,'x','y'};FILE *file=fopen("slot1.json","ab");assert(file);
   assert(fwrite(tail,1,sizeof(tail),file)==sizeof(tail));assert(!fclose(file));
 }
 if(mode==7){held=false;g_asset_active_slot=1;assert(pet_p4_asset_catalog_parse(&state.asset_catalog,manifest));strcpy(state.asset_manifest_json,manifest);held=true;}
 char request[200];snprintf(request,sizeof(request),"{\"transferId\":\"tx\",\"slot\":1,\"packId\":\"%s\"}",PACK);
 cJSON*p=cJSON_Parse(request);assert(p);if(mode==12)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p,"slot"),1.5);unsigned old_revision=state.asset_revision;
 handle_asset_activate(&state,p,sender,NULL);
 assert(held&&acks==1&&ack_ok==(mode==0||mode==7));
 if(ack_ok){assert(g_asset_active_slot==1&&state.asset_revision==old_revision+1&&state.asset_catalog.count==1);assert(!strcmp(state.asset_manifest_json,manifest));assert(!g_appearance_session.active);}
 else{assert(g_asset_active_slot==0&&state.asset_family_count==1&&!strcmp(state.asset_manifest_json,"old"));}
 if(mode==4||mode==5||mode==10)assert(g_appearance_session.active);else assert(!g_appearance_session.active);
 if(mode==0||mode==4||mode==5||mode==10)assert(marker_writes==1);else assert(marker_writes==0);
 if(mode!=9&&mode!=12)assert(checks>0);
 if(mode==15){
   assert(raw_reads>0&&state.asset_revision==old_revision&&marker_writes==0&&acks==1&&!ack_ok);
   FILE *file=fopen("marker.txt","rb");assert(file);assert(fgetc(file)=='0'&&fgetc(file)=='\n'&&fgetc(file)==EOF);assert(!fclose(file));
 }
 pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.leases[PET_RESOURCE_STORAGE_WORK]&&!usage.live_owners);
 cJSON_Delete(p);return 0;
}
"""
    return compile_c(tmp_path_factory.mktemp('activation'),fixture,'activation',assets=True,resources=True)
@pytest.mark.parametrize('case',range(16))
def test_actual_activation_interleavings_and_failclosed_publication(activation_binary,case):execute(activation_binary,case)
