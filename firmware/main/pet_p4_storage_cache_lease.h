/* Native STORAGE_WORK lifetime bridge. No component, filename or grant policy.
 * Optional caching never makes an unavailable resource or failed IO succeed.
 * Stop only after physical IO/close returns, before finishing the native lease.
 * Expiry is still a bypass: never extend/reset a diagnostic or another epoch.
 */
#pragma once
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_resources_device.h"
#if PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED || PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
typedef struct pet_storage_cache_lease {
  const struct pet_storage_cache_lease *self;
  TaskHandle_t task;
  uint32_t generation;
} pet_storage_cache_lease_t;
static inline bool pet_storage_cache_lease_begin(pet_storage_cache_lease_t *out,
    pet_resource_lease_t lease){
  if(!out||out->self||out->task||out->generation
      ||lease.kind!=PET_RESOURCE_STORAGE_WORK||lease.units!=1
      ||!pet_resources_device_current(lease))return false;
  TaskHandle_t task=xTaskGetCurrentTaskHandle();if(!task)return false;
  uint32_t generation=pet_storage_io_begin(PET_STORAGE_IO_MAX_WINDOW_US);
  if(!generation)return false; /* A busy/manual observer is NEVER adopted. */
  *out=(pet_storage_cache_lease_t){.self=out,.task=task,.generation=generation};
  return true;
}
static inline bool pet_storage_cache_lease_finish(pet_storage_cache_lease_t *scope){
  if(!scope||scope->self!=scope||!scope->generation
      ||scope->task!=xTaskGetCurrentTaskHandle())return false;
  uint32_t generation=scope->generation;
  *scope=(pet_storage_cache_lease_t){0};
  /* Revocation must not prevent physical completion from releasing its own
   * observer. An already replaced generation cannot stop the newer one. */
  return pet_storage_io_stop(generation);
}
/* Optional native boot/read preparation. This acquires occupancy only for
 * cache lifetime, not permission to accept data: the caller retains its
 * existing physical byte/format/checksum/error checks even on a cache miss.
 * No UI/guest can create these native pointers or resource tickets. */
typedef struct pet_storage_cache_work {
  const struct pet_storage_cache_work *self;
  TaskHandle_t task;
  pet_resource_owner_t owner;
  pet_resource_lease_t lease;
  pet_storage_cache_lease_t cache;
} pet_storage_cache_work_t;
static inline bool pet_storage_cache_work_begin(pet_storage_cache_work_t *out){
  if(!out||out->self||out->task||out->owner.generation||out->lease.serial
      ||out->cache.self||out->cache.task||out->cache.generation)return false;
  TaskHandle_t task=xTaskGetCurrentTaskHandle();if(!task)return false;
  pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
  if(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK),&owner)!=PET_RESOURCE_OK)return false;
  if(pet_resources_device_acquire(owner,PET_RESOURCE_STORAGE_WORK,1,&lease)!=PET_RESOURCE_OK){
    pet_resources_device_revoke(owner);return false;
  }
  *out=(pet_storage_cache_work_t){.self=out,.task=task,.owner=owner,.lease=lease};
  (void)pet_storage_cache_lease_begin(&out->cache,lease);
  return true;
}
static inline void pet_storage_cache_work_finish(pet_storage_cache_work_t *scope){
  if(!scope||scope->self!=scope||!scope->lease.serial
      ||scope->task!=xTaskGetCurrentTaskHandle())return;
  (void)pet_storage_cache_lease_finish(&scope->cache);
  (void)pet_resources_device_finish(scope->lease);
  pet_resources_device_revoke(scope->owner);
  *scope=(pet_storage_cache_work_t){0};
}
#if PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED
/* Only at an existing physical package boundary, after its own IO returned.
 * Keep a still-active finite epoch, never refresh its start/deadline. Expired
 * metadata with inflight work cannot be reset. Manual/newer epochs are never
 * adopted, and any failed optimization leaves physical validation unchanged. */
static inline bool pet_storage_cache_work_step(pet_storage_cache_work_t *scope){
  if(!scope)return false;
  if(!scope->self)return pet_storage_cache_work_begin(scope);
  if(scope->self!=scope||scope->task!=xTaskGetCurrentTaskHandle()||!scope->lease.serial)return false;
  if(!pet_resources_device_current(scope->lease)){pet_storage_cache_work_finish(scope);return false;}
  if(scope->cache.generation){
    pet_storage_io_status_t status={0};
    if(pet_storage_io_status(scope->cache.generation,&status)){
      if(status.active)return true;
      if(status.inflight||!status.complete)return false;
    }
    (void)pet_storage_cache_lease_finish(&scope->cache);
  }
  (void)pet_storage_cache_lease_begin(&scope->cache,scope->lease);
  return true;
}
#endif
#endif
