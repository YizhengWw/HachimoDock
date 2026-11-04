/* Fixed-storage, nonblocking immutable native observation cache.
 * No driver, heap, authority, IO or task.
 * Fresh {0} storage only; never reset/copy/move while calls may exist.
 * Each published byte string is <=64 bytes. Read requires the exact native
 * payload size; failure leaves the caller's output untouched. This is a
 * coherent completed snapshot, not a promise of freshness or service rights.
 * Writer/reader source-level acquisition attempts are bounded. Slow
 * readers retain their physical slot pin, never allowing early buffer reuse.
 * Caller buffers cannot overlap cache storage. Native bytes are not a wire ABI.
 * Only 32-bit lock-free atomics; no wrapping sample-generation counter. A weak
 * CAS may yield BUSY instead of retrying. Hardware atomic timing and ISR/task
 * contention still need measurement; this is not a wall-time guarantee.
 */
#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#define PET_OBSERVATION_BYTES 64U
#define PET_OBSERVATION_SLOTS 3U
typedef struct {
  struct {
    atomic_uint readers;
    uint8_t length;
    unsigned char data[PET_OBSERVATION_BYTES];
  } slots[PET_OBSERVATION_SLOTS];
  atomic_uint published,writer;
} pet_observation_t;
typedef enum {
  PET_OBSERVATION_OK,PET_OBSERVATION_INVALID,PET_OBSERVATION_BUSY,PET_OBSERVATION_NOT_READY
} pet_observation_result_t;
pet_observation_result_t pet_observation_publish(pet_observation_t *cache,const void *bytes,size_t length);
pet_observation_result_t pet_observation_capture(pet_observation_t *cache,void *out,size_t length);
