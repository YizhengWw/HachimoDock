#include "pet_p4_storage_io_trace.h"
#include "pet_p4_reset_phase.h"
#if PET_P4_STORAGE_IO_TRACE_ENABLE
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spiffs.h"
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#ifndef PET_STORAGE_IO_HOST_TEST
#include "esp_idf_version.h"
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || (ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1) && \
    !(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4) && \
      CONFIG_ESP_REV_MIN_FULL == 300 && CONFIG_ESP_REV_MAX_FULL == 399 && !CONFIG_SECURE_ENABLE_TEE))
#error "Audit real SPIFFS HAL callbacks and link redirection on a new SDK"
#endif
#if ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4)
_Static_assert(sizeof(void *) == 4 && sizeof(s32_t) == 4 && sizeof(u32_t) == 4, "review RV32 SPIFFS HAL ABI");
_Static_assert(SPIFFS_HAL_CALLBACK_EXTRA == 1, "HAL must carry the live filesystem owner");
_Static_assert(__builtin_types_compatible_p(spiffs_read, s32_t (*)(spiffs *, u32_t, u32_t, u8_t *)), "review SPIFFS read callback");
_Static_assert(__builtin_types_compatible_p(spiffs_write, s32_t (*)(spiffs *, u32_t, u32_t, u8_t *)), "review SPIFFS write callback");
_Static_assert(__builtin_types_compatible_p(spiffs_erase, s32_t (*)(spiffs *, u32_t, u32_t)), "review SPIFFS erase callback");
#endif
#endif
static DRAM_ATTR portMUX_TYPE io_mux=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR _Atomic uint32_t active_generation;
#ifndef PET_STORAGE_IO_HOST_TEST
static DRAM_ATTR _Atomic uintptr_t cooperative_owner;
static int64_t cooperative_last_us;
void pet_storage_io_protocol_window(bool enabled) {
  uintptr_t task=(uintptr_t)xTaskGetCurrentTaskHandle();
  if(enabled){
    cooperative_last_us=esp_timer_get_time();
    atomic_store_explicit(&cooperative_owner,task,memory_order_release);
  }else{
    (void)atomic_compare_exchange_strong_explicit(&cooperative_owner,&task,0,
      memory_order_release,memory_order_relaxed);
  }
}
static void protocol_io_cooperate(void) {
  if(atomic_load_explicit(&cooperative_owner,memory_order_acquire)
      !=(uintptr_t)xTaskGetCurrentTaskHandle())return;
  int64_t now=esp_timer_get_time();
  if(now>=cooperative_last_us&&now-cooperative_last_us>=10000
      &&!xPortInIsrContext()&&xPortCanYield()){
    cooperative_last_us=now;
    /* All native HAL work/cache invalidation and observer critical sections
     * have RETURNED. SPIFFS retains its ordinary file mutex, not an IRQ lock;
     * its current call/GC finishes normally without injected errors or replay. */
    vTaskDelay(1);
  }
}
#else
void pet_storage_io_protocol_window(bool enabled){(void)enabled;}
static void protocol_io_cooperate(void){}
#endif

/* Zero-initialized internal SRAM, without a flash-backed DRAM data payload. */
static pet_storage_io_status_t io_status;
/* Standard BSS is internal RAM (not PSRAM): keep the same zeroed counters
 * without storing their all-zero initializer in the flash data image. */
static pet_storage_io_counter_t counters[4][3];
static DRAM_ATTR void *tasks[3];
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
typedef struct {pet_storage_io_status_t status;pet_storage_io_counter_t counters[4][3];} completed_io_t;
/* Standard BSS stays in internal RAM, not PSRAM. Preserve the zeroed
 * completed observations without storing 1848 zero bytes in flash data. */
