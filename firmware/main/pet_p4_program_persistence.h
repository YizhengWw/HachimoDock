/* Trusted native bridge to the existing private A/B save worker. Not a package
 * command or capability grant. No default/reset/replay path is provided. */
#pragma once
#include "pet_p4_app_store.h"
#include "pet_p4_program_store.h"

/* Platform-owned namespace; components cannot choose this identifier. */
#define PET_PROGRAM_SAVE_DOMAIN UINT32_C(1)
typedef enum {
  PET_PROGRAM_SAVE_VALUE, PET_PROGRAM_SAVE_EMPTY, PET_PROGRAM_SAVE_BUSY,
  PET_PROGRAM_SAVE_UNAVAILABLE, PET_PROGRAM_SAVE_INVALID,
  PET_PROGRAM_SAVE_INCOMPATIBLE, PET_PROGRAM_SAVE_CORRUPT
} pet_program_save_result_t;
/* Worker only, outside state/render locks. image must be trusted, validated,
 * immutable staging for this call. Every non-VALUE outcome preserves seed.
 * Only EMPTY proves absence; all other errors prohibit a blank-state fallback.
 * Preserve this prepare result under the parent package/lifecycle transaction;
 * it is not itself permission to activate or replace an installed program. */
pet_program_save_result_t pet_program_persistence_load(const char *component_id,
  const pet_program_image_t *image,pet_program_snapshot_t *seed);
/* Package worker already owns STORAGE_WORK: borrow its exact ticket, never
 * acquire a second job or release the caller's physical reservation. */
pet_program_save_result_t pet_program_persistence_load_with_lease(const char *component_id,
  const pet_program_image_t *image,pet_program_snapshot_t *seed,pet_resource_lease_t borrowed);
/* Latest confirmed RAM checkpoint only. Safe at the publication boundary;
 * uncached/loading/failed is not EMPTY, and every error preserves seed. */
pet_program_save_result_t pet_program_persistence_load_cached(const char *component_id,
  const pet_program_image_t *image,pet_program_snapshot_t *seed);
/* Capture only committed state via host_snapshot under owner serialization,
 * then enqueue the immutable snapshot here. Success is RAM acceptance, not
 * fsync, task execution, external effects or permission to replay an event. */
bool pet_program_persistence_enqueue(const char *component_id,
  const pet_program_snapshot_t *committed);
/* RAM-only exact-content observation; never flushes or starts IO. A caller
 * must retain its expected snapshot until reconciliation is complete. */
pet_app_checkpoint_status_t pet_program_persistence_checkpoint(const char *component_id,
  const pet_program_snapshot_t *committed);
