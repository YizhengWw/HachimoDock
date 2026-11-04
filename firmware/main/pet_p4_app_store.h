#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "pet_p4_resources.h"
#include "pet_p4_storage_snapshot.h"
#include "pet_p4_save_policy.h"
#if PET_P4_DIAGNOSTIC_STORAGE_CACHED
/* Fixed native platform hook, not a component callback. Existing low-priority
 * worker calls it only AFTER flush_one returns, with no store/model lock held.
 * Default native test implementation is empty; platform override is immutable. */
void pet_app_store_housekeeping(void);
#endif
/* Private device saves, never included in exported component packages. */
#if PET_P4_PROGRAM_V2 || PET_P4_QUOTA_V3
/* P4S2 holds every int32 cell in one existing atomic A/B record. */
#define PET_APP_SAVE_BYTES 2304
#else
#define PET_APP_SAVE_BYTES 1024
#endif
/* Do not reclaim until EVERY long-lived native consumer pins its key.
 * The existing foreground runtime has not yet adopted registry lifetime pins;
 * ordinary/UI/program profiles therefore retain their original no-eviction
 * semantics. Only explicit host tests / a separately reviewed parent opt in. */
#ifndef PET_APP_SCOPED_RECLAIM_REVIEWED
#define PET_APP_SCOPED_RECLAIM_REVIEWED 0
#endif
typedef enum {
  PET_APP_STORE_EMPTY=0, PET_APP_STORE_VALUE=1,
  PET_APP_STORE_BUSY=2, PET_APP_STORE_UNAVAILABLE=3, PET_APP_STORE_INVALID=4
} pet_app_store_status_t;
bool pet_app_store_init(void);
/* EMPTY means both generations are confirmed missing, never an I/O failure.
 * UNAVAILABLE retains the existing files and suppresses default-value flush. */
pet_app_store_status_t pet_app_store_load(const char *id, char out[PET_APP_SAVE_BYTES]);
/* Trusted caller already owns STORAGE_WORK (e.g. package preparation). The
 * token is validated against the device broker and is borrowed, never released
 * here. No package/wire data can create this authority. */
pet_app_store_status_t pet_app_store_load_with_lease(const char *id,
  char out[PET_APP_SAVE_BYTES],pet_resource_lease_t lease);
bool pet_app_store_read(const char *id, char out[PET_APP_SAVE_BYTES]);
/* Never touches the filesystem. Preload with read outside the state lane,
 * then use this to merge the latest in-memory updates at publication time. */
bool pet_app_store_read_cached(const char *id, char out[PET_APP_SAVE_BYTES]);
bool pet_app_store_update(const char *id, const char *json);
void pet_app_store_forget(const char *id);
/* Storage-worker only, model/render mutex released. Persist the independently
 * marked forget epoch with the caller's admitted lease before acknowledging
 * explicit clear-save deletion. Does not flush a new application snapshot. */
bool pet_app_store_forget_with_lease(const char *id,pet_resource_lease_t lease);

/* Trusted native namespaces, not wire/package authority. Zero is reserved for
 * legacy widget saves. Separate capacity and filename prefixes preserve legacy
 * IDs, files and all 32 legacy cache slots. Load before update; unreadable saves
 * never become empty defaults. Namespace allocation belongs to the platform. */
pet_app_store_status_t pet_app_store_load_scoped(uint32_t domain,const char *id,
  char out[PET_APP_SAVE_BYTES]);
pet_app_store_status_t pet_app_store_load_scoped_with_lease(uint32_t domain,const char *id,
  char out[PET_APP_SAVE_BYTES],pet_resource_lease_t borrowed);
/* RAM-only observation after worker preparation. Never reserves/reclaims a
 * slot, starts IO or treats an uncached/unreadable record as EMPTY. */
pet_app_store_status_t pet_app_store_read_scoped_cached(uint32_t domain,const char *id,char out[PET_APP_SAVE_BYTES]);
bool pet_app_store_update_scoped(uint32_t domain,const char *id,const char *json);
/* Native RAM-cache lifetime only, never IO or a resource/execution grant.
 * A loaded, non-failed scoped key can be pinned by ONE fixed-address owner.
 * Tokens carry a monotonic serial; stale/unbalanced unpin cannot release a
 * later occupant. No caller may copy/reset the owning lifetime while pinned.
 * In an explicitly reviewed parent, clean loaded unpinned scoped entries may be reclaimed by a subsequent
 * authoritative load. Legacy 32 entries, disk files and dirty/error/loading
 * entries are never evicted. Acquire a pin before retaining a background key.
 */
typedef struct {const void *owner;uint64_t serial;uint8_t slot;} pet_app_store_pin_t;
bool pet_app_store_pin_scoped(uint32_t domain,const char *id,const void *owner,pet_app_store_pin_t *out);
bool pet_app_store_unpin(pet_app_store_pin_t *pin);
typedef enum {
  PET_APP_CHECKPOINT_INVALID, PET_APP_CHECKPOINT_BLOCKED,
  PET_APP_CHECKPOINT_ABSENT, PET_APP_CHECKPOINT_SUPERSEDED,
  PET_APP_CHECKPOINT_PENDING, PET_APP_CHECKPOINT_DURABLE
} pet_app_checkpoint_status_t;
/* RAM-only exact-content observation. DURABLE requires the deferred worker's
 * successful fsync/close reconciliation, never merely update acceptance.
 * An uncached/unreadable key is BLOCKED, not proof of absence. ABSENT requires
 * a successfully loaded empty/tombstone value in this process.
 * Not a job receipt or reboot-persistent exactly-once guarantee. */
pet_app_checkpoint_status_t pet_app_store_checkpoint_scoped(uint32_t domain,
  const char *id,const char *expected_json);
