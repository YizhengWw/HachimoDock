#include "pet_p4_resources_device.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#define THREADS 8
static atomic_bool start;
static pet_resource_debug_session_t attempts[THREADS];
static pet_resource_result_t results[THREADS];
static void *contend(void *arg) {
  unsigned index = (unsigned)(uintptr_t)arg;
  while (!atomic_load(&start)) sched_yield();
  results[index] = pet_resources_device_debug_begin(PET_RESOURCE_SNAPSHOT_BYTES, false, &attempts[index]);
  return NULL;
}

int main(void) {
  pet_resource_usage_t used;
  pet_resource_view_session_t view = {0};
  assert(pet_resources_device_view_begin(&view) == PET_RESOURCE_OK);
  pet_resource_view_session_t old_view = view, denied_view = {0};
  assert(pet_resources_device_view_begin(&denied_view) == PET_RESOURCE_BUSY);
  pet_resource_debug_session_t job = {0}, denied = {0};
  assert(pet_resources_device_debug_begin(PET_RESOURCE_SNAPSHOT_BYTES + 1, false, &job) == PET_RESOURCE_INVALID);
  pet_resources_device_usage(&used);assert(used.live_owners == 1 && !used.revoked_owners);
  assert(pet_resources_device_debug_begin(153600, true, &job) == PET_RESOURCE_PROTECTED);
  assert(pet_resources_device_view_current(&view));
  assert(pet_resources_device_debug_begin(153600, false, &job) == PET_RESOURCE_OK);
  pet_resource_debug_session_t old = job;
  assert(pet_resources_device_debug_begin(153600, false, &denied) == PET_RESOURCE_BUSY);
  pet_resources_device_audio_observed(true);
  pet_resources_device_usage(&used);
  assert(used.units[PET_RESOURCE_SNAPSHOT] == 153600 && used.leases[PET_RESOURCE_DEBUG_BULK] == 1);
  /* Observing audio in the worker revokes authority, but cannot free raw bytes. */
  assert(!pet_resources_device_debug_current(&job, true));
  assert(!pet_resources_device_debug_current(&job, false));
  assert(pet_resources_device_debug_begin(153600, false, &denied) == PET_RESOURCE_BUSY);
  pet_resources_device_usage(&used);assert(used.revoked_owners == 1 && used.live_owners == 1);
  /* The physical cleanup point: only now can another screenshot allocate. */
  assert(!pet_resources_device_debug_end(&job)); // canceled completion cannot publish
  assert(pet_resources_device_debug_begin(153600, false, &job) == PET_RESOURCE_OK);
  assert(!pet_resources_device_debug_revoke(&old));
  assert(!pet_resources_device_debug_end(&old)); // stale worker cannot cancel or finish the new job
  assert(pet_resources_device_debug_current(&job, false));
  assert(pet_resources_device_view_current(&view));
  pet_resource_debug_session_t mismatched = job;mismatched.owner = view.owner;
  assert(!pet_resources_device_debug_revoke(&mismatched));
  assert(!pet_resources_device_debug_end(&mismatched));
  assert(pet_resources_device_debug_current(&job, false));
  assert(pet_resources_device_debug_end(&job));
  assert(!pet_resources_device_debug_end(&job)); // idempotent cleanup
  pet_resources_device_view_end(&view);
  assert(pet_resources_device_view_begin(&view) == PET_RESOURCE_OK);
  pet_resources_device_view_end(&old_view);
  assert(pet_resources_device_view_current(&view));

  /* Two real device reader paths may contend. Only one pair is admitted. */
  pthread_t threads[THREADS];
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_create(&threads[i], NULL, contend, (void *)(uintptr_t)i));
  atomic_store(&start, true);
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_join(threads[i], NULL));
  unsigned accepted = 0, winner = 0;
  for (unsigned i = 0; i < THREADS; i++) {
    if (results[i] == PET_RESOURCE_OK) {accepted++;winner = i;}
    else assert(results[i] == PET_RESOURCE_BUSY);
  }
  assert(accepted == 1);
  pet_resources_device_usage(&used);
  assert(used.live_owners == 2 && !used.revoked_owners);
  assert(used.leases[PET_RESOURCE_SNAPSHOT] == 1 && used.units[PET_RESOURCE_SNAPSHOT] == PET_RESOURCE_SNAPSHOT_BYTES);
  assert(used.leases[PET_RESOURCE_DEBUG_BULK] == 1 && used.leases[PET_RESOURCE_HOST_VIEW] == 1);
  assert(pet_resources_device_debug_revoke(&attempts[winner]));
  assert(!pet_resources_device_debug_current(&attempts[winner], false));
  pet_resources_device_usage(&used);assert(used.leases[PET_RESOURCE_DEBUG_BULK] == 1);
  assert(!pet_resources_device_debug_end(&attempts[winner]));
  pet_resources_device_view_end(&view);
  pet_resources_device_usage(&used);assert(!used.live_owners && !used.revoked_owners);
  for (unsigned i = 0; i < PET_RESOURCE_KIND_COUNT; i++) assert(!used.leases[i] && !used.units[i]);
  puts("PASS device adapter: atomic debug pair, audio-observed cancellation, view isolation, concurrent admission");
  return 0;
}
