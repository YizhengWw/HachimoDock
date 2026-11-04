/* Deferred, checksummed A/B snapshots. No filesystem work in runtime critical sections. */
#include "pet_p4_app_store.h"
#include "pet_p4_task_policy.h"
#include "pet_p4_file_read.h"
#include "pet_p4_audio.h"
#include "pet_p4_widget_trace.h"
#include "pet_p4_fs_activity.h"
#include "pet_p4_sched_trace.h"
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_storage_cache_lease.h"
#include "pet_p4_reset_phase.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#define SLOTS 32
#define SCOPED_SLOTS 8
#ifndef PET_APP_SAVE_ROOT
#define PET_APP_SAVE_ROOT "/spiffs"
#endif
typedef struct {char id[48],json[PET_APP_SAVE_BYTES];uint32_t seq,domain,epoch,forget_serial;uint64_t dirty_since,pin_serial;const void *pin_owner;uint8_t file_slot;bool dirty,loaded,deleted,loading,load_failed,epoch_pending;} save_t;
typedef struct {uint32_t magic,seq,size,checksum;char id[48];} header_t;
/* One independent cold-load workspace per caller. Allocate/free outside the
 * store mutex; no shared scratch and no format-sized automatic records. */
typedef struct {save_t a,b;} save_load_workspace_t;
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static save_load_workspace_t *save_load_workspace_acquire(void){return heap_caps_calloc(1,sizeof(save_load_workspace_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static void save_load_workspace_free(void *p){heap_caps_free(p);}
#else
#include <stdlib.h>
static save_load_workspace_t *save_load_workspace_acquire(void){return calloc(1,sizeof(save_load_workspace_t));}
static void save_load_workspace_free(void *p){free(p);}
#endif
static void save_load_workspace_finish(save_load_workspace_t **p){if(p&&*p)save_load_workspace_free(*p);}

#if PET_P4_SAVE_TRACE_DETACHED
/* Native worker-only scratch, protected by the exact pending scope below.
 * This does NOT alias the snapshot/JSON or a mutable model slot. Keep both
 * the physical IO and later diagnostic call chains off a large shared frame. */
static EXT_RAM_BSS_ATTR struct {char path[64];header_t header;} save_write_scratch;
#endif
static EXT_RAM_BSS_ATTR save_t saves[SLOTS+SCOPED_SLOTS];
static SemaphoreHandle_t lock;
static TaskHandle_t worker;
static uint8_t flush_cursor;
static uint64_t next_pin_serial;
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
/* One stable, worker-owned copy. Moving the snapshot out of the 4KiB worker
 * stack preserves concurrent update reconciliation; it is NEVER a pointer to
 * a mutable cache slot. Reentry must refuse BEFORE touching this copy, even
 * when subsequent STORAGE_WORK admission would have refused the nested job. */
typedef struct save_pending_scope {
  const struct save_pending_scope *self;
  TaskHandle_t task;
} save_pending_scope_t;
static save_pending_scope_t *save_pending_scope;
static bool save_stack_sample_due;
static portMUX_TYPE save_stack_mux=portMUX_INITIALIZER_UNLOCKED;
static pet_app_save_stack_t save_stack;
static bool save_pending_begin(save_pending_scope_t *scope){
  if(!scope||scope->self||scope->task||!lock)return false;
  TaskHandle_t task=xTaskGetCurrentTaskHandle();if(!task)return false;
  bool ok=false;xSemaphoreTake(lock,portMAX_DELAY);
  if(!save_pending_scope){
    *scope=(save_pending_scope_t){.self=scope,.task=task};save_pending_scope=scope;ok=true;
  }
  xSemaphoreGive(lock);return ok;
}
static void save_pending_finish(save_pending_scope_t *scope){
  if(!scope||scope->self!=scope||scope->task!=xTaskGetCurrentTaskHandle())return;
  xSemaphoreTake(lock,portMAX_DELAY);
  if(save_pending_scope==scope){save_pending_scope=NULL;*scope=(save_pending_scope_t){0};}
  xSemaphoreGive(lock);
}
static void save_stack_sample(void){
  _Static_assert(sizeof(StackType_t)==1,"ESP-IDF high-water observation must be byte-labelled");
  uint32_t free_bytes=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
  portENTER_CRITICAL(&save_stack_mux);
  if(free_bytes>PET_APP_SAVE_TASK_STACK_BYTES){
    if(save_stack.invalid_samples<UINT32_MAX)save_stack.invalid_samples++;
  }else{
    if(!save_stack.observations||free_bytes<save_stack.minimum_free_bytes)save_stack.minimum_free_bytes=free_bytes;
    if(save_stack.observations<UINT32_MAX)save_stack.observations++;
  }
  save_stack.available=save_stack.observations&&!save_stack.invalid_samples;
  portEXIT_CRITICAL(&save_stack_mux);
}
bool pet_app_store_worker_stack(pet_app_save_stack_t *out){
  if(!out)return false;
  portENTER_CRITICAL(&save_stack_mux);*out=save_stack;portEXIT_CRITICAL(&save_stack_mux);return true;
}
#endif
typedef struct {pet_resource_owner_t owner;pet_resource_lease_t lease;bool owned;
#if PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED
  pet_storage_cache_lease_t cache;
#endif
} save_io_t;
static bool save_io_begin(save_io_t *io,const pet_resource_lease_t *borrowed){
  if(borrowed){
    if(borrowed->kind!=PET_RESOURCE_STORAGE_WORK||!pet_resources_device_current(*borrowed))return false;
    io->lease=*borrowed;return true;
  }
  if(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&io->owner)!=PET_RESOURCE_OK)return false;
  if(pet_resources_device_acquire(io->owner,PET_RESOURCE_STORAGE_WORK,1,&io->lease)!=PET_RESOURCE_OK){
    pet_resources_device_revoke(io->owner);memset(&io->owner,0,sizeof(io->owner));return false;
  }
  io->owned=true;
  return true;
}
static void save_io_cache_begin(save_io_t *io){
#if PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED
  /* Borrowed miniapp leases already own their cache scope. Only this native
   * save owner's independently admitted physical job may start a new epoch.
   * Run AFTER the save-model mutex is released, never inside admission. */
  if(io->owned)(void)pet_storage_cache_lease_begin(&io->cache,io->lease);
#else
  (void)io;
#endif
}
static void save_io_end(save_io_t *io){
  if(io->owned){
#if PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED
    (void)pet_storage_cache_lease_finish(&io->cache);
#endif
    (void)pet_resources_device_finish(io->lease);
    pet_resources_device_revoke(io->owner);
  }
  memset(io,0,sizeof(*io));
}
static uint64_t now_ms(void){return (uint64_t)esp_timer_get_time()/1000;}
static uint32_t checksum(const char *p,size_t n){uint32_t h=2166136261u;for(size_t i=0;i<n;i++){h^=(unsigned char)p[i];h*=16777619u;}return h;}
static bool valid_id(const char *id){
  if(!id||!id[0]||strlen(id)>47)return false;
  for(const char *p=id;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return false;
  return true;
}
static void path_for(const char *id,unsigned slot,char out[64]){
  unsigned char hash[32];mbedtls_sha256((const unsigned char*)id,strlen(id),hash,0);
  snprintf(out,64,PET_APP_SAVE_ROOT "/as-%02x%02x%02x%02x%02x%02x%02x%02x-%u",hash[0],hash[1],hash[2],hash[3],hash[4],hash[5],hash[6],hash[7],slot);
}
static void scoped_path_for(const char *id,uint32_t domain,unsigned slot,char out[64]){
  if(!domain){path_for(id,slot,out);return;}
  unsigned char hash[32];mbedtls_sha256((const unsigned char*)id,strlen(id),hash,0);
  snprintf(out,64,PET_APP_SAVE_ROOT "/ns-%08lx-%02x%02x%02x%02x%02x%02x%02x%02x-%u",
    (unsigned long)domain,hash[0],hash[1],hash[2],hash[3],hash[4],hash[5],hash[6],hash[7],slot);
}
/* Explicit forget has its own durable A/B epoch, independent of save seq.
 * UI/update only marks RAM. The admitted worker commits the epoch BEFORE any
 * new snapshot, so a concurrent reinstall cannot resurrect a high legacy seq.
 * Existing epoch-zero filenames/headers remain readable without migration. */
typedef struct {uint32_t magic,epoch,check;char id[48];} save_epoch_t;
static bool read_epoch(const char *id,uint32_t domain,uint32_t *epoch,unsigned *slot,pet_resource_lease_t lease,bool explicit_forget){
  PET_FS_ACTIVITY_SCOPE(epoch_read);
  PET_WIDGET_TRACE_SCOPE(epoch_read_trace,PET_WTRACE_SAVE_READ);
  *epoch=0;*slot=4;bool valid=false,corrupt=false;
  for(unsigned i=2;i<4;i++){
    if(!pet_resources_device_current(lease))return false;
    char path[64];scoped_path_for(id,domain,i,path);FILE *f;int open_error=0;
    {PET_WIDGET_TRACE_SCOPE(epoch_open,PET_WTRACE_FS_OPEN);f=fopen(path,"rb");if(!f)open_error=errno;}
    if(!f){if(open_error!=ENOENT)return false;continue;}
    save_epoch_t e={0};bool ok;
    {PET_WIDGET_TRACE_SCOPE(epoch_data,PET_WTRACE_FS_DATA);ok=fread(&e,1,sizeof(e),f)==sizeof(e)&&fgetc(f)==EOF&&!ferror(f);}
    {PET_WIDGET_TRACE_SCOPE(epoch_close,PET_WTRACE_FS_CLOSE);if(fclose(f)!=0)return false;}
    uint32_t check=e.check;e.check=0;
    ok=ok&&e.magic==0x41534531&&memchr(e.id,0,sizeof(e.id))&&!strcmp(e.id,id)&&checksum((const char*)&e,sizeof(e))==check;
    if(!ok){corrupt=true;continue;}
    if(!valid||e.epoch>*epoch){*epoch=e.epoch;*slot=i;}valid=true;
  }
  return !corrupt||(explicit_forget&&valid);
}
static void epoch_path_for(const char *id,uint32_t domain,unsigned slot,uint32_t epoch,char out[64]){
  scoped_path_for(id,domain,slot,out);
  if(epoch){size_t n=strlen(out);snprintf(out+n,64-n,"-e%08lx",(unsigned long)epoch);}
}
static bool write_epoch_record(const char *id,uint32_t domain,unsigned slot,const save_epoch_t *e,pet_resource_lease_t lease){
  if(!pet_resources_device_current(lease))return false;
  char path[64];scoped_path_for(id,domain,slot,path);FILE *f;
  {PET_WIDGET_TRACE_SCOPE(epoch_open,PET_WTRACE_FS_OPEN);f=fopen(path,"wb");}if(!f)return false;
  bool ok;
  {PET_WIDGET_TRACE_SCOPE(epoch_data,PET_WTRACE_FS_DATA);ok=pet_resources_device_current(lease)&&fwrite(e,1,sizeof(*e),f)==sizeof(*e)&&fflush(f)==0&&fsync(fileno(f))==0;}
  {PET_WIDGET_TRACE_SCOPE(epoch_close,PET_WTRACE_FS_CLOSE);if(fclose(f)!=0)ok=false;}
  return ok;
}
static bool write_forget_epoch(const char *id,uint32_t domain,uint32_t *epoch,pet_resource_lease_t lease){
  PET_FS_ACTIVITY_SCOPE(epoch_write);
  PET_WIDGET_TRACE_SCOPE(epoch_write_trace,PET_WTRACE_SAVE_WRITE);
  uint32_t old;unsigned slot;
  if(!pet_resources_device_current(lease)||!read_epoch(id,domain,&old,&slot,lease,true)||!pet_resources_device_current(lease)||old==UINT32_MAX)return false;
  save_epoch_t e={.magic=0x41534531,.epoch=old};snprintf(e.id,sizeof(e.id),"%s",id);
  if(slot==4){
    /* First deletion needs a durable epoch-zero fallback. Publish that anchor
     * from its own temp only after fsync; a torn first write must not turn a
     * still-valid legacy save into permanently corrupt epoch metadata. */
    e.check=checksum((const char*)&e,sizeof(e));
    if(!write_epoch_record(id,domain,4,&e,lease))return false;
    char tmp[64],path[64];scoped_path_for(id,domain,4,tmp);scoped_path_for(id,domain,2,path);
    {PET_WIDGET_TRACE_SCOPE(epoch_publish,PET_WTRACE_FS_RENAME);if(!pet_resources_device_current(lease)||rename(tmp,path)!=0)return false;}
    slot=2;
  }
  e.epoch=old+1;e.check=0;e.check=checksum((const char*)&e,sizeof(e));
  /* A corrupt peer is never used for ordinary restore. Explicit forget may
   * repair it using the valid epoch, but first empties the target namespace:
   * it might have belonged to a prior committed-but-now-corrupt peer. */
  char target[64];
  for(unsigned i=0;i<2;i++){
    if(!pet_resources_device_current(lease))return false;
    epoch_path_for(id,domain,i,e.epoch,target);
    PET_WIDGET_TRACE_SCOPE(epoch_target_remove,PET_WTRACE_FS_REMOVE);
    if(unlink(target)!=0&&errno!=ENOENT)return false;
  }
  if(!write_epoch_record(id,domain,slot==2?3:2,&e,lease))return false;
  *epoch=e.epoch;
  /* Old bytes belong to an explicit forget only. The committed epoch already
   * makes them ineligible even if removal is interrupted or fails. */
  char path[64];
  for(unsigned i=0;i<2&&pet_resources_device_current(lease);i++){epoch_path_for(id,domain,i,old,path);PET_WIDGET_TRACE_SCOPE(epoch_remove,PET_WTRACE_FS_REMOVE);(void)unlink(path);}
  return true;
}
static bool commit_forget_epoch(save_t *save,pet_resource_lease_t lease){
  if(!write_forget_epoch(save->id,save->domain,&save->epoch,lease))return false;
  save->epoch_pending=false;return true;
}
typedef enum { SAVE_MISSING=0,SAVE_VALID=1,SAVE_CORRUPT=2,SAVE_UNAVAILABLE=3 } save_read_t;
static save_read_t read_slot(const char *id,uint32_t domain,unsigned slot,save_t *out,pet_resource_lease_t lease){
  if(!pet_resources_device_current(lease))return SAVE_UNAVAILABLE;
  PET_FS_ACTIVITY_SCOPE(storage_read);
  PET_WIDGET_TRACE_SCOPE(save_read,PET_WTRACE_SAVE_READ);
  char path[64];epoch_path_for(id,domain,slot,out->epoch,path);FILE *f;int open_error=0;
  {PET_WIDGET_TRACE_SCOPE(open_read,PET_WTRACE_FS_OPEN);f=fopen(path,"rb");if(!f)open_error=errno;}
  if(!f)return open_error==ENOENT?SAVE_MISSING:SAVE_UNAVAILABLE;
  header_t h={0};bool ok,metadata_failed=false;size_t extent=0;
  {PET_WIDGET_TRACE_SCOPE(read_data,PET_WTRACE_FS_DATA);
    pet_file_size_result_t sized=pet_file_read_extent(f,sizeof(h)+PET_APP_SAVE_BYTES-1,&extent);
    metadata_failed=sized==PET_FILE_SIZE_IO;
    ok=pet_resources_device_current(lease)&&sized==PET_FILE_SIZE_OK&&extent>=sizeof(h)
      &&fread(&h,1,sizeof(h),f)==sizeof(h)&&h.magic==0x41535631&&h.size>0&&h.size<PET_APP_SAVE_BYTES
      &&extent==sizeof(h)+h.size&&memchr(h.id,0,sizeof(h.id))&&!strcmp(h.id,id);
    if(ok){
      ok=pet_resources_device_current(lease)&&fread(out->json,1,h.size,f)==h.size;
      size_t after=0;
      if(ok&&(pet_file_read_extent(f,sizeof(h)+PET_APP_SAVE_BYTES-1,&after)!=PET_FILE_SIZE_OK||after!=extent)){
        metadata_failed=true;ok=false;
      }
      out->json[h.size]=0;ok=ok&&checksum(out->json,h.size)==h.checksum&&!memchr(out->json,0,h.size);
    }
  }
  bool unavailable=metadata_failed||ferror(f)!=0;
  {PET_WIDGET_TRACE_SCOPE(close_read,PET_WTRACE_FS_CLOSE);if(fclose(f)!=0)unavailable=true;}
  if(unavailable||!pet_resources_device_current(lease))return SAVE_UNAVAILABLE;
  if(ok){snprintf(out->id,sizeof(out->id),"%s",id);out->domain=domain;out->seq=h.seq;out->file_slot=slot;out->loaded=true;}
  return ok?SAVE_VALID:SAVE_CORRUPT;
}
static bool write_slot(const save_t *save,pet_resource_lease_t lease){
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(save_phase,PET_PHASE_SAVE);
#endif
  if(!pet_resources_device_current(lease))return false;
  PET_FS_ACTIVITY_SCOPE(storage_write);
  PET_WIDGET_TRACE_SCOPE(save_write,PET_WTRACE_SAVE_WRITE);
#if PET_P4_SAVE_TRACE_DETACHED
  if(!save_pending_scope||save_pending_scope->self!=save_pending_scope
      ||save_pending_scope->task!=xTaskGetCurrentTaskHandle())return false;
  char *path=save_write_scratch.path;
#else
  char path[64];
#endif
  epoch_path_for(save->id,save->domain,save->file_slot,save->epoch,path);
  if(save->deleted){bool ok=true;for(unsigned i=0;i<2;i++){if(!pet_resources_device_current(lease))return false;epoch_path_for(save->id,save->domain,i,save->epoch,path);PET_WIDGET_TRACE_SCOPE(remove_save,PET_WTRACE_FS_REMOVE);if(unlink(path)!=0&&errno!=ENOENT)ok=false;}return ok;}
#if PET_P4_SAVE_TRACE_DETACHED
  header_t *h=&save_write_scratch.header;
  *h=(header_t){.magic=0x41535631,.seq=save->seq,.size=(uint32_t)strlen(save->json)};
#else
  header_t local_header={.magic=0x41535631,.seq=save->seq,.size=(uint32_t)strlen(save->json)};
  header_t *h=&local_header;
#endif
  memcpy(h->id,save->id,sizeof(h->id));h->checksum=checksum(save->json,h->size);
  FILE *f;
  {PET_WIDGET_TRACE_SCOPE(open_write,PET_WTRACE_FS_OPEN);f=fopen(path,"wb");}
  if(!f)return false;
  bool ok;
  {PET_WIDGET_TRACE_SCOPE(write_data,PET_WTRACE_FS_DATA);
#if PET_P4_RESET_PHASE_TRACE
    PET_RESET_PHASE_SCOPE(data_phase,PET_PHASE_FILE_DATA);
#endif
    ok=pet_resources_device_current(lease)&&fwrite(h,1,sizeof(*h),f)==sizeof(*h)
      &&pet_resources_device_current(lease)&&fwrite(save->json,1,h->size,f)==h->size
      &&pet_resources_device_current(lease)&&fflush(f)==0;
    if(ok){
#if PET_P4_RESET_PHASE_TRACE
      PET_RESET_PHASE_SCOPE(sync_phase,PET_PHASE_FILE_SYNC);
#endif
      ok=pet_resources_device_current(lease)&&fsync(fileno(f))==0;
    }
  }
  /* Closing is mandatory even after revocation. Once fsync succeeded, do not
   * turn that completed durable snapshot into a cancellation or replay. */
  {PET_WIDGET_TRACE_SCOPE(close_write,PET_WTRACE_FS_CLOSE);
#if PET_P4_RESET_PHASE_TRACE
    PET_RESET_PHASE_SCOPE(close_phase,PET_PHASE_FILE_CLOSE);
#endif
    if(fclose(f)!=0)ok=false;
  }
  return ok; // prior generation is never overwritten until a new full snapshot exists
}
/* Observation/update paths must never reserve or evict a different key. */
static int lookup_slot(uint32_t domain,const char *id){
  int first=domain?SLOTS:0,last=domain?SLOTS+SCOPED_SLOTS:SLOTS;
  for(int i=first;i<last;i++)if(saves[i].domain==domain&&!strcmp(saves[i].id,id))return i;
  return -1;
}
static int reserve_slot(uint32_t domain,const char *id){
  int present=lookup_slot(domain,id);if(present>=0)return present;
  int first=domain?SLOTS:0,last=domain?SLOTS+SCOPED_SLOTS:SLOTS;
  for(int i=first;i<last;i++)if(!saves[i].id[0])return i;
  if(domain&&PET_APP_SCOPED_RECLAIM_REVIEWED)for(int i=first;i<last;i++){
    const save_t *s=&saves[i];
    if(s->loaded&&!s->dirty&&!s->loading&&!s->load_failed&&!s->deleted&&!s->pin_serial)return i;
  }
  return -1;
}
static int find_slot(const char *id){return reserve_slot(0,id);}
#if PET_P4_SAVE_TRACE_DETACHED
static __attribute__((noinline)) void flush_one(pet_widget_trace_operation_t *completed,bool *completed_ok){
#else
static void flush_one(void){
#endif
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
    static EXT_RAM_BSS_ATTR save_t pending;
    save_pending_scope_t pending_owner __attribute__((cleanup(save_pending_finish)))={0};
#else
    save_t pending;
#endif
    /* Fast observation plus same-broker atomic admission below. A busy lease
     * is ordinary deferral: keep the dirty snapshot for the next worker tick. */
    if(pet_p4_audio_stream_playing()||pet_p4_audio_music_active()||pet_p4_audio_conversation_active()||pet_p4_audio_active()||pet_p4_audio_playback_active())return;
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
    if(!save_pending_begin(&pending_owner))return;
#endif
    for(int offset=0;offset<SLOTS+SCOPED_SLOTS;offset++){
      int i=(flush_cursor+offset)%(SLOTS+SCOPED_SLOTS);
      xSemaphoreTake(lock,portMAX_DELAY);
      /* A cold load owns this slot until its disk sequence is reconciled.
       * Concurrent updates may proceed, but cannot flush a guessed sequence. */
      bool ready=saves[i].dirty&&!saves[i].loading&&!saves[i].load_failed&&now_ms()-saves[i].dirty_since>=30000;
      if(ready){pending=saves[i];pending.file_slot^=1;}xSemaphoreGive(lock);
      if(!ready)continue;
      save_io_t io __attribute__((cleanup(save_io_end)))={0};
      if(!save_io_begin(&io,NULL))return;
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
      save_stack_sample_due=true;
#endif
      save_io_cache_begin(&io);
      flush_cursor=(uint8_t)((i+1)%(SLOTS+SCOPED_SLOTS));
#if PET_P4_SAVE_TRACE_DETACHED
      *completed=pet_widget_trace_begin(PET_WTRACE_SAVE_FLUSH);
#else
      PET_WIDGET_TRACE_OPERATION(op,PET_WTRACE_SAVE_FLUSH);
#endif
      bool epoch_ok=!pending.epoch_pending||commit_forget_epoch(&pending,io.lease);
      bool ok=epoch_ok&&write_slot(&pending,io.lease);
      xSemaphoreTake(lock,portMAX_DELAY);
      if(epoch_ok&&saves[i].domain==pending.domain&&!strcmp(saves[i].id,pending.id)
          &&saves[i].forget_serial==pending.forget_serial){saves[i].epoch=pending.epoch;saves[i].epoch_pending=false;}

      if(ok&&saves[i].domain==pending.domain&&!strcmp(saves[i].id,pending.id)){
        saves[i].file_slot=pending.file_slot;saves[i].dirty_since=now_ms();
        if(saves[i].seq==pending.seq){saves[i].dirty=false;if(pending.deleted)memset(&saves[i],0,sizeof(save_t));}
      }
      xSemaphoreGive(lock);
#if PET_P4_SAVE_TRACE_DETACHED
      *completed_ok=ok;
#else
      pet_widget_trace_end(&op,ok);
#endif
      break; // at most one bounded save per second
    }
}
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
__attribute__((weak)) void pet_app_store_housekeeping(void){}
#endif
static void save_worker(void *arg){
  (void)arg;
#if PET_P4_RESET_PHASE_TRACE
  (void)pet_reset_phase_bind(PET_RESET_SAVE);
#endif
#if PET_P4_SCHED_TRACE_ENABLE
  bool trace_registered=pet_sched_trace_register(PET_SCHED_TASK_SAVE,xTaskGetCurrentTaskHandle());
  configASSERT(trace_registered);(void)trace_registered;
#endif
#if PET_P4_STORAGE_IO_TRACE_ENABLE
  bool io_registered=pet_storage_io_register(PET_STORAGE_IO_SAVE,xTaskGetCurrentTaskHandle());
  configASSERT(io_registered);(void)io_registered;
#endif
  for(;;){
    vTaskDelay(pdMS_TO_TICKS(1000));
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
    save_stack_sample_due=false;
#endif
#if PET_P4_SAVE_TRACE_DETACHED
    /* Same task, same numeric operation. Physical/cache/lease cleanup has
     * returned before formatting its trace; do not retain the filesystem
     * caller's large frame during logging. Still sample AFTER both paths.
     * This extends observation through cleanup, never excludes physical IO. */
    pet_widget_trace_operation_t completed __attribute__((cleanup(pet_widget_trace_operation_cleanup)))={0};
    bool completed_ok=false;
    flush_one(&completed,&completed_ok);
    pet_widget_trace_end(&completed,completed_ok);
#else
    flush_one();
#endif
#if PET_APP_SAVE_OWNED_PENDING_ENABLED
    if(save_stack_sample_due){save_stack_sample_due=false;save_stack_sample();}
#endif
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
    pet_app_store_housekeeping();
#endif
  }
}
bool pet_app_store_init(void){
  if (worker) return true;
  if (!lock) lock = xSemaphoreCreateMutex();
  if (!lock) return false;
  return pet_p4_storage_worker_create(save_worker,"pet_app_save",PET_APP_SAVE_TASK_STACK_BYTES,NULL,1,&worker)==pdPASS;
}
static pet_app_store_status_t load_with_authority(uint32_t domain,const char *id,char out[PET_APP_SAVE_BYTES],
  const pet_resource_lease_t *borrowed){
  if (!lock || !valid_id(id) || !out) return PET_APP_STORE_INVALID;
  out[0] = 0;
  xSemaphoreTake(lock,portMAX_DELAY);int i=reserve_slot(domain,id);
  if(i<0){xSemaphoreGive(lock);return PET_APP_STORE_UNAVAILABLE;}
  bool exact=saves[i].domain==domain&&!strcmp(saves[i].id,id);
  if(saves[i].loading){xSemaphoreGive(lock);return PET_APP_STORE_BUSY;}
  if(exact&&saves[i].loaded&&!saves[i].load_failed){bool ok=!saves[i].deleted&&saves[i].json[0];if(ok)memcpy(out,saves[i].json,PET_APP_SAVE_BYTES);xSemaphoreGive(lock);return ok?PET_APP_STORE_VALUE:PET_APP_STORE_EMPTY;}
  save_io_t io __attribute__((cleanup(save_io_end)))={0};
  /* Admission only enters the bounded broker critical section: it never
   * waits, calls back or does I/O. Keep identity reservation atomic with it so
   * a racing first update cannot skip reconciliation of an existing disk seq. */
  if(!save_io_begin(&io,borrowed)){xSemaphoreGive(lock);return PET_APP_STORE_BUSY;}
  /* Only AFTER successful admission may a clean unpinned victim be replaced.
   * loading/dirty/failure/pins excluded above; its disk generations remain.
   * A failed admission must leave even the old RAM checkpoint observable. */
  if(!exact)memset(&saves[i],0,sizeof(saves[i]));
  /* Reserve the identity, not the mutex, across flash reads. UI ticks and
   * saves must never wait behind SPIFFS. loading also prevents slot reuse and
   * the save worker from publishing an uninitialised disk sequence. */
  snprintf(saves[i].id,sizeof(saves[i].id),"%s",id);saves[i].domain=domain;saves[i].loading=true;
  uint32_t load_forget_serial=saves[i].forget_serial;
  xSemaphoreGive(lock);
  save_load_workspace_t *workspace __attribute__((cleanup(save_load_workspace_finish)))=save_load_workspace_acquire();
  if(!workspace){
    xSemaphoreTake(lock,portMAX_DELAY);saves[i].loading=false;
    bool forgotten=saves[i].loaded&&saves[i].deleted;
    saves[i].load_failed=!forgotten;xSemaphoreGive(lock);
    return forgotten?PET_APP_STORE_EMPTY:PET_APP_STORE_UNAVAILABLE;
  }
  save_io_cache_begin(&io);
  save_t *a=&workspace->a,*b=&workspace->b;uint32_t epoch=0;unsigned epoch_slot;
  bool epoch_ok=read_epoch(id,domain,&epoch,&epoch_slot,io.lease,false);
  a->epoch=b->epoch=epoch;
  save_read_t ar=epoch_ok?read_slot(id,domain,0,a,io.lease):SAVE_UNAVAILABLE;
  save_read_t br=epoch_ok?read_slot(id,domain,1,b,io.lease):SAVE_UNAVAILABLE;
  bool av=ar==SAVE_VALID,bv=br==SAVE_VALID;
  const save_t *disk=bv&&(!av||(int32_t)(b->seq-a->seq)>0)?b:a;
  xSemaphoreTake(lock,portMAX_DELAY);
  if(saves[i].forget_serial!=load_forget_serial){
    /* This read started before an explicit forget. Neither its old epoch nor
     * old seq (nor a resulting missing/error file) may override the new RAM
     * intent/value. Its next snapshot is fenced by the independent epoch. */
    saves[i].loading=false;saves[i].load_failed=false;
    bool value=!saves[i].deleted&&saves[i].json[0];if(value)memcpy(out,saves[i].json,PET_APP_SAVE_BYTES);
    xSemaphoreGive(lock);return value?PET_APP_STORE_VALUE:PET_APP_STORE_EMPTY;
  }
  if(ar==SAVE_UNAVAILABLE||br==SAVE_UNAVAILABLE
      ||(!av&&!bv&&(ar!=SAVE_MISSING||br!=SAVE_MISSING))){
    /* An unreadable generation may contain newer data. A corrupt generation
     * may fall back to a valid peer, but never to an invented empty save. */
    saves[i].loading=false;
    /* An explicit forget is stronger authority than a failed background read:
     * it removes both generations and does not depend on their sequence. */
    bool forgotten=saves[i].loaded&&saves[i].deleted;
    saves[i].load_failed=!forgotten;xSemaphoreGive(lock);
    return forgotten?PET_APP_STORE_EMPTY:PET_APP_STORE_UNAVAILABLE;
  }
  if(!saves[i].epoch_pending)saves[i].epoch=epoch;
  if(!saves[i].loaded){
    if(av||bv)saves[i]=*disk;
    else{saves[i].loaded=true;saves[i].epoch=epoch;}
  }else if(av||bv){
    /* update/forget during the read wins, including a tombstone. Preserve the
     * latest durable generation as the fallback and order the new save after
     * it, rather than replacing the user's newer RAM value with old disk data. */
    if(!saves[i].epoch_pending)saves[i].epoch=epoch;
    saves[i].file_slot=disk->file_slot;
    if((int32_t)(saves[i].seq-disk->seq)<=0){saves[i].seq=disk->seq+1;if(!saves[i].seq)saves[i].seq=1;}
  }
  saves[i].loading=false;saves[i].load_failed=false;
  bool ok=!saves[i].deleted&&saves[i].json[0];
  if(ok)memcpy(out,saves[i].json,PET_APP_SAVE_BYTES);
  xSemaphoreGive(lock);
  return ok?PET_APP_STORE_VALUE:PET_APP_STORE_EMPTY;
}
pet_app_store_status_t pet_app_store_load(const char *id,char out[PET_APP_SAVE_BYTES]){
  return load_with_authority(0,id,out,NULL);
}
pet_app_store_status_t pet_app_store_load_with_lease(const char *id,char out[PET_APP_SAVE_BYTES],pet_resource_lease_t lease){
  return load_with_authority(0,id,out,&lease);
}
bool pet_app_store_read(const char *id,char out[PET_APP_SAVE_BYTES]){
  return pet_app_store_load(id,out)==PET_APP_STORE_VALUE;
}
bool pet_app_store_read_cached(const char *id,char out[PET_APP_SAVE_BYTES]){
  if(!lock||!valid_id(id)||!out)return false;
  out[0]=0;xSemaphoreTake(lock,portMAX_DELAY);int i=find_slot(id);
  bool ok=i>=0&&saves[i].loaded&&!saves[i].loading&&!saves[i].load_failed&&!saves[i].deleted&&saves[i].json[0];
  if(ok)memcpy(out,saves[i].json,PET_APP_SAVE_BYTES);
  xSemaphoreGive(lock);return ok;
}
static bool update_in_domain(uint32_t domain,const char *id,const char *json,bool require_loaded){
  if(!lock||!valid_id(id)||!json||!json[0]||strlen(json)>=PET_APP_SAVE_BYTES)return false;
  xSemaphoreTake(lock,portMAX_DELAY);int i=domain?lookup_slot(domain,id):find_slot(id);bool ok=i>=0&&!saves[i].load_failed;
  if(ok&&require_loaded)ok=saves[i].loaded&&!saves[i].loading&&saves[i].domain==domain&&!strcmp(saves[i].id,id);
  if(ok&&strcmp(saves[i].json,json)){
    save_t *s=&saves[i];s->domain=domain;snprintf(s->id,48,"%s",id);snprintf(s->json,sizeof(s->json),"%s",json);s->seq++;if(!s->seq)s->seq=1;
    if (!s->dirty) s->dirty_since = now_ms();
    s->loaded=true;s->dirty=true;s->deleted=false;
  }xSemaphoreGive(lock);return ok;
}
bool pet_app_store_update(const char *id,const char *json){return update_in_domain(0,id,json,false);}
pet_app_store_status_t pet_app_store_load_scoped(uint32_t domain,const char *id,char out[PET_APP_SAVE_BYTES]){
  if(!domain)return PET_APP_STORE_INVALID;
  return load_with_authority(domain,id,out,NULL);
}
pet_app_store_status_t pet_app_store_load_scoped_with_lease(uint32_t domain,const char *id,char out[PET_APP_SAVE_BYTES],pet_resource_lease_t borrowed){
  if(!domain)return PET_APP_STORE_INVALID;
  return load_with_authority(domain,id,out,&borrowed);
}
bool pet_app_store_update_scoped(uint32_t domain,const char *id,const char *json){
  return domain&&update_in_domain(domain,id,json,true);
}
pet_app_store_status_t pet_app_store_read_scoped_cached(uint32_t domain,const char *id,char out[PET_APP_SAVE_BYTES]){
  if(!lock||!domain||!valid_id(id)||!out)return PET_APP_STORE_INVALID;
  out[0]=0;xSemaphoreTake(lock,portMAX_DELAY);int i=lookup_slot(domain,id);
  pet_app_store_status_t result=PET_APP_STORE_UNAVAILABLE;
  if(i>=0){const save_t *s=&saves[i];
    if(s->loading)result=PET_APP_STORE_BUSY;
    else if(s->loaded&&!s->load_failed){
      result=s->deleted||!s->json[0]?PET_APP_STORE_EMPTY:PET_APP_STORE_VALUE;
      if(result==PET_APP_STORE_VALUE)memcpy(out,s->json,PET_APP_SAVE_BYTES);
    }
  }
  xSemaphoreGive(lock);return result;
}
static bool pin_alias(const void *p,size_t n){
  uintptr_t x=(uintptr_t)p,y=(uintptr_t)saves;
  return x<=y?y-x<n:x-y<sizeof(saves);
}
bool pet_app_store_pin_scoped(uint32_t domain,const char *id,const void *owner,pet_app_store_pin_t *out){
  if(!lock||!domain||!valid_id(id)||!owner||!out||out->owner||out->serial||out->slot||pin_alias(out,sizeof(*out)))return false;
  xSemaphoreTake(lock,portMAX_DELAY);int i=lookup_slot(domain,id);bool ok=false;
  if(i>=SLOTS&&next_pin_serial!=UINT64_MAX){
    save_t *s=&saves[i];
    if(s->loaded&&!s->loading&&!s->load_failed&&!s->deleted&&!s->pin_serial){
      s->pin_serial=++next_pin_serial;s->pin_owner=owner;
      *out=(pet_app_store_pin_t){owner,s->pin_serial,(uint8_t)i};ok=true;
    }
  }
  xSemaphoreGive(lock);return ok;
}
bool pet_app_store_unpin(pet_app_store_pin_t *pin){
  if(!lock||!pin||!pin->owner||!pin->serial||pin->slot<SLOTS||pin->slot>=SLOTS+SCOPED_SLOTS||pin_alias(pin,sizeof(*pin)))return false;
  xSemaphoreTake(lock,portMAX_DELAY);save_t *s=&saves[pin->slot];
  bool ok=s->pin_serial==pin->serial&&s->pin_owner==pin->owner;
  if(ok){s->pin_serial=0;s->pin_owner=NULL;*pin=(pet_app_store_pin_t){0};}
  xSemaphoreGive(lock);return ok;
}
pet_app_checkpoint_status_t pet_app_store_checkpoint_scoped(uint32_t domain,const char *id,const char *expected){
  if(!lock||!domain||!valid_id(id)||!expected||!expected[0]||strlen(expected)>=PET_APP_SAVE_BYTES)return PET_APP_CHECKPOINT_INVALID;
  xSemaphoreTake(lock,portMAX_DELAY);int i=lookup_slot(domain,id);
  /* An uncached key may already exist on disk: this RAM-only query cannot
   * prove absence. Only an authoritative loaded empty/tombstone is ABSENT. */
  pet_app_checkpoint_status_t status=PET_APP_CHECKPOINT_BLOCKED;
  if(i>=0&&saves[i].domain==domain&&!strcmp(saves[i].id,id)){
    save_t *s=&saves[i];
    if(s->loading||s->load_failed||!s->loaded)status=PET_APP_CHECKPOINT_BLOCKED;
    else if(s->deleted||!s->json[0])status=PET_APP_CHECKPOINT_ABSENT;
    else if(strcmp(s->json,expected))status=PET_APP_CHECKPOINT_SUPERSEDED;
    else status=s->dirty?PET_APP_CHECKPOINT_PENDING:PET_APP_CHECKPOINT_DURABLE;
  }
  xSemaphoreGive(lock);return status;
}
void pet_app_store_forget(const char *id){
  if (!lock || !valid_id(id)) return;
  xSemaphoreTake(lock,portMAX_DELAY);int i=find_slot(id);
  if(i>=0){save_t *s=&saves[i];snprintf(s->id,48,"%s",id);s->json[0]=0;s->seq++;s->loaded=true;s->dirty=true;s->deleted=true;s->epoch_pending=true;s->forget_serial++;s->load_failed=false;s->dirty_since=0;}
  xSemaphoreGive(lock);
}

/* This authority belongs to the already admitted storage worker, never an
 * input/render callback. Keep the independent deletion intent durable even
 * when physical old-save reclamation or later new snapshots are interrupted. */
bool pet_app_store_forget_with_lease(const char *id,pet_resource_lease_t lease){
  if(!lock||!valid_id(id)||lease.kind!=PET_RESOURCE_STORAGE_WORK||!pet_resources_device_current(lease))return false;
  pet_app_store_forget(id);
  xSemaphoreTake(lock,portMAX_DELAY);int i=lookup_slot(0,id);
  bool valid=i>=0&&saves[i].epoch_pending;uint32_t serial=valid?saves[i].forget_serial:0;
  xSemaphoreGive(lock);if(!valid)return false;
  save_io_t io __attribute__((cleanup(save_io_end)))={0};if(!save_io_begin(&io,&lease))return false;
  save_io_cache_begin(&io);uint32_t epoch=0;
  if(!write_forget_epoch(id,0,&epoch,lease))return false;
  xSemaphoreTake(lock,portMAX_DELAY);
  valid=saves[i].domain==0&&!strcmp(saves[i].id,id)&&saves[i].forget_serial==serial;
  if(valid){saves[i].epoch=epoch;saves[i].epoch_pending=false;}
  xSemaphoreGive(lock);return valid;
}
