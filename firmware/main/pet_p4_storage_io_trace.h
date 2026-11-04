/* Private, bounded observation of the real SPIFFS physical HAL. No filesystem
 * authority, retries, task, path or package permission. Optional exact-byte
 * caches and their native parent lifetime require separate default-off gates. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef PET_P4_STORAGE_IO_TRACE_ENABLE
#define PET_P4_STORAGE_IO_TRACE_ENABLE 0
#endif
#ifndef PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
#define PET_P4_STORAGE_UNIFORM_CACHE_ENABLE 0
#endif
#ifndef PET_P4_STORAGE_UNIFORM_CACHE_APPLY
#define PET_P4_STORAGE_UNIFORM_CACHE_APPLY 0
#endif
#ifndef PET_P4_STORAGE_PAGE_CACHE_ENABLE
#define PET_P4_STORAGE_PAGE_CACHE_ENABLE 0
#endif
#ifndef PET_P4_STORAGE_PAGE_CACHE_APPLY
#define PET_P4_STORAGE_PAGE_CACHE_APPLY 0
#endif
#ifndef PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH
#define PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH 0
#endif
#ifndef PET_P4_STORAGE_PAGE_CACHE_PARTIAL
#define PET_P4_STORAGE_PAGE_CACHE_PARTIAL 0
#endif
#ifndef PET_P4_STORAGE_PAGE_CACHE_SPARSE
#define PET_P4_STORAGE_PAGE_CACHE_SPARSE 0
#endif
#if PET_P4_STORAGE_PAGE_CACHE_SPARSE && (!PET_P4_STORAGE_PAGE_CACHE_ENABLE || !PET_P4_STORAGE_PAGE_CACHE_APPLY)
#error Sparse exact pages require the reviewed applied page cache
#endif
#ifndef PET_P4_STORAGE_IO_RETAIN_COMPLETED
#define PET_P4_STORAGE_IO_RETAIN_COMPLETED 0
#endif
#if PET_P4_STORAGE_PAGE_CACHE_PARTIAL && !PET_P4_STORAGE_PAGE_CACHE_ENABLE
#error "Partial byte hits require the reviewed full physical page cache"
#endif
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED && !PET_P4_STORAGE_IO_TRACE_ENABLE
#error "Completed observations require the physical HAL observer"
#endif
#ifndef PET_P4_STORAGE_CACHE_SCOPE_ENABLED
#define PET_P4_STORAGE_CACHE_SCOPE_ENABLED 0
#endif
#ifndef PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED
#define PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED 0
#endif
#ifndef PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED
#define PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED 0
#endif
#ifndef PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED
#define PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED 0
#endif
#ifndef PET_P4_STORAGE_PRESERVE_UNCHANGED
#define PET_P4_STORAGE_PRESERVE_UNCHANGED 0
#endif
#ifndef PET_P4_SPARE_RECREATE
#define PET_P4_SPARE_RECREATE 0
#endif
#ifndef PET_P4_SPARE_EXACT_REUSE
#define PET_P4_SPARE_EXACT_REUSE 0
#endif
#ifndef PET_P4_SPARE_TARGET_FILTER
#define PET_P4_SPARE_TARGET_FILTER 0
#endif
#ifndef PET_P4_SPARE_UPDATE
#define PET_P4_SPARE_UPDATE 0
#endif
#ifndef PET_P4_SPARE_UNBUFFERED
#define PET_P4_SPARE_UNBUFFERED 0
#endif
/* Private lean install-commit IO: catalog generations rewritten in place with a
 * self-check instead of tmp/remove/rename, plus a boot-local sprite cleanup memo.
 * No existence authority, no new buffer, no change to any publication rule. */
