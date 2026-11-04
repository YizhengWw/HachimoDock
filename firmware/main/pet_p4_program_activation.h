/* Trusted package-worker prepare/publication boundary. No wire commands,
 * package filesystem writes, capability enabling, or automatic task replay.
 * The existing installer owns package verification, publication and its state
 * mutex; this bridge never substitutes an uncommitted image for installed data.
 */
#pragma once
#include "pet_p4_program_host.h"
#include "pet_p4_program_persistence.h"

typedef struct {
  pet_program_image_t image;
  pet_program_snapshot_t seed;
  pet_resource_lease_t storage;
  uint64_t operation;
  char component_id[48];
  unsigned char package_identity[32];
  bool valid,inside,has_seed;
} pet_program_prepared_t;
typedef enum {
  PET_PROGRAM_PREPARED, PET_PROGRAM_PREPARE_INVALID,
  PET_PROGRAM_PREPARE_BUSY, PET_PROGRAM_PREPARE_CANCELLED,
  PET_PROGRAM_PREPARE_BAD_IMAGE, PET_PROGRAM_PREPARE_SAVE_ERROR
} pet_program_prepare_result_t;

/* Output is private inactive {0} staging, never an active host or wire object.
 * identity comes from the trusted verified package transaction, not proof
 * supplied by a component. Bytes/id/identity are immutable for this call.
 * Caller keeps its STORAGE_WORK ticket through prepare and publication. All
 * filesystem work stays here on the worker, outside UI/state locks. Failure
 * invalidates staging only, preserves old host and all existing stored data.
 * No schema migration or permission to replace unreadable saves is inferred.
 */
pet_program_prepare_result_t pet_program_activation_prepare(pet_program_prepared_t *out,
  const char *id,const unsigned char identity[32],uint64_t operation,
  const uint8_t *bytes,size_t length,pet_resource_lease_t storage);
/* Parent holds its package/lifecycle lock and supplies the CURRENT verified
 * transaction identity. Stale identities never touch the host. A successful
 * start consumes this preparation once. Denial preserves previous host image,
 * committed state and reports; no implicit cancellation/replacement/retry.
 * Callers must still satisfy the P4 performance/atomic audio-transition gates
 * before enabling PROGRAM. Source-level/library tests are not those gates.
 */
pet_program_runtime_result_t pet_program_activation_publish(pet_program_prepared_t *prepared,
  pet_program_host_t *host,const char *current_id,const unsigned char current_identity[32],
  uint64_t current_operation,uint16_t quantum,uint64_t *generation);
/* Receiver bounds are native metadata for the current verified transaction.
 * Incompatible saved outputs leave preparation, old host and durable data
 * unchanged. This restricts output publication; it grants no UI/service access. */
pet_program_runtime_result_t pet_program_activation_publish_with_contract(pet_program_prepared_t *prepared,
  pet_program_host_t *host,const char *current_id,const unsigned char current_identity[32],
  uint64_t current_operation,uint16_t quantum,const pet_program_output_contract_t *contract,uint64_t *generation);
/* Same verified one-shot publication, retaining the receiver's immutable UI
 * mapping as well as deriving its output ranges. Metadata must come from this
 * CURRENT verified package/model, not a component's claim to another UI. All
 * alias/stale/save/admission failures keep preparation and prior host intact.
 * This still grants no UI lease and opens no package/wire capability. */
pet_program_runtime_result_t pet_program_activation_publish_with_projection(pet_program_prepared_t *prepared,
  pet_program_host_t *host,const char *current_id,const unsigned char current_identity[32],
  uint64_t current_operation,uint16_t quantum,const pet_program_ui_projection_t *projection,uint64_t *generation);
/* Pure local cleanup, not authority to release a borrowed physical lease. */
bool pet_program_activation_discard(pet_program_prepared_t *prepared);
