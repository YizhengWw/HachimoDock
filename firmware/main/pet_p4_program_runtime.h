/* Default-off, cooperative pure-compute runtime for staged P4P1 images.
 * This does NOT enable PROGRAM in the resource broker or add a package command.
 * The trusted host MUST hold an admitted PROGRAM lease (units >= image fuel)
 * before start, retain it
 * through cancellation/completion, and release it only after this code returns.
 * current() checks that exact live lease/generation on every instruction and
 * must observe audio protection. No arbitrary effects or peripheral access.
 *
 * Every API is externally serialized with committed state. Independent owner
 * revocation must share the legacy caller lock OR the native commit publisher.
 * The inside flag rejects callback reentry; it is NOT a cross-thread mutex.
 * current() is bounded trusted code, never IO, waiting, or state mutation. Inputs
 * and output buffers must not alias the runtime. Runtime storage must not move.
 * Caller yields between pump calls; NEVER drain it in a synchronous busy loop.
 * Software bounds are not wall-time guarantees. Begin/step/commit/start/loader
 * all still require independent P4 timing gates before integration is enabled.
 */
#pragma once
#include "pet_p4_program_image.h"

#define PET_PROGRAM_QUEUE 4U

typedef struct {
  uint32_t schema, revision;
  pet_program_cells_t cells;
  int32_t values[PET_PROGRAM_CELLS], outputs[PET_PROGRAM_OUTPUTS];
} pet_program_snapshot_t;

typedef struct {
  uint64_t generation, sequence;
  pet_program_result_t result;
  uint32_t revision;
  uint16_t used_fuel;
} pet_program_report_t;

typedef enum {
  PET_RUNTIME_OK, PET_RUNTIME_INVALID, PET_RUNTIME_BUSY, PET_RUNTIME_STALE,
  PET_RUNTIME_DENIED, PET_RUNTIME_BAD_IMAGE, PET_RUNTIME_BAD_STATE,
  PET_RUNTIME_QUEUE_FULL, PET_RUNTIME_DUPLICATE, PET_RUNTIME_IDLE,
  PET_RUNTIME_YIELDED, PET_RUNTIME_COMPLETE, PET_RUNTIME_REPORT_PENDING,
  PET_RUNTIME_STOPPED,
  PET_RUNTIME_UNCHANGED /* Native snapshot observation only, never a pump phase. */
} pet_program_runtime_result_t;

/* Trusted parent cursor, never serialized or accepted from an image/event.
 * Valid only with a previously validated snapshot in the SAME fixed-address
 * runtime lifetime. Clear on parent reuse/init or failed validation. Generation
 * and committed revision cannot wrap; immutable image layout belongs to that
 * generation. No pointer to the selected publication state escapes its lock. */
typedef struct {
  const void *runtime;
  uint64_t generation;
  uint32_t schema, revision;
  pet_program_cells_t cells;
} pet_program_snapshot_cursor_t;

typedef struct {
  /* Private-by-contract storage. No fields or pointers come from wire input. */
  const void *instance;
  pet_program_image_t image;
  pet_program_state_t state;
#if PET_P4_PROGRAM_V2
  pet_program_publication_t publication;
  pet_program_state_t publication_slots[2];
#endif
  pet_program_workspace_t workspace;
  pet_program_event_t queue[PET_PROGRAM_QUEUE];
  uint64_t sequences[PET_PROGRAM_QUEUE];
  pet_program_authority_t authority;
  pet_program_report_t report;
  pet_program_output_contract_t output_contract;
  pet_program_publish_fn publish;
  void *publish_context;
  uint64_t generation, last_sequence, inflight;
  uint32_t marker, cancelled_events;
  uint16_t quantum;
  uint8_t head, queued;
  bool active, inside, report_pending,output_limited;
} pet_program_runtime_t;

/* Only on fresh/inert storage; never to restart a live owner. */
void pet_program_runtime_init(pet_program_runtime_t *runtime);
/* Transactional activation: all validation/checks happen before replacing the
 * inactive runtime. Failure leaves committed state/image/queue/report unchanged.
 * A live runtime or unread terminal report cannot be replaced. New owner
 * generations are strictly increasing, so delayed events cannot target restart.
 * NULL seed explicitly means a NEW blank model, never implicit save migration.
 * Non-NULL seed requires exact schema/cell-layout match. Host must preserve old
 * saves on mismatch, migrate separately, and never infer permission to reset.
 * UINT32_MAX revision is a valid exhausted snapshot: restore it unchanged. The
 * core rejects subsequent events with OVERFLOW, never wraps/resets revision.
 * The image is copied; the caller can release inactive staging after success.
 * No grants/intents are admitted; authority is created here with zero grants.
 */
pet_program_runtime_result_t pet_program_runtime_start(pet_program_runtime_t *runtime,
  const pet_program_image_t *image, uint64_t generation,
  bool (*current)(void *context, uint64_t generation), void *context,
  uint16_t quantum, const pet_program_snapshot_t *seed);
/* Optional immutable receiver limits copied on successful activation. A seed
 * (or explicit new blank model) must fit before any publication. Narrowing that
 * rejects an old save is BAD_STATE, never permission to reset/migrate it. */
pet_program_runtime_result_t pet_program_runtime_start_with_contract(pet_program_runtime_t *runtime,
  const pet_program_image_t *image,uint64_t generation,
  bool (*current)(void *context,uint64_t generation),void *context,
  uint16_t quantum,const pet_program_snapshot_t *seed,
  const pet_program_output_contract_t *contract);
/* Native-only immutable publication adapter. Same activation transaction and
 * data checks as above; no image/event can choose this function or its context.
 * It linearizes the fixed commit copy with the real broker/audio revocation. */
pet_program_runtime_result_t pet_program_runtime_start_with_publication(pet_program_runtime_t *runtime,
  const pet_program_image_t *image,uint64_t generation,
  bool (*current)(void *context,uint64_t generation),void *context,
  uint16_t quantum,const pet_program_snapshot_t *seed,
  const pet_program_output_contract_t *contract,pet_program_publish_fn publish,void *publish_context);
/* Sequence is a host-generated nonzero monotonic ID within this generation.
 * Accepted IDs never execute twice, including after a trap. Queue-full/rejected
 * events do not consume their ID. New events cannot overtake queued events.
 * Queue/receipts are volatile, not a durable job journal: do not restore or
 * automatically replay them after reboot. Durable task deduplication belongs
 * to the host transaction store; this layer promises dedup within one owner.
 */
pet_program_runtime_result_t pet_program_runtime_enqueue(pet_program_runtime_t *runtime,
  uint64_t generation, uint64_t sequence, const pet_program_event_t *event);
/* Performs ONE begin, <=quantum instructions, or one commit; never a second
 * event. COMPLETE stores one terminal report. Caller must consume it before
 * further execution (backpressure, no lost outcomes and no implicit retry).
 */
pet_program_runtime_result_t pet_program_runtime_pump(pet_program_runtime_t *runtime,
  uint64_t generation);
/* Native per-turn policy only; begin/commit remain one separately bounded
 * phase. A checkpoint yield does not consume an event/report or restart it. */
pet_program_runtime_result_t pet_program_runtime_pump_with_checkpoint(pet_program_runtime_t *runtime,
  uint64_t generation,pet_program_checkpoint_fn checkpoint,void *context);
/* Revocation is latched. Drops unstarted events and private work, preserving
 * committed state and any terminal report. Never resumes when current() becomes
 * true later. cancelled_events counts inflight+queued discarded events. Exact
 * generation required. No native handle release or IO happens in this module.
 */
pet_program_runtime_result_t pet_program_runtime_cancel(pet_program_runtime_t *runtime,
  uint64_t generation);
/* Reports remain readable after cancellation; a completed commit is never
 * relabelled failed by a later revocation. No authority callback after commit.
 */
pet_program_runtime_result_t pet_program_runtime_take_report(pet_program_runtime_t *runtime,
  uint64_t generation, pet_program_report_t *out);
/* Reads committed data only, including while private work is running/stopped.
 * This is a native snapshot, not a portable save file. Durable encode/atomic
 * publish/migration belong to the trusted transaction installer/store adapter.
 */
pet_program_runtime_result_t pet_program_runtime_snapshot(pet_program_runtime_t *runtime,
  uint64_t generation, pet_program_snapshot_t *out);
/* Same ownership/generation/reentry/alias checks as snapshot, including after
 * stop/revocation. Read the authoritative publication slot, not facade arrays.
 * UNCHANGED leaves out completely untouched; OK supplies a full snapshot with
 * the original zero-padding/unused-cell semantics. Errors change neither out
 * nor cursor. The parent installs cursor ONLY after validating the full result.
 * All buffers are pairwise disjoint and external to runtime; known may be NULL.
 * This is an observation, not an authority check, pump, or report consumption. */
pet_program_runtime_result_t pet_program_runtime_snapshot_if_changed(pet_program_runtime_t *runtime,
  uint64_t generation,const pet_program_snapshot_cursor_t *known,
  pet_program_snapshot_t *out,pet_program_snapshot_cursor_t *cursor);
