"""Actual query/owner closure and resource core; synthetic OS/ready-header IO only."""
from pathlib import Path
import subprocess
from p4_control_integrity_test import _c_function as function

ROOT = Path(__file__).resolve().parents[1]


def test_slot_query_io_and_tx_release_state_lock_with_exact_owner(tmp_path):
    protocol = (ROOT / 'main/pet_p4_protocol.c').read_text()
    main = (ROOT / 'main/pet_p4_main.c').read_text()
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
#include "pet_p4_assets.h"
#include "pet_p4_raw_assets.h"
#include "pet_p4_resources.h"
#include "pet_p4_appearance_session.h"
#define PET_P4_PACK_ID_MAX 65
#define PET_P4_ASSET_SLOT_COUNT 2
#define APPEARANCE_OBSERVATION_SCHEMA "p4-appearance-quiescent-v1"
typedef void(*pet_p4_send_line_fn)(const char*,void*);
typedef struct {pet_p4_asset_catalog_t asset_catalog;char board_device_id[64];bool asset_transfer_active;unsigned asset_revision;}pet_p4_runtime_state_t;
typedef struct {bool transfer_active,receiving,worker_pending;uint32_t epoch;}pet_p4_native_io_snapshot_t;
typedef struct {uint32_t native_epoch;bool asset_transfer,raw_receiving,appearance_owner,file_open,slot_pending,native_transfer,native_receiving,native_pending,worker_pending,storage_pending,firmware_ota,reboot_pending;}appearance_io_observation_t;
static pet_appearance_session_t g_appearance_session;
static pet_resource_broker_t broker;
static struct {bool active;}g_raw_asset_chunk;
static FILE*g_asset_chunk_file;static int g_asset_transfer_slot=-1;
static _Atomic int g_asset_active_slot;
static int held=1,task=7,g_protocol_task=7,g_state_mutex=1;
static bool g_protocol_storage_command_entered=true,change_slot;
static unsigned unlocked,locked,reads,sends,rejected,allocations,fail_at,live;
static bool (*g_asset_query_pending)(void*);
static void *g_asset_query_pending_context;
/* OS/SDK observation boundary only: logical owner/phase/resource logic below
 * is the actual extracted production implementation, not forced success. */
static int xTaskGetCurrentTaskHandle(void){return task;}
static int xSemaphoreGetMutexHolder(int mutex){assert(mutex==1);return held?7:0;}
static void storage_command_unlock(void *ctx){assert(!ctx&&held);held=0;unlocked++;if(change_slot)g_asset_active_slot=1;}
static void storage_command_lock(void *ctx){assert(!ctx&&!held);held=1;locked++;}
static const char*pet_p4_boot_identity(void){return "0000000000000001";}
static bool pending(void*c){(void)c;assert(held);return false;}
static pet_p4_native_io_snapshot_t pet_p4_native_usb_io_snapshot(void){assert(held);return (pet_p4_native_io_snapshot_t){.epoch=9};}
static bool pet_p4_miniapp_storage_busy(void){assert(held);return false;}
static bool pet_p4_ota_transfer_active(void){assert(held);return false;}
static bool pet_p4_diagnostics_reboot_pending(void){assert(held);return false;}
static bool read_ready_slot_pack_id(int slot,char *out,size_t size){
 assert(!held);reads++;assert(size==65);memset(out,'a',64);out[64]=0;return slot==0;
}
static pet_resource_result_t pet_resources_device_owner_open(uint32_t m,pet_resource_owner_t*out){assert(held);return pet_resources_owner_open(&broker,m,out);}
static pet_resource_result_t pet_resources_device_acquire(pet_resource_owner_t o,pet_resource_kind_t k,uint32_t u,pet_resource_lease_t*out){assert(held);return pet_resources_try_acquire(&broker,o,k,u,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){assert(held);return pet_resources_current(&broker,l);}
static void pet_resources_device_revoke(pet_resource_owner_t o){assert(held);(void)pet_resources_owner_revoke(&broker,o);}
static pet_resource_finish_t pet_resources_device_finish(pet_resource_lease_t l){assert(held);return pet_resources_finish(&broker,l);}
static void send_topic_payload(pet_p4_send_line_fn send,void*ctx,const char*topic,cJSON*payload){
 assert(!held&&!strcmp(topic,"asset/ack"));
 cJSON *root=cJSON_CreateObject();assert(root);assert(cJSON_AddStringToObject(root,"topic",topic));
 assert(cJSON_AddItemToObject(root,"payload",payload));char *line=cJSON_PrintUnformatted(root);
 assert(line);send(line,ctx);cJSON_free(line);cJSON_Delete(root);
}
static void tx(const char *line,void *ctx){
 assert(!held&&ctx==(void*)123);
 cJSON *r=cJSON_Parse(line);assert(r);cJSON *p=cJSON_GetObjectItemCaseSensitive(r,"payload");
 assert(!strcmp(cJSON_GetObjectItemCaseSensitive(p,"transferId")->valuestring,"same-transfer"));
 if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p,"ok"))){
  sends++;assert(!change_slot);assert(cJSON_GetObjectItemCaseSensitive(p,"activeSlot")->valueint==0);
  cJSON *slots=cJSON_GetObjectItemCaseSensitive(p,"slots");assert(cJSON_GetArraySize(slots)==2);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(slots,0),"valid")));
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(slots,1),"valid")));
 }else{rejected++;assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(p,"error")));}
 cJSON_Delete(r);
}
static bool (*g_asset_query_io_begin)(void *);
static void (*g_asset_query_io_end)(void *);
static void *g_asset_query_io_context;
'''
    code += '\ntypedef struct {char path[PET_P4_ASSET_PATH_MAX];uint32_t size;} asset_query_video_t;\nstatic asset_query_video_t g_asset_query_videos[PET_P4_ASSET_CATALOG_MAX_FAMILIES];\nstatic unsigned g_asset_query_video_count;\n\nvoid pet_p4_audio_status_cues_defer(bool busy){(void)busy;}\nbool pet_p4_raw_assets_stat_for_slot(int slot,const char*pack,const char*path,size_t expected,uint32_t*size,unsigned long long*checksum){(void)slot;(void)pack;(void)path;(void)expected;(void)size;(void)checksum;assert(!"empty catalog video proof");return false;}\n'
    for name in ('asset_query_io_begin','asset_query_io_end'):
        code += function(main,name)+'\n'
    for name in ('copy_text','asset_query_snapshot_videos','asset_query_video_proof','json_string','slot_is_valid','pet_p4_appearance_active','pet_p4_appearance_mutation_blocked',
                 'send_asset_ack_indexed','send_asset_ack','appearance_io_observe','appearance_io_busy',
                 'pet_p4_protocol_asset_query_io_configure','asset_query_refuse','asset_query_modern',
                 'asset_query_bound','asset_query_not_proof','send_asset_slot_query_ack'):
        code += function(protocol,name)+'\n'
    code += r'''
