/* Actual native adapter with passive SDK/RTOS stubs. RTC persistence is
 * simulated by resetting only DRAM variables, not a real reset/timing proof. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
static esp_app_desc_t app;
static esp_reset_reason_t why=ESP_RST_POWERON;
static uintptr_t task=1;
static bool in_isr,critical,cache_enabled=true,global_irq=true;
static uint32_t random_nonce=7,cycle;
const esp_app_desc_t *esp_app_get_description(void){return &app;}
esp_reset_reason_t esp_reset_reason(void){return why;}
uint32_t esp_random(void){return ++random_nonce;}
uint32_t esp_cpu_get_cycle_count(void){return ++cycle;}
uint32_t resetphase_test_mstatus(void){return global_irq?(1U<<3):0;}
bool spi_flash_cache_enabled(void){assert(global_irq);return cache_enabled;}
int xPortInIsrContext(void){assert(cache_enabled&&global_irq);return in_isr;}
int xPortCanYield(void){return !critical;}
unsigned xPortGetCoreID(void){return task==2?1:0;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){assert(cache_enabled&&global_irq&&!in_isr&&!critical);return (TaskHandle_t)task;}
/* Include the ACTUAL adapter to simulate BSS reset without adding a reset or
 * private inspection hook to production. The pure RTC codec is linked whole. */
#include "pet_p4_reset_phase.c"
static void reset_dram_only(void){
  atomic_store(&ready,false);atomic_store(&errors,0);nonce=0;
  memset(&previous,0,sizeof(previous));memset(sequences,0,sizeof(sequences));
  memset(phases,0,sizeof(phases));memset(depths,0,sizeof(depths));
  memset(next_scope,0,sizeof(next_scope));memset(top_scope,0,sizeof(top_scope));
  for(unsigned i=0;i<4;i++)atomic_store(&owners[i],0);
}
int main(void){
  for(unsigned i=0;i<32;i++)app.app_elf_sha256[i]=(uint8_t)(i+1);
  pet_reset_phase_boot();assert(atomic_load(&ready)&&atomic_load(&owners[0])==1);
  pet_reset_record_snapshot_t snap;uint32_t flags;
  assert(pet_reset_phase_previous(&snap,&flags)&&!snap.header_valid&&!flags);
  uint32_t seq=sequences[0];pet_reset_phase_boot();assert(sequences[0]==seq);
  pet_reset_phase_set(PET_PHASE_IDLE);
  pet_reset_phase_scope_t outer=pet_reset_phase_enter(PET_PHASE_STATE_WAIT);
  assert(outer.active&&depths[0]==1&&phases[0]==PET_PHASE_STATE_WAIT);
  pet_reset_phase_set(PET_PHASE_STATE_HELD);
  pet_reset_phase_scope_t inner=pet_reset_phase_enter(PET_PHASE_PROGRAM_PUBLICATION);
  assert(inner.active&&depths[0]==2);
  pet_reset_phase_leave(&inner);assert(depths[0]==1&&phases[0]==PET_PHASE_STATE_HELD);
  pet_reset_phase_scope_t stale=outer;pet_reset_phase_leave(&outer);
  assert(!depths[0]&&phases[0]==PET_PHASE_IDLE);
  pet_reset_phase_scope_t next=pet_reset_phase_enter(PET_PHASE_SAVE);
  seq=sequences[0];pet_reset_phase_leave(&stale);assert(sequences[0]==seq&&depths[0]==1&&atomic_load(&errors));
  pet_reset_phase_leave(&next);assert(!depths[0]&&phases[0]==PET_PHASE_IDLE);
  task=2;assert(pet_reset_phase_bind(PET_RESET_PROTOCOL));assert(pet_reset_phase_bind(PET_RESET_PROTOCOL));
  assert(!pet_reset_phase_bind(PET_RESET_SAVE));
  pet_reset_record_storage_t copy=retained;
  task=3;assert(!pet_reset_phase_enter(PET_PHASE_RENDER).active);pet_reset_phase_set(PET_PHASE_RENDER);
  task=0;assert(!pet_reset_phase_enter(PET_PHASE_RENDER).active);pet_reset_phase_set(PET_PHASE_RENDER);
  task=1;in_isr=true;assert(!pet_reset_phase_enter(PET_PHASE_RENDER).active);pet_reset_phase_set(PET_PHASE_RENDER);
  in_isr=false;critical=true;assert(!pet_reset_phase_enter(PET_PHASE_RENDER).active);pet_reset_phase_set(PET_PHASE_RENDER);critical=false;
  /* Native callbacks can mask global IRQ without a CLIC critical threshold.
   * The IRAM gate must stop before task/owner access and every RTC store. */
  global_irq=false;assert(!pet_reset_phase_enter(PET_PHASE_RENDER).active);pet_reset_phase_set(PET_PHASE_RENDER);assert(!pet_reset_phase_bind(PET_RESET_SAVE));global_irq=true;
  cache_enabled=false;assert(!pet_reset_phase_enter(PET_PHASE_RENDER).active);pet_reset_phase_set(PET_PHASE_RENDER);assert(!pet_reset_phase_bind(PET_RESET_SAVE));cache_enabled=true;
  assert(!memcmp(&copy,&retained,sizeof(copy)));
  assert(!pet_reset_phase_enter((pet_reset_phase_t)-1).active);
  pet_reset_phase_scope_t stack[8];for(unsigned i=0;i<8;i++){stack[i]=pet_reset_phase_enter(PET_PHASE_FILE_DATA);assert(stack[i].active);}
  assert(!pet_reset_phase_enter(PET_PHASE_FILE_SYNC).active);
  for(unsigned i=8;i>0;i--)pet_reset_phase_leave(&stack[i-1]);
  assert(!depths[0]);
  /* Simulate a reset while publication was in progress. No watchdog is fired. */
  pet_reset_phase_scope_t unfinished=pet_reset_phase_enter(PET_PHASE_PROGRAM_PUBLICATION);assert(unfinished.active);
  uint32_t prior_nonce=nonce;reset_dram_only();why=ESP_RST_WDT;pet_reset_phase_boot();
  assert(nonce!=prior_nonce&&pet_reset_phase_previous(&snap,&flags)&&snap.header_valid&&snap.same_build);
  assert((snap.valid_mask&1)&&((snap.records[0].phase_core&255)==PET_PHASE_PROGRAM_PUBLICATION));
  assert(!pet_reset_phase_previous(&previous,&flags));
  assert(!pet_reset_phase_previous(&snap,(uint32_t*)&snap));
  assert(!pet_reset_phase_previous((void*)&retained,&flags));
  assert(!pet_reset_phase_previous(&snap,(uint32_t*)&previous));
  app.app_elf_sha256[3]^=1;reset_dram_only();why=ESP_RST_SW;pet_reset_phase_boot();
  assert(pet_reset_phase_previous(&snap,&flags)&&snap.header_valid&&!snap.same_build&&!snap.valid_mask);
  reset_dram_only();why=ESP_RST_POWERON;pet_reset_phase_boot();
  assert(pet_reset_phase_previous(&snap,&flags)&&!snap.header_valid&&!snap.valid_mask);
  sequences[0]=UINT32_MAX;seq=sequences[0];pet_reset_phase_set(PET_PHASE_FILE_DATA);
  assert(sequences[0]==seq&&(atomic_load(&errors)&1));
  next_scope[0]=UINT32_MAX;assert(!pet_reset_phase_enter(PET_PHASE_FILE_SYNC).active);
  puts("native phase ownership/nesting/stale/default-context/boot cases passed");return 0;
}
