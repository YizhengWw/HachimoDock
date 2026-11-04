#pragma once
#include "pet_p4_component_program_session.h"
#include "pet_p4_program_persistence.h"
#include "pet_p4_program_phase.h"
#if PET_COMPONENT_BACKGROUND_ENABLED
/* Fixed native parent storage, externally serialized with model/publication.
 * This table is NOT a resource grant, filesystem loader, RTOS task or package
 * callback. Its entries and embedded live sessions must never move or reset.
 * A verified installer supplies exact content identity, immutable descriptor,
 * authoritative loaded seed and native policy. Old hidden-stop semantics are
 * preserved by background=false; only a separately negotiated true policy may
 * survive view detachment. The real broker can admit fewer than four slots.
 */
#define PET_PROGRAM_REGISTRY_SLOTS PET_PROGRAM_SCHEDULER_SLOTS
struct pet_program_registry;
typedef struct {const struct pet_program_registry *registry;uint64_t serial;uint8_t slot;} pet_program_registration_t;
typedef struct {
    pet_component_program_session_t session;
    pet_component_program_config_t config;
    pet_program_snapshot_t committed;
    /* Cursor is published only with a completely validated committed cache.
     * Projection changes require seed revalidation, even at the same revision. */
    pet_program_snapshot_cursor_t capture_cursor;
    pet_program_ui_projection_t capture_projection;
    pet_app_store_pin_t pin;
    uint64_t serial,content[3];
    int32_t values[8];
    char id[48];
    bool occupied,background,stopped,checkpoint_pending;
} pet_program_registry_entry_t;
typedef struct pet_program_registry {
    const void *instance;
    pet_program_scheduler_t scheduler;
    pet_program_registry_entry_t entries[PET_PROGRAM_REGISTRY_SLOTS];
    uint64_t next_serial;
    uint8_t checkpoint_cursor;
    bool inside;
#if PET_PROGRAM_PHASE_TRACE
    pet_program_phase_t last_phase;
#endif
} pet_program_registry_t;
typedef enum {
    PET_REGISTRY_OK,PET_REGISTRY_ATTACHED,PET_REGISTRY_INVALID,PET_REGISTRY_FULL,
    PET_REGISTRY_CONFLICT,PET_REGISTRY_STOPPED,PET_REGISTRY_DENIED,
    PET_REGISTRY_PENDING,PET_REGISTRY_EXHAUSTED
} pet_program_registry_result_t;
/* Native immutable factory only. Partial failure leaves an inert, non-reusable
 * table; do not retry init. Production uses the real broker/publication/clock. */
bool pet_program_registry_init(pet_program_registry_t *r,bool (*factory)(pet_program_host_t*));
/* No disk access. Pin only a successfully preloaded private save. Duplicate
 * same-content attachment returns the existing owner WITHOUT timer restart or
 * reverting its snapshot to the supplied old seed. Changed content refuses;
 * parent must explicitly stop/checkpoint/release before a new installation.
 * Out starts zero and changes only on OK/ATTACHED. Unknown retirement retains
 * its slot/pin, accessible through native find; never silently release/replay. */
pet_program_registry_result_t pet_program_registry_attach(pet_program_registry_t *r,const char *id,
    const uint64_t content[3],const pet_component_program_config_t *config,
    const pet_program_snapshot_t *seed,const int32_t base[8],bool background,uint64_t now_ms,
    pet_program_registration_t *out);
bool pet_program_registry_find(pet_program_registry_t *r,const char *id,pet_program_registration_t *out);
/* Historical committed snapshot only, without replacing independent UI base. */
bool pet_program_registry_snapshot(pet_program_registry_t *r,pet_program_registration_t handle,pet_program_snapshot_t *out);
bool pet_program_registry_view(pet_program_registry_t *r,pet_program_registration_t handle,
    const int32_t base[8],int32_t values[8],pet_program_snapshot_t *snapshot);
pet_program_runtime_result_t pet_program_registry_enqueue(pet_program_registry_t *r,
    pet_program_registration_t handle,const char *action,const int32_t base[8]);
/* EXACTLY ONE aggregate dispatcher step. Collect/snapshot are observations,
 * never more pumps; UI publication belongs to the exact external view binding.
 * No persistence, IO, audio start, driver or native effect in this operation. */
pet_program_schedule_result_t pet_program_registry_step(pet_program_registry_t *r,uint64_t now_ms);
/* RAM-only one-entry checkpoint, under the same external serialization and
 * OUTSIDE UI/model locks. Acceptance is not durability; dirty data survives a
 * refusal. Real deferred worker remains responsible for fsync/A-B recovery. */
bool pet_program_registry_checkpoint_one(pet_program_registry_t *r);
pet_program_runtime_result_t pet_program_registry_stop(pet_program_registry_t *r,pet_program_registration_t handle);
/* Only physically stopped + latest checkpoint accepted. No disk deletion,
 * native struct reset or automatic resume. Old handles can never target reuse. */
bool pet_program_registry_release(pet_program_registry_t *r,pet_program_registration_t handle);
#endif