#ifndef PET_P4_COMMIT_LEAN_IO
#define PET_P4_COMMIT_LEAN_IO 0
#endif
#if PET_P4_COMMIT_LEAN_IO && !PET_P4_SPARE_UPDATE
#error "Lean commit IO requires the reviewed in-place spare update parent"
#endif
#if PET_P4_SPARE_UNBUFFERED && !PET_P4_SPARE_UPDATE
#error "Unbuffered spare writer requires the reviewed update parent"
#endif
#if PET_P4_SPARE_UPDATE && (!PET_P4_SPARE_TARGET_FILTER || !PET_P4_SPARE_EXACT_REUSE || PET_P4_SPARE_RECREATE)
#error "Spare update requires the exact target-filter parent, not recreation"
#endif
#if PET_P4_SPARE_TARGET_FILTER && !PET_P4_SPARE_EXACT_REUSE
#error "Target spare filter requires exact physical byte reuse"
#endif
#if PET_P4_SPARE_EXACT_REUSE && (!PET_P4_STORAGE_PRESERVE_UNCHANGED || PET_P4_SPARE_RECREATE)
#error "Exact spare reuse requires the reviewed storage parent, not recreation"
#endif
#if PET_P4_SPARE_RECREATE && !PET_P4_STORAGE_PRESERVE_UNCHANGED
#error "Spare recreation requires the reviewed transactional storage parent"
#endif
#if PET_P4_STORAGE_PRESERVE_UNCHANGED && !PET_P4_STORAGE_IO_TRACE_ENABLE
#error "Exact unchanged verification requires the reviewed physical HAL boundary"
#endif
#if PET_P4_STORAGE_BOOT_CACHE_BATCH_ENABLED && !PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED
#error "Native boot batch requires the reviewed boot cache lifetime"
#endif
#if (PET_P4_STORAGE_SAVE_CACHE_SCOPE_ENABLED || PET_P4_STORAGE_BOOT_CACHE_SCOPE_ENABLED) && !PET_P4_STORAGE_CACHE_SCOPE_ENABLED
#error "Native save cache scopes require the reviewed storage-lifecycle cache"
#endif
#if PET_P4_STORAGE_CACHE_SCOPE_ENABLED && (!PET_P4_STORAGE_PAGE_CACHE_ENABLE || !PET_P4_STORAGE_PAGE_CACHE_APPLY || !PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH)
#error "Private automatic scope cache requires the reviewed exact-byte stride candidate"
#endif
#if PET_P4_STORAGE_PAGE_CACHE_STRIDE_HASH && !PET_P4_STORAGE_PAGE_CACHE_ENABLE
#error "Private stride comparison requires bounded byte cache"
#endif
#if PET_P4_STORAGE_PAGE_CACHE_ENABLE && (!PET_P4_STORAGE_IO_TRACE_ENABLE || PET_P4_STORAGE_UNIFORM_CACHE_ENABLE)
#error "Private full-page cache requires observer and excludes compressed reference"
#endif
#if PET_P4_STORAGE_PAGE_CACHE_APPLY && !PET_P4_STORAGE_PAGE_CACHE_ENABLE
#error "Private full-page cache application requires bounded allocation"
#endif
#if PET_P4_STORAGE_UNIFORM_CACHE_ENABLE && !PET_P4_STORAGE_IO_TRACE_ENABLE
#error "Private byte cache requires bounded observer windows"
#endif
#if PET_P4_STORAGE_UNIFORM_CACHE_APPLY && !PET_P4_STORAGE_UNIFORM_CACHE_ENABLE
#error "Byte cache application requires explicitly reviewed private allocation"
#endif
#define PET_STORAGE_IO_MAX_WINDOW_US UINT32_C(5000000)
typedef enum {PET_STORAGE_IO_MAIN=1,PET_STORAGE_IO_SAVE,PET_STORAGE_IO_PROTOCOL,PET_STORAGE_IO_OTHER} pet_storage_io_actor_t;
typedef enum {PET_STORAGE_IO_READ,PET_STORAGE_IO_WRITE,PET_STORAGE_IO_ERASE} pet_storage_io_operation_t;
enum {PET_STORAGE_IO_EXPIRED=1,PET_STORAGE_IO_BAD_CLOCK=2,PET_STORAGE_IO_LATE_RETURN=4,PET_STORAGE_IO_OVERFLOW=8};
typedef struct {
  uint64_t bytes,wall_us; /* Requested bytes, including failed/partial calls; NOT accepted bytes. */
  uint32_t calls,errors,max_wall_us,buckets[4],cache_hits; /* Physical calls only; hit count separate. <=16, <=256, <=4096, >4096 */
} pet_storage_io_counter_t;
typedef struct {
  uint64_t started_us,stop_requested_us;
  uint32_t generation,duration_us,inflight,flags;
  bool active,complete;
  uint8_t owner_actor; /* Numeric native caller only, no package identity. */
} pet_storage_io_status_t;
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
/* Native-only, fixed-address/task-bound cache bypass. The caller must retain
 * its STORAGE_WORK lease and close physical files before releasing this scope.
 * Never serialized, borrowed, nested or used as filesystem authority. */
