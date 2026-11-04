/*
 * [Input] Persisted/embedded component packages, bounded sprite packs, and runtime input actions.
 * [Output] Bounded component catalog, builtin synchronization, active views, and sprite snapshots.
 * [Pos] Public contract for the ESP32-P4 component runtime.
 * [Sync] If this file changes, update `firmware/.folder.md` and `protocol.md`.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "pet_p4_game.h"
#include "pet_p4_stats.h"
#include "pet_p4_widget_data.h"
#include "pet_p4_fruit_core.h"
#include "pet_p4_surface.h"
#include "pet_p4_side_scene_json.h"
#include "pet_p4_storage_lane.h"
#include "pet_p4_catalog_profile.h"
#if PET_P4_CELLS_V1
#include "pet_p4_cells.h"
#endif
#include "pet_p4_program_phase.h"
#include "pet_p4_query_reply.h"
#include "pet_p4_widget_trace.h"

typedef struct {
  int32_t x,y,vx,vy,score;
  uint32_t elapsed_ms,revision,last_step_us,max_step_us;
  uint16_t coins;
  uint8_t phase,lives,checkpoint;
  int8_t direction;
  bool grounded,jump_down;
} pet_p4_miniapp_side_status_t;

#ifdef __cplusplus
extern "C" {
#endif

bool pet_p4_miniapp_side_status(pet_p4_miniapp_side_status_t*out);
typedef struct {
  uint32_t revision,last_step_us,max_step_us;
  uint16_t used_fuel,slice_budget_us;
  uint8_t phase,result;
  bool live,retiring,report_valid,slice_stalled;
} pet_p4_miniapp_program_status_t;
/* Categorical status/timing only: no program, cells, event words or owner token. */
bool pet_p4_miniapp_program_status(pet_p4_miniapp_program_status_t *out);

#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_P4_MINIAPP_ACTION_MAX 48
#define PET_P4_MINIAPP_BACKGROUND_MAX 4
#define PET_P4_MINIAPP_BACKGROUND_ENGINE "p4-program-background-v2"
typedef struct {
  char widget_id[48];uint64_t registration;
  uint32_t revision,last_step_us,max_step_us;uint8_t phase;
  bool live,stopped,checkpoint_pending;
} pet_p4_miniapp_background_status_t;
/* Native explicit controller of verified installed pure programs. No automatic
 * package start/boot policy, host request replay, audio or peripheral grant.
 * Start requires exact device-catalog checksums, and never navigates the UI.
 * Stop uses the exact opaque serial, not an ID-only stale cancellation.
 * Protocol worker owns the storage-command/state lane when starting. */
bool pet_p4_miniapp_background_available(void);
#if PET_PROGRAM_PHASE_TRACE
/* Boot-lifetime worst aggregate turn, read-only under the native registry lock. */
bool pet_p4_miniapp_program_peak(pet_program_phase_t *out);
#if PET_P4_QUERY_OBSERVATION
bool pet_p4_miniapp_program_peak_context(pet_p4_query_observation_t *query,pet_widget_trace_lane_snapshot_t *save);
#endif
#endif
/* RAM-only authoritative catalog identity, under the caller's model lock.
 * Requires previously verified resources. Does not parse/start a program,
 * navigate, pin storage, load files or grant a resource. */
bool pet_p4_miniapp_background_content(const char *id,uint32_t content[3]);
size_t pet_p4_miniapp_background_list(pet_p4_miniapp_background_status_t out[PET_P4_MINIAPP_BACKGROUND_MAX]);
bool pet_p4_miniapp_background_start(const char *id,const uint32_t content[3],uint64_t *registration,char *error,size_t error_size);
bool pet_p4_miniapp_background_stop(const char *id,uint64_t registration,char *error,size_t error_size);
/* Resource pack capacity. p4-graphics-v2 (PET_P4_GRAPHICS_V2, declared by a
 * package as graphics:"p4-graphics-v2") raises the declared ceilings to 8 scene
 * sprites / 16384 sprite pixels and 6 surface images / 256 KiB compiled; the
 * keyframe background parent adds room for up to 3 extra 100 KB P4J1 frames.
 * Packages without the declaration keep the v1 numbers on every build. */
#include "pet_p4_render_policy.h"
#define PET_P4_MINIAPP_SPRITE_V1_MAX 4
#define PET_P4_MINIAPP_SPRITE_V1_PIXELS 4096
#define PET_P4_MINIAPP_SPRITE_ID_MAX 16
#if PET_P4_GRAPHICS_V2
#define PET_P4_MINIAPP_SPRITE_DECLARED_MAX 8
#define PET_P4_MINIAPP_SPRITE_V2_PIXELS 16384
#if PET_P4_KEYFRAME_BG
#define PET_P4_MINIAPP_KEYFRAME_EXTRA 3
#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (256 * 1024 + 3 * 100000)
#else
#define PET_P4_MINIAPP_KEYFRAME_EXTRA 0
#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (256 * 1024)
#endif
#define PET_P4_MINIAPP_SPRITE_MAX (PET_P4_MINIAPP_SPRITE_DECLARED_MAX + PET_P4_MINIAPP_KEYFRAME_EXTRA)
#else
#define PET_P4_MINIAPP_SPRITE_DECLARED_MAX 4
#define PET_P4_MINIAPP_SPRITE_V2_PIXELS PET_P4_MINIAPP_SPRITE_V1_PIXELS
#define PET_P4_MINIAPP_SPRITE_MAX 4
#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (128 * 1024)
#endif

typedef struct {
  char id[PET_P4_MINIAPP_SPRITE_ID_MAX];
  uint16_t frame_width;
  uint16_t frame_height;
  uint8_t frames;
  uint8_t fps;
  uint32_t data_offset;
  uint32_t data_length;
} pet_p4_miniapp_sprite_t;

typedef struct {
  uint8_t count;
  uint32_t data_length;
  pet_p4_miniapp_sprite_t items[PET_P4_MINIAPP_SPRITE_MAX];
  uint8_t pixels[PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX];
} pet_p4_miniapp_sprite_pack_t;

typedef struct {
  bool active;
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  char state[24];
  char page[24];
  char title[64];
  char eyebrow[96];
  char headline[160];
  char metric_label[96];
  char metric_value[64];
  char metric_unit[32];
  char badge[16];
  char note[160];
  char footer[160];
  int progress_percent;
  char progress_label[64];
  char visual_style[20];
  char visual_palette[20];
  char visual_layout[20];
  char visual_sprite[20];
  pet_p4_game_frame_t game;
  uint32_t revision;
  char data_source[48];
  int32_t data_page;
  pet_p4_data_view_t data;
  bool media;
  bool app_surface;
  bool app_host;
  uint8_t surface_page;
  bool instrument;
  bool slicing;
  uint8_t effect_volume, ambience_volume;
} pet_p4_miniapp_view_t;

/* Native renderer-owned buffers. One runtime-lock capture binds the view,
 * resolved app surface, side scene and changed image bytes to one publication.
 * Not an input grant; dispatch continues to validate current native ownership.
 * Allocate in PSRAM/static storage, never a render-task stack. */
#ifndef PET_P4_APP_PHASE_TRACE
#define PET_P4_APP_PHASE_TRACE 0
#endif
typedef struct {
  pet_p4_miniapp_view_t view;
  pet_surface_t surface;
  pet_side_scene_t scene;
  pet_side_art_t art;
  char media_source[48];
  char confirmation_prompt[121];
  uint32_t image_revision;
  uint32_t media_generation; /* captured with view under the same runtime lock */
  bool surface_active,side_active,images_changed,confirmation_active;
#if PET_P4_CELLS_V1
  pet_cells_t cells; /* p4-cells-v1 grid captured with the surface for cells nodes */
#endif
#if PET_P4_APP_PHASE_TRACE
  uint32_t capture_wait_us,capture_total_us;
#endif
} pet_p4_miniapp_render_snapshot_t;

typedef struct {
  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];
  char title[64];
  bool active;
  uint8_t resource_state;
  uint8_t image_count;
} pet_p4_miniapp_catalog_entry_t;

