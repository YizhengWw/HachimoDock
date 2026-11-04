#include "pet_p4_reset_phase.h"
#if PET_P4_RESET_PHASE_TRACE
#include <stdatomic.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_private/cache_utils.h"
#include "riscv/rv_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || (ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5,5,1) && \
    !(ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4) && \
      CONFIG_ESP_REV_MIN_FULL == 300 && CONFIG_ESP_REV_MAX_FULL == 399 && !CONFIG_SECURE_ENABLE_TEE))
#error "Review RTC placement/retention and task/cycle ABI before another SDK/target"
#endif
#if CONFIG_SECURE_ENABLE_TEE
#error "Review native interrupt-state observation for non-M-mode execution first"
#endif
/* Exact v3 SDK contract, not a broad newer-IDF admission. Opaque task handles
 * and the app digest layout are the only SDK object fields used here. */
#if ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(5,5,4)
#include <stddef.h>
_Static_assert(sizeof(void *) == 4 && sizeof(TaskHandle_t) == 4, "review RV32 task ABI");
_Static_assert(sizeof(esp_app_desc_t) == 256 && offsetof(esp_app_desc_t, app_elf_sha256) == 144
    && sizeof(((esp_app_desc_t *)0)->app_elf_sha256) == 32, "review app identity ABI");
_Static_assert(__builtin_types_compatible_p(__typeof__(&xTaskGetCurrentTaskHandle), TaskHandle_t (*)(void)), "review task accessor signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&spi_flash_cache_enabled), bool (*)(void)), "review cache accessor signature");
#endif
_Static_assert(ATOMIC_POINTER_LOCK_FREE==2,"phase ownership must not call locks");
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"phase flags must not call locks");
_Static_assert(PET_RESET_ACTORS==4,"fixed native actor ABI");
static RTC_NOINIT_ATTR pet_reset_record_storage_t retained;
static DRAM_ATTR pet_reset_record_snapshot_t previous;
static DRAM_ATTR _Atomic(uintptr_t) owners[PET_RESET_ACTORS];
static DRAM_ATTR _Atomic bool ready;
static DRAM_ATTR _Atomic uint32_t errors;
static DRAM_ATTR uint32_t sequences[PET_RESET_ACTORS],nonce;
static DRAM_ATTR uint32_t next_scope[PET_RESET_ACTORS],top_scope[PET_RESET_ACTORS];
static DRAM_ATTR uint8_t phases[PET_RESET_ACTORS],depths[PET_RESET_ACTORS];
static bool phase_overlap(const void *a,size_t an,const void *b,size_t bn){
  uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}

/* The gate itself must be fetchable before querying a potentially cache-off
 * native callback. CLIC threshold alone does not test global MIE. These SDK
 * cache/context helpers and all steady recorder callees are IRAM/ROM checked
 * in the actual private ELF; mutable state stays in internal DRAM/RTC. */
