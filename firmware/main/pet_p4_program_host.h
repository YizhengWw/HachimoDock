/* Default-off trusted lifecycle adapter, not a package/wire entry point.
 * All storage is caller-owned, fixed-address and private to trusted C. Never
 * serialize a backend/owner/ticket/pointer, or accept one from a component.
 * Ordinary factories create PROGRAM-only owners; production quota stays zero.
 * A separate trusted constructor admits ONLY fixed native diagnostic vectors
 * using real NATIVE_PROBE leases. No component can select this constructor.
 *
 * EXTERNAL SERIALIZATION IS REQUIRED: one host/state/lifecycle mutex through
 * every API, including commit and owner revocation. Backend functions use the
 * same broker's short lock, never the host mutex; no lock may be held across IO.
 * inside only rejects synchronous callback reentry, not concurrent threads.
 * Backend functions are bounded trusted broker/flag operations, not providers
 * or untrusted effects. They cannot mutate the host or invoke lifecycle changes.
 * The actual device factory installs an immutable fixed-copy publisher: the
 * broker mux linearizes commit with realtime reservation/revocation, not VM
 * execution. Other backends still require that reviewed boundary; a flag
 * sample alone does not prove exclusion. This module does not wire scheduling,
 * installation or persistent save publication.
 * P4 time/stack/audio-contention gates remain required before any integration.
 */
#pragma once
#include "pet_p4_program_runtime.h"
#include "pet_p4_resources.h"
#include "pet_p4_program_deadline.h"

typedef struct {
  void *context;
  bool (*audio_busy)(void *context);
  pet_resource_result_t (*owner_open)(void *context, uint32_t mask, pet_resource_owner_t *out);
  pet_resource_result_t (*acquire)(void *context, pet_resource_owner_t owner,
    pet_resource_kind_t kind, uint32_t units, pet_resource_lease_t *out);
  bool (*current)(void *context, pet_resource_lease_t exact_lease);
  void (*revoke)(void *context, pet_resource_owner_t exact_owner);
  pet_resource_finish_t (*finish)(void *context, pet_resource_lease_t exact_lease);
} pet_program_host_backend_t;

#define PET_PROGRAM_UI_VARIABLES 8U
#define PET_PROGRAM_UI_UNBOUND UINT8_MAX
typedef bool (*pet_program_host_publish_fn)(void *context,pet_resource_lease_t exact_lease,
  pet_program_state_t *target,const pet_program_state_t *candidate);
/* Trusted receiver metadata for integer model slots, not a package grant.
 * One output can write one slot, and each slot has at most one writer. The
 * receiver derives domains from its own model. Enum/text fields require their
 * own validator and are NOT made safe by an integer range. Unused domains are
 * zero, unmapped output slots are UI_UNBOUND. No pointers/callbacks retained. */
typedef struct {
  uint32_t schema;
  int32_t minimum[PET_PROGRAM_UI_VARIABLES],maximum[PET_PROGRAM_UI_VARIABLES];
  uint8_t variables,target[PET_PROGRAM_OUTPUTS];
} pet_program_ui_projection_t;
typedef struct {
  uint64_t generation;
  uint32_t schema,revision;
  int32_t values[PET_PROGRAM_UI_VARIABLES];
  uint8_t variables;
} pet_program_ui_snapshot_t;

typedef struct {
  /* Private by contract. Do not copy, move, inspect or mutate live storage. */
  const void *instance;
  pet_program_runtime_t runtime;
  pet_program_image_t staged;
  pet_program_host_backend_t backend;
  pet_program_host_publish_fn publish;
  void *publish_context;
  pet_program_clock_fn clock;
  void *clock_context;
  uint32_t slice_budget_us;
  int64_t last_clock_us;
  uint8_t zero_progress_turns;
  pet_resource_owner_t owner;
  pet_resource_lease_t lease;
  uint64_t greatest_generation;
  uint32_t marker;
  uint16_t admitted_fuel;
  pet_resource_kind_t admission_kind; /* Immutable native constructor choice. */
  pet_resource_result_t last_admission;
  pet_resource_finish_t last_finish;
  pet_program_ui_projection_t projection;
  /* Native-only unique lifecycle attachment. Never serialized/dereferenced by
   * the VM. Retained on failed restart; cleared ONLY on a successful new owner. */
  const struct pet_program_group *group_owner;
  bool inside, staged_ready, bound, projection_bound,admission_attempted,clock_seen,slice_stalled;
} pet_program_host_t;

/* Fresh storage with zero-initialized members (e.g. {0}) only. Semantic
 * lifecycle/identity checks ignore padding, then initialize all storage. Never
 * re-init even a stopped host: use start, preserving history and reports. */
bool pet_program_host_init(pet_program_host_t *host, const pet_program_host_backend_t *backend);
/* Fixed native diagnostic factories only, never package/wire selection. Same
 * runtime/current/lifecycle boundaries, but a real NATIVE_PROBE ticket; it is
 * not a virtual PROGRAM grant or evidence arbitrary packages are admitted. */
bool pet_program_host_init_native_probe(pet_program_host_t *host,const pet_program_host_backend_t *backend);
/* Fresh trusted factory only, before the first admission attempt; immutable
 * for every later owner. No user-supplied callback and no implicit fallback
 * after registration. A failed callback must leave the complete state intact. */
