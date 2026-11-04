#include <assert.h>
#include <string.h>
#include "pet_p4_storage_cache_lease.h"
unsigned io_meta_depth;
static TaskHandle_t task=(TaskHandle_t)3;
static bool active;
static uint32_t epoch,next_epoch,starts,stops;
#if PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED
static bool pending;
bool pet_storage_io_status(uint32_t generation,pet_storage_io_status_t *out){
  if(generation!=epoch)return false;
  *out=(pet_storage_io_status_t){.generation=epoch,.duration_us=5000000,.active=active,.inflight=pending?1:0,.complete=!active&&!pending};
  return true;
}
#endif
TaskHandle_t xTaskGetCurrentTaskHandle(void){assert(!io_meta_depth);return task;}
uint32_t pet_storage_io_begin(uint32_t duration){
  assert(!io_meta_depth&&duration==5000000);if(active)return 0;
  active=true;starts++;return epoch=++next_epoch;
}
bool pet_storage_io_stop(uint32_t generation){
  assert(!io_meta_depth);stops++;if(epoch!=generation)return false;active=false;return true;
}
static void empty_usage(void){
  pet_resource_usage_t u;pet_resources_device_usage(&u);
  assert(!u.leases[PET_RESOURCE_STORAGE_WORK]&&!u.units[PET_RESOURCE_STORAGE_WORK]);
  assert(!u.live_owners&&!u.revoked_owners);
}
int main(void){
  pet_storage_cache_work_t live={0},empty={0};
  pet_resource_owner_t holder={0};pet_resource_lease_t held={0};
  assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&holder)==PET_RESOURCE_OK);
  assert(pet_resources_device_acquire(holder,PET_RESOURCE_STORAGE_WORK,1,&held)==PET_RESOURCE_OK);
  assert(!pet_storage_cache_work_begin(&live)&&!memcmp(&live,&empty,sizeof(live))&&!starts);
  assert(pet_resources_device_current(held));
  pet_resources_device_finish(held);pet_resources_device_revoke(holder);empty_usage();
  assert(pet_storage_cache_work_begin(&live)&&starts==1&&active);
  pet_storage_cache_work_t copy=live;
  pet_storage_cache_work_finish(&copy);assert(active&&!stops&&pet_resources_device_current(live.lease));
  task=(TaskHandle_t)4;pet_storage_cache_work_finish(&live);assert(active&&!stops);task=(TaskHandle_t)3;
  pet_resource_realtime_t audio={0};assert(pet_resources_device_realtime_begin(&audio)==PET_RESOURCE_OK);
  assert(!pet_resources_device_current(live.lease));
  pet_storage_cache_work_finish(&live);assert(!active&&stops==1);empty_usage();
  assert(!pet_storage_cache_work_begin(&live));empty_usage();pet_resources_device_realtime_end(&audio);
  uint32_t manual=pet_storage_io_begin(5000000);assert(manual);
  assert(pet_storage_cache_work_begin(&live)&&!live.cache.generation);
  unsigned before=stops;pet_storage_cache_work_finish(&live);assert(active&&epoch==manual&&stops==before);empty_usage();
  assert(pet_storage_io_stop(manual));
  assert(pet_storage_cache_work_begin(&live));copy=live;
  pet_storage_cache_work_finish(&live);assert(!active);empty_usage();
  assert(pet_storage_cache_work_begin(&live));before=stops;
  pet_storage_cache_work_finish(&copy);assert(active&&stops==before&&pet_resources_device_current(live.lease));
  pet_storage_cache_work_finish(&live);empty_usage();
  for(unsigned i=0;i<10000;i++){
    assert(pet_storage_cache_work_begin(&live));pet_storage_cache_work_finish(&live);
    assert(!memcmp(&live,&empty,sizeof(live))&&!active);empty_usage();
  }
#if PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED
  assert(pet_storage_cache_work_step(&live));uint32_t generation=epoch;unsigned begun=starts,finished=stops;
  for(unsigned i=0;i<10000;i++)assert(pet_storage_cache_work_step(&live));
  assert(epoch==generation&&starts==begun&&stops==finished); /* No TTL renewal. */
  copy=live;assert(!pet_storage_cache_work_step(&copy));assert(epoch==generation&&starts==begun&&stops==finished);
  task=(TaskHandle_t)4;assert(!pet_storage_cache_work_step(&live));task=(TaskHandle_t)3;
  active=false;pending=true;assert(!pet_storage_cache_work_step(&live));assert(starts==begun&&stops==finished);
  pending=false;assert(pet_storage_cache_work_step(&live));assert(epoch>generation&&starts==begun+1&&stops==finished+1);
  generation=epoch;assert(pet_storage_io_stop(generation));manual=pet_storage_io_begin(5000000);assert(manual>generation);
  assert(pet_storage_cache_work_step(&live)&&active&&epoch==manual&&!live.cache.generation);
  before=stops;for(unsigned i=0;i<10;i++)assert(pet_storage_cache_work_step(&live));
  pet_storage_cache_work_finish(&live);assert(active&&epoch==manual&&stops==before);empty_usage();
  assert(pet_storage_io_stop(manual));assert(pet_storage_cache_work_step(&live));
  assert(pet_resources_device_realtime_begin(&audio)==PET_RESOURCE_OK);
  assert(!pet_storage_cache_work_step(&live)&&!active&&!memcmp(&live,&empty,sizeof(live)));
  empty_usage();pet_resources_device_realtime_end(&audio);
#endif
  assert(!io_meta_depth);return 0;
}
