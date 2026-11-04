"""Production save algorithm with hardware/scheduler adapters, ASAN + UBSAN."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def test_private_save_generations_audio_deferral_and_corruption(tmp_path):
    _run_private_save_generations(tmp_path, False)

def test_native_cache_scope_preserves_all_save_generations_audio_fault_and_reclaim_cases(tmp_path):
    _run_private_save_generations(tmp_path, True)

def test_owned_pending_preserves_complete_save_suite_and_rejects_nested_flush(tmp_path):
    _run_private_save_generations(tmp_path, True, owned_pending=True)

def _run_private_save_generations(tmp_path, native_cache, owned_pending=False, wide_program=False):
    stubs={
        'esp_attr.h':'#define EXT_RAM_BSS_ATTR\n',
        'esp_timer.h':'#include <stdint.h>\nstatic int64_t clock_us;\nstatic int64_t esp_timer_get_time(void){return clock_us;}\n',
        'freertos/FreeRTOS.h':'#include <assert.h>\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(m) ((void)(m))\n#define portEXIT_CRITICAL(m) ((void)(m))\n#define portMAX_DELAY 0\n#define pdPASS 1\n#define pdMS_TO_TICKS(x) (x)\n#define configASSERT(x) assert(x)\n',
        'freertos/semphr.h':'#include <assert.h>\ntypedef int SemaphoreHandle_t;\nstatic unsigned store_locked;\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(a,b) (assert(!store_locked),store_locked=1)\n#define xSemaphoreGive(a) (assert(store_locked),store_locked=0)\n',
        'freertos/task.h':'#pragma once\ntypedef void *TaskHandle_t;\nstatic inline TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)1;}\n#define xTaskCreate(a,b,c,d,e,f) ((void)(a),*(f)=(void*)1,1)\n#define vTaskDelay(n) ((void)(n))\n',
        'mbedtls/sha256.h':'#include <string.h>\nstatic int mbedtls_sha256(const unsigned char *p,size_t n,unsigned char *o,int b){(void)b;memset(o,0,32);for(size_t i=0;i<n;i++)o[i%32]^=p[i];return 0;}\n'
    }
    if owned_pending:
        stubs['freertos/task.h']=r'''
#pragma once
#include <stdint.h>
#include <assert.h>
typedef void *TaskHandle_t;
typedef uint8_t StackType_t;
static TaskHandle_t current_task=(void*)1;
static unsigned water_bytes=2048,water_calls;
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task){assert(!task);water_calls++;return water_bytes;}
#define xTaskCreate(a,b,c,d,e,f) (assert((c)==4096&&(e)==1),(void)(a),*(f)=(void*)1,1)
#define vTaskDelay(n) ((void)(n))
'''
    for name,body in stubs.items():
        p=tmp_path/name;p.parent.mkdir(exist_ok=True,parents=True);p.write_text(body)
    source=(ROOT/'main/pet_p4_app_store.c').read_text().replace('#include "pet_p4_audio.h"','''static int audio,playback;
static int pet_p4_audio_stream_playing(void){return audio;}
static int pet_p4_audio_music_active(void){return 0;}
static int pet_p4_audio_conversation_active(void){return 0;}
static int pet_p4_audio_active(void){return 0;}
static int pet_p4_audio_playback_active(void){return playback;}
''')
    # Re-enter the real update/forget paths at the physical read boundary.
    # This deterministically reproduces an input arriving during a cold load.
    source=source.replace('f=fopen(path,"rb");','f=read_open(path);')
    source=source.replace('f=fopen(path,"wb");','f=write_open(path);')
    source=source.replace('fsync(fileno(f))','sync_snapshot(f)')
    source=source.replace('#define SLOTS 32',r'''
static FILE *read_open(const char *path);
static FILE *write_open(const char *path);
static int sync_snapshot(FILE *file);
#define SLOTS 32''')
    if native_cache:
        source=source.replace('#define SLOTS 32',r'''
static uint32_t save_cache_epoch,save_cache_next,save_cache_begins,save_cache_ends;
bool pet_storage_io_register(pet_storage_io_actor_t actor,void *task){assert(actor==PET_STORAGE_IO_SAVE&&task);return true;}
uint32_t pet_storage_io_begin(uint32_t duration){
 assert(duration==5000000&&!store_locked);
 if(save_cache_epoch)return 0;
 save_cache_epoch=++save_cache_next;save_cache_begins++;return save_cache_epoch;
}
bool pet_storage_io_stop(uint32_t epoch){
 assert(!store_locked);
 if(epoch!=save_cache_epoch)return false;
 save_cache_epoch=0;save_cache_ends++;return true;
}
#define SLOTS 32''')
        source='\n'.join('#define '+name+' 1' for name in (
            'PET_P4_STORAGE_IO_TRACE_ENABLE','PET_P4_STORAGE_PAGE_CACHE_ENABLE',
            'PET_P4_STORAGE_PAGE_CACHE_APPLY','PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH',
            'PET_P4_STORAGE_CACHE_SCOPE_ENABLED','PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED'))+'\n'+source
    if owned_pending:
        source='#define PET_APP_SAVE_OWNED_PENDING_ENABLED 1\n'+source
    source='#define PET_FS_ACTIVITY_TEST 1\n#include "pet_p4_fs_activity.c"\n'+source
    harness=tmp_path/'save.c'
    harness.write_text(source+r'''
#include <assert.h>
#include <sys/stat.h>
#include "pet_p4_program_persistence.h"
#include "pet_p4_program_activation.h"
// Reuse the existing test-only admission model, not a second VM or modified
// production broker. Actual storage admission/IO below still uses the broker.
#define main program_host_unit_entry
#include "p4_program_host_test.c"
#undef main
static bool program_current(void *context,uint64_t generation){return *(uint64_t*)context==generation;}
static size_t encode_install_image(uint8_t out[PET_PROGRAM_IMAGE_MAX_BYTES],const pet_program_image_t *im){
 memset(out,0,PET_PROGRAM_IMAGE_MAX_BYTES);memcpy(out,"P4P1",4);out[4]=1;out[6]=24;
 out[8]=(uint8_t)im->count;out[9]=(uint8_t)(im->count>>8);out[10]=(uint8_t)im->fuel;out[11]=(uint8_t)(im->fuel>>8);out[12]=im->cells;
 for(unsigned j=0;j<4;j++)out[16+j]=(uint8_t)(im->schema>>(8*j));
 for(unsigned i=0;i<im->count;i++){
  uint8_t *insn=out+24+i*8;insn[0]=im->code[i].op;insn[1]=im->code[i].a;insn[2]=im->code[i].b;insn[3]=im->code[i].c;
  for(unsigned j=0;j<4;j++)insn[4+j]=(uint8_t)((uint32_t)im->code[i].imm>>(8*j));
 }
 size_t length=24+im->count*8U;uint32_t crc=UINT32_MAX;
 for(size_t i=0;i<length;i++){if(i>=20&&i<24)continue;crc^=out[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^((0U-(crc&1U))&UINT32_C(0xedb88320));}
 crc=~crc;for(unsigned j=0;j<4;j++)out[20+j]=(uint8_t)(crc>>(8*j));return length;
}
static int interleave,reads,read_failure,write_interleave,sync_interleave,writes;
static pet_resource_realtime_t realtime;
static FILE *write_open(const char *path){
 #if PET_APP_SAVE_OWNED_PENDING_ENABLED
 // A nested flush would overwrite the shared pending copy before resource
 // admission without the native original-address/task scope. Exercise every
 // real physical write, including fault/revocation paths in the old suite.
 if(save_pending_scope){
  assert(save_pending_scope->self==save_pending_scope);
  save_t before[SLOTS+SCOPED_SLOTS];memcpy(before,saves,sizeof(before));
  uint8_t cursor=flush_cursor;unsigned before_writes=(unsigned)writes;
  flush_one();assert(flush_cursor==cursor&&(unsigned)writes==before_writes);
  assert(!memcmp(before,saves,sizeof(before))&&save_pending_scope);
 }
 #endif
 assert(!store_locked);writes++;FILE *f=fopen(path,"wb");
 if(write_interleave){write_interleave=0;
   assert(pet_app_store_update("live","{\"count\":99}"));
   assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
 }
 return f;
}
static int sync_snapshot(FILE *file){
 int rc=fsync(fileno(file));
 if(sync_interleave){sync_interleave=0;assert(!rc);
   assert(pet_app_store_update("live","{\"count\":101}"));
   assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
 }
 return rc;
}
static FILE *read_open(const char *path){
 assert(!store_locked);reads++;
 if(read_failure){read_failure--;errno=EIO;return NULL;}
 if(interleave){int mode=interleave;interleave=0;
   if(mode==1)assert(pet_app_store_update("garden","{\"count\":88}"));
   else if(mode==2)pet_app_store_forget("garden");
   else if(mode==4){pet_app_store_forget("garden");assert(pet_app_store_update("garden","{\"count\":89}"));}
   else assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
   assert(saves[0].loading);clock_us+=31000000;flush_one();if(mode<3)assert(saves[0].dirty);
 }
 return fopen(path,"rb");
}
int main(void){
 pet_fs_activity_enable_observation();
 assert(pet_app_store_init());char out[PET_APP_SAVE_BYTES],path[64];
 #if PET_APP_SAVE_OWNED_PENDING_ENABLED
 // Copies and other tasks cannot release the one physical snapshot owner.
 save_pending_scope_t owner={0},nested={0};assert(save_pending_begin(&owner));
 save_pending_scope_t copy=owner;save_pending_finish(&copy);
 assert(save_pending_scope==&owner&&!save_pending_begin(&nested));
 current_task=(void*)2;save_pending_finish(&owner);
 assert(save_pending_scope==&owner&&!save_pending_begin(&nested));
 current_task=(void*)1;save_pending_finish(&owner);
 assert(!save_pending_scope&&!owner.self&&!owner.task);
 assert(save_pending_begin(&nested));save_pending_finish(&nested);
 pet_app_save_stack_t sample;
 assert(!pet_app_store_worker_stack(NULL));
 assert(pet_app_store_worker_stack(&sample)&&!sample.available&&!sample.observations&&!water_calls);
 save_stack_sample();assert(water_calls==1);
 assert(pet_app_store_worker_stack(&sample)&&sample.available&&sample.minimum_free_bytes==2048);
 water_bytes=1536;save_stack_sample();water_bytes=3000;save_stack_sample();
 assert(pet_app_store_worker_stack(&sample)&&sample.observations==3&&sample.minimum_free_bytes==1536);
 unsigned scans=water_calls;assert(pet_app_store_worker_stack(&sample)&&water_calls==scans);
 water_bytes=4097;save_stack_sample();
 assert(pet_app_store_worker_stack(&sample)&&!sample.available&&sample.invalid_samples==1);
 water_bytes=0;save_stack_sample();
 assert(pet_app_store_worker_stack(&sample)&&!sample.available&&sample.minimum_free_bytes==0);
 save_stack.observations=UINT32_MAX;save_stack.invalid_samples=UINT32_MAX;
 water_bytes=5000;save_stack_sample();water_bytes=100;save_stack_sample();
 assert(save_stack.observations==UINT32_MAX&&save_stack.invalid_samples==UINT32_MAX);
 memset(&save_stack,0,sizeof(save_stack));
 #endif
 assert(!pet_app_store_read("garden",out));
 assert(pet_app_store_update("garden","{\"count\":1}"));
 assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":1}"));
 clock_us=31000000;audio=1;flush_one();path_for("garden",1,path);assert(access(path,F_OK));
 // Ordinary file/shared-PCM playback is independent of stream/capture flags.
 audio=0;playback=1;uint64_t before_audio=0,after_audio=0;
 assert(pet_fs_activity_idle_epoch(&before_audio));save_t deferred=saves[0];
 flush_one();flush_one();assert(access(path,F_OK)&&!memcmp(&deferred,&saves[0],sizeof(deferred)));
 assert(pet_fs_activity_idle_epoch(&after_audio)&&after_audio==before_audio);
 playback=0;flush_one();assert(!access(path,F_OK)&&!saves[0].dirty);
 assert(pet_app_store_update("garden","{\"count\":99}"));
 assert(pet_app_store_update("garden","{\"count\":2}"));clock_us=62000000;flush_one();
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":2}"));

 // A cold explicit forget followed by immediate reinstall/update cannot clear
 // the independent deletion epoch or revive either old high-seq generation.
 assert(!pet_app_store_read("epoch-case",out));assert(pet_app_store_update("epoch-case","{\"old\":99}"));
 int ei=find_slot("epoch-case");saves[ei].seq=100;clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_app_store_update("epoch-case","{\"old\":100}"));clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 memset(saves,0,sizeof(saves));pet_app_store_forget("epoch-case");ei=find_slot("epoch-case");
 assert(saves[ei].epoch_pending&&pet_app_store_load("epoch-case",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_update("epoch-case","{\"new\":1}"));assert(saves[ei].epoch_pending&&!saves[ei].deleted);
 clock_us+=31000000;for(int k=0;k<40;k++)flush_one();assert(!saves[ei].dirty&&!saves[ei].epoch_pending&&saves[ei].epoch==1);
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("epoch-case",out)&&!strcmp(out,"{\"new\":1}"));
 // An epoch-only committed deletion is authoritative across reboot. Legacy
 // A/B bytes can still physically exist: they cannot bypass this marker.
 pet_app_store_forget("epoch-case");ei=find_slot("epoch-case");
 char stale_path[64];epoch_path_for("epoch-case",0,saves[ei].file_slot,saves[ei].epoch,stale_path);
 char stale_bytes[PET_APP_SAVE_BYTES+sizeof(header_t)];FILE *old_file=fopen(stale_path,"rb");assert(old_file);size_t stale_size=fread(stale_bytes,1,sizeof(stale_bytes),old_file);assert(stale_size&&feof(old_file));fclose(old_file);
 save_t marker=saves[ei];save_io_t epoch_io={0};assert(save_io_begin(&epoch_io,NULL));assert(commit_forget_epoch(&marker,epoch_io.lease));save_io_end(&epoch_io);
 // Simulate old bytes left physically present after interrupted reclamation.
 old_file=fopen(stale_path,"wb");assert(old_file);assert(fwrite(stale_bytes,1,stale_size,old_file)==stale_size);assert(!fclose(old_file));
 memset(saves,0,sizeof(saves));assert(pet_app_store_load("epoch-case",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_update("epoch-case","{\"new\":2}"));clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("epoch-case",out)&&!strcmp(out,"{\"new\":2}"));
 // Actual explicit-delete storage-lease API makes intent durable before ACK,
 // without waiting for the ordinary deferred snapshot worker.
 save_io_t delete_io={0};assert(save_io_begin(&delete_io,NULL));
 assert(pet_app_store_forget_with_lease("epoch-case",delete_io.lease));save_io_end(&delete_io);
 memset(saves,0,sizeof(saves));assert(pet_app_store_load("epoch-case",out)==PET_APP_STORE_EMPTY);
 assert(!pet_app_store_forget_with_lease("epoch-case",(pet_resource_lease_t){0}));
 // Return the original test identity to the first cache slot for its explicit
 // concurrent-load assertions. All existing generations remain unchanged.
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("garden",out));
 // Torn newer generation falls back to the untouched prior generation.
 path_for("garden",0,path);FILE *f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":1}"));
 // A transient failure is not a new user. Even a valid peer cannot prove
 // whether the unreadable generation was newer, so no default save may flush.
 memset(saves,0,sizeof(saves));read_failure=1;
 assert(pet_app_store_load("garden",out)==PET_APP_STORE_UNAVAILABLE&&saves[0].load_failed);
 assert(!pet_app_store_update("garden","{\"count\":0}"));clock_us+=31000000;flush_one();
 assert(!pet_app_store_read_cached("garden",out));
 assert(pet_app_store_load("garden",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"count\":1}"));
 assert(!saves[0].load_failed);
 // Cache-only publication never starts a cold read, and a read in progress
 // cannot overwrite a newer value or a concurrent deletion.
 memset(saves,0,sizeof(saves));int before=reads;
 assert(!pet_app_store_read_cached("garden",out)&&reads==before);
 interleave=1;assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":88}"));
 assert(saves[0].seq>1&&saves[0].dirty&&!saves[0].loading);
 before=reads;assert(pet_app_store_read_cached("garden",out)&&reads==before);
 memset(saves,0,sizeof(saves));interleave=4;
 assert(pet_app_store_read("garden",out)&&!strcmp(out,"{\"count\":89}"));assert(saves[0].epoch_pending&&!saves[0].loading);
 memset(saves,0,sizeof(saves));interleave=2;
 assert(!pet_app_store_read("garden",out)&&saves[0].deleted&&saves[0].dirty&&!saves[0].loading);
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("garden",out));
 // No valid generation is not equivalent to two absent files. Corrupt saves
 // remain protected until an explicit forget; that authority may delete them.
 for(unsigned slot=0;slot<2;slot++){path_for("broken",slot,path);FILE *bad=fopen(path,"wb");assert(bad);fputs("torn",bad);fclose(bad);}
 assert(pet_app_store_load("broken",out)==PET_APP_STORE_UNAVAILABLE);
 assert(!pet_app_store_update("broken","{}"));
 pet_app_store_forget("broken");clock_us+=31000000;flush_one();
 path_for("broken",0,path);assert(access(path,F_OK));path_for("broken",1,path);assert(access(path,F_OK));
 assert(!pet_app_store_read("another",out));assert(!pet_app_store_update("../garden","x"));
 pet_app_store_forget("garden");clock_us=93000000;playback=1;path_for("garden",1,path);
 assert(!access(path,F_OK));assert(pet_fs_activity_idle_epoch(&before_audio));deferred=saves[0];
 flush_one();assert(!access(path,F_OK)&&!memcmp(&deferred,&saves[0],sizeof(deferred)));
 assert(pet_fs_activity_idle_epoch(&after_audio)&&after_audio==before_audio);
 playback=0;flush_one();assert(access(path,F_OK));memset(saves,0,sizeof(saves));assert(!pet_app_store_read("garden",out));
 // Match the full catalog capacity; visiting the seventeenth saved app must not lose progress.
 memset(saves,0,sizeof(saves));
 for(int i=0;i<32;i++){char id[48];snprintf(id,sizeof(id),"app-%02d",i);assert(pet_app_store_update(id,"{\"count\":7}"));}
 for(int i=0;i<32;i++){char id[48];snprintf(id,sizeof(id),"app-%02d",i);assert(pet_app_store_read(id,out)&&!strcmp(out,"{\"count\":7}"));}
 assert(!pet_app_store_update("overflow","{}"));
 // The optimization guard never changes save behavior and fails closed on
 // overlapping writers or exhausted epochs; all write failures close it.
 uint64_t epoch=0,previous=0;assert(pet_fs_activity_idle_epoch(&previous));
 pet_fs_activity_t first=pet_fs_activity_begin(),second=pet_fs_activity_begin();
 assert(!pet_fs_activity_idle_epoch(&epoch));pet_fs_activity_end(&first);
 assert(!pet_fs_activity_idle_epoch(&epoch));pet_fs_activity_end(&second);
 assert(pet_fs_activity_idle_epoch(&epoch)&&epoch==previous+4);
 save_t failed={.id="failed-write",.json="{}"};path_for(failed.id,0,path);assert(!mkdir(path,0700));
 save_io_t failed_io={0};assert(save_io_begin(&failed_io,NULL));
 assert(!write_slot(&failed,failed_io.lease));save_io_end(&failed_io);assert(pet_fs_activity_idle_epoch(&previous)&&previous==epoch+2);
 // Cold admission busy is not an empty save or a poisoned cache. A trusted
 // outer package load may borrow exactly its own active storage lease.
 memset(saves,0,sizeof(saves));save_io_t outer={0};assert(save_io_begin(&outer,NULL));before=reads;
 assert(pet_app_store_load("garden",out)==PET_APP_STORE_BUSY&&reads==before&&!saves[0].id[0]);
 assert(pet_app_store_load_with_lease("garden",out,outer.lease)==PET_APP_STORE_EMPTY);
 assert(pet_resources_device_current(outer.lease));save_io_end(&outer);
 // Revocation during a cold read stops the second generation and never
 // publishes invented defaults. A fresh authoritative load may retry later.
 memset(saves,0,sizeof(saves));before=reads;interleave=3;
 assert(pet_app_store_load("garden",out)==PET_APP_STORE_UNAVAILABLE&&reads==before+1&&saves[0].load_failed);
 assert(!pet_app_store_update("garden","{}"));pet_resources_device_realtime_end(&realtime);
 assert(pet_app_store_load("garden",out)==PET_APP_STORE_EMPTY&&!saves[0].load_failed);
 // Revocation after opening but before the write keeps the prior generation
 // and newer RAM input dirty; no later writes proceed until re-admission.
 assert(pet_app_store_load("live",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_update("live","{\"count\":1}"));clock_us+=31000000;write_interleave=1;flush_one();
 int live_slot=find_slot("live");assert(saves[live_slot].dirty&&!strcmp(saves[live_slot].json,"{\"count\":99}"));
 assert(saves[live_slot].file_slot==0);
 int before_write=writes;flush_one();assert(writes==before_write);
 pet_resources_device_realtime_end(&realtime);clock_us+=31000000;flush_one();assert(!saves[live_slot].dirty);
 // Revocation after fsync is too late to cancel the durable result. Reconcile
 // its slot and retain concurrent input for the next generation, never replay.
 assert(pet_app_store_update("live","{\"count\":100}"));clock_us+=31000000;sync_interleave=1;flush_one();
 assert(saves[live_slot].dirty&&saves[live_slot].file_slot==0&&!strcmp(saves[live_slot].json,"{\"count\":101}"));
 before_write=writes;flush_one();assert(writes==before_write);
 pet_resources_device_realtime_end(&realtime);clock_us+=31000000;flush_one();assert(!saves[live_slot].dirty);
 memset(saves,0,sizeof(saves));assert(pet_app_store_read("live",out)&&!strcmp(out,"{\"count\":101}"));
 pet_resource_usage_t usage;pet_resources_device_usage(&usage);
 assert(!usage.live_owners&&!usage.revoked_owners&&!usage.leases[PET_RESOURCE_STORAGE_WORK]&&!usage.realtime_reservations);
 // Native namespaces cannot collide with a component's legacy save or each
 // other, even for identical full IDs. Accepted RAM updates are not durable.
 assert(pet_app_store_load_scoped(0,"live",out)==PET_APP_STORE_INVALID);
 int cached_reads=reads;
 assert(pet_app_store_read_scoped_cached(1,"missing",out)==PET_APP_STORE_UNAVAILABLE&&reads==cached_reads);
 assert(pet_app_store_read_scoped_cached(0,"live",out)==PET_APP_STORE_INVALID&&reads==cached_reads);
 assert(!pet_app_store_update_scoped(1,"live","{}"));
 assert(pet_app_store_load_scoped(1,"live",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_read_scoped_cached(1,"live",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_load_scoped(2,"live",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_update_scoped(1,"live","{\"v\":7}"));
 assert(pet_app_store_update_scoped(2,"live","{\"v\":8}"));
 assert(pet_app_store_checkpoint_scoped(1,"live","{\"v\":7}")==PET_APP_CHECKPOINT_PENDING);
 assert(pet_app_store_checkpoint_scoped(1,"live","{}")==PET_APP_CHECKPOINT_SUPERSEDED);
 assert(pet_app_store_read("live",out)&&!strcmp(out,"{\"count\":101}"));
 clock_us+=31000000;audio=1;before_write=writes;flush_one();assert(writes==before_write);
 assert(pet_app_store_checkpoint_scoped(1,"live","{\"v\":7}")==PET_APP_CHECKPOINT_PENDING);
 audio=0;for(int k=0;k<40;k++)flush_one();
 assert(pet_app_store_checkpoint_scoped(1,"live","{\"v\":7}")==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_app_store_checkpoint_scoped(2,"live","{\"v\":8}")==PET_APP_CHECKPOINT_DURABLE);
 char domain_path[64];scoped_path_for("live",1,1,path);scoped_path_for("live",2,1,domain_path);
 assert(strcmp(path,domain_path)&&!access(path,F_OK)&&!access(domain_path,F_OK));
 memset(saves,0,sizeof(saves));
 assert(pet_app_store_checkpoint_scoped(1,"live","{\"v\":7}")==PET_APP_CHECKPOINT_BLOCKED);
 assert(pet_app_store_load_scoped(1,"live",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"v\":7}"));
 assert(pet_app_store_checkpoint_scoped(1,"live","{\"v\":7}")==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_app_store_load_scoped(2,"live",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"v\":8}"));
 assert(pet_app_store_read("live",out)&&!strcmp(out,"{\"count\":101}"));
 assert(pet_app_store_update_scoped(1,"live","{\"v\":9}"));clock_us+=31000000;flush_one();
 scoped_path_for("live",1,0,path);f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
 memset(saves,0,sizeof(saves));
 assert(pet_app_store_load_scoped(1,"live",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"v\":7}"));
 assert(pet_app_store_load_scoped(2,"live",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"v\":8}"));
 memset(saves,0,sizeof(saves));read_failure=1;
 assert(pet_app_store_load_scoped(1,"live",out)==PET_APP_STORE_UNAVAILABLE);
 assert(pet_app_store_checkpoint_scoped(1,"live","{\"v\":7}")==PET_APP_CHECKPOINT_BLOCKED);
 assert(!pet_app_store_update_scoped(1,"live","{}"));
 assert(pet_app_store_load_scoped(1,"live",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"v\":7}"));
 // Actual VM -> portable codec -> actual A/B store -> cache restart -> seed.
 // Successful VM authority is synthetic and confined to this host test.
 static pet_program_runtime_t program_runtime;
 pet_program_image_t program_image={.schema=42,.count=1,.fuel=1,.cells=1,.valid=true};
 pet_program_snapshot_t seed,committed,old_seed;memset(&seed,0xa5,sizeof(seed));old_seed=seed;
 save_io_t program_io={0};assert(save_io_begin(&program_io,NULL));
 assert(pet_program_persistence_load("prepared-case",&program_image,&seed)==PET_PROGRAM_SAVE_BUSY);
 assert(!memcmp(&seed,&old_seed,sizeof(seed)));
 assert(pet_program_persistence_load_with_lease("prepared-case",&program_image,&seed,program_io.lease)==PET_PROGRAM_SAVE_EMPTY);
 assert(pet_resources_device_current(program_io.lease));save_io_end(&program_io);
 assert(pet_program_persistence_load("program-case",&program_image,&seed)==PET_PROGRAM_SAVE_EMPTY);
 assert(!memcmp(&seed,&old_seed,sizeof(seed)));
 pet_program_runtime_init(&program_runtime);uint64_t program_generation=1;
 assert(pet_program_runtime_start(&program_runtime,&program_image,1,program_current,&program_generation,1,NULL)==PET_RUNTIME_OK);
 pet_program_event_t event={0};assert(pet_program_runtime_enqueue(&program_runtime,1,1,&event)==PET_RUNTIME_OK);
 for(int pump=0;pump<3;pump++)pet_program_runtime_pump(&program_runtime,1);
 assert(pet_program_runtime_snapshot(&program_runtime,1,&committed)==PET_RUNTIME_OK&&committed.revision==1);
 assert(pet_program_persistence_enqueue("program-case",&committed));
 assert(pet_program_persistence_checkpoint("program-case",&committed)==PET_APP_CHECKPOINT_PENDING);
 pet_program_snapshot_t cached_seed={.schema=1,.cells=1};cached_reads=reads;
 assert(pet_program_persistence_load_cached("program-case",&program_image,&cached_seed)==PET_PROGRAM_SAVE_VALUE&&reads==cached_reads);
 assert(cached_seed.revision==committed.revision&&!memcmp(cached_seed.values,committed.values,sizeof(cached_seed.values)));
 clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_program_persistence_checkpoint("program-case",&committed)==PET_APP_CHECKPOINT_DURABLE);
 memset(saves,0,sizeof(saves));
 assert(pet_program_persistence_load("program-case",&program_image,&seed)==PET_PROGRAM_SAVE_VALUE);
 assert(!memcmp(&seed,&committed,sizeof(seed)));
 assert(pet_program_persistence_checkpoint("program-case",&committed)==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_program_runtime_cancel(&program_runtime,1)==PET_RUNTIME_OK);
 pet_program_report_t report;assert(pet_program_runtime_take_report(&program_runtime,1,&report)==PET_RUNTIME_OK);
 program_generation=2;
 assert(pet_program_runtime_start(&program_runtime,&program_image,2,program_current,&program_generation,1,&seed)==PET_RUNTIME_OK);
 assert(pet_program_runtime_pump(&program_runtime,2)==PET_RUNTIME_IDLE&&program_runtime.state.revision==1);
 old_seed=seed;program_image.schema=43;
 assert(pet_program_persistence_load("program-case",&program_image,&seed)==PET_PROGRAM_SAVE_INCOMPATIBLE);
 assert(!memcmp(&seed,&old_seed,sizeof(seed)));program_image.schema=42;
 assert(pet_app_store_update_scoped(PET_PROGRAM_SAVE_DOMAIN,"program-case","{\"bad\":true}"));
 assert(pet_program_persistence_load("program-case",&program_image,&seed)==PET_PROGRAM_SAVE_CORRUPT);
 assert(!memcmp(&seed,&old_seed,sizeof(seed)));
 assert(pet_program_persistence_checkpoint("program-case",&committed)==PET_APP_CHECKPOINT_SUPERSEDED);
 // Real package preparation: wire decoder + actual scoped store + exact
 // storage ticket + host lifecycle. Program execution admission is synthetic,
 // as in existing host tests; production PROGRAM quota remains zero.
 setup();uint8_t install_image[PET_PROGRAM_IMAGE_MAX_BYTES];size_t install_length=encode_install_image(install_image,&image);
 static pet_program_prepared_t prepared;
 unsigned char identity_hash[32]={9},wrong_identity[32]={8};uint64_t activated=777;
 assert(save_io_begin(&program_io,NULL));
 assert(pet_program_activation_prepare(&prepared,"activation-case",identity_hash,41,install_image,install_length,program_io.lease)==PET_PROGRAM_PREPARED);
 assert(!prepared.has_seed&&pet_resources_device_current(program_io.lease));
 assert(pet_program_activation_publish(&prepared,&host,"activation-case",wrong_identity,41,1,&activated)==PET_RUNTIME_STALE);
 assert(pet_program_activation_publish(&prepared,&host,"activation-case",identity_hash,40,1,&activated)==PET_RUNTIME_STALE);
 assert(activated==777&&!mock.opens&&!host.runtime.active);
 mock.enabled=false;pet_program_image_t unchanged_staging=host.staged;
 assert(pet_program_activation_publish(&prepared,&host,"activation-case",identity_hash,41,1,&activated)==PET_RUNTIME_DENIED);
 assert(activated==777&&!memcmp(&unchanged_staging,&host.staged,sizeof(host.staged))&&prepared.valid);
 mock.enabled=true;
 assert(pet_program_activation_publish(&prepared,&host,"activation-case",identity_hash,41,1,&activated)==PET_RUNTIME_OK);
 assert(!prepared.valid&&pet_resources_device_current(program_io.lease));save_io_end(&program_io);
 assert(pet_program_activation_publish(&prepared,&host,"activation-case",identity_hash,41,1,&activated)==PET_RUNTIME_INVALID);
 enqueue(activated,1,6);complete(activated);
 assert(pet_program_host_take_report(&host,activated,&report)==PET_RUNTIME_OK);
 assert(pet_program_host_snapshot(&host,activated,&committed)==PET_RUNTIME_OK&&committed.values[0]==6);
 assert(pet_program_persistence_enqueue("activation-case",&committed));
 clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_program_persistence_checkpoint("activation-case",&committed)==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_program_host_cancel(&host,activated)==PET_RUNTIME_OK);
 memset(saves,0,sizeof(saves));assert(pet_program_activation_discard(&prepared));assert(save_io_begin(&program_io,NULL));
 assert(pet_program_activation_prepare(&prepared,"activation-case",identity_hash,42,install_image,install_length,program_io.lease)==PET_PROGRAM_PREPARED&&prepared.has_seed);
 assert(prepared.seed.values[0]==6&&prepared.seed.revision==1);
 pet_program_output_contract_t projection={0};projection.maximum[0]=5;
 pet_program_snapshot_t kept_seed=prepared.seed;pet_program_runtime_t kept_runtime=host.runtime;
 uint64_t kept_generation=activated;
 assert(pet_program_activation_publish_with_contract(&prepared,&host,"activation-case",identity_hash,42,1,&projection,&activated)==PET_RUNTIME_BAD_STATE);
 assert(activated==kept_generation&&prepared.valid&&!memcmp(&kept_seed,&prepared.seed,sizeof(kept_seed))&&!memcmp(&kept_runtime,&host.runtime,sizeof(kept_runtime)));
 assert(pet_resources_device_current(program_io.lease));projection.maximum[0]=10;
 assert(pet_program_activation_publish_with_contract(&prepared,&host,"activation-case",identity_hash,42,1,&projection,&activated)==PET_RUNTIME_OK);
 save_io_end(&program_io);assert(pet_program_host_pump(&host,activated)==PET_RUNTIME_IDLE);
 enqueue(activated,1,2);complete(activated);assert(host.runtime.state.cells[0]==8);
 assert(pet_program_host_take_report(&host,activated,&report)==PET_RUNTIME_OK);
 assert(pet_program_host_cancel(&host,activated)==PET_RUNTIME_OK);
 // The parent captures and resolves its latest committed checkpoint BEFORE
 // preparing the next installed version; never replace live value 8 with the
 // older on-disk seed 6. Publication itself performs no filesystem work.
 assert(pet_program_host_snapshot(&host,activated,&committed)==PET_RUNTIME_OK&&committed.values[0]==8);
 assert(pet_program_persistence_enqueue("activation-case",&committed));
 clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_program_persistence_checkpoint("activation-case",&committed)==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_program_activation_discard(&prepared));assert(save_io_begin(&program_io,NULL));
 assert(pet_program_activation_prepare(&prepared,"activation-case",identity_hash,43,install_image,install_length,program_io.lease)==PET_PROGRAM_PREPARED&&prepared.has_seed);
 assert(prepared.seed.values[0]==8&&prepared.seed.outputs[0]==8);
 pet_program_ui_projection_t ui_projection={.schema=42,.variables=3,.minimum={0,0,0},.maximum={10,7,10}};
 memset(ui_projection.target,PET_PROGRAM_UI_UNBOUND,sizeof(ui_projection.target));ui_projection.target[0]=1;
 kept_seed=prepared.seed;kept_runtime=host.runtime;kept_generation=activated;
 pet_program_image_t kept_staging=host.staged;
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",wrong_identity,43,1,&ui_projection,&activated)==PET_RUNTIME_STALE);
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,42,1,&ui_projection,&activated)==PET_RUNTIME_STALE);
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,NULL,&activated)==PET_RUNTIME_INVALID);
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,(pet_program_ui_projection_t*)&prepared.seed,&activated)==PET_RUNTIME_INVALID);
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,&host.projection,&activated)==PET_RUNTIME_INVALID);
 union {uint64_t generation;pet_program_ui_projection_t projection;} ui_alias={.projection=ui_projection};
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,&ui_alias.projection,&ui_alias.generation)==PET_RUNTIME_INVALID);
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,&ui_projection,&activated)==PET_RUNTIME_BAD_STATE);
 assert(activated==kept_generation&&prepared.valid&&!memcmp(&kept_seed,&prepared.seed,sizeof(kept_seed))&&!memcmp(&kept_runtime,&host.runtime,sizeof(kept_runtime))&&!memcmp(&kept_staging,&host.staged,sizeof(kept_staging)));
 ui_projection.maximum[1]=10;mock.enabled=false;
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,&ui_projection,&activated)==PET_RUNTIME_DENIED&&prepared.valid);
 mock.enabled=true;
 assert(pet_program_activation_publish_with_projection(&prepared,&host,"activation-case",identity_hash,43,1,&ui_projection,&activated)==PET_RUNTIME_OK&&!prepared.valid);
 ui_projection.target[0]=0; /* installed recipe is copied, not a live pointer */
 assert(pet_resources_device_current(program_io.lease));save_io_end(&program_io);
 int32_t ui_base[8]={3,1,5,77};pet_program_ui_snapshot_t ui={0};
 assert(pet_program_host_project_current(&host,activated,ui_base,&ui)==PET_RUNTIME_OK&&ui.values[0]==3&&ui.values[1]==8&&ui.values[2]==5&&ui.values[3]==77);
 enqueue(activated,1,2);complete(activated);assert(pet_program_host_take_report(&host,activated,&report)==PET_RUNTIME_OK);
 assert(pet_program_host_project_current(&host,activated,ui_base,&ui)==PET_RUNTIME_OK&&ui.values[1]==10);
 assert(pet_program_host_snapshot(&host,activated,&committed)==PET_RUNTIME_OK&&committed.values[0]==10);
 assert(pet_program_persistence_enqueue("activation-case",&committed));clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_program_persistence_checkpoint("activation-case",&committed)==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_program_host_cancel(&host,activated)==PET_RUNTIME_OK);pet_program_ui_snapshot_t kept_ui=ui;
 assert(pet_program_host_project_current(&host,activated,ui_base,&ui)==PET_RUNTIME_STOPPED&&!memcmp(&kept_ui,&ui,sizeof(ui)));
 assert(pet_program_activation_discard(&prepared));assert(save_io_begin(&program_io,NULL));
 install_image[0]^=1;
 assert(pet_program_activation_prepare(&prepared,"activation-case",identity_hash,44,install_image,install_length,program_io.lease)==PET_PROGRAM_PREPARE_BAD_IMAGE&&!prepared.valid);
 install_image[0]^=1;
 assert(pet_program_activation_prepare(&prepared,"activation-case",identity_hash,44,install_image,install_length,program_io.lease)==PET_PROGRAM_PREPARED);
 pet_resources_device_revoke(program_io.owner);
 assert(pet_program_activation_publish(&prepared,&host,"activation-case",identity_hash,44,1,&activated)==PET_RUNTIME_DENIED);
 assert(!host.runtime.active&&host.runtime.state.cells[0]==10);save_io_end(&program_io);
 // Native capacity never consumes legacy 32 entries. All-eight pinned or
 // dirty/loading/failed exhaustion refuses, never silently losing live data.
 memset(saves,0,sizeof(saves));
 pet_app_store_pin_t pins[8]={0};
 for(unsigned domain=1;domain<=8;domain++){
   assert(pet_app_store_load_scoped(domain,"native",out)==PET_APP_STORE_EMPTY);
   assert(pet_app_store_pin_scoped(domain,"native",&pins[domain-1],&pins[domain-1]));
 }
 assert(pet_app_store_load_scoped(9,"native",out)==PET_APP_STORE_UNAVAILABLE);
 for(int k=0;k<32;k++){char id[48];snprintf(id,sizeof(id),"kept-%02d",k);assert(pet_app_store_update(id,"{}"));}
 assert(!pet_app_store_update("kept-overflow","{}"));
 assert(pet_app_store_checkpoint_scoped(1,"native","{}")==PET_APP_CHECKPOINT_ABSENT);
 #if PET_APP_SCOPED_RECLAIM_REVIEWED
 // Read-only/update failures never reserve a slot or evict a clean victim.
 assert(pet_app_store_checkpoint_scoped(99,"missing","{}")==PET_APP_CHECKPOINT_BLOCKED);
 assert(!pet_app_store_update_scoped(99,"missing","{}"));
 pet_app_store_pin_t old=pins[0];assert(pet_app_store_unpin(&pins[0]));
 assert(!pet_app_store_unpin(&pins[0]));
 assert(pet_resources_device_realtime_begin(&realtime)==PET_RESOURCE_OK);
 assert(pet_app_store_load_scoped(9,"native",out)==PET_APP_STORE_BUSY);
 assert(pet_app_store_checkpoint_scoped(1,"native","{}")==PET_APP_CHECKPOINT_ABSENT);
 pet_resources_device_realtime_end(&realtime);
 assert(pet_app_store_load_scoped(9,"native",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_checkpoint_scoped(1,"native","{}")==PET_APP_CHECKPOINT_BLOCKED);
 assert(pet_app_store_pin_scoped(9,"native",&pins[0],&pins[0]));
 assert(!pet_app_store_unpin(&old)); // Cannot unpin the new slot occupant.
 assert(!pet_app_store_pin_scoped(9,"native",&old,&old));
 assert(pet_app_store_update_scoped(9,"native","{\"keep\":81}"));
 assert(pet_app_store_unpin(&pins[0]));
 assert(pet_app_store_load_scoped(10,"native",out)==PET_APP_STORE_UNAVAILABLE); // dirty
 clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_app_store_checkpoint_scoped(9,"native","{\"keep\":81}")==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_app_store_load_scoped(10,"native",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_load_scoped(9,"native",out)==PET_APP_STORE_VALUE&&!strcmp(out,"{\"keep\":81}"));
 // Unreadable/cache-loading records cannot be reclaimed as default values.
 int guarded=lookup_slot(9,"native");assert(guarded>=SLOTS);
 saves[guarded].load_failed=true;
 assert(pet_app_store_load_scoped(11,"native",out)==PET_APP_STORE_UNAVAILABLE);
 saves[guarded].load_failed=false;saves[guarded].loading=true;
 assert(pet_app_store_load_scoped(11,"native",out)==PET_APP_STORE_UNAVAILABLE);
 saves[guarded].loading=false;
 assert(pet_app_store_load_scoped(11,"native",out)==PET_APP_STORE_EMPTY);
 for(unsigned i=1;i<8;i++)assert(pet_app_store_unpin(&pins[i]));
 // Serial exhaustion never wraps and never grants a pin.
 uint64_t serial=next_pin_serial;next_pin_serial=UINT64_MAX;pet_app_store_pin_t exhausted={0};
 assert(!pet_app_store_pin_scoped(11,"native",&exhausted,&exhausted));next_pin_serial=serial;
 #else
 for(unsigned i=0;i<8;i++)assert(pet_app_store_unpin(&pins[i]));
 assert(pet_app_store_load_scoped(9,"native",out)==PET_APP_STORE_UNAVAILABLE);
 #endif
 fs_activity_epoch=UINT64_MAX-1;first=pet_fs_activity_begin();pet_fs_activity_end(&first);
 assert(!pet_fs_activity_idle_epoch(&epoch)&&epoch==UINT64_MAX);
 first=pet_fs_activity_begin();pet_fs_activity_end(&first);assert(!pet_fs_activity_idle_epoch(&epoch));
 #if PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED
 assert(save_cache_begins>0&&save_cache_begins==save_cache_ends&&!save_cache_epoch);
 #endif
 return 0;
}
''')
    if wide_program:
        code=harness.read_text().replace('pet_program_image_t program_image={.schema=42,.count=1,.fuel=1,.cells=1,.valid=true};',
            'pet_program_image_t program_image={.schema=42,.count=1,.fuel=1,.cells=256,.valid=true};')
        code=code.replace(' assert(pet_program_persistence_enqueue("program-case",&committed));',
            ' committed.values[64]=INT32_MIN;committed.values[255]=INT32_MAX;\n assert(pet_program_persistence_enqueue("program-case",&committed));')
        code=code.replace(' assert(pet_program_persistence_checkpoint("program-case",&committed)==PET_APP_CHECKPOINT_DURABLE);\n memset(saves,0,sizeof(saves));', r'''
 assert(pet_program_persistence_checkpoint("program-case",&committed)==PET_APP_CHECKPOINT_DURABLE);
 // Publish a second complete wide snapshot, then tear only its disk generation.
 // Restart must recover the first P4S2, including both high-index extrema.
 pet_program_snapshot_t newer=committed;newer.revision++;newer.values[255]=1234;
 assert(pet_program_persistence_enqueue("program-case",&newer));clock_us+=31000000;
 for(int k=0;k<40;k++)flush_one();
 assert(pet_program_persistence_checkpoint("program-case",&newer)==PET_APP_CHECKPOINT_DURABLE);
 int wide_slot=lookup_slot(PET_PROGRAM_SAVE_DOMAIN,"program-case");assert(wide_slot>=0);
 scoped_path_for("program-case",PET_PROGRAM_SAVE_DOMAIN,saves[wide_slot].file_slot,path);
 FILE *torn_wide=fopen(path,"wb");assert(torn_wide);fputs("torn",torn_wide);fclose(torn_wide);
 memset(saves,0,sizeof(saves));''',1)
        code=code.replace('static save_load_workspace_t *save_load_workspace_acquire(void){return calloc(1,sizeof(save_load_workspace_t));}',r"""