bool pet_program_host_bind_publication(pet_program_host_t *host,pet_program_host_publish_fn publish,void *context);
/* Fresh trusted factory only; immutable before the FIRST valid admission
 * attempt, including an audio/busy denial. 1..2000us is an early-yield policy,
 * not a claim of hard real-time execution. Package clocks/budgets forbidden. */
bool pet_program_host_bind_clock(pet_program_host_t *host,pet_program_clock_fn clock,void *context,uint32_t budget_us);
/* Full validation and owned immutable copy. May stage while the old model is
 * active; this never mutates its code/state. Failure preserves previous staging
 * as well as committed model/report. Inputs/outputs must not overlap the host. */
pet_program_runtime_result_t pet_program_host_stage(pet_program_host_t *host,
  const pet_program_image_t *image);
/* No automatic replacement/cancellation: a live model or unread report refuses
 * start. Failure leaves the old runtime/snapshot/report/generation readable.
 * Generation comes ONLY from a newly opened native owner, never the caller.
 * A failed admission still consumes its observed owner generation, so restart
 * can never regress. NULL seed explicitly means new blank state; no migration.
 * Success writes generation; every error leaves it unchanged. */
pet_program_runtime_result_t pet_program_host_start(pet_program_host_t *host,
  uint16_t quantum, const pet_program_snapshot_t *seed, uint64_t *generation);
/* Transactional install publication: unlike stage()+start(), a denied start
 * does not replace even the prior staging image. External validated image and
 * seed remain immutable; caller holds the parent package/lifecycle lock. This
 * API does not write package files or bypass production PROGRAM admission. */
pet_program_runtime_result_t pet_program_host_start_image(pet_program_host_t *host,
  const pet_program_image_t *image,uint16_t quantum,
  const pet_program_snapshot_t *seed,uint64_t *generation);
/* Same transactional start, with receiver-derived restrictive output limits.
 * Failure (including incompatible seed) preserves the previous image/state,
 * limits, reports and staging; it does not reset data or broaden a grant. */
pet_program_runtime_result_t pet_program_host_start_image_with_contract(pet_program_host_t *host,
  const pet_program_image_t *image,uint16_t quantum,const pet_program_snapshot_t *seed,
  const pet_program_output_contract_t *contract,uint64_t *generation);
/* Atomically activates an image and copies receiver-owned projection metadata,
 * deriving restrictive VM output domains from it. Failure preserves the old
 * recipe, runtime and staging. NULL is invalid (use the legacy APIs instead).
 * This does not bind an installed package or grant a UI resource lease. */
pet_program_runtime_result_t pet_program_host_start_image_with_projection(pet_program_host_t *host,
  const pet_program_image_t *image,uint16_t quantum,const pet_program_snapshot_t *seed,
  const pet_program_ui_projection_t *projection,uint64_t *generation);
pet_program_runtime_result_t pet_program_host_enqueue(pet_program_host_t *host,
  uint64_t generation, uint64_t sequence, const pet_program_event_t *event);
/* Exactly ONE runtime pump, no loop/drain/retry. Caller must yield afterwards. */
pet_program_runtime_result_t pet_program_host_pump(pet_program_host_t *host, uint64_t generation);
/* First synchronously discard private work, then revoke/finish the exact lease.
 * Cancellation is latched; no automatic resume when audio/current recovers.
 * A completed report is preserved, never relabelled by later cancellation. */
pet_program_runtime_result_t pet_program_host_cancel(pet_program_host_t *host, uint64_t generation);
pet_program_runtime_result_t pet_program_host_take_report(pet_program_host_t *host,
  uint64_t generation, pet_program_report_t *out);
pet_program_runtime_result_t pet_program_host_snapshot(pet_program_host_t *host,
  uint64_t generation, pet_program_snapshot_t *out);
pet_program_runtime_result_t pet_program_host_snapshot_if_changed(pet_program_host_t *host,
  uint64_t generation,const pet_program_snapshot_cursor_t *known,
  pet_program_snapshot_t *out,pet_program_snapshot_cursor_t *cursor);
/* Bounded current-owner observation, no pump/enqueue/report consumption. A
 * denied exact lease/audio check latches cancellation before physical release.
 * Useful to retire dependent timers/subscriptions before offering more work.
 * Not an atomic exclusion against another task: the same parent lock is needed. */
pet_program_runtime_result_t pet_program_host_check_current(pet_program_host_t *host,
  uint64_t generation);
/* Unlike the historical snapshot API, this is current-owner-only. Stage a
 * complete integer model from the caller's latest base; preserve unbound slots
 * and do not consume a report. Recheck exact lease/audio before publication.
 * Observed revocation latches cancellation. Every error leaves out unchanged.
 * Caller MUST hold the same package/model/lifecycle mutex through this call
 * AND installation of the returned model, checking its own UI identity/lease.
 * This library cannot authorize another UI or resolve concurrent form writers.
 * All buffers are disjoint from each other and the host; arrays have 8 slots. */
pet_program_runtime_result_t pet_program_host_project_current(pet_program_host_t *host,
  uint64_t generation,const int32_t base[PET_PROGRAM_UI_VARIABLES],pet_program_ui_snapshot_t *out);