esp_err_t pet_p4_miniapp_init(void);
esp_err_t pet_p4_miniapp_sync_builtins(void);
bool pet_p4_miniapp_active(void);
bool pet_p4_miniapp_media_active(void);
bool pet_p4_miniapp_media_surface_animated(void);
bool pet_p4_miniapp_media_config(char source[48],pet_surface_t *surface);
bool pet_p4_miniapp_app_surface(pet_surface_t *surface);
bool pet_p4_miniapp_instrument_config(uint8_t *effect, uint8_t *ambience);
bool pet_p4_miniapp_slicing_active(void);
bool pet_p4_miniapp_fruit_snapshot(pet_fruit_t *out);
void pet_p4_miniapp_fruit_process(uint64_t now_ms, bool visible);
/* p4-sfx-v1: enables the bounded cue path while a declaring component is visible
 * and drains cues queued by transitions, ticks and scene rules. */
void pet_p4_miniapp_sfx_process(uint64_t now_ms, bool audible);
#if PET_P4_EVENTS
/* p4-events-v1 (Track C): PC-authorised background bubble policy and the one pending
 * hidden-alarm notification (title = component title, text <= 40 bytes). */
void pet_p4_miniapp_alarm_policy(bool background_notify);
/* Existing protocol storage worker only: restore saved hidden alarms before
 * granting per-boot delivery. Failure preserves prior RAM state and policy. */
bool pet_p4_miniapp_alarm_policy_prepare(bool background_notify);
bool pet_p4_miniapp_alarm_notification(char title[64], char text[41]);
#endif
void pet_p4_miniapp_pointer(const pet_p4_pointer_event_t *event);
bool pet_p4_miniapp_pointer_active(void);
bool pet_p4_miniapp_active_id(char *out, size_t out_size);
bool pet_p4_miniapp_get_view(pet_p4_miniapp_view_t *out);
/* Bounded read-only evidence. Digests exclude instance/revision so an explicit
 * reboot readback can compare content; none of these fields prove durability. */
typedef struct {
  char widget_id[48];
  uint32_t instance_generation,view_revision,persist_digest,cells_digest,vm_digest,vm_revision,vm_schema;
  uint16_t cells_count,vm_cells;
  uint8_t cols,rows,bits;
  bool active,has_cells,has_vm;
} pet_p4_miniapp_observation_t;
bool pet_p4_miniapp_observe(pet_p4_miniapp_observation_t *out,pet_p4_miniapp_view_t *view);
bool pet_p4_miniapp_render_snapshot(pet_p4_miniapp_render_snapshot_t *out,
  pet_p4_miniapp_sprite_pack_t *images,uint32_t *cached_image_revision);
bool pet_p4_miniapp_side_snapshot(pet_side_scene_t *scene,pet_side_art_t *art);
bool pet_p4_miniapp_side_axis(int horizontal);
void pet_p4_miniapp_host_process(uint64_t now_ms,bool visible);
bool pet_p4_miniapp_host_source(const char *source);
bool pet_p4_miniapp_get_surface_images(pet_p4_miniapp_sprite_pack_t *out, uint32_t *revision);
bool pet_p4_miniapp_get_sprites(pet_p4_miniapp_sprite_pack_t *out);
bool pet_p4_miniapp_installed_id(char *out, size_t out_size);
size_t pet_p4_miniapp_catalog_count(void);
size_t pet_p4_miniapp_catalog_capacity(void);
bool pet_p4_miniapp_catalog_recovery_blocked(void);
bool pet_p4_miniapp_resource_repair_active(void);
size_t pet_p4_miniapp_catalog_selected(void);
bool pet_p4_miniapp_catalog_get(size_t index, pet_p4_miniapp_catalog_entry_t *out);
void pet_p4_miniapp_catalog_focus_active(void);
bool pet_p4_miniapp_catalog_move(int delta);
/* Full physical metadata, including hidden records. No resource/save reads.
 * Reconcile is one CAS catalog commit; never install/activate or clean files.
 * The protocol verifies board/boot identity before invoking it. */
/* Local browsing queues one immutable token onto the existing protocol worker;
 * no flash on touch/render/input. Stale tokens do nothing. No retry on failure.
 * Worker calls run under its existing state lock and storage command lane. */
