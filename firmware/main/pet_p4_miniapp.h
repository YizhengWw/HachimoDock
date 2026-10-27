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

#ifdef __cplusplus
extern "C" {
#endif

#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_P4_MINIAPP_ACTION_MAX 48
#define PET_P4_MINIAPP_CATALOG_MAX 32
#define PET_P4_MINIAPP_SPRITE_MAX 4
#define PET_P4_MINIAPP_SPRITE_ID_MAX 16
#define PET_P4_MINIAPP_SPRITE_PIXEL_BYTES_MAX (128 * 1024)

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
void pet_p4_miniapp_pointer(const pet_p4_pointer_event_t *event);
bool pet_p4_miniapp_pointer_active(void);
bool pet_p4_miniapp_active_id(char *out, size_t out_size);
bool pet_p4_miniapp_get_view(pet_p4_miniapp_view_t *out);
void pet_p4_miniapp_host_process(uint64_t now_ms,bool visible);
bool pet_p4_miniapp_host_source(const char *source);
bool pet_p4_miniapp_get_surface_images(pet_p4_miniapp_sprite_pack_t *out, uint32_t *revision);
bool pet_p4_miniapp_get_sprites(pet_p4_miniapp_sprite_pack_t *out);
bool pet_p4_miniapp_installed_id(char *out, size_t out_size);
size_t pet_p4_miniapp_catalog_count(void);
bool pet_p4_miniapp_catalog_recovery_blocked(void);
bool pet_p4_miniapp_resource_repair_active(void);
size_t pet_p4_miniapp_catalog_selected(void);
bool pet_p4_miniapp_catalog_get(size_t index, pet_p4_miniapp_catalog_entry_t *out);
void pet_p4_miniapp_catalog_focus_active(void);
bool pet_p4_miniapp_catalog_move(int delta);
bool pet_p4_miniapp_catalog_select(size_t index);
bool pet_p4_miniapp_catalog_activate_selected(void);
bool pet_p4_miniapp_catalog_activate_id(const char *widget_id);
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
bool pet_p4_miniapp_clock_sync(const cJSON *payload, uint64_t now_ms);
bool pet_p4_miniapp_install_chunk(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_install_commit(const cJSON *payload, char *error, size_t error_size);
bool pet_p4_miniapp_remove(const char *widget_id, char *error, size_t error_size);
bool pet_p4_miniapp_remove_preserving_save(const char *widget_id, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif
