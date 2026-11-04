/* Default-closed owned collection lifecycle bridge, not an installed control.
 * All storage is fixed-address, native-private and externally serialized with
 * the SAME UI/model/lifecycle lock; inside only rejects callback reentry.
 * Inactive bank preparation runs on an admitted worker, NOT a render/input
 * critical path. Parent budgets PSRAM/work and keeps actual physical leases.
 * No heap, IO, scheduler, native task, installer/wire route or package grant.
 * No record edits/submission, state reset, replay or automatic restart.
 */
#pragma once
#include "pet_p4_collection.h"
#define PET_COLLECTION_IDENTITY 32U
struct pet_collection_host;
typedef struct {char key[64],label[513],detail[513];} pet_collection_record_t;
typedef struct {
  pet_collection_record_t records[PET_COLLECTION_ROWS];
  pet_collection_row_t rows[PET_COLLECTION_ROWS];
  uint8_t identity[PET_COLLECTION_IDENTITY];
  uint64_t revision;
  const struct pet_collection_host *bound;
  uint32_t marker;
  uint16_t count;
} pet_collection_dataset_t;
typedef struct {
  void *context;
  uint64_t generation;
  /* Trusted bounded observer of EXACT current UI/lease/lifetime and required
   * audio policy. No IO/mutations; never a callback supplied by package data.
   * A true sample does not itself make publication atomic with audio start;
   * the parent must supply the actual shared synchronization/resource policy. */
  bool (*current)(void *context,uint64_t generation);
  /* Expected verified source/declaration identity, copied at init. A candidate
   * bank may NOT choose this receiver identity on first publication. */
  uint8_t identity[PET_COLLECTION_IDENTITY];
} pet_collection_authority_t;
typedef struct pet_collection_host {
  pet_collection_authority_t authority;
  pet_collection_dataset_t *dataset;
  pet_collection_state_t state;
  pet_collection_view_t view;
  uint8_t identity[PET_COLLECTION_IDENTITY];
  uint64_t epoch,revision;
  uint32_t marker;
  bool inside,stopped,published;
} pet_collection_host_t;
typedef struct {
  const pet_collection_host_t *host;
  uint64_t generation,epoch,revision;
} pet_collection_observation_t;
typedef struct {
  pet_collection_state_t state;
  pet_collection_view_t view;
  pet_collection_workspace_t core;
} pet_collection_host_workspace_t;
typedef enum {PET_COLLECTION_HOST_OK,PET_COLLECTION_HOST_INVALID,PET_COLLECTION_HOST_BUSY,
  PET_COLLECTION_HOST_STALE,PET_COLLECTION_HOST_DENIED,PET_COLLECTION_HOST_STOPPED,
  PET_COLLECTION_HOST_UNPUBLISHED,PET_COLLECTION_HOST_BAD_DATA,PET_COLLECTION_HOST_BAD_STATE,
  PET_COLLECTION_HOST_NO_ROW,PET_COLLECTION_HOST_EXHAUSTED} pet_collection_host_result_t;
/* Fresh {0} bank or a previously prepared UNBOUND bank only. All complete input
 * text is copied; no pointer to caller source survives. Inputs/outputs/work must
 * not overlap. Failure preserves the old bank (scratch may change). Identity is
 * a trusted declaration/source identity, NOT content authentication or a grant;
 * revision is a nonzero trusted monotonic dataset revision within that identity.
 * Once prepared, do not copy/move/edit storage: rows point into its own records.
 * Reprepare only after the parent proves it unbound and no worker/reader owns it.
 */
pet_collection_host_result_t pet_collection_dataset_prepare(pet_collection_dataset_t *out,
  const uint8_t identity[PET_COLLECTION_IDENTITY],uint64_t revision,
  const pet_collection_row_t *rows,unsigned count,pet_collection_host_workspace_t *work);
/* Fresh zero native metadata only; state/view are output-only scratch cleared
 * by initialization, NOT saved data. Existing saved state is supplied explicitly
 * at first publication. No reinit of initialized/stopped storage is permitted. */
bool pet_collection_host_init(pet_collection_host_t *host,const pet_collection_authority_t *authority);
/* First publication requires explicit seed (including explicit {0} for a new
 * model). Every bank must match the receiver's copied source identity, including
 * first publication. A replacement with NULL seed retains navigation/selection;
 * incompatible seeds are refused, never reset. New banks must have SAME source
 * identity and STRICTLY greater revision. Failure preserves old bank binding,
 * state/cache/epoch, except observed authority loss latches cancellation.
 * Preparation is outside the UI lock; this final pure projection/publication
 * uses already validated immutable records and caller-private off-stack scratch.
 */
pet_collection_host_result_t pet_collection_host_publish(pet_collection_host_t *host,
  uint64_t generation,pet_collection_dataset_t *dataset,const pet_collection_state_t *seed,
  unsigned visible,pet_collection_host_workspace_t *work);
/* Read-only cached view, no full-record scan or row validation per paint.
 * Returns current native-only observation, not a wire token or service grant.
 * Parent captures under UI/model lock. Rasterize only the COPIED view outside
 * that lock; retain actual render-buffer ownership through physical completion.
 * Before frame publication, reacquire the same lock and revalidate observation,
 * UI identity and physical lease; a captured view is not permission to publish.
 * All outputs are distinct private buffers, unchanged on failure. */
pet_collection_host_result_t pet_collection_host_read(pet_collection_host_t *host,uint64_t generation,
  pet_collection_view_t *out,pet_collection_observation_t *observation);
/* Constant-size exact-context recheck, no view/data scan, mutation or event.
 * Parent uses it under the publication lock and still owns resource admission.
 * It cannot authorize another UI or make a flag sample atomic with audio start.
 */
pet_collection_host_result_t pet_collection_host_observation_current(pet_collection_host_t *host,
  uint64_t generation,const pet_collection_observation_t *observed);
/* Every action requires exact observed generation/epoch/dataset revision.
 * Reorder/filter/page/any successful prior action invalidates an old gesture,
 * including away-and-back or an action leaving displayed values unchanged.
 * No-row/failure leaves committed state unchanged. No implicit action retry.
 */
pet_collection_host_result_t pet_collection_host_apply(pet_collection_host_t *host,uint64_t generation,
  pet_collection_operation_t operation,const pet_collection_observation_t *observed,
  pet_collection_host_workspace_t *work);
/* Historical committed state is readable after stop without a live grant;
 * saving/migration is the trusted parent's separate transaction, not done here. */
pet_collection_host_result_t pet_collection_host_snapshot(pet_collection_host_t *host,uint64_t generation,
  pet_collection_state_t *out);
/* Latched stop: detaches only its exact bank binding after synchronous work has
 * returned. Parent retains physical lease cleanup; this never releases one.
 * Cached historical state survives; restoring callback/audio cannot resume it. */
pet_collection_host_result_t pet_collection_host_cancel(pet_collection_host_t *host,uint64_t generation);