typedef bool (*pet_p4_miniapp_catalog_cursor_schedule_fn)(uint64_t token,void *context);
bool pet_p4_miniapp_catalog_cursor_configure(pet_p4_miniapp_catalog_cursor_schedule_fn schedule,void *context);
bool pet_p4_miniapp_catalog_cursor_run(uint64_t token);
/* PC selection waits for physical NVS commit before a positive ACK. */
bool pet_p4_miniapp_catalog_select_durable(size_t index);
cJSON *pet_p4_miniapp_catalog_inspect(void);
const char *pet_p4_miniapp_catalog_reconcile(const cJSON *request,char *error,size_t error_size);
bool pet_p4_miniapp_catalog_select(size_t index);
bool pet_p4_miniapp_catalog_activate_selected(void);
bool pet_p4_miniapp_catalog_activate_id(const char *widget_id);
/* UI requests enqueue one immutable token, never perform filesystem work on
 * the rendering/input task. All calls require the host state lock. The worker
 * must grant its storage-lane command token before run(); run restores that
 * lock on every return. Schedule must use a bounded, zero-wait queue. */
typedef bool (*pet_p4_miniapp_activation_schedule_fn)(uint64_t token,void *context);
typedef enum {
  PET_MINIAPP_ACTIVATION_REJECTED=0,
  PET_MINIAPP_ACTIVATION_READY=1,
  PET_MINIAPP_ACTIVATION_QUEUED=2,
} pet_p4_miniapp_activation_request_t;
typedef enum {
  PET_MINIAPP_ACTIVATION_FAILED=0,
  PET_MINIAPP_ACTIVATION_OPENED=1,
  PET_MINIAPP_ACTIVATION_OPENED_UNSAVED=2,
  PET_MINIAPP_ACTIVATION_CANCELLED=3,
} pet_p4_miniapp_activation_result_t;
bool pet_p4_miniapp_activation_configure(pet_p4_miniapp_activation_schedule_fn schedule,void *context);
pet_p4_miniapp_activation_request_t pet_p4_miniapp_activation_request_selected(void);
pet_p4_miniapp_activation_result_t pet_p4_miniapp_activation_run(uint64_t token);
void pet_p4_miniapp_activation_cancel(void);
void pet_p4_miniapp_activation_guard(bool component_center_visible);
bool pet_p4_miniapp_activation_navigation_current(uint64_t token);
bool pet_p4_miniapp_activation_pending(void);
/* Retire an exact consumed queue token only if no worker preparation started.
 * Never releases a running scope or another request's occupancy. */
bool pet_p4_miniapp_activation_reject_not_started(uint64_t token);
bool pet_p4_miniapp_sync_stats(const pet_p4_stats_model_t *stats);
void pet_p4_miniapp_process(uint64_t now_ms, bool visible);
bool pet_p4_miniapp_dispatch_action(const char *action, uint64_t now_ms);
bool pet_p4_miniapp_has_input(const char *event_name);
bool pet_p4_miniapp_input_policy(const char *event_name,bool press);
bool pet_p4_miniapp_hold_input(const char *event_name);
bool pet_p4_miniapp_hold_event(const char *action,int phase,unsigned owner,uint64_t now_ms);
uint64_t pet_p4_miniapp_input_context(void);
bool pet_p4_miniapp_confirmation(char prompt[121],uint64_t now_ms);
bool pet_p4_miniapp_resolve_input(
  const char *event_name,
  char *action,
  size_t action_size
);
bool pet_p4_miniapp_dispatch_input(
  const char *event_name,
  uint64_t now_ms,
  char *action,
  size_t action_size
);

bool pet_p4_miniapp_install_begin(const cJSON *payload, char *error, size_t error_size);
#if PET_P4_STORAGE_PRESERVE_UNCHANGED
bool pet_p4_miniapp_install_unchanged(void);
#endif
bool pet_p4_miniapp_clock_sync(const cJSON *payload, uint64_t now_ms);
bool pet_p4_miniapp_install_chunk(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_install_commit(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_remove(const char *widget_id, char *error, size_t error_size);
bool pet_p4_miniapp_remove_preserving_save(const char *widget_id, char *error, size_t error_size);
/* Configure after boot migration, then grant/drop one explicit command token
 * while the protocol worker holds its state mutex. Calls without a token stay
 * synchronous: neither boot nor native fallback may unlock a mutex it lacks. */
bool pet_p4_miniapp_storage_lane_configure(pet_storage_lock_fn unlock,
  pet_storage_lock_fn lock,pet_storage_owner_fn owner,void *context);
bool pet_p4_miniapp_storage_lane_enter(void);
bool pet_p4_miniapp_storage_lane_leave(void);
bool pet_p4_miniapp_storage_busy(void);

#ifdef __cplusplus
}
#endif