typedef struct pet_storage_physical_scope {
  const struct pet_storage_physical_scope *self;
  void *task;
} pet_storage_physical_scope_t;
bool pet_storage_physical_begin(pet_storage_physical_scope_t *scope);
bool pet_storage_physical_finish(pet_storage_physical_scope_t *scope);
#endif
#if PET_P4_STORAGE_IO_TRACE_ENABLE
/* Native command owner only, after dropping model locks. Returned HAL calls
 * may yield one tick to CPU idle; no yield inside a physical/cache-off call. */
void pet_storage_io_protocol_window(bool enabled);
bool pet_storage_io_register(pet_storage_io_actor_t actor,void *task);
bool pet_storage_io_ready(void);
uint32_t pet_storage_io_begin(uint32_t duration_us);
bool pet_storage_io_stop(uint32_t generation);
bool pet_storage_io_status(uint32_t generation,pet_storage_io_status_t *out);
/* Read-only latest metadata, no admission, reset, stop or physical IO. The
 * caller still binds this observation to its boot before any further reads. */
bool pet_storage_io_latest(pet_storage_io_status_t *out);
#if PET_P4_STORAGE_IO_RETAIN_COMPLETED
/* Last physically completed window PER registered native caller. A later save
 * cannot replace the protocol caller's evidence. All three operation counters
 * are copied together; expected_generation=0 selects latest, never retries IO. */
bool pet_storage_io_completed_read(pet_storage_io_actor_t owner,uint32_t expected_generation,
  pet_storage_io_actor_t actor,pet_storage_io_status_t *status,pet_storage_io_counter_t out[3]);
#endif
/* Only a stopped window with no physical call still in flight is readable.
 * Durations include preemption; independent OTHER calls may be added, never
 * inferred as paired threads or summed into exclusive CPU time. */
bool pet_storage_io_read(uint32_t generation,pet_storage_io_actor_t actor,
  pet_storage_io_operation_t operation,pet_storage_io_counter_t *out);
#else
static inline void pet_storage_io_protocol_window(bool enabled){(void)enabled;}
static inline bool pet_storage_io_register(pet_storage_io_actor_t a,void *p){(void)a;(void)p;return false;}
static inline bool pet_storage_io_ready(void){return false;}
static inline uint32_t pet_storage_io_begin(uint32_t n){(void)n;return 0;}
static inline bool pet_storage_io_stop(uint32_t g){(void)g;return false;}
static inline bool pet_storage_io_status(uint32_t g,pet_storage_io_status_t *o){(void)g;(void)o;return false;}
static inline bool pet_storage_io_latest(pet_storage_io_status_t *o){(void)o;return false;}
static inline bool pet_storage_io_read(uint32_t g,pet_storage_io_actor_t a,pet_storage_io_operation_t p,pet_storage_io_counter_t *o){(void)g;(void)a;(void)p;(void)o;return false;}
#endif