static completed_io_t completed_io[3];
_Static_assert(sizeof(completed_io)<=2048,"bounded numeric completed observations");
static void completed_publish_locked(void){
  if(!io_status.complete||io_status.active||io_status.inflight||io_status.owner_actor<1||io_status.owner_actor>3)return;
  completed_io_t *out=&completed_io[io_status.owner_actor-1];
  out->status=io_status;memcpy(out->counters,counters,sizeof(counters));
}
#endif
_Static_assert(sizeof(counters)<=640,"bounded private metadata reset, no trace ring");
typedef struct {uint64_t started,mutation;uint32_t generation,size;uint8_t actor,operation,bucket;bool physical;} io_ticket_t;
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
static DRAM_ATTR pet_storage_physical_scope_t *physical_scope;
static DRAM_ATTR void *physical_task;
bool pet_storage_physical_begin(pet_storage_physical_scope_t *scope){
  if(!scope||scope->self||scope->task)return false;
  void *task=xTaskGetCurrentTaskHandle();if(!task)return false;
  bool ok=false;portENTER_CRITICAL(&io_mux);
  if(!physical_scope){
    *scope=(pet_storage_physical_scope_t){.self=scope,.task=task};
    physical_scope=scope;physical_task=task;ok=true;
  }
  portEXIT_CRITICAL(&io_mux);return ok;
}
bool pet_storage_physical_finish(pet_storage_physical_scope_t *scope){
  if(!scope||scope->self!=scope||scope->task!=xTaskGetCurrentTaskHandle())return false;
  bool ok=false;portENTER_CRITICAL(&io_mux);
  if(physical_scope==scope&&physical_task==scope->task){
    physical_scope=NULL;physical_task=NULL;*scope=(pet_storage_physical_scope_t){0};ok=true;
  }
  portEXIT_CRITICAL(&io_mux);return ok;
}
#endif
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
/* Compressed SUCCESSFUL exact 512-byte reads: constant prefix + exact tail.
 * No filename or existence result. Same allocation in observe/apply control. Window epochs
 * avoid clearing the table with interrupts disabled. Only trusted SDK HAL
 * sees these buffers; no cache operation is a component permission. */
enum {UNIFORM_SETS=1024,UNIFORM_WAYS=2,UNIFORM_BYTES=512};
typedef struct {uint32_t address,generation;uint8_t tail[4],value;} uniform_entry_t;
typedef struct {uint8_t value,tail[4];} uniform_bytes_t;
static DRAM_ATTR uniform_entry_t uniform_entries[UNIFORM_SETS][UNIFORM_WAYS] __attribute__((used));
static DRAM_ATTR uint8_t uniform_replace[UNIFORM_SETS];
static DRAM_ATTR uint64_t uniform_mutation;
static DRAM_ATTR uint32_t uniform_disabled_generation;
static DRAM_ATTR spiffs *uniform_owner;
static DRAM_ATTR uint32_t uniform_owner_generation;
#ifndef PET_STORAGE_IO_HOST_TEST
_Static_assert(sizeof(uniform_entries)+sizeof(uniform_replace)<=43008,"private compressed read cache RAM bound");
#endif
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
enum {PAGE_CACHE_SETS=1024,PAGE_CACHE_WAYS=2,PAGE_CACHE_BYTES=512,PAGE_CACHE_SLOTS=2048};
#include "pet_p4_storage_page_entry.h"
/* Standard .bss is internal SRAM NOLOAD in both P4 SDK linker profiles,
 * not .ext_ram.bss/PSRAM. Keep zero-initialized metadata out of flash .data;
 * capacity, startup zero values and cache access/locking stay unchanged. */
