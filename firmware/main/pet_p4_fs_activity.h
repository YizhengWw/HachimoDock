#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Observation, not filesystem serialization. Begin before a concurrent
 * SPIFFS operation can touch VFS/shared SPIFFS errno; end only after its final
 * close/error path. Includes readers: SPIFFS errno is shared across tasks.
 * Current independent actors are app_store, asset preparation and file-cue
 * playback. Other package/asset I/O is serialized with cleanup by state_mutex
 * (or runs before RX at boot). New actors must be instrumented before enabling
 * this optimization. The enable flag is not proof that coverage is complete. */
typedef struct { bool counted; } pet_fs_activity_t;
#if defined(ESP_PLATFORM) || defined(PET_FS_ACTIVITY_TEST)
pet_fs_activity_t pet_fs_activity_begin(void);
void pet_fs_activity_end(pet_fs_activity_t *activity);
bool pet_fs_activity_idle_epoch(uint64_t *epoch);
/* Explicit init opt-in after auditing the complete actor set. Never resets
 * an epoch or in-flight count; exhaustion permanently disables eligibility. */
void pet_fs_activity_enable_observation(void);
#else
/* Uninstrumented portable consumers cannot claim a reliable snapshot. */
static inline pet_fs_activity_t pet_fs_activity_begin(void){return (pet_fs_activity_t){0};}
static inline void pet_fs_activity_end(pet_fs_activity_t *a){(void)a;}
static inline bool pet_fs_activity_idle_epoch(uint64_t *epoch){if(epoch)*epoch=0;return false;}
static inline void pet_fs_activity_enable_observation(void){}
#endif
#define PET_FS_ACTIVITY_SCOPE(name) \
  pet_fs_activity_t name __attribute__((cleanup(pet_fs_activity_end))) = pet_fs_activity_begin()
