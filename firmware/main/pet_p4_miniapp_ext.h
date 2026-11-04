/* Track C hook prototypes for pet_p4_miniapp.c only; bodies live in
 * pet_p4_miniapp_ext.inc, which the runtime includes at its end so the hooks
 * share its verified loader, model lock and dispatch path. Never a separate owner. */
#pragma once
#include "pet_p4_component_features.h"
#if PET_P4_CAPABILITY_EXT
#include "pet_p4_component_ext.h"
#include "pet_p4_component_media.h"
/* "event:<name>" is a valid transition trigger only with p4-events-v1. */
static inline bool miniapp_ext_event_action(const char *action) { return action && !strncmp(action, "event:", 6) && pet_cev_name_valid(action + 6); }
#define MINIAPP_EXT_OR_EVENT_ACTION(action) || miniapp_ext_event_action(action)
static bool miniapp_ext_parse(miniapp_runtime_t *parsed, const cJSON *root, bool runtime_v4, char *error, size_t error_size);
static bool miniapp_ext_validate_bindings(const miniapp_runtime_t *runtime, const cJSON *root, const char *buttons_json, char *error, size_t error_size);
static void miniapp_ext_fire(miniapp_runtime_t *runtime, const pet_cext_ops_t *ops);
static void miniapp_ext_audio_apply(const pet_cext_audio_t *audio, const pet_seq_program_t *program);
static void miniapp_ext_process(uint64_t now_ms, bool visible);
/* Caller retains the exact ticket address until commit/abort; next stays alive.
 * Prepare precedes catalog durable commit. Every catalog failure aborts. Commit
 * follows durable catalog commit, then shared owner atomically publishes runtime.
 * Commit is not called with the runtime mutex held (alarm merge acquires it). */
typedef struct { pet_cmedia_ticket_t media; miniapp_runtime_t *next; } pet_cext_publish_ticket_t;
static bool miniapp_ext_prepare(miniapp_runtime_t *next, size_t slot, uint8_t generation, pet_cext_publish_ticket_t *ticket);
static bool miniapp_ext_commit(pet_cext_publish_ticket_t *ticket);
static bool miniapp_ext_abort(pet_cext_publish_ticket_t *ticket);
/* Compatibility immediate transaction, not a catalog rollback boundary. */
static bool miniapp_ext_publish(miniapp_runtime_t *next, size_t slot, uint8_t generation);
static void miniapp_ext_installed(const miniapp_runtime_t *next);
static void miniapp_ext_removed(const char *widget_id, size_t slot);
static bool miniapp_ext_install_chunk(const cJSON *payload, const char *path, const char *data, long index, char *error, size_t error_size);
static bool miniapp_ext_save_text(char *out, size_t cap);
static bool miniapp_ext_restore_text(miniapp_runtime_t *r, const cJSON *saved);
#else
#define MINIAPP_EXT_OR_EVENT_ACTION(action)
#endif