static bool IRAM_ATTR safe_context(void){
  return (RV_READ_CSR(mstatus)&MSTATUS_MIE)!=0&&spi_flash_cache_enabled()
    &&!xPortInIsrContext()&&xPortCanYield();
}
static unsigned IRAM_ATTR actor_for(uintptr_t owner){
  for(unsigned i=0;i<PET_RESET_ACTORS;i++)if(atomic_load_explicit(&owners[i],memory_order_acquire)==owner)return i;
  return PET_RESET_ACTORS;
}
static void IRAM_ATTR mark(unsigned actor,pet_reset_phase_t phase){
  if(actor>=PET_RESET_ACTORS||(unsigned)phase>=PET_PHASE_COUNT)return;
  phases[actor]=(uint8_t)phase;
  if(sequences[actor]==UINT32_MAX){atomic_fetch_or_explicit(&errors,1U<<actor,memory_order_relaxed);return;}
  uint32_t sequence=++sequences[actor];
  if(!pet_reset_record_write(&retained,actor,nonce,sequence,(uint32_t)phase,
      (uint32_t)xPortGetCoreID(),esp_cpu_get_cycle_count()))
    atomic_fetch_or_explicit(&errors,1U<<actor,memory_order_relaxed);
}
void pet_reset_phase_boot(void){
  /* Native boot only, before creating other task owners. Never reset an
   * active recorder to hide a fault or rebuild a publication authority. */
  if(atomic_load_explicit(&ready,memory_order_acquire))return;
  const esp_app_desc_t *app=esp_app_get_description();if(!app)return;
  uint32_t build[8];memcpy(build,app->app_elf_sha256,sizeof(build));
  (void)pet_reset_record_observe(&retained,build,&previous);
  esp_reset_reason_t why=esp_reset_reason();
  if(why!=ESP_RST_SW&&why!=ESP_RST_PANIC&&why!=ESP_RST_INT_WDT&&why!=ESP_RST_TASK_WDT&&why!=ESP_RST_WDT)
    memset(&previous,0,sizeof(previous));
  nonce=esp_random();if(!nonce)nonce=1;
  if(!pet_reset_record_initialize(&retained,build,nonce))return;
  atomic_store_explicit(&ready,true,memory_order_release);
  if(!pet_reset_phase_bind(PET_RESET_MAIN))atomic_fetch_or_explicit(&errors,UINT32_C(0x40000000),memory_order_relaxed);
  pet_reset_phase_set(PET_PHASE_BOOT);
}
bool pet_reset_phase_bind(pet_reset_actor_t actor){
  if(!atomic_load_explicit(&ready,memory_order_acquire)||(unsigned)actor>=PET_RESET_ACTORS||!safe_context())return false;
  uintptr_t owner=(uintptr_t)xTaskGetCurrentTaskHandle();if(!owner)return false;
  unsigned present=actor_for(owner);
  if(present<PET_RESET_ACTORS)return present==(unsigned)actor;
  uintptr_t expected=0;
  if(!atomic_compare_exchange_strong_explicit(&owners[actor],&expected,owner,memory_order_release,memory_order_relaxed))return false;
  mark((unsigned)actor,PET_PHASE_IDLE);return true;
}
void IRAM_ATTR pet_reset_phase_set(pet_reset_phase_t phase){
  if(!atomic_load_explicit(&ready,memory_order_acquire)||!safe_context()||(unsigned)phase>=PET_PHASE_COUNT)return;
  uintptr_t owner=(uintptr_t)xTaskGetCurrentTaskHandle();if(!owner)return;
  unsigned actor=actor_for(owner);mark(actor,phase);
}
pet_reset_phase_scope_t IRAM_ATTR pet_reset_phase_enter(pet_reset_phase_t phase){
  pet_reset_phase_scope_t scope={0};
  if(!atomic_load_explicit(&ready,memory_order_acquire)||!safe_context()||(unsigned)phase>=PET_PHASE_COUNT)return scope;
  uintptr_t owner=(uintptr_t)xTaskGetCurrentTaskHandle();unsigned actor=actor_for(owner);
  if(!owner||actor>=PET_RESET_ACTORS)return scope;
  if(depths[actor]>=8||next_scope[actor]==UINT32_MAX){atomic_fetch_or_explicit(&errors,1U<<actor,memory_order_relaxed);return scope;}
  scope=(pet_reset_phase_scope_t){.owner=owner,.actor=(uint8_t)actor,.previous=phases[actor],.depth=++depths[actor],
    .serial=++next_scope[actor],.previous_serial=top_scope[actor],.active=true};
  top_scope[actor]=scope.serial;
  mark(actor,phase);return scope;
}
void IRAM_ATTR pet_reset_phase_leave(pet_reset_phase_scope_t *scope){
  if(!scope||!scope->active)return;
  if(!safe_context()||scope->actor>=PET_RESET_ACTORS||scope->owner!=(uintptr_t)xTaskGetCurrentTaskHandle()
      ||scope->depth!=depths[scope->actor]||scope->serial!=top_scope[scope->actor]){
    atomic_fetch_or_explicit(&errors,UINT32_C(0x80000000),memory_order_relaxed);return;
  }
  depths[scope->actor]--;top_scope[scope->actor]=scope->previous_serial;
  mark(scope->actor,(pet_reset_phase_t)scope->previous);scope->active=false;
}
bool pet_reset_phase_previous(pet_reset_record_snapshot_t *out,uint32_t *current_errors){
  if(!out||!current_errors||((uintptr_t)out&3U)||((uintptr_t)current_errors&3U)
      ||!atomic_load_explicit(&ready,memory_order_acquire))return false;
  if(phase_overlap(out,sizeof(*out),current_errors,sizeof(*current_errors))
      ||phase_overlap(out,sizeof(*out),&previous,sizeof(previous))
      ||phase_overlap(out,sizeof(*out),&retained,sizeof(retained))
      ||phase_overlap(current_errors,sizeof(*current_errors),&previous,sizeof(previous))
      ||phase_overlap(current_errors,sizeof(*current_errors),&retained,sizeof(retained)))return false;
  *out=previous;*current_errors=atomic_load_explicit(&errors,memory_order_relaxed);return true;
}
#endif
