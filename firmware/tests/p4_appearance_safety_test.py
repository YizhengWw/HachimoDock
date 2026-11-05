"""Real admission/commit/transport helpers, injected IO; never a device/PIO test."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function
from p4_appearance_upload_phase_test import appearance_upload_binary,run_upload_case
ROOT=Path(__file__).resolve().parents[1]
def run_c(tmp_path,code,name,resources=False):
    src=tmp_path/(name+'.c');src.write_text(code);exe=tmp_path/name
    command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(src),str(ROOT/'tests/vendor/cjson/cJSON.c')]
    if resources:command.append(str(ROOT/'main/pet_p4_resources.c'))
    if name=='native':
        (tmp_path/'esp_heap_caps.h').write_text('#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define heap_caps_calloc(n,z,c) calloc(n,z)\n#define heap_caps_free free\n')
        command[1:1]=['-I',str(tmp_path)]
        command.append(str(ROOT/'main/pet_p4_assets.c'))
    subprocess.run([*command,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=tmp_path,check=True,timeout=20)

def test_pure_appearance_session_routes_ids_kinds_numeric_bounds_and_no_steal(tmp_path):
    run_c(tmp_path,r'''
#include "pet_p4_appearance_session.h"
#include <assert.h>
#include <math.h>
static void a(const char*s,void*c){}
static void b(const char*s,void*c){}
int main(void){
 pet_appearance_session_t s={0},old;int uart=0,jtag=1;
 assert(!pet_appearance_claim(&s,a,&uart,"",1));
 char id[65];memset(id,'x',64);id[64]=0;assert(!pet_appearance_claim(&s,a,&uart,id,1));id[63]=0;
 assert(pet_appearance_claim(&s,a,&uart,id,1));old=s;
 assert(!pet_appearance_claim(&s,b,&jtag,"other",2)&&!memcmp(&s,&old,sizeof(s)));
 assert(!pet_appearance_matches(&s,a,&jtag,id,1));assert(!pet_appearance_matches(&s,b,&uart,id,1));
 assert(!pet_appearance_matches(&s,a,&uart,id,2));assert(!pet_appearance_release(&s,a,&uart,"old",1));
 assert(pet_appearance_release(&s,a,&uart,id,1));assert(!s.active&&!s.sender&&!s.id[0]);
 assert(!pet_appearance_release(&s,a,&uart,id,1));
 uint32_t n=99;assert(!pet_appearance_size(-1,&n)&&n==99);assert(!pet_appearance_size(NAN,&n));
 assert(!pet_appearance_size(INFINITY,&n)&&!pet_appearance_size(0.5,&n));
 assert(pet_appearance_size(UINT32_MAX,&n)&&n==UINT32_MAX);assert(!pet_appearance_size(4294967296.,&n));
 assert(pet_appearance_checksum("0123456789abcdef"));assert(!pet_appearance_checksum("0"));assert(!pet_appearance_checksum("0123456789abcdeg"));
 return 0;
}''','session')

def test_jtag_real_queue_never_offers_more_than_ring_and_keeps_exact_bytes(tmp_path):
    main=(ROOT/'main/pet_p4_main.c').read_text()
    run_c(tmp_path,r'''
#include "pet_p4_transport_audit.h"
#include <assert.h>
#include <string.h>
#define pdMS_TO_TICKS(x) (x)
static unsigned calls,mode;static size_t used;static char sent[40000];
static int usb_serial_jtag_write_bytes(const void *p,size_t n,int timeout){
 assert(n&&n<=4096&&timeout==20);calls++;
 if(mode==1)return 0;if(mode==2)return (int)n+1;
 if(mode==3&&calls==2)return 0;
 memcpy(sent+used,p,n);used+=n;return (int)n;
}
'''+function(main,'usb_queue_all')+r'''
int main(void){
 char body[32768];for(size_t i=0;i<sizeof(body);i++)body[i]=(char)(i%251);
 const size_t sizes[]={1,4095,4096,4097,5889,8641,32768};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++){
  used=calls=mode=0;pet_transport_write_observation_t o={0};usb_queue_all(body,sizes[i],&o);
  assert(used==sizes[i]&&o.accepted==sizes[i]&&!memcmp(sent,body,used)&&!o.invalid_return);
 }
 for(mode=1;mode<=3;mode++){
  used=calls=0;pet_transport_write_observation_t o={0};usb_queue_all(body,8641,&o);
  assert(o.accepted==used&&calls==(mode==3?2:1));assert(mode!=2||o.invalid_return);
  assert(used==(mode==3?4096:0));
 }
 return 0;
}''','jtag')

def native_harness():
    native=(ROOT/'main/pet_p4_usb_native.c').read_text();protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    helpers='\n'.join(function(native,n) for n in ('fnv1a64_update','fnv1a64_file','json_string','json_u64','parse_payload_json','native_route_context','pet_p4_native_usb_route_current'))
    authority='\n'.join(function(protocol,n) for n in ('pet_p4_appearance_claim','pet_p4_appearance_owned','pet_p4_appearance_end','pet_p4_appearance_active','copy_text'))
    commit='\n'.join(function(protocol,n) for n in (
        'pet_p4_protocol_source_valid','p4_asset_manifest_path_for_candidate','p4_asset_ready_marker_path',
        'read_slot_pack_id','read_ready_slot_pack_id','prepare_asset_snapshot','write_asset_activation_marker',
        'pet_p4_appearance_mutation_blocked','asset_phase_send_current','commit_owner_current','write_ready_pack_exact','pet_p4_appearance_commit_slot'))
    handlers='\n'.join(function(native,n) for n in ('clear_slot_files','ensure_transfer_slot','native_owned','native_retire','handle_file_begin','handle_file_data','handle_file_end','native_commit_current','native_commit_io_begin','native_commit_io_end','native_commit_ack','handle_commit','handle_asset_abort_json','pet_p4_native_usb_process'))
    return r'''
#include "cJSON.h"
#include "pet_p4_json_source.h"
#include "pet_p4_appearance_session.h"
#include "pet_p4_resources.h"
#include "pet_p4_assets.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stddef.h>
#define PET_P4_RAW_APPEARANCE_SLOT 1
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_FS_PATH_MAX 64
#define PET_P4_SPIFFS_PREFIX "/spiffs"
typedef pet_appearance_sender_t pet_p4_send_line_fn;
#define PET_P4_ASSET_SLOT_MARKER "/spiffs/p4slot.txt"
#define PET_P4_ASSET_TRANSFER_IDLE_TIMEOUT_MS 15000
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
enum {PET_APPEARANCE_SERIAL=1,PET_APPEARANCE_NATIVE=2};
typedef struct {bool asset_transfer_active;unsigned asset_family_count,asset_revision;char asset_manifest_json[65536];pet_p4_asset_catalog_t asset_catalog;}pet_p4_runtime_state_t;
typedef struct {bool(*begin)(void*);void(*end)(void*);bool(*current)(void*);void*context;}pet_appearance_commit_io_t;
typedef enum {PET_APPEARANCE_COMMIT_FAILED,PET_APPEARANCE_COMMIT_OK,PET_APPEARANCE_COMMIT_UNKNOWN}pet_appearance_commit_result_t;
typedef struct {uint32_t connection;}native_commit_context_t;
#define PET_P4_ASSET_MANIFEST_MAX 65536
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define configASSERT assert
typedef int BaseType_t;
static bool held=true,commit_testing;static void*g_state_mutex=(void*)1,*g_native_worker_task=(void*)2;
static _Atomic uint32_t g_connection_generation=1;static uint32_t g_native_transfer_connection;
static void*xTaskGetCurrentTaskHandle(void){return g_native_worker_task;}
static void*xSemaphoreGetMutexHolder(void*m){return held?g_native_worker_task:NULL;}
static int xSemaphoreGive(void*m){assert(held);held=false;return pdTRUE;}
static int xSemaphoreTake(void*m,unsigned n){assert(!held);held=true;return pdTRUE;}
static bool close_asset_chunk_file(void){return true;}
typedef struct {char manifest[65536];pet_p4_asset_catalog_t catalog;char pack_id[65];}asset_activation_snapshot_t;
static pet_p4_runtime_state_t state,*g_state=&state;
static pet_appearance_session_t g_appearance_session;
static char g_transfer_id[64],g_cleaned_transfer_id[64],g_current_path[96],g_expected_checksum[17];
static unsigned long long g_expected_size,g_received_size,g_transfer_last_activity_ms;
static int g_target_slot=-1;
static bool g_native_file_raw,g_native_failed,g_native_preserve_raw;
static uint64_t g_native_raw_limit,g_native_raw_bytes;
static uint32_t g_native_chunk_index,g_native_files;
static uint64_t g_native_total_bytes;
static _Atomic int g_asset_active_slot;
static bool suspended,ack_ok,raw_available,raw_prepared,raw_finished,raw_committed;
static unsigned writes,erases,raw_writes,raw_finishes,raw_commits,fail_at;
static unsigned long long raw_size,raw_hash;
static char ack_phase[24];
static pet_resource_broker_t broker;static pet_resource_owner_t worker_owner;static pet_resource_lease_t worker_lease;
static pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t*out){return pet_resources_owner_open(&broker,m,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t n,pet_resource_lease_t*out){return pet_resources_try_acquire(&broker,o,k,n,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
static void pet_resources_device_revoke(pet_resource_owner_t o){(void)pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){return pet_resources_finish(&broker,l);}
static const char PACK[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static void pet_p4_native_usb_send_json_line(const char*s,void*c){}
static void pet_p4_renderer_set_asset_preparation_suspended(bool value){suspended=value;if(value&&worker_owner.generation)pet_resources_owner_revoke(&broker,worker_owner);}
static void worker_begin(void){assert(!worker_lease.serial);assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_ASSET_PREPARE),&worker_owner)==PET_RESOURCE_OK);assert(pet_resources_try_acquire(&broker,worker_owner,PET_RESOURCE_ASSET_PREPARE,1,&worker_lease)==PET_RESOURCE_OK);}
static void worker_retire(void){assert(!pet_resources_current(&broker,worker_lease));assert(pet_resources_finish(&broker,worker_lease)==PET_RESOURCE_FINISH_REVOKED);worker_lease=(pet_resource_lease_t){0};worker_owner=(pet_resource_owner_t){0};}
static const char*local(const char*p){return !strncmp(p,"/spiffs/",8)?p+8:p;}
static FILE*checked_fopen(const char*p,const char*m){if(commit_testing){assert(!held);if(fail_at==6&&strstr(p,"manifest")&&!strchr(m,'w'))return NULL;}if(strchr(m,'w')||strchr(m,'a')){assert(suspended);writes++;if((fail_at==5&&strstr(p,"ready"))||(fail_at==7&&strstr(p,".tmp"))){errno=EIO;return NULL;}}return fopen(local(p),m);}
static int checked_remove(const char*p){assert(suspended);writes++;return remove(local(p));}
static int checked_rename(const char*a,const char*b){assert(suspended);writes++;return rename(local(a),local(b));}
static DIR*checked_opendir(const char*p){assert(!strcmp(p,"/spiffs"));if(fail_at==8)return NULL;return opendir(".");}
#define fopen checked_fopen
#define remove checked_remove
#define rename checked_rename
#define opendir checked_opendir
static bool slot_is_valid(int slot){return slot==0||slot==1;}
static bool pet_p4_asset_fs_path_for_slot(int slot,const char*p,bool tmp,char*out,size_t n){
 if(slot!=1)return false;const char*stem=!strcmp(p,"p4/manifest.json")?"manifest":!strcmp(p,"p4/clip.h264")?"clip":!strcmp(p,"p4/test.bin")?"test":NULL;
 if(!stem)return false;snprintf(out,n,"/spiffs/s1_%s%s",stem,tmp?".tmp":"");return true;
}
static bool pet_p4_raw_assets_available(void){return raw_available;}
static bool pet_p4_raw_assets_slot_available(int slot){return slot==1&&raw_available;}
static unsigned long long pet_p4_raw_assets_capacity_bytes(void){return 10481664;}
static bool pet_p4_raw_assets_supports_path(const char*p){return !strcmp(p,"p4/clip.h264");}
static void pet_p4_raw_assets_reset_transfer(void){raw_prepared=raw_finished=false;raw_size=0;raw_hash=0xcbf29ce484222325ULL;}
static bool pet_p4_raw_assets_invalidate(char*e,size_t n){assert(suspended);erases++;raw_committed=false;pet_p4_raw_assets_reset_transfer();return true;}
static bool pet_p4_raw_assets_prepare(unsigned long long n,char*e,size_t c){assert(suspended&&n<=10481664);if(fail_at==1)return false;raw_prepared=true;return true;}
static bool pet_p4_raw_assets_write_chunk(const char*p,const char*i,const unsigned char*d,size_t n,char*e,size_t c);
static bool pet_p4_raw_assets_finish_file(const char*p,unsigned long long size,const char*h,char*e,size_t c);
static bool pet_p4_raw_assets_commit_observed(const char*p,char*e,size_t c,bool*changed){*changed=true;assert(suspended&&raw_prepared&&raw_finished&&!strcmp(p,PACK));raw_commits++;if(fail_at==4)return false;raw_committed=true;return true;}
static bool pet_p4_raw_assets_committed_pack_id_for_slot(int slot,char*out,size_t n){if(slot!=1||!raw_committed)return false;snprintf(out,n,"%s",PACK);return true;}


static bool pet_p4_diagnostics_reboot_pending(void){return false;}
static bool pet_p4_ota_transfer_active(void){return false;}
static unsigned long long native_now_ms(void){return 100;}
static void send_asset_ack(const char*i,const char*p,const char*path,bool ok,const char*e){ack_ok=ok;snprintf(ack_phase,sizeof(ack_phase),"%s",p);}
'''+helpers+authority+commit+r'''
static bool pet_p4_raw_assets_write_chunk(const char*p,const char*i,const unsigned char*d,size_t n,char*e,size_t c){
 assert(suspended&&raw_prepared&&!strcmp(p,"p4/clip.h264"));assert(strtoul(i,NULL,10)==raw_writes);raw_writes++;
 if(fail_at==2)return false;raw_hash=fnv1a64_update(raw_hash,d,n);raw_size+=n;return true;
}
static bool pet_p4_raw_assets_finish_file(const char*p,unsigned long long size,const char*h,char*e,size_t c){
 assert(suspended&&raw_prepared);raw_finishes++;char hash[17];snprintf(hash,sizeof(hash),"%016llx",raw_hash);
 if(fail_at==3||size!=raw_size||strcmp(h,hash))return false;raw_finished=true;return true;
}
'''+handlers+r'''
static const unsigned char data[]={1,2,3};
static char manifest[512];
static void begin_with_raw(const char*id,const char*path,const unsigned char*bytes,size_t n,unsigned budget){
 char meta[384];snprintf(meta,sizeof(meta),"{\"transferId\":\"%s\",\"path\":\"%s\",\"size\":%u,\"checksum\":\"%016llx\",\"rawBytes\":%u}",id,path,(unsigned)n,fnv1a64_update(0xcbf29ce484222325ULL,bytes,n),budget);handle_file_begin((const uint8_t*)meta,(uint32_t)strlen(meta));
}
static void begin(const char*id,const char*path,const unsigned char*bytes,size_t n){begin_with_raw(id,path,bytes,n,3);}
static void end(const char*id,const char*path){char meta[192];snprintf(meta,sizeof(meta),"{\"transferId\":\"%s\",\"path\":\"%s\"}",id,path);handle_file_end((const uint8_t*)meta,(uint32_t)strlen(meta));}
static void commit(const char*id,unsigned files,unsigned bytes){char meta[192];snprintf(meta,sizeof(meta),"{\"transferId\":\"%s\",\"fileCount\":%u,\"totalBytes\":%u}",id,files,bytes);commit_testing=true;handle_commit((const uint8_t*)meta,(uint32_t)strlen(meta));commit_testing=false;}
static void abort_id(const char*id){char meta[192];snprintf(meta,sizeof(meta),"{\"topic\":\"asset/abort\",\"payload\":{\"transferId\":\"%s\"}}",id);assert(handle_asset_abort_json((const uint8_t*)meta,(uint32_t)strlen(meta)));}
int main(void){
 snprintf(manifest,sizeof(manifest),"{\"packId\":\"%s\",\"format\":\"p4-h264-v1\",\"width\":640,\"height\":480,\"fps\":10,\"families\":[{\"family\":\"idle\",\"path\":\"p4/clip.h264\",\"frames\":1,\"streamBytes\":3}]}",PACK);
 /* Invalid objects cannot clear a committed slot. */
 const char*bad[]={"{}","[]","{\"transferId\":\"X\",\"path\":\"../bad\",\"size\":3,\"checksum\":\"0000000000000000\"}","{\"transferId\":\"X\",\"path\":\"p4/manifest.json\",\"size\":-1,\"checksum\":\"0000000000000000\"}"};
 raw_available=raw_committed=true;
 g_connection_generation=UINT32_MAX;begin("exhausted","p4/manifest.json",(const uint8_t*)manifest,strlen(manifest));
 assert(!ack_ok&&!writes&&!erases&&!pet_p4_appearance_active());g_connection_generation=1;
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){handle_file_begin((const uint8_t*)bad[i],(uint32_t)strlen(bad[i]));assert(!ack_ok&&!writes&&!erases&&raw_committed&&!pet_p4_appearance_active());}
 /* Another serial route owns physical storage: native begin/abort/commit
  * have zero writes and cannot retire that owner. */
 int other=2;assert(pet_p4_appearance_claim(pet_p4_native_usb_send_json_line,&other,"serial",PET_APPEARANCE_SERIAL));
 begin("native","p4/clip.h264",data,3);assert(!ack_ok&&!writes&&!erases);abort_id("serial");assert(!ack_ok&&!writes);commit("serial",1,3);assert(!ack_ok&&!writes);
 assert(pet_p4_appearance_end(pet_p4_native_usb_send_json_line,&other,"serial",PET_APPEARANCE_SERIAL));
 /* Real resource ownership still revokes prepare without freeing it early. */
 worker_begin();begin("good","p4/clip.h264",data,3);assert(ack_ok&&state.asset_transfer_active);worker_retire();
 handle_file_data(data,3);assert(raw_writes==1&&raw_size==3&&!writes);unsigned effects=erases+writes;
 end("wrong","p4/clip.h264");assert(!ack_ok&&erases+writes==effects&&g_received_size==3);
 end("good","p4/clip.h264");assert(ack_ok&&raw_finishes==1);
 begin("good","p4/manifest.json",(const uint8_t*)manifest,strlen(manifest));assert(ack_ok);
 handle_file_data((const uint8_t*)manifest,(uint32_t)strlen(manifest));end("good","p4/manifest.json");assert(ack_ok);
 commit("old",2,3+(unsigned)strlen(manifest));assert(!ack_ok&&!raw_committed);
 commit("good",2,3+(unsigned)strlen(manifest));assert(ack_ok&&raw_committed&&raw_commits==1&&g_asset_active_slot==1&&state.asset_family_count==1);
 assert(!state.asset_transfer_active&&!pet_p4_appearance_active()&&g_target_slot==-1);
 effects=writes+erases;abort_id("good");assert(!ack_ok&&effects==writes+erases&&raw_committed);handle_commit(NULL,0);assert(!ack_ok&&effects==writes+erases);
 /* Explicit rawBytes=0 is metadata only, not prepare(0)/erase-all. */
 unsigned old_erases=erases,old_raw_commits=raw_commits;
 begin_with_raw("metadata","p4/clip.h264",data,3,0);assert(!ack_ok&&erases==old_erases&&!pet_p4_appearance_active());
 begin_with_raw("metadata","p4/manifest.json",(const uint8_t*)manifest,strlen(manifest),0);assert(ack_ok&&erases==old_erases&&raw_committed&&!raw_prepared);
 handle_file_data((const uint8_t*)manifest,(uint32_t)strlen(manifest));end("metadata","p4/manifest.json");assert(ack_ok);
 commit("metadata",1,(unsigned)strlen(manifest));assert(ack_ok&&raw_committed&&erases==old_erases&&raw_commits==old_raw_commits);
 /* Every backend failure is a negative ACK, never a success or automatic
  * cleanup. Only the exact known owner may explicitly abort. */
 for(fail_at=1;fail_at<=6;fail_at++){
  unsigned phase=fail_at;raw_writes=raw_finishes=raw_commits=0;
  begin("failure","p4/clip.h264",data,3);
  if(phase==1){assert(!ack_ok);}else{
   assert(ack_ok);handle_file_data(data,3);
   if(phase==2){assert(!ack_ok);}else{
    end("failure","p4/clip.h264");
    if(phase==3){assert(!ack_ok);}else{
     assert(ack_ok);begin("failure","p4/manifest.json",(const uint8_t*)manifest,strlen(manifest));assert(ack_ok);
     handle_file_data((const uint8_t*)manifest,(uint32_t)strlen(manifest));end("failure","p4/manifest.json");assert(ack_ok);
     commit("failure",2,3+(unsigned)strlen(manifest));assert(!ack_ok);
    }
   }
  }
  assert(pet_p4_appearance_active()&&state.asset_transfer_active);
  effects=writes+erases;abort_id("other");assert(!ack_ok&&effects==writes+erases);
  fail_at=0;abort_id("failure");
  if(phase==4||phase==5){assert(!ack_ok&&pet_p4_appearance_mutation_blocked()&&effects==writes+erases);
   /* Explicit simulated reboot clears volatile ownership, never fake-aborts UNKNOWN. */
   memset(&g_appearance_session,0,sizeof(g_appearance_session));native_retire();
   atomic_fetch_add(&g_connection_generation,1);
  }else assert(ack_ok&&!pet_p4_appearance_active());
  fail_at=phase;
 }
 fail_at=0;raw_available=false;begin("idle","p4/test.bin",data,3);assert(ack_ok);
 fail_at=8;pet_p4_native_usb_process(20000);assert(pet_p4_appearance_active()&&state.asset_transfer_active&&g_native_failed&&!g_transfer_last_activity_ms);
 unsigned idle_effects=writes+erases;pet_p4_native_usb_process(40000);assert(writes+erases==idle_effects);
 fail_at=0;abort_id("idle");assert(ack_ok&&!pet_p4_appearance_active());
 begin("idle-success","p4/test.bin",data,3);assert(ack_ok);
 pet_p4_native_usb_process(20000);assert(!pet_p4_appearance_active()&&!state.asset_transfer_active);
 pet_resource_usage_t usage;pet_resources_usage(&broker,&usage);assert(!usage.live_owners&&!usage.revoked_owners);
 return 0;
}
'''

def test_actual_native_handlers_bound_owner_ready_raw_writer_and_faults(tmp_path,appearance_upload_binary):
    # Entry-point coverage, not direct execution of unlocked IO leaf helpers.
    for case in ['native-begin', 'native-begin-erase', 'native-data', 'native-data-fail', 'native-disconnect', 'native-fs-ok', 'native-fs-close-fail', 'native-fs-rename-fail', 'native-stale-frame', 'native-invalid-existing', 'native-abort', 'native-idle']:
        run_upload_case(appearance_upload_binary,tmp_path,case)

def test_native_mount_callbacks_only_publish_bounded_work_not_block_usb_task(tmp_path):
    source=(ROOT/'main/pet_p4_usb_native.c').read_text()
    funcs='\n'.join(function(source,n)for n in ('observation_changed','native_mount_changed','tud_mount_cb','tud_umount_cb','native_mount_poll'))
    run_c(tmp_path,r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
typedef struct{uint8_t kind;uint32_t seq,payload_len;uint8_t*payload;}pet_p4_native_rx_frame_t;
typedef struct{char board_device_id[64];bool mounted;}state_t;
static state_t state={"review-board",false},*g_state=&state;
static int g_state_mutex=1,g_rx_queue=1;static _Atomic int g_mount_pending;static _Atomic uint32_t g_observation_epoch,g_connection_generation;
static bool in_callback,queue_full;static unsigned sends,locks,wakes;
#define portMAX_DELAY 100000
static int xQueueSend(int q,const pet_p4_native_rx_frame_t*f,int wait){assert(in_callback&&q==1&&!wait&&!f->kind&&!f->payload);wakes++;return !queue_full;}
static int xSemaphoreTake(int m,int wait){assert(!in_callback&&m==1&&wait==portMAX_DELAY);locks++;return 1;}
static void xSemaphoreGive(int m){assert(locks==1);locks--;}
static unsigned long long native_now_ms(void){return 100;}
static void pet_p4_state_set_native_usb_mounted(state_t*s,bool mounted,unsigned long long now){assert(locks==1&&!in_callback&&now==100);s->mounted=mounted;}
static void pet_p4_native_usb_send_json_line(const char*s,void*c){}
static void pet_p4_send_hello_for_board(const char*b,void(*send)(const char*,void*),void*ctx){assert(!in_callback&&!locks&&send==pet_p4_native_usb_send_json_line&&!ctx);sends++;}
'''+funcs+r'''
int main(void){
 in_callback=true;tud_mount_cb();in_callback=false;assert(!sends&&!locks&&!state.mounted&&wakes==1);
 assert(g_observation_epoch==1);native_mount_poll();assert(sends==1&&state.mounted&&!locks);native_mount_poll();assert(sends==1);
 queue_full=true;in_callback=true;tud_mount_cb();tud_umount_cb();in_callback=false;
 native_mount_poll();assert(!state.mounted&&sends==1&&!locks);
 in_callback=true;tud_mount_cb();in_callback=false;native_mount_poll();assert(state.mounted&&sends==2);
 return 0;
}''','mount')
    worker=function(source,'native_protocol_worker_task')
    assert worker.index('native_mount_poll()')<worker.index('handle_complete_frame(')
    header=function(source,'start_payload_from_header')
    assert 'g_frame_kind<PET_P4_NATIVE_KIND_JSON' in header


def test_actual_serial_raw_bytes_are_routed_without_stealing_other_json(tmp_path,appearance_upload_binary):
    # Entry-point coverage, not direct execution of unlocked IO leaf helpers.
    for case in ['raw-route', 'raw-8192', 'raw-65536', 'raw-write-fail', 'raw-stale-revision']:
        run_upload_case(appearance_upload_binary,tmp_path,case)

def test_actual_serial_admission_rejects_wrong_route_id_kind_and_partial_raw(tmp_path,appearance_upload_binary):
    # Entry-point coverage, not direct execution of unlocked IO leaf helpers.
    for case in ['serial-admission', 'serial-raw-oom', 'serial-abort', 'serial-abort-fail']:
        run_upload_case(appearance_upload_binary,tmp_path,case)
