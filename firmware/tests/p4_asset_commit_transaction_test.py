"""Real private-candidate commit C + real assets/broker/cJSON, synthetic IO only.
No device/network/PIO. SDK wrappers provide fault/interleave injection, not a
replacement implementation of commit, ownership, parser or resource admission.
"""
from pathlib import Path
import json
import os, subprocess, sys
import pytest
ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'main'
sys.path.insert(0,str(ROOT/'tests'))
from p4_render_trace_test import function

@pytest.fixture(scope='module')
def binary(tmp_path_factory):
 p=SRC/'pet_p4_protocol.c';code=p.read_text();native=(SRC/'pet_p4_usb_native.c').read_text()
 fixture=r'''
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
#include "pet_p4_appearance_session.h"
#include "pet_p4_resources.h"
#include "pet_p4_assets.h"
#include "pet_p4_json_source.h"
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_ASSET_MANIFEST_MAX 65536
#define PET_P4_FS_PATH_MAX 96
#define PET_P4_ASSET_SLOT_COUNT 2
#define PET_P4_JSON_SAFE_INTEGER_MAX 9007199254740991ULL
#define PET_P4_RAW_APPEARANCE_SLOT 1
#define PET_P4_ASSET_SLOT_MARKER "active.marker"
#define PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS 15000
#define PET_APPEARANCE_SERIAL 1
#define PET_APPEARANCE_NATIVE 2
#define ESP_LOGW(...) ((void)0)
#define configASSERT assert
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
typedef int BaseType_t;
typedef void(*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char asset_manifest_json[65536];pet_p4_asset_catalog_t asset_catalog;unsigned asset_family_count,asset_revision;bool asset_transfer_active;}pet_p4_runtime_state_t;
typedef struct {bool(*begin)(void*);void(*end)(void*);bool(*current)(void*);void*context;}pet_appearance_commit_io_t;
typedef enum {PET_APPEARANCE_COMMIT_FAILED,PET_APPEARANCE_COMMIT_OK,PET_APPEARANCE_COMMIT_UNKNOWN}pet_appearance_commit_result_t;
static pet_p4_runtime_state_t state,*g_state=&state;
static pet_appearance_session_t g_appearance_session;
static pet_resource_broker_t broker;
static _Atomic int g_asset_active_slot;
static bool g_asset_phase_write_attempted;
static _Atomic bool g_asset_cleanup_due,g_native_cleanup_due;
static FILE*g_asset_chunk_file;
static char g_asset_chunk_logical_path[128],g_asset_chunk_error[128];
static unsigned long long g_asset_chunk_size,g_asset_chunk_checksum;
static bool g_asset_chunk_stat_valid;
static struct {bool active;unsigned char*data;}g_raw_asset_chunk;
static pet_p4_send_line_fn g_asset_sender;static void*g_asset_route;
static int g_asset_transfer_slot=1;static char g_asset_transfer_id[64];
static unsigned long long g_asset_transfer_last_activity_ms=1;
static char g_transfer_id[64],g_cleaned_transfer_id[64],g_current_path[96],g_expected_checksum[17];
static unsigned long long g_expected_size,g_received_size,g_transfer_last_activity_ms=1;
static int g_target_slot=1;
static bool g_native_file_raw,g_native_failed,g_native_preserve_raw;
static uint64_t g_native_raw_limit,g_native_raw_bytes,g_native_total_bytes=11;
static uint32_t g_native_chunk_index,g_native_files=1;
static _Atomic uint32_t g_connection_generation=1;static uint32_t g_native_transfer_connection=1;
static void*g_state_mutex=(void*)1,*g_native_worker_task=(void*)2;
static bool held=true,suspended,peer_ok=true,ack_ok,raw_present,raw_enabled;
static unsigned mode,borrows,acks,ready_writes,marker_writes,raw_writes,removes,cleanup_calls,frame_turns;
static bool inject_once;static FILE*staged;static FILE*ready_file;static FILE*active_file;static FILE*manifest_file;
static bool ready_read,active_read;
static const char PACK[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void interleave(void);
static void*xTaskGetCurrentTaskHandle(void){return g_native_worker_task;}
static void*xSemaphoreGetMutexHolder(void*m){return held?g_native_worker_task:NULL;}
static int xSemaphoreGive(void*m){assert(held);held=false;borrows++;interleave();return pdTRUE;}
static int xSemaphoreTake(void*m,unsigned n){assert(!held);held=true;return pdTRUE;}
static void pet_p4_renderer_set_asset_preparation_suspended(bool b){suspended=b;}
static bool begin(void*c){assert(held);pet_p4_renderer_set_asset_preparation_suspended(true);return xSemaphoreGive(g_state_mutex)==pdTRUE;}
static void end(void*c){assert(xSemaphoreTake(g_state_mutex,portMAX_DELAY)==pdTRUE);}
static bool current(void*c){return peer_ok;}
static bool(*g_asset_query_io_begin)(void*)=begin;static void(*g_asset_query_io_end)(void*)=end;static void*g_asset_query_io_context;
static pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t*out){return pet_resources_owner_open(&broker,m,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t u,pet_resource_lease_t*out){return pet_resources_try_acquire(&broker,o,k,u,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
static void pet_resources_device_revoke(pet_resource_owner_t o){(void)pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){return pet_resources_finish(&broker,l);}
static bool slot_is_valid(int s){return s==0||s==1;}
static bool p4_asset_manifest_path_for_candidate(int s,char*out,size_t n){snprintf(out,n,"s%d.json",s);return slot_is_valid(s);}
static bool p4_asset_ready_marker_path(int s,char*out,size_t n){snprintf(out,n,"s%d.ready",s);return slot_is_valid(s);}
static bool pet_p4_raw_assets_slot_available(int s){return s==1&&raw_enabled;}
static bool pet_p4_raw_assets_available(void){return raw_enabled;}
static bool pet_p4_raw_assets_committed_pack_id_for_slot(int s,char*out,size_t n){assert(!held);if(!raw_present)return false;snprintf(out,n,"%s",PACK);return s==1;}
static bool pet_p4_raw_assets_commit_observed(const char*p,char*e,size_t n,bool*changed){assert(!held);*changed=false;if(mode==17)return false;*changed=true;raw_writes++;raw_present=true;return mode!=16;}
static void pet_p4_raw_assets_reset_transfer(void){}
static unsigned long long native_now_ms(void){return 50000;}
static void sender(const char*l,void*c){}
static void pet_p4_native_usb_send_json_line(const char*l,void*c){}
static void serial_ack(pet_p4_send_line_fn f,void*c,const char*i,const char*p,const char*path,bool ok,const char*e){assert(!held&&(!strcmp(p,"commit")||!strcmp(p,"patch")));acks++;ack_ok=ok;if(!ok)assert(e&&*e);}
static void native_ack(const char*i,const char*p,const char*path,bool ok,const char*e){assert(!held&&!strcmp(p,"commit"));acks++;ack_ok=ok;if(!ok)assert(e&&*e);}
static bool clear_slot_files(int s){cleanup_calls++;return true;}
static FILE*fault_open(const char*p,const char*m){assert(!held);bool write=strchr(m,'w')!=NULL;
 if(write&&strstr(p,"ready")){ready_writes++;if(mode==5){errno=ENOSPC;return NULL;}}
 if(write&&!strcmp(p,"active.marker")){marker_writes++;if(mode==7){errno=ENOSPC;return NULL;}}
 FILE*f=fopen(p,m);if(f&&strstr(p,"ready")){ready_file=f;ready_read=!write;}
 if(f&&!strcmp(p,"active.marker")){active_file=f;active_read=!write;}
 if(f&&strstr(p,".json"))manifest_file=f;
 return f;
}
static int fault_close(FILE*f){assert(!held);bool st=f==staged,rd=f==ready_file,ac=f==active_file,mf=f==manifest_file;
 if(st)staged=NULL;if(rd)ready_file=NULL;if(ac)active_file=NULL;if(mf)manifest_file=NULL;
 int rc=fclose(f);if((st&&mode==2)||(rd&&!ready_read&&mode==6)||(ac&&!active_read&&mode==8)
  ||(mf&&mode==18)||(rd&&ready_read&&mode==19)||(ac&&active_read&&mode==20)){errno=EIO;return EOF;}return rc;
}
static void*workspace_calloc(size_t n,size_t z){assert(!held);return mode==3?NULL:calloc(n,z);}
void*test_heap_caps_calloc(size_t n,size_t z,unsigned c){assert(!held);return calloc(n,z);}
static int fault_remove(const char*p){removes++;assert(!"no commit cleanup/remove permitted");return -1;}
#define fopen fault_open
#define fclose fault_close
#define calloc workspace_calloc
#define remove fault_remove
'''
 fixture+='\n'.join(function(native,n) for n in ('native_route_context','pet_p4_native_usb_route_current'))
 # Actual ownership and physical validators, no replacement parser.
 fixture+='\n'.join(function(code,n) for n in ('json_string','json_u64','copy_text',
  'pet_p4_protocol_source_valid','pet_p4_appearance_claim','pet_p4_appearance_owned','pet_p4_appearance_end',
  'pet_p4_appearance_active','close_asset_chunk_file','reset_asset_chunk_tracking','reset_raw_asset_chunk','read_active_slot_marker',
  'read_slot_pack_id','read_ready_slot_pack_id'))
 start=code.index('typedef struct {\n  char manifest[PET_P4_ASSET_MANIFEST_MAX]')
 stop=code.index('static void send_asset_activation_ack(',start)
 fixture+=code[start:stop]
 fixture+='\n'.join(function(code,n) for n in ('prepare_asset_snapshot','write_asset_activation_marker',
  'pet_p4_appearance_mutation_blocked','asset_phase_send_current','commit_owner_current','write_ready_pack_exact','pet_p4_appearance_commit_slot'))
 fixture+='\n'.join(function(code,n) for n in ('reset_asset_transfer_slot','abort_serial_asset_transfer','pet_p4_asset_transfer_process'))
 # Forward close/timeout helpers need this declaration only, not alternate logic.
 fixture=fixture.replace('static bool clear_slot_files(int s)', 'static bool pet_p4_appearance_mutation_blocked(void);\nstatic bool clear_slot_files(int s)')
 fixture=fixture.replace('bool pet_p4_appearance_mutation_blocked(void) {','static bool pet_p4_appearance_mutation_blocked(void) {')
 fixture+='\n#define send_asset_ack serial_ack\n'
 fixture+='\n'.join(function(code,n) for n in ('send_serial_commit_ack','handle_serial_asset_commit'))
 fixture+='\n#undef send_asset_ack\n#define send_asset_ack native_ack\n'
 fixture+='\n'.join(function(native,n) for n in ('native_owned','native_retire','pet_p4_native_usb_process'))
 fixture+='\ntypedef struct {uint32_t connection;}native_commit_context_t;\n'
 fixture+='\n'.join(function(native,n) for n in ('native_commit_current','native_commit_io_begin','native_commit_io_end','native_commit_ack'))
 fixture+=function(native,'parse_payload_json')
 fixture+=function(native,'handle_commit')
 fixture+=r'''
#undef fopen
#undef fclose
#undef calloc
#undef remove
#undef send_asset_ack
static void put(const char*p,const char*s){FILE*f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
static void interleave(void){
 assert(!held);frame_turns++; /* Simulated main can take a short snapshot lock. */
 held=true;
 if(g_appearance_session.operation_active){
  assert(!pet_p4_appearance_end(g_appearance_session.sender,(void*)g_appearance_session.route,g_appearance_session.id,g_appearance_session.kind));
  assert(!pet_p4_appearance_claim(sender,(void*)99,"steal",PET_APPEARANCE_NATIVE));
  pet_p4_asset_transfer_process(&state,100000);pet_p4_native_usb_process(100000);
  assert(!cleanup_calls&&state.asset_transfer_active);
  pet_resource_owner_t o={0};pet_resource_lease_t l={0};
  assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&o)==PET_RESOURCE_OK);
  pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);
  if(usage.leases[PET_RESOURCE_STORAGE_WORK]) {
    pet_resource_result_t denied=pet_resources_device_acquire(o,PET_RESOURCE_STORAGE_WORK,1,&l);
    assert(denied==((mode==13&&inject_once)?PET_RESOURCE_PROTECTED:PET_RESOURCE_BUSY));
  }
  pet_resources_device_revoke(o);
 }
 held=false;
 if(inject_once)return;
 if((mode==9&&borrows==1)||(mode==10&&borrows==2)){state.asset_revision++;inject_once=true;}
 if(mode==11&&borrows==2){g_appearance_session.route=(void*)77;inject_once=true;}
 if(mode==12&&borrows==1){g_appearance_session.generation++;inject_once=true;}
 if(mode==13&&borrows==2){pet_resource_realtime_t rt={0};assert(pet_resources_realtime_begin(&broker,&rt)==PET_RESOURCE_OK);inject_once=true;}
 if(mode==21&&borrows==2){atomic_fetch_add(&g_connection_generation,1);inject_once=true;}
}
int main(int argc,char**argv){
 assert(argc==4);mode=(unsigned)atoi(argv[1]);bool patch=atoi(argv[2]),native=atoi(argv[3]);
 raw_enabled=mode==16||mode==17;
 char manifest[1000];snprintf(manifest,sizeof(manifest),"{\"packId\":\"%s\",\"format\":\"p4-h264-v1\",\"width\":640,\"height\":480,\"fps\":10,\"families\":[{\"family\":\"idle\",\"path\":\"p4/a.h264\",\"frames\":1,\"streamBytes\":42}]}",PACK);
 if(mode==4)strcpy(manifest,"{\"packId\":\"bad\",\"families\":[]}");
 put("s1.json",manifest);put("active.marker","0\n");
 if(mode==22){FILE*f=fopen("s1.json","ab");assert(f);assert(fwrite("\0tail",1,5,f)==5);assert(!fclose(f));}
 put("s1.ready","old-evidence");
 strcpy(state.asset_manifest_json,"old");state.asset_revision=7;state.asset_family_count=1;state.asset_transfer_active=true;
 assert(pet_p4_appearance_claim(native?pet_p4_native_usb_send_json_line:sender,NULL,"tx",native?PET_APPEARANCE_NATIVE:PET_APPEARANCE_SERIAL));
 strcpy(g_asset_transfer_id,"tx");g_asset_sender=sender;g_asset_route=NULL;strcpy(g_transfer_id,"tx");
 if(!native){held=false;g_asset_chunk_file=staged=fopen("staged.bin","wb");assert(staged);assert(fwrite("buffered",1,8,staged)==8);held=true;}
 if(mode==14){g_asset_route=(void*)77;} /* exact route mismatch reaches helper below */
 if(native){
  const char*body="{\"transferId\":\"tx\",\"fileCount\":1,\"totalBytes\":11}";
  handle_commit((const uint8_t*)body,(uint32_t)strlen(body));
 }else if(mode==14){handle_serial_asset_commit(&state,sender,(void*)77,"tx",patch);}
 else handle_serial_asset_commit(&state,sender,NULL,"tx",patch);
 assert(held&&acks<2&&!removes&&!cleanup_calls);
 bool success=mode==0 || (patch&&mode!=2&&mode!=9&&mode!=12&&mode!=14);
 if(native&&mode==21){assert(!acks&&!ack_ok);}else assert(acks==1&&ack_ok==success);
 if(success){assert(!g_appearance_session.active&&!state.asset_transfer_active);
  if(!patch)assert(g_asset_active_slot==1&&state.asset_revision==8&&!strcmp(state.asset_manifest_json,manifest));
  else assert(g_asset_active_slot==0&&state.asset_revision==7&&!strcmp(state.asset_manifest_json,"old"));
 }else {
  assert(g_appearance_session.active&&state.asset_transfer_active);
  assert(g_asset_active_slot==0&&!strcmp(state.asset_manifest_json,"old"));
  if(mode==2||mode==5||mode==6||mode==7||mode==8||mode==10||mode==11||mode==12||mode==13||mode==16||mode==19||mode==20||mode==21){
   assert(pet_p4_appearance_mutation_blocked());
   unsigned writes=ready_writes+marker_writes+raw_writes;
   unsigned generation=(unsigned)g_appearance_session.generation;
   assert(!abort_serial_asset_transfer(&state));native_retire();
   pet_p4_asset_transfer_process(&state,100000);pet_p4_native_usb_process(100000);
   assert(!cleanup_calls&&g_appearance_session.active&&g_appearance_session.generation==generation);
   char error[128];pet_appearance_commit_io_t io={begin,end,current,NULL};
   assert(pet_p4_appearance_commit_slot(&state,1,sender,NULL,"tx",PET_APPEARANCE_SERIAL,patch,&io,error,sizeof(error))!=PET_APPEARANCE_COMMIT_OK);
   assert(writes==ready_writes+marker_writes+raw_writes);
  }
 }
 if(mode==3||mode==4||mode==9||mode==14||mode==17||mode==18||mode==22)assert(!ready_writes&&!marker_writes&&!raw_writes);
 if(mode==17)assert(!g_appearance_session.recovery_blocked); /* raw pre-write allocation rejection */
 if(patch)assert(!ready_writes&&!marker_writes&&!raw_writes);
 if(!native&&mode!=14)assert(!g_asset_chunk_file); /* attempted close, never a retry */
 if(mode==14){assert(g_asset_chunk_file);fclose(g_asset_chunk_file);g_asset_chunk_file=NULL;}
 pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.leases[PET_RESOURCE_STORAGE_WORK]&&!usage.live_owners);
 assert(frame_turns>0);
 /* Real bytes after failed close remain evidence, not rollback candidates.
  * A new boot may observe a durable new marker even while this command was UNKNOWN. */
 if(mode==8&&!patch){held=false;g_asset_active_slot=-1;read_active_slot_marker();assert(g_asset_active_slot==1);held=true;}
 return 0;
}
'''
 tmp=tmp_path_factory.mktemp('commit-c')
 (tmp/'esp_heap_caps.h').write_text('#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nvoid *test_heap_caps_calloc(size_t,size_t,unsigned);\n#define heap_caps_calloc test_heap_caps_calloc\n#define heap_caps_free free\n')
 src=tmp/'commit.c';src.write_text(fixture);exe=tmp/'commit'
 cmd=['/usr/bin/cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
  '-Wno-unused-variable','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-omit-frame-pointer',
  '-I',str(tmp),'-I',str(SRC),'-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),
  str(src),str(ROOT/'tests/vendor/cjson/cJSON.c'),str(ROOT/'main/pet_p4_assets.c'),
  str(ROOT/'main/pet_p4_resources.c'),'-o',str(exe)]
 (tmp/'compile-invocation.json').write_text(json.dumps({'argv':cmd,'cwd':str(tmp)},indent=2))
 subprocess.run(cmd,cwd=tmp,check=True,timeout=60)
 return exe