static page_cache_entry_t page_entries[PAGE_CACHE_SETS][PAGE_CACHE_WAYS] __attribute__((used));
static uint8_t page_replace[PAGE_CACHE_SETS];
static EXT_RAM_BSS_ATTR uint8_t page_bytes[PAGE_CACHE_SLOTS][PAGE_CACHE_BYTES] __attribute__((used));
static DRAM_ATTR uint64_t page_mutation;
static DRAM_ATTR uint32_t page_disabled_generation,page_owner_generation;
static DRAM_ATTR spiffs *page_owner;
_Static_assert(sizeof(page_entries)+sizeof(page_replace)==17408,"private full-page metadata budget");
#endif
static bool overlaps(const void *a,size_t an,const void *b,size_t bn){uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;}
static bool output_external(const void *out,size_t n){
  return out&&!overlaps(out,n,&io_status,sizeof(io_status))&&!overlaps(out,n,counters,sizeof(counters))
    &&!overlaps(out,n,tasks,sizeof(tasks))&&!overlaps(out,n,&io_mux,sizeof(io_mux))&&!overlaps(out,n,&active_generation,sizeof(active_generation))
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
    &&!overlaps(out,n,completed_io,sizeof(completed_io))
#endif
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
    &&!overlaps(out,n,&physical_scope,sizeof(physical_scope))&&!overlaps(out,n,&physical_task,sizeof(physical_task))
#endif
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
    &&!overlaps(out,n,uniform_entries,sizeof(uniform_entries))&&!overlaps(out,n,uniform_replace,sizeof(uniform_replace))
    &&!overlaps(out,n,&uniform_mutation,sizeof(uniform_mutation))&&!overlaps(out,n,&uniform_disabled_generation,sizeof(uniform_disabled_generation))
    &&!overlaps(out,n,&uniform_owner,sizeof(uniform_owner))&&!overlaps(out,n,&uniform_owner_generation,sizeof(uniform_owner_generation))
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
    &&!overlaps(out,n,page_entries,sizeof(page_entries))&&!overlaps(out,n,page_replace,sizeof(page_replace))
    &&!overlaps(out,n,page_bytes,sizeof(page_bytes))&&!overlaps(out,n,&page_mutation,sizeof(page_mutation))
    &&!overlaps(out,n,&page_disabled_generation,sizeof(page_disabled_generation))&&!overlaps(out,n,&page_owner_generation,sizeof(page_owner_generation))
    &&!overlaps(out,n,&page_owner,sizeof(page_owner))
#endif
    ;
}
static void increment(uint32_t *n){if(*n<UINT32_MAX)(*n)++;else io_status.flags|=PET_STORAGE_IO_OVERFLOW;}
static void add(uint64_t *n,uint64_t value){if(UINT64_MAX-*n<value){*n=UINT64_MAX;io_status.flags|=PET_STORAGE_IO_OVERFLOW;}else *n+=value;}
static bool ready_locked(void){return tasks[0]&&tasks[1]&&tasks[2];}
static void close_locked(uint64_t now,uint32_t flag){
  io_status.active=false;io_status.stop_requested_us=now;io_status.flags|=flag;
  io_status.complete=io_status.inflight==0;
  atomic_store_explicit(&active_generation,0,memory_order_release);
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
  completed_publish_locked();
#endif
}
static void expire_locked(int64_t now){
  if(!io_status.active)return;
  if(now<0||(uint64_t)now<io_status.started_us)close_locked(io_status.started_us,PET_STORAGE_IO_BAD_CLOCK);
  else if((uint64_t)now-io_status.started_us>=io_status.duration_us)close_locked((uint64_t)now,PET_STORAGE_IO_EXPIRED);
}
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
#include "pet_p4_storage_uniform_cache.inc"
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
#include "pet_p4_storage_page_cache.inc"
#endif
bool pet_storage_io_register(pet_storage_io_actor_t actor,void *task){
  if(actor<PET_STORAGE_IO_MAIN||actor>PET_STORAGE_IO_PROTOCOL||!task)return false;
  bool ok=false;portENTER_CRITICAL(&io_mux);
  if(!io_status.active&&!io_status.inflight){
    ok=true;for(unsigned i=0;i<3;i++)if(i!=(unsigned)actor-1&&tasks[i]==task)ok=false;
    if(ok)tasks[actor-1]=task;
  }
  portEXIT_CRITICAL(&io_mux);return ok;
}
bool pet_storage_io_ready(void){bool ok;portENTER_CRITICAL(&io_mux);ok=ready_locked();portEXIT_CRITICAL(&io_mux);return ok;}
uint32_t pet_storage_io_begin(uint32_t duration){
  if(!duration||duration>PET_STORAGE_IO_MAX_WINDOW_US)return 0;
  int saved=errno;int64_t now=esp_timer_get_time();errno=saved;if(now<0)return 0;
  void *task=xTaskGetCurrentTaskHandle();errno=saved;
  uint32_t generation=0;portENTER_CRITICAL(&io_mux);
  if(ready_locked()&&!io_status.active&&!io_status.inflight&&io_status.generation<UINT32_MAX){
    generation=io_status.generation+1;
    memset(counters,0,sizeof(counters));
    unsigned owner=PET_STORAGE_IO_OTHER;
    for(unsigned i=0;i<3;i++)if(tasks[i]==task){owner=i+1;break;}
    io_status=(pet_storage_io_status_t){.started_us=(uint64_t)now,.generation=generation,.duration_us=duration,.active=true,.owner_actor=(uint8_t)owner};
    atomic_store_explicit(&active_generation,generation,memory_order_release);
  }
  portEXIT_CRITICAL(&io_mux);return generation;
}
bool pet_storage_io_stop(uint32_t generation){
  if(!generation)return false;int saved=errno;int64_t now=esp_timer_get_time();errno=saved;
  bool ok=false;portENTER_CRITICAL(&io_mux);
  if(io_status.generation==generation){
    ok=true;expire_locked(now);
    if(io_status.active)close_locked((uint64_t)now,0);
  }
  portEXIT_CRITICAL(&io_mux);return ok;
}
bool pet_storage_io_status(uint32_t generation,pet_storage_io_status_t *out){
  if(!generation||!output_external(out,sizeof(*out)))return false;int saved=errno;int64_t now=esp_timer_get_time();errno=saved;
  bool ok=false;pet_storage_io_status_t copy;portENTER_CRITICAL(&io_mux);
  if(io_status.generation==generation){expire_locked(now);copy=io_status;ok=true;}
  portEXIT_CRITICAL(&io_mux);if(ok)*out=copy;return ok;
}
bool pet_storage_io_latest(pet_storage_io_status_t *out){
  if(!output_external(out,sizeof(*out)))return false;int saved=errno;int64_t now=esp_timer_get_time();errno=saved;
  bool ok=false;pet_storage_io_status_t copy;portENTER_CRITICAL(&io_mux);
  if(io_status.generation){expire_locked(now);copy=io_status;ok=true;}
  portEXIT_CRITICAL(&io_mux);if(ok)*out=copy;return ok;
}
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
bool pet_storage_io_completed_read(pet_storage_io_actor_t owner,uint32_t expected_generation,
    pet_storage_io_actor_t actor,pet_storage_io_status_t *status,pet_storage_io_counter_t out[3]){
  if(owner<PET_STORAGE_IO_MAIN||owner>PET_STORAGE_IO_PROTOCOL||actor<PET_STORAGE_IO_MAIN||actor>PET_STORAGE_IO_OTHER
      ||!output_external(status,sizeof(*status))||!output_external(out,3*sizeof(*out))
      ||overlaps(status,sizeof(*status),out,3*sizeof(*out)))return false;
  pet_storage_io_status_t observed;pet_storage_io_counter_t result[3];bool ok=false;
  portENTER_CRITICAL(&io_mux);const completed_io_t *last=&completed_io[owner-1];
  if(last->status.generation&&(!expected_generation||last->status.generation==expected_generation)
      &&last->status.complete&&!last->status.active&&!last->status.inflight){
    observed=last->status;memcpy(result,last->counters[actor-1],sizeof(result));ok=true;
  }
  portEXIT_CRITICAL(&io_mux);
  if(ok){*status=observed;memcpy(out,result,sizeof(result));}return ok;
}
#endif
bool pet_storage_io_read(uint32_t generation,pet_storage_io_actor_t actor,pet_storage_io_operation_t operation,pet_storage_io_counter_t *out){
  if(!generation||!output_external(out,sizeof(*out))||actor<PET_STORAGE_IO_MAIN||actor>PET_STORAGE_IO_OTHER||operation<PET_STORAGE_IO_READ||operation>PET_STORAGE_IO_ERASE)return false;
  bool ok=false;pet_storage_io_counter_t copy;portENTER_CRITICAL(&io_mux);
  if(io_status.generation==generation&&io_status.complete&&!io_status.active&&!io_status.inflight){copy=counters[actor-1][operation];ok=true;}
  portEXIT_CRITICAL(&io_mux);if(ok)*out=copy;return ok;
}
static io_ticket_t observe_begin(pet_storage_io_operation_t operation,uint32_t size){
  io_ticket_t ticket={0};uint32_t generation=atomic_load_explicit(&active_generation,memory_order_acquire);
  if(!generation)return ticket;
  int64_t now=esp_timer_get_time();void *task=xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&io_mux);expire_locked(now);
  if(io_status.active&&io_status.generation==generation&&io_status.inflight<UINT32_MAX){
    unsigned actor=PET_STORAGE_IO_OTHER;
    for(unsigned i=0;i<3;i++)if(tasks[i]==task){actor=i+1;break;}
    ticket=(io_ticket_t){.started=(uint64_t)now,.generation=generation,.size=size,.actor=(uint8_t)(actor-1),
      .operation=(uint8_t)operation,.bucket=(uint8_t)(size<=16?0:size<=256?1:size<=4096?2:3)};
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
    ticket.physical=physical_scope&&physical_task==task;
#endif
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
    ticket.mutation=uniform_mutation;
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
    ticket.mutation=page_mutation;
#endif
    io_status.inflight++;
  }
  portEXIT_CRITICAL(&io_mux);return ticket;
}
static void observe_end(io_ticket_t ticket,s32_t result,bool cached){
  if(!ticket.generation)return;int64_t now=esp_timer_get_time();
  portENTER_CRITICAL(&io_mux);
  if(io_status.generation==ticket.generation){
    expire_locked(now);pet_storage_io_counter_t *c=&counters[ticket.actor][ticket.operation];
    if(cached)increment(&c->cache_hits);
    else{increment(&c->calls);if(result!=0)increment(&c->errors);increment(&c->buckets[ticket.bucket]);add(&c->bytes,ticket.size);}
    if(now<0||(uint64_t)now<ticket.started)io_status.flags|=PET_STORAGE_IO_BAD_CLOCK;
    else if(!cached){uint64_t elapsed=(uint64_t)now-ticket.started;add(&c->wall_us,elapsed);uint32_t bounded=elapsed>UINT32_MAX?UINT32_MAX:(uint32_t)elapsed;if(bounded>c->max_wall_us)c->max_wall_us=bounded;}
    if(!io_status.active)io_status.flags|=PET_STORAGE_IO_LATE_RETURN;
    if(io_status.inflight)io_status.inflight--;else io_status.flags|=PET_STORAGE_IO_OVERFLOW;
    io_status.complete=!io_status.active&&!io_status.inflight;
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
    completed_publish_locked();
#endif
  }
  portEXIT_CRITICAL(&io_mux);
}
s32_t __real_spiffs_api_read(spiffs *fs,uint32_t address,uint32_t size,uint8_t *buffer);
s32_t __real_spiffs_api_write(spiffs *fs,uint32_t address,uint32_t size,uint8_t *buffer);
s32_t __real_spiffs_api_erase(spiffs *fs,uint32_t address,uint32_t size);
#ifndef PET_P4_RESET_PHASE_CACHE_HITS
#define PET_P4_RESET_PHASE_CACHE_HITS 1
#endif
s32_t __wrap_spiffs_api_read(spiffs *fs,uint32_t address,uint32_t size,uint8_t *buffer){
#if PET_P4_RESET_PHASE_TRACE && PET_P4_RESET_PHASE_CACHE_HITS
  PET_RESET_PHASE_SCOPE(hal_phase,PET_PHASE_HAL_READ_META);
#endif
  int saved=errno;io_ticket_t t=observe_begin(PET_STORAGE_IO_READ,size);errno=saved;
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
  uniform_bytes_t value={0};
  if(uniform_get(t,fs,address,size,&value)){
    memset(buffer,value.value,size-4);memcpy(buffer+size-4,value.tail,4);observe_end(t,0,true);protocol_io_cooperate();errno=saved;return 0;
  }
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
  if(page_read(t,fs,address,size,buffer)){observe_end(t,0,true);protocol_io_cooperate();errno=saved;return 0;}
#endif
#if PET_P4_RESET_PHASE_TRACE && !PET_P4_RESET_PHASE_CACHE_HITS
  /* Cache-hit software copies keep the caller's existing FILE_DATA/SYNC
   * phase. Only actual physical reads/fills need the finer native boundary.
   * Never weaken byte validity, invalidation, codec checks or failure paths. */
  PET_RESET_PHASE_SCOPE(hal_phase,PET_PHASE_HAL_READ_META);
#endif
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_HAL_READ_NATIVE);
#endif
  errno=saved;s32_t result=__real_spiffs_api_read(fs,address,size,buffer);saved=errno;
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_HAL_READ_META);
#endif
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
  if(result==0)uniform_fill(t,fs,address,size,buffer);
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
  if(result==0)page_fill(t,fs,address,size,buffer);
