#include "pet_p4_resources_device.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_bool start;
static pet_resource_owner_t owner;
static pet_resource_lease_t storage;
static pet_resource_realtime_t realtime;
static pet_resource_result_t storage_result;
static void *storage_start(void *unused) {
  (void)unused; while (!atomic_load(&start)) sched_yield();
  storage_result = pet_resources_device_acquire(owner, PET_RESOURCE_STORAGE_WORK, 1, &storage);
  return NULL;
}
static void *realtime_start(void *unused) {
  (void)unused; while (!atomic_load(&start)) sched_yield();
  assert(pet_resources_device_realtime_begin(&realtime) == PET_RESOURCE_OK);
  return NULL;
}
static void core_boundaries(void) {
  pet_resource_broker_t broker = {0};
  pet_resource_owner_t owners[PET_RESOURCE_OWNER_SLOTS] = {0};
  for (unsigned i = 0; i < PET_RESOURCE_OWNER_SLOTS; i++)
    assert(pet_resources_owner_open(&broker, PET_RESOURCE_ALL_MASK, &owners[i]) == PET_RESOURCE_OK);
  pet_resource_realtime_t tickets[PET_RESOURCE_REALTIME_SLOTS] = {0};
  for (unsigned i = 0; i < PET_RESOURCE_REALTIME_SLOTS; i++)
    assert(pet_resources_realtime_begin(&broker, &tickets[i]) == PET_RESOURCE_OK);
  pet_resource_realtime_t extra = {0};
  assert(pet_resources_realtime_begin(&broker, &extra) == PET_RESOURCE_BUSY);
  pet_resource_realtime_t old = tickets[0];
  assert(pet_resources_realtime_finish(&broker, tickets[0]));
  tickets[0] = (pet_resource_realtime_t){0};
  assert(pet_resources_realtime_begin(&broker, &tickets[0]) == PET_RESOURCE_OK);
  assert(!pet_resources_realtime_finish(&broker, old));
  assert(pet_resources_realtime_current(&broker, tickets[0]));
  pet_resource_broker_t foreign = {0};
  assert(!pet_resources_realtime_finish(&foreign, tickets[0]));
  broker.next_realtime_serial = UINT64_MAX;
  assert(pet_resources_realtime_begin(&broker, &extra) == PET_RESOURCE_EXHAUSTED);
  for (unsigned i = 0; i < PET_RESOURCE_REALTIME_SLOTS; i++)
    assert(pet_resources_realtime_finish(&broker, tickets[i]));
}
int main(void) {
  core_boundaries();
  for (unsigned round = 0; round < 200; round++) {
    owner = (pet_resource_owner_t){0}; storage = (pet_resource_lease_t){0}; realtime = (pet_resource_realtime_t){0};
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK), &owner) == PET_RESOURCE_OK);
    atomic_store(&start, false);
    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, storage_start, NULL));
    assert(!pthread_create(&threads[1], NULL, realtime_start, NULL));
    atomic_store(&start, true);
    assert(!pthread_join(threads[0], NULL)); assert(!pthread_join(threads[1], NULL));
    assert(storage_result == PET_RESOURCE_OK || storage_result == PET_RESOURCE_PROTECTED);
    pet_resource_usage_t usage; pet_resources_device_usage(&usage);
    assert(usage.realtime_reservations == 1);
    if (storage_result == PET_RESOURCE_OK) {
      assert(!pet_resources_device_current(storage));
      assert(usage.leases[PET_RESOURCE_STORAGE_WORK] == 1); /* Physical FS may still be executing. */
    }
    pet_resources_device_audio_observed(false);
    pet_resource_debug_session_t debug = {0};
    assert(pet_resources_device_debug_begin(32, false, &debug) == PET_RESOURCE_PROTECTED);
    pet_resources_device_usage(&usage);
    assert(usage.protected_mask & PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK));
    pet_resource_owner_t next = {0}; pet_resource_lease_t lease = {0};
    assert(pet_resources_device_owner_open(PET_RESOURCE_MASK(PET_RESOURCE_STORAGE_WORK), &next) == PET_RESOURCE_OK);
    assert(pet_resources_device_acquire(next, PET_RESOURCE_STORAGE_WORK, 1, &lease) == PET_RESOURCE_PROTECTED);
    pet_resources_device_realtime_end(&realtime);
    if (storage_result == PET_RESOURCE_OK) {
      assert(pet_resources_device_acquire(next, PET_RESOURCE_STORAGE_WORK, 1, &lease) == PET_RESOURCE_BUSY);
      assert(pet_resources_device_finish(storage) == PET_RESOURCE_FINISH_REVOKED);
    }
    assert(pet_resources_device_acquire(next, PET_RESOURCE_STORAGE_WORK, 1, &lease) == PET_RESOURCE_OK);
    assert(pet_resources_device_finish(lease) == PET_RESOURCE_FINISH_CURRENT);
    pet_resources_device_revoke(next); pet_resources_device_revoke(owner);
  }
  pet_resource_usage_t usage; pet_resources_device_usage(&usage);
  assert(!usage.live_owners && !usage.revoked_owners && !usage.realtime_reservations);
  puts("PASS atomic realtime/background admission, retained physical leases, observer isolation and ABA");
  return 0;
}