def invoke(binary,tmp_path,case,patch,native):
 argv=[str(binary),str(case),str(patch),str(native)]
 (tmp_path/'invocation.json').write_text(json.dumps({'argv':argv,'cwd':str(tmp_path)},indent=2))
 subprocess.run(argv,cwd=tmp_path,check=True,timeout=15)

@pytest.mark.parametrize('case',[0,2,3,4,5,6,7,8,9,10,11,12,13,14,16,17,18,19,20,22])
def test_real_serial_full_commit_faults(binary,tmp_path,case):
 invoke(binary,tmp_path,case,0,0)

@pytest.mark.parametrize('case',[0,2,9,12,14])
def test_real_serial_patch_commit_close_and_owner(binary,tmp_path,case):
 invoke(binary,tmp_path,case,1,0)

@pytest.mark.parametrize('case',[0,3,4,5,6,7,8,9,10,12,13,16,17,18,19,20,21,22])
def test_real_native_commit_connection_and_quarantine(binary,tmp_path,case):
 invoke(binary,tmp_path,case,0,1)

def test_whole_real_raw_module_and_observed_header_outcome(tmp_path):
 source=(SRC/'pet_p4_raw_assets.c').read_text()
 for include in ('#include "esp_partition.h"','#include "freertos/FreeRTOS.h"','#include "freertos/task.h"'):
  source=source.replace(include,'')
 harness=(ROOT/'tests/p4_raw_asset_cache_test.c').read_text()
 harness=harness.replace('static _Atomic bool block_erase,','static bool fail_after_write;\nstatic _Atomic bool block_erase,')
 old='pthread_mutex_unlock(&flash_lock); return ESP_OK;\n}'
 # Only the SDK write wrapper, never the actual production writer, is injected.
 at=harness.index('static int esp_partition_write(');end=harness.index('static int esp_partition_erase_range(',at)
 writer=harness[at:end]
 assert writer.count(old)==1
 writer=writer.replace(old,'pthread_mutex_unlock(&flash_lock); return fail_after_write ? 1 : ESP_OK;\n}')
 harness=harness[:at]+writer+harness[end:]
 harness=harness.replace('int main(void) {','static int existing_cache_main(void) {')
 harness=harness.replace('/* RAW_PRODUCTION */',source)
 harness+=r'''
int main(void){
 assert(existing_cache_main()==0);
 char error[128];bool changed=true;uint8_t before[4096];char pack[65];
 stage_commit();memcpy(before,flash_bytes[1],sizeof(before));fail_malloc=true;
 assert(!pet_p4_raw_assets_commit_observed(new_pack,error,sizeof(error),&changed));fail_malloc=false;
 assert(!changed&&!memcmp(before,flash_bytes[1],sizeof(before)));assert_no_mutation();
 changed=true;assert(!pet_p4_raw_assets_commit_observed("invalid",error,sizeof(error),&changed));assert(!changed);
 assert(begin_cache_mutation(1));changed=true;
 assert(!pet_p4_raw_assets_commit_observed(new_pack,error,sizeof(error),&changed));assert(!changed);end_cache_mutation(1);
 /* The driver can physically write a whole header and still report failure. */
 stage_commit();fail_after_write=true;changed=false;
 assert(!pet_p4_raw_assets_commit_observed(new_pack,error,sizeof(error),&changed));fail_after_write=false;
 assert(changed&&g_transfer.prepared);assert_no_mutation();
 assert(pet_p4_raw_assets_committed_pack_id_for_slot(1,pack,sizeof(pack))&&!strcmp(pack,new_pack));
 /* The new observed API preserves the existing writer's complete semantics. */
 assert(pet_p4_raw_assets_commit_observed(new_pack,error,sizeof(error),&changed));assert(changed);assert_no_mutation();
 puts("whole raw module + observed prewrite/driver-after-write/success classification passed");return 0;
}
'''
 src=tmp_path/'raw.c';src.write_text(harness);exe=tmp_path/'raw'
 argv=['/usr/bin/cc','-std=c11','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined',
  '-I',str(SRC),'-I',str(ROOT/'main'),str(src),'-o',str(exe)]
 (tmp_path/'compile-invocation.json').write_text(json.dumps({'argv':argv,'cwd':str(tmp_path)},indent=2))
 subprocess.run(argv,cwd=tmp_path,check=True,timeout=60)
 invoke(exe,tmp_path,0,0,0)

def test_actual_public_commit_and_raw_header_signatures(tmp_path):
 src=tmp_path/'interface.c'
 src.write_text('''#include "pet_p4_protocol.h"
#include "pet_p4_raw_assets.h"
static pet_appearance_commit_result_t (*commit_fn)(pet_p4_runtime_state_t*,int,
 pet_p4_send_line_fn,void*,const char*,unsigned,bool,const pet_appearance_commit_io_t*,
 char*,size_t)=pet_p4_appearance_commit_slot;
static bool (*raw_fn)(const char*,char*,size_t,bool*)=pet_p4_raw_assets_commit_observed;
int main(void){return !commit_fn||!raw_fn;}
''')
 argv=['/usr/bin/cc','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(SRC),str(src)]
 (tmp_path/'invocation.json').write_text(json.dumps({'argv':argv,'cwd':str(tmp_path)},indent=2))
 subprocess.run(argv,cwd=tmp_path,check=True,timeout=15)
