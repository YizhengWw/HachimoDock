/* Actual native lifetime adapter, copied/stale/mixed-owner rejection. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "pet_p4_storage_cache_lease.h"
static bool current=true,ready=true,active;
static TaskHandle_t task=(TaskHandle_t)3;
static uint32_t next=1,generation,stops,starts;
bool pet_resources_device_current(pet_resource_lease_t lease){
  return current&&lease.serial==17&&lease.owner.generation==11;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return task;}
uint32_t pet_storage_io_begin(uint32_t duration){
  assert(duration==5000000);if(active||!ready)return 0;
  generation=next++;active=true;starts++;return generation;
}
bool pet_storage_io_stop(uint32_t value){
  stops++;if(value!=generation)return false;active=false;return true;
}
int main(void){
  pet_resource_lease_t lease={.kind=PET_RESOURCE_STORAGE_WORK,.units=1,.serial=17,.owner={.generation=11}};
  pet_storage_cache_lease_t live={0},empty={0};
  current=false;assert(!pet_storage_cache_lease_begin(&live,lease));current=true;
  lease.units=2;assert(!pet_storage_cache_lease_begin(&live,lease));lease.units=1;
  lease.kind=PET_RESOURCE_PROGRAM;assert(!pet_storage_cache_lease_begin(&live,lease));lease.kind=PET_RESOURCE_STORAGE_WORK;
  ready=false;assert(!pet_storage_cache_lease_begin(&live,lease));ready=true;
  task=0;assert(!pet_storage_cache_lease_begin(&live,lease));task=(TaskHandle_t)3;
  assert(!starts&&!stops&&!memcmp(&live,&empty,sizeof(live)));
  assert(pet_storage_cache_lease_begin(&live,lease)&&starts==1&&live.self==&live);
  pet_storage_cache_lease_t copy=live;
  assert(!pet_storage_cache_lease_begin(&live,lease)&&!pet_storage_cache_lease_finish(&copy));
  task=(TaskHandle_t)4;assert(!pet_storage_cache_lease_finish(&live)&&active&&!stops);task=(TaskHandle_t)3;
  current=false;assert(pet_storage_cache_lease_finish(&live)&&!active&&stops==1);current=true;
  assert(!pet_storage_cache_lease_finish(&copy)&&!pet_storage_cache_lease_finish(&live));
  /* Explicit diagnostic owns its generation; native IO is a plain bypass. */
  uint32_t manual=pet_storage_io_begin(5000000);assert(manual&&active);
  assert(!pet_storage_cache_lease_begin(&live,lease)&&!live.generation);
  assert(!pet_storage_cache_lease_finish(&live)&&active&&generation==manual);
  assert(pet_storage_io_stop(manual));
  assert(pet_storage_cache_lease_begin(&live,lease));copy=live;
  assert(pet_storage_io_stop(live.generation)); /* Exact own epoch expired. */
  manual=pet_storage_io_begin(5000000);assert(manual);
  assert(!pet_storage_cache_lease_finish(&live)&&active&&generation==manual);
  assert(!pet_storage_cache_lease_finish(&copy)&&active&&generation==manual);
  assert(!memcmp(&live,&empty,sizeof(live)));assert(pet_storage_io_stop(manual));
  for(unsigned i=0;i<10000;i++){
    assert(pet_storage_cache_lease_begin(&live,lease));copy=live;
    assert(pet_storage_cache_lease_finish(&live));assert(!pet_storage_cache_lease_finish(&copy));
  }
  assert(!active);return 0;
}