static unsigned fail_load_alloc,live_load_alloc;static bool forget_load_alloc;
static save_load_workspace_t *save_load_workspace_acquire(void){
 assert(!store_locked);if(fail_load_alloc){fail_load_alloc--;if(forget_load_alloc)pet_app_store_forget("oom-cold");return NULL;}
 live_load_alloc++;return calloc(1,sizeof(save_load_workspace_t));
}""")
        code=code.replace('static void save_load_workspace_free(void *p){free(p);}',
            'static void save_load_workspace_free(void *p){assert(!store_locked&&live_load_alloc);live_load_alloc--;free(p);}')
        code=code.replace(' assert(pet_app_store_init());char out[PET_APP_SAVE_BYTES],path[64];',r"""
 assert(pet_app_store_init());char out[PET_APP_SAVE_BYTES],path[64];
 fail_load_alloc=1;assert(pet_app_store_load("oom-cold",out)==PET_APP_STORE_UNAVAILABLE);
 int oom_slot=lookup_slot(0,"oom-cold");assert(oom_slot>=0&&!saves[oom_slot].loading&&saves[oom_slot].load_failed);
 assert(!pet_app_store_update("oom-cold","{}"));
 pet_resource_usage_t oom_usage;pet_resources_device_usage(&oom_usage);
 assert(!oom_usage.live_owners&&!oom_usage.revoked_owners&&!oom_usage.leases[PET_RESOURCE_STORAGE_WORK]);
 assert(pet_app_store_load("oom-cold",out)==PET_APP_STORE_EMPTY&&!live_load_alloc);
 memset(saves,0,sizeof(saves));save_io_t oom_io={0};assert(save_io_begin(&oom_io,NULL));
 fail_load_alloc=1;assert(pet_app_store_load_with_lease("oom-cold",out,oom_io.lease)==PET_APP_STORE_UNAVAILABLE);
 assert(pet_resources_device_current(oom_io.lease));save_io_end(&oom_io);
 memset(saves,0,sizeof(saves));fail_load_alloc=1;forget_load_alloc=true;
 assert(pet_app_store_load("oom-cold",out)==PET_APP_STORE_EMPTY);forget_load_alloc=false;
 assert(!saves[lookup_slot(0,"oom-cold")].loading&&!live_load_alloc);memset(saves,0,sizeof(saves));