static void *allocate(size_t n){if(++allocations==fail_at)return NULL;void*p=malloc(n);if(p)live++;return p;}
static void release(void*p){if(p){assert(live);live--;free(p);}}
static void no_resources_leaked(void){pet_resource_usage_t u;pet_resources_usage(&broker,&u);assert(!u.live_owners&&!u.leases[PET_RESOURCE_STORAGE_WORK]);}
int main(void){
 pet_p4_runtime_state_t state={.board_device_id="synthetic-board"};g_asset_query_pending=pending;
 assert(!pet_p4_protocol_asset_query_io_configure(NULL,asset_query_io_end,NULL));
 assert(pet_p4_protocol_asset_query_io_configure(asset_query_io_begin,asset_query_io_end,NULL));
 assert(!pet_p4_protocol_asset_query_io_configure(asset_query_io_begin,asset_query_io_end,NULL));
 cJSON *request=cJSON_Parse("{\"transferId\":\"same-transfer\"}");assert(request);
 send_asset_slot_query_ack(&state,request,tx,(void*)123,"same-transfer");
 assert(held&&reads==2&&sends==1&&unlocked==2&&locked==2&&!state.asset_transfer_active);no_resources_leaked();
 // Actual task/command ownership rejects IO borrowing, without a bogus ACK.
 task=8;send_asset_slot_query_ack(&state,request,tx,(void*)123,"same-transfer");
 task=7;g_protocol_storage_command_entered=false;send_asset_slot_query_ack(&state,request,tx,(void*)123,"same-transfer");
 assert(held&&!rejected&&reads==2&&sends==1&&unlocked==2&&locked==2);no_resources_leaked();
 g_protocol_storage_command_entered=true;
 // A changed slot while unlocked invalidates publication; no stale success.
 change_slot=true;send_asset_slot_query_ack(&state,request,tx,(void*)123,"same-transfer");
 assert(held&&rejected==1&&sends==1&&reads==4);change_slot=false;g_asset_active_slot=0;no_resources_leaked();
 cJSON_Delete(request);
 // Payload/array OOM closes BOTH IO borrows and releases actual STORAGE_WORK.
 for(unsigned fail=1;fail<=2;fail++){
  cJSON_Hooks hooks={allocate,release};cJSON_InitHooks(&hooks);allocations=0;fail_at=fail;
  unsigned old_reads=reads,old_unlocks=unlocked;
  send_asset_slot_query_ack(&state,NULL,tx,(void*)123,"same-transfer");
  assert(held&&reads==old_reads&&unlocked==old_unlocks+2&&locked==unlocked&&!state.asset_transfer_active&&!live);
  no_resources_leaked();cJSON_InitHooks(NULL);
 }
 assert(sends==1&&rejected==2);return 0;
}
'''
    src=tmp_path/'slot_query.c';binary=tmp_path/'slot_query';src.write_text(code)
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
                    '-fsanitize=address,undefined','-I',str(ROOT/'main'),'-I',str(vendor),
                    str(src),str(ROOT/'main/pet_p4_resources.c'),str(vendor/'cJSON.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