#endif
  observe_end(t,result,false);protocol_io_cooperate();errno=saved;return result;
}
s32_t __wrap_spiffs_api_write(spiffs *fs,uint32_t address,uint32_t size,uint8_t *buffer){
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(hal_phase,PET_PHASE_HAL_WRITE_META);
#endif
  int saved=errno;io_ticket_t t=observe_begin(PET_STORAGE_IO_WRITE,size);errno=saved;
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
  uniform_invalidate(fs,address,size);
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
  page_invalidate(fs,address,size);
#endif
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_HAL_WRITE_NATIVE);
#endif
  errno=saved;s32_t result=__real_spiffs_api_write(fs,address,size,buffer);saved=errno;
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_HAL_WRITE_META);
#endif
  observe_end(t,result,false);protocol_io_cooperate();errno=saved;return result;
}
s32_t __wrap_spiffs_api_erase(spiffs *fs,uint32_t address,uint32_t size){
#if PET_P4_RESET_PHASE_TRACE
  PET_RESET_PHASE_SCOPE(hal_phase,PET_PHASE_HAL_ERASE_META);
#endif
  int saved=errno;io_ticket_t t=observe_begin(PET_STORAGE_IO_ERASE,size);errno=saved;
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
  uniform_invalidate(fs,address,size);
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE
  page_invalidate(fs,address,size);
#endif
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_HAL_ERASE_NATIVE);
#endif
  errno=saved;s32_t result=__real_spiffs_api_erase(fs,address,size);saved=errno;
#if PET_P4_RESET_PHASE_TRACE
  pet_reset_phase_set(PET_PHASE_HAL_ERASE_META);
#endif
  observe_end(t,result,false);protocol_io_cooperate();errno=saved;return result;
}
#endif