""")
        harness.write_text(code)
    program_sources=[ROOT/'main'/('pet_p4_program_'+name+'.c') for name in ('core','image','runtime','host','store','persistence','activation')]
    resource_device=ROOT/'main/pet_p4_resources_device.c'
    for reviewed in (0,1):
        exe=tmp_path/f'save-{reviewed}';directory=tmp_path/f'files-{reviewed}';directory.mkdir()
        flags=['-DPET_APP_SCOPED_RECLAIM_REVIEWED=1'] if reviewed else []
        if wide_program:
            flags+=['-DPET_P4_PROGRAM_V2=1','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1']
        subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*flags,'-DPET_APP_SAVE_ROOT="."','-I',str(tmp_path),'-I',str(ROOT/'main'),'-I',str(ROOT/'tests'),str(harness),str(ROOT/'main/pet_p4_resources.c'),str(resource_device),*map(str,program_sources),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],cwd=directory,check=True)
    if wide_program:
        stack_source=tmp_path/'save-stack.c'
        stack_source.write_text(harness.read_text().replace('static pet_app_store_status_t load_with_authority(',
            'static __attribute__((noinline)) pet_app_store_status_t load_with_authority('))
        subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-O2','-fstack-usage',*flags,
                        '-I',str(tmp_path),'-I',str(ROOT/'main'),'-I',str(ROOT/'tests'),
                        '-c',str(stack_source),'-o',str(tmp_path/'save-stack.o')],check=True)
        frames=[line for line in (tmp_path/'save-stack.su').read_text().splitlines() if 'load_with_authority' in line]
        assert frames and all(int(line.split('\t')[1])<=512 for line in frames),frames
